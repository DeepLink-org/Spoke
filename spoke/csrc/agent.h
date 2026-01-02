#pragma once
#include "actor.h"
#include "serializer.h"
#include "types.h"
#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/poll.h>
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
        int p2c[2], c2p[2];
        if (pipe(p2c) != 0 || pipe(c2p) != 0)
            return -1;
        pid_t pid = fork();
        if (pid == 0) {
            close(p2c[1]);
            close(c2p[0]);
            auto actor = ActorFactory::instance().create(type, id, p2c[0], c2p[1]);
            if (actor)
                actor->run();
            else
                std::cerr << "[Agent] Unknown type: " << type << std::endl;
            exit(0);
        }
        close(p2c[0]);
        close(c2p[1]);
        std::lock_guard<std::mutex> lk(mtx_);
        registry_[id] = {pid, p2c[1], c2p[0]};
        return pid;
    }

    // ==========================================
    // [v1 修复] 模板模式启动 (供 smoke_v1 本地测试使用)
    // ==========================================
    template<typename T>
    pid_t spawnActor(const std::string& id)
    {
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
        write(fd, &ph, sizeof(ph));
        if (!data.empty())
            write(fd, data.data(), data.size());

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
            if (poll(pfds.data(), pfds.size(), 50) <= 0)
                continue;

            for (auto& p : pfds) {
                if (p.revents & POLLIN) {
                    PipeHeader ph;
                    if (read(p.fd, &ph, sizeof(ph)) > 0) {
                        std::string body;
                        body.resize(ph.data_size);
                        if (ph.data_size > 0) {
                            size_t tot = 0;
                            while (tot < ph.data_size) {
                                int r = read(p.fd, body.data() + tot, ph.data_size - tot);
                                if (r <= 0)
                                    break;
                                tot += r;
                            }
                        }
                        std::lock_guard<std::mutex> lk(mtx_);
                        if (promises_.count(ph.seq_id)) {
                            promises_[ph.seq_id]->set_value(body);
                            promises_.erase(ph.seq_id);
                        }
                    }
                }
            }
        }
    }

    std::map<std::string, ActorHandle>                             registry_;
    std::map<uint32_t, std::shared_ptr<std::promise<std::string>>> promises_;
    std::mutex                                                     mtx_;
    std::atomic<uint32_t>                                          seq_{0};
    std::atomic<bool>                                              running_{true};
    std::thread                                                    monitor_thread_;
};

}  // namespace spoke
