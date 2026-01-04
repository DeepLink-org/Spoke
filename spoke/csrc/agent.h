#pragma once
#include "actor.h"
#include "serializer.h"
#include "types.h"
#include <atomic>
#include <cstdio>  // For fprintf
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace spoke {

struct ActorHandle {
    pid_t pid;
    int   tx_fd;
    int   rx_fd;
};

class Agent {
public:
    Agent(): monitor_thread_(&Agent::monitor, this) {}

    ~Agent()
    {
        running_ = false;
        if (monitor_thread_.joinable())
            monitor_thread_.join();
        for (auto& [id, h] : registry_) {
            close(h.tx_fd);
            close(h.rx_fd);
            kill(h.pid, SIGTERM);
            waitpid(h.pid, nullptr, 0);
        }
    }

    // ==========================================
    // [v5 核心] 工厂模式启动 (供 Daemon 使用)
    // ==========================================
    pid_t spawnActor(const std::string& type, const std::string& id)
    {
        // Enforce serialized forking to avoid multi-thread fork hazards
        std::lock_guard<std::mutex> fork_lk(spawn_mtx_);

        // Fix: Cleanup existing actor if ID collision
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (registry_.find(id) != registry_.end()) {
                auto& h = registry_[id];
                close(h.tx_fd);
                close(h.rx_fd);
                kill(h.pid, SIGTERM);
                waitpid(h.pid, nullptr, 0);
                registry_.erase(id);
                std::cout << "[Agent] Cleaned up existing actor: " << id << std::endl;
            }
        }

        int p2c[2], c2p[2];
        if (pipe(p2c) != 0 || pipe(c2p) != 0)
            return -1;
        std::cout << "[Agent] Forking for actor: " << id << "..." << std::endl;
        pid_t pid = fork();
        if (pid == 0) {
            // ... child ...
            // Child process: Exec into a new image to clear all threads/mutexes
            close(p2c[1]);
            close(c2p[0]);

            char    exe_path[1024];
            ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
            if (len == -1) {
                perror("readlink");
                exit(1);
            }
            exe_path[len] = '\0';

            std::string rx_str = std::to_string(p2c[0]);
            std::string tx_str = std::to_string(c2p[1]);

            // Args: <exe> --worker <type> <id> <rx> <tx> NULL
            std::vector<char*> args;
            args.push_back(exe_path);
            args.push_back(strdup("--worker"));
            args.push_back(strdup(type.c_str()));
            args.push_back(strdup(id.c_str()));
            args.push_back(strdup(rx_str.c_str()));
            args.push_back(strdup(tx_str.c_str()));
            args.push_back(nullptr);

            // Ensure child dies if parent (Daemon) dies
            prctl(PR_SET_PDEATHSIG, SIGTERM);

            execv(exe_path, args.data());

            // If execv returns, it failed
            perror("execv failed");
            exit(1);
        }
        std::cout << "[Agent] Forked PID: " << pid << ". Updating registry..." << std::endl;
        close(p2c[0]);  // Close Parent Read from Child (Wait, p2c is Parent->Child)
        // p2c: 0=Read, 1=Write. Parent writes to 1. Child reads from 0.
        // Parent MUST CLOSE 0.
        close(c2p[1]);  // c2p: 0=Read, 1=Write. Child writes to 1. Parent reads from 0.
        // Parent MUST CLOSE 1.

        std::cout << "[Agent] Parent Pipes: TX=" << p2c[1] << " (p2c[1]), RX=" << c2p[0] << " (c2p[0])" << std::endl;

        std::lock_guard<std::mutex> lk(mtx_);
        registry_[id] = {pid, p2c[1], c2p[0]};
        std::cout << "[Agent] Actor spawned successfully: " << id << " with RX_FD=" << c2p[0] << std::endl;
        return pid;
    }

    // ==========================================
    // [v1 修复] 模板模式启动 (供 smoke_v1 本地测试使用)
    // ==========================================
    template<typename T>
    pid_t spawnActor(const std::string& id)
    {
        std::lock_guard<std::mutex> fork_lk(spawn_mtx_);

        int p2c[2], c2p[2];
        if (pipe(p2c) != 0 || pipe(c2p) != 0)
            return -1;
        pid_t pid = fork();
        if (pid == 0) {
            close(p2c[1]);
            close(c2p[0]);
            // 直接构造 T，不查工厂
            T instance(id, p2c[0], c2p[1]);
            instance.run();
            exit(0);
        }
        close(p2c[0]);
        close(c2p[1]);
        std::lock_guard<std::mutex> lk(mtx_);
        registry_[id] = {pid, p2c[1], c2p[0]};
        return pid;
    }

    // ==========================================
    // [v5 核心] 底层 Raw 调用 (传输序列化后的字节)
    // ==========================================
    std::future<std::string> callRemoteRaw(const std::string& id, Action act, const std::string& data)
    {
        auto     promise = std::make_shared<std::promise<std::string>>();
        auto     future  = promise->get_future();
        uint32_t s       = seq_++;

        std::lock_guard<std::mutex> lk(mtx_);
        if (registry_.find(id) == registry_.end()) {
            promise->set_value("");
            return future;
        }
        promises_[s] = promise;

        PipeHeader ph{act, s, (uint32_t)data.size()};
        int        fd = registry_[id].tx_fd;

        if (write(fd, &ph, sizeof(ph)) < 0) {
            std::cerr << "[Agent] Failed to write header to actor " << id << std::endl;
            promises_.erase(s);
            promise->set_value("");
            return future;
        }
        if (!data.empty()) {
            if (write(fd, data.data(), data.size()) < 0) {
                std::cerr << "[Agent] Failed to write body to actor " << id << std::endl;
                promises_.erase(s);
                promise->set_value("");
                return future;
            }
        }

        return future;
    }

    // ==========================================
    // [v1 修复] 本地泛型调用辅助 (供 smoke_v1 使用)
    // ==========================================
    template<typename ReqT, typename RespT>
    std::future<RespT> callRemote(const std::string& id, Action act, const ReqT& req)
    {
        // 1. 序列化请求
        std::string req_raw = Pack(req);

        // 2. 异步调用 Raw 接口并转换结果
        // 注意：这里使用 std::async 启动一个任务来等待 raw future 并反序列化
        return std::async(std::launch::async, [this, id, act, req_raw]() {
            auto        fut      = this->callRemoteRaw(id, act, req_raw);
            std::string resp_raw = fut.get();
            return Unpack<RespT>(resp_raw);
        });
    }

private:
    void monitor()
    {
        while (running_) {
            std::vector<pollfd> pfds;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& [id, h] : registry_)
                    pfds.push_back({h.rx_fd, POLLIN, 0});
            }
            if (pfds.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Debug Heartbeat
            static auto last_log = std::chrono::steady_clock::now();
            auto        now      = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 3) {
                fprintf(stderr, "[Agent] Monitor Polling %lu FDs. First FD: %d\n", pfds.size(), pfds[0].fd);
                last_log = now;
            }

            int ret = poll(pfds.data(), pfds.size(), 50);
            if (ret <= 0)
                continue;

            // fprintf(stderr, "[Agent] Poll returned %d events.\n", ret);

            for (auto& p : pfds) {
                if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
                    PipeHeader ph;
                    int        n = read(p.fd, &ph, sizeof(ph));
                    fprintf(stderr, "[Agent] read(FD=%d) returned %d bytes. Expected %lu.\n", p.fd, n, sizeof(ph));

                    if (n == sizeof(ph)) {
                        std::string body;
                        if (ph.data_size > 0) {
                            body.resize(ph.data_size);
                            size_t tot = 0;
                            while (tot < ph.data_size) {
                                int r = read(p.fd, body.data() + tot, ph.data_size - tot);
                                if (r <= 0) {
                                    fprintf(stderr, "[Agent] Error/EOF reading body from FD %d. r=%d\n", p.fd, r);
                                    break;
                                }
                                tot += r;
                            }
                            if (tot < ph.data_size) {
                                fprintf(
                                    stderr, "[Agent] Incomplete body read. Got %zu, expected %u\n", tot, ph.data_size);
                                continue;
                            }
                        }

                        std::lock_guard<std::mutex> lk(mtx_);
                        if (promises_.count(ph.seq_id)) {
                            // fprintf(stderr, "[Agent] Fulfilling promise for Seq: %u\n", ph.seq_id);
                            promises_[ph.seq_id]->set_value(body);
                            promises_.erase(ph.seq_id);
                        }
                        else {
                            fprintf(stderr, "[Agent] Promise NOT FOUND for Seq: %u (Orphaned?)\n", ph.seq_id);
                        }
                    }
                    else if (n > 0) {
                        fprintf(stderr, "[Agent] Partial header read! Got %d bytes. Dropping.\n", n);
                    }
                    else if (n == 0) {
                        // EOF
                        fprintf(stderr, "[Agent] Actor disconnected (EOF) on FD %d\n", p.fd);
                        std::lock_guard<std::mutex> lk(mtx_);
                        std::string                 dead_id;
                        for (auto& [id, h] : registry_) {
                            if (h.rx_fd == p.fd) {
                                dead_id = id;
                                break;
                            }
                        }
                        if (!dead_id.empty()) {
                            fprintf(stderr, "[Agent] Detected death of actor: %s\n", dead_id.c_str());
                            registry_.erase(dead_id);
                            close(p.fd);
                        }
                    }
                    else {
                        // Error
                        perror("[Agent] read error");
                    }
                }
            }
        }
    }

    std::map<std::string, ActorHandle>                             registry_;
    std::map<uint32_t, std::shared_ptr<std::promise<std::string>>> promises_;
    std::mutex                                                     mtx_;
    std::mutex                                                     spawn_mtx_;  // Serialize forks
    std::atomic<uint32_t>                                          seq_{0};
    std::atomic<bool>                                              running_{true};
    std::thread                                                    monitor_thread_;
};

}  // namespace spoke
