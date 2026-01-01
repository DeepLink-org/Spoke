#pragma once

#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "actor.h"
#include "types.h"

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

    pid_t spawnActor(const std::string& actor_type, const std::string& actor_id)
    {
        int p2c[2], c2p[2];
        if (pipe(p2c) != 0 || pipe(c2p) != 0)
            return -1;

        pid_t pid = fork();
        if (pid == 0) {  // Child
            close(p2c[1]);
            close(c2p[0]);
            auto actor = ActorFactory::instance().create(actor_type, actor_id, p2c[0], c2p[1]);
            if (actor)
                actor->run();
            else
                std::cerr << "[Child] Unknown type: " << actor_type << std::endl;
            exit(0);
        }
        // Parent
        close(p2c[0]);
        close(c2p[1]);
        std::lock_guard<std::mutex> lk(mtx_);
        registry_[actor_id] = {pid, p2c[1], c2p[0]};
        return pid;
    }

    template<typename T>
    pid_t spawnActor(const std::string& actor_id)
    {
        int p2c[2], c2p[2];
        if (pipe(p2c) != 0 || pipe(c2p) != 0)
            return -1;

        pid_t pid = fork();
        if (pid == 0) {  // Child
            close(p2c[1]);
            close(c2p[0]);
            // 直接构造，无需工厂
            T instance(actor_id, p2c[0], c2p[1]);
            instance.run();
            exit(0);
        }
        // Parent
        close(p2c[0]);
        close(c2p[1]);
        std::lock_guard<std::mutex> lk(mtx_);
        registry_[actor_id] = {pid, p2c[1], c2p[0]};
        return pid;
    }

    std::future<double> callRemote(const std::string& id, Action act, double val)
    {
        uint32_t s       = seq_++;
        auto     promise = std::promise<double>();
        auto     future  = promise.get_future();

        std::lock_guard<std::mutex> lk(mtx_);
        if (registry_.find(id) == registry_.end())
            return future;

        promises_[s] = std::move(promise);
        Message m    = {act, s, {.value_f64 = val}};
        write(registry_[id].tx_fd, &m, sizeof(Message));
        return future;
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
                    Response r;
                    if (read(p.fd, &r, sizeof(Response)) > 0) {
                        std::lock_guard<std::mutex> lk(mtx_);
                        if (promises_.count(r.seq_id)) {
                            promises_[r.seq_id].set_value(r.result);
                            promises_.erase(r.seq_id);
                        }
                    }
                }
            }
        }
    }

    std::map<std::string, ActorHandle>       registry_;
    std::map<uint32_t, std::promise<double>> promises_;
    std::mutex                               mtx_;
    std::atomic<uint32_t>                    seq_{0};
    std::atomic<bool>                        running_{true};
    std::thread                              monitor_thread_;
};

}  // namespace spoke
