#pragma once
#include "serializer.h"
#include "types.h"
#include <arpa/inet.h>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace spoke {

class Client {
public:
    Client(const std::string& ip, int port): target_ip_(ip), target_port_(port), use_hub_(false)
    {
        connectToDaemon(ip, port);
    }

    Client(const std::string& hub_ip, int hub_port, bool is_hub_mode):
        hub_ip_(hub_ip), hub_port_(hub_port), use_hub_(is_hub_mode)
    {
    }

    ~Client()
    {
        running_ = false;
        if (sock_ != -1) {
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
        }
        if (receiver_thread_.joinable())
            receiver_thread_.join();
    }

    void spawnRemote(const std::string& type, const std::string& id)
    {
        if (use_hub_) {
            std::string node_addr = queryHubForNode();
            if (node_addr.empty()) {
                std::cerr << "[Client] Hub returned no nodes!" << std::endl;
                return;
            }
            char ip_buf[64];
            int  port = 0;
            sscanf(node_addr.c_str(), "%[^:]:%d", ip_buf, &port);
            connectToDaemon(ip_buf, port);
        }
        if (sock_ == -1)
            return;

        NetMeta meta{Action::kNetSpawn, 0, "", ""};
        strncpy(meta.actor_id, id.c_str(), 31);
        strncpy(meta.actor_type, type.c_str(), 31);
        sendRequest(meta, "");
    }

    // [关键更新] 泛型调用，使用 Pack/Unpack
    template<typename ReqT, typename RespT>
    std::future<RespT> callRemote(const std::string& id, Action act, const ReqT& req_data)
    {
        auto prom = std::make_shared<std::promise<RespT>>();
        auto fut  = prom->get_future();
        if (sock_ == -1)
            return fut;

        uint32_t sid = seq_++;
        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            response_handlers_[sid] = [prom](const std::string& raw) {
                // 回调时自动反序列化
                prom->set_value(Unpack<RespT>(raw));
            };
        }

        NetMeta meta{act, sid, "", ""};
        strncpy(meta.actor_id, id.c_str(), 31);

        // 发送时自动序列化
        std::string body = Pack(req_data);
        sendRequest(meta, body);
        return fut;
    }

private:
    void connectToDaemon(const std::string& ip, int port)
    {
        if (sock_ != -1) {
            running_ = false;
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
            if (receiver_thread_.joinable())
                receiver_thread_.join();
        }
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(port);
        inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
        if (connect(sock_, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            std::cerr << "[Client] Connect failed " << ip << ":" << port << std::endl;
            sock_ = -1;
            return;
        }
        running_         = true;
        receiver_thread_ = std::thread(&Client::receiveLoop, this);
    }

    std::string queryHubForNode()
    {
        int                hs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(hub_port_);
        inet_pton(AF_INET, hub_ip_.c_str(), &sa.sin_addr);
        if (connect(hs, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(hs);
            return "";
        }

        NetMeta   meta{Action::kHubFindNode, 999, "", ""};
        NetHeader hdr{0x504F4B45, sizeof(NetMeta), 0};
        send(hs, &hdr, sizeof(hdr), 0);
        send(hs, &meta, sizeof(meta), 0);

        NetHeader rh;
        recv(hs, &rh, sizeof(rh), MSG_WAITALL);
        NetRespMeta rm;
        recv(hs, &rm, sizeof(rm), MSG_WAITALL);
        std::string body;
        body.resize(rh.data_size);
        if (rh.data_size > 0)
            recv(hs, body.data(), rh.data_size, MSG_WAITALL);
        close(hs);
        return body;
    }

    void sendRequest(const NetMeta& meta, const std::string& body)
    {
        NetHeader                   hdr{0x504F4B45, sizeof(NetMeta), (uint32_t)body.size()};
        std::lock_guard<std::mutex> lk(sock_mtx_);
        send(sock_, &hdr, sizeof(hdr), 0);
        send(sock_, &meta, sizeof(meta), 0);
        if (!body.empty())
            send(sock_, body.data(), body.size(), 0);
    }

    void receiveLoop()
    {
        while (running_) {
            NetHeader hdr;
            if (recv(sock_, &hdr, sizeof(hdr), MSG_WAITALL) <= 0)
                break;
            NetRespMeta meta;
            recv(sock_, &meta, sizeof(meta), MSG_WAITALL);
            std::string body;
            body.resize(hdr.data_size);
            if (hdr.data_size > 0)
                recv(sock_, body.data(), hdr.data_size, MSG_WAITALL);

            std::lock_guard<std::mutex> lk(map_mtx_);
            if (response_handlers_.count(meta.seq_id)) {
                response_handlers_[meta.seq_id](body);
                response_handlers_.erase(meta.seq_id);
            }
        }
    }

    int                                                         sock_ = -1;
    std::atomic<uint32_t>                                       seq_{1};
    std::atomic<bool>                                           running_{false};
    std::mutex                                                  sock_mtx_, map_mtx_;
    std::thread                                                 receiver_thread_;
    bool                                                        use_hub_;
    std::string                                                 hub_ip_;
    int                                                         hub_port_;
    std::string                                                 target_ip_;
    int                                                         target_port_;
    std::map<uint32_t, std::function<void(const std::string&)>> response_handlers_;
};

}  // namespace spoke
