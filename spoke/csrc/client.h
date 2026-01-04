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

#include <unistd.h>

#include "dlslime/engine/rdma/rdma_endpoint.h"
#include "dlslime/engine/rdma/rdma_future.h"
#include "dlslime/json.hpp"

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
        std::cerr << "[Client] Destructor start. Thread ID: " << std::this_thread::get_id() << std::endl;
        running_ = false;
        if (sock_ != -1) {
            std::cerr << "[Client] Closing socket" << std::endl;
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
        }
        if (rdma_ep_) {
            std::cerr << "[Client] Shutting down RDMA" << std::endl;
            rdma_ready_ = false;  // Signal recv loop to exit
            rdma_ep_->shutdown();
        }

        try {
            if (receiver_thread_.joinable()) {
                std::cerr << "[Client] Joining receiver thread (ID: " << receiver_thread_.get_id() << ")" << std::endl;
                if (receiver_thread_.get_id() == std::this_thread::get_id()) {
                    std::cerr << "[Client] ERROR: Joining SELF (Receiver Thread)!" << std::endl;
                    receiver_thread_.detach();
                }
                else {
                    receiver_thread_.join();
                    std::cerr << "[Client] Joined receiver thread" << std::endl;
                }
            }
            if (rdma_receiver_thread_.joinable()) {
                std::cerr << "[Client] Joining RDMA thread (ID: " << rdma_receiver_thread_.get_id() << ")" << std::endl;
                if (rdma_receiver_thread_.get_id() == std::this_thread::get_id()) {
                    std::cerr << "[Client] ERROR: Joining SELF (RDMA Receiver Thread)!" << std::endl;
                    rdma_receiver_thread_.detach();
                }
                else {
                    rdma_receiver_thread_.join();
                    std::cerr << "[Client] Joined RDMA thread" << std::endl;
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[Client] Exception during join: " << e.what() << std::endl;
        }
        std::cerr << "[Client] Destructor done" << std::endl;
    }

    void spawnRemote(const std::string& type, const std::string& id)
    {
        if (use_hub_) {
            std::string node_addr;
            // Retry loop for finding a node (Agent startup might be slower)
            for (int i = 0; i < 10; ++i) {
                node_addr = queryHubForNode();
                if (!node_addr.empty())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            if (node_addr.empty()) {
                std::cerr << "[Client] Hub returned no nodes within timeout!" << std::endl;
                return;
            }
            char ip_buf[64];
            int  port = 0;
            sscanf(node_addr.c_str(), "%[^:]:%d", ip_buf, &port);
            connectToDaemon(ip_buf, port);
        }
        if (sock_ == -1)
            return;

        auto     prom = std::make_shared<std::promise<bool>>();
        auto     fut  = prom->get_future();
        uint32_t sid  = seq_++;
        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            response_handlers_[sid] = [prom](const std::string&) { prom->set_value(true); };
        }

        NetMeta meta{Action::kNetSpawn, sid, "", ""};
        strncpy(meta.actor_id, id.c_str(), 31);
        strncpy(meta.actor_type, type.c_str(), 31);
        sendRequest(meta, "");

        // Wait for Ack
        fut.wait();
    }

    // [New] Stop remote actor (Fire-and-Forget)
    void stopRemote(const std::string& id)
    {
        if (sock_ == -1)
            return;
        uint32_t sid = seq_++;
        NetMeta  meta{Action::kStop, sid, "", ""};
        strncpy(meta.actor_id, id.c_str(), 31);
        sendRequest(meta, "");
    }

    // [关键更新] 泛型调用，使用 Pack/Unpack
    template<typename ReqT, typename RespT>
    std::future<RespT> callRemote(const std::string& id, Action act, const ReqT& req_data)
    {
        auto prom = std::make_shared<std::promise<RespT>>();
        auto fut  = prom->get_future();
        if (sock_ == -1) {
            try {
                throw std::runtime_error("[Client] Not connected");
            }
            catch (...) {
                prom->set_exception(std::current_exception());
            }
            return fut;
        }

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

        // **Fast Path: RDMA Write with Imm**
        if (rdma_ready_) {
            size_t body_size  = PackedSize(req_data);
            size_t total_size = 12 + body_size;

            // Realloc Remote if needed
            if (total_size > remote_buf_size_) {
                size_t new_size = total_size * 3 / 2;

                auto realloc_fut =
                    callRemote<std::string, std::string>(id, Action::kRdmaRealloc, std::to_string(new_size));
                std::string realloc_resp = realloc_fut.get();

                if (realloc_resp.empty()) {
                    std::cerr << "[Client] RDMA Realloc failed." << std::endl;
                    // Fallback to slow path
                    std::string body = Pack(req_data);
                    sendRequest(meta, body);
                    return fut;
                }

                auto      json          = dlslime::json::parse(realloc_resp);
                uintptr_t new_addr      = json["buffer_addr"].get<uintptr_t>();
                size_t    reported_size = json["buffer_size"].get<size_t>();

                // Update Client State
                remote_addr_     = new_addr;
                remote_buf_size_ = reported_size;
                rdma_ep_->registerOrAccessRemoteMemoryRegion(new_addr, json["mr_info"][std::to_string(new_addr)]);

                std::cout << "[Client] Remote RDMA Buffer Expanded to " << (remote_buf_size_ / 1024 / 1024) << "MB"
                          << std::endl;
            }

            // Realloc Local if needed
            if (total_size > rdma_buf_size_) {
                delete[] local_rdma_buf_;
                rdma_buf_size_  = total_size * 3 / 2;
                local_rdma_buf_ = new char[rdma_buf_size_];
                uintptr_t addr  = (uintptr_t)local_rdma_buf_;
                rdma_ep_->registerOrAccessMemoryRegion(addr, addr, 0, rdma_buf_size_);
            }

            // Proceed with Write (Direct Serialize)
            uint32_t* hdr_ptr = (uint32_t*)local_rdma_buf_;
            hdr_ptr[0]        = static_cast<uint32_t>(act);
            hdr_ptr[1]        = sid;
            hdr_ptr[2]        = (uint32_t)body_size;

            // DIRECT SERIALIZATION into pinned buffer!
            PackTo(req_data, local_rdma_buf_ + 12);

            std::vector<dlslime::assign_tuple_t> chunks;
            chunks.emplace_back((uintptr_t)local_rdma_buf_, (uintptr_t)remote_addr_, 0, 0, total_size);

            auto write_fut = rdma_ep_->writeWithImm(chunks, sid, nullptr);
            write_fut->wait();

            return fut;
        }

        // Slow Path (Socket)
        std::string body = Pack(req_data);
        sendRequest(meta, body);
        return fut;
    }

    // Initialize RDMA channel with a specific actor
    bool initRDMA(const std::string& actor_id)
    {
        if (rdma_ready_)
            return true;

        try {
            // 1. Create Local EP (Default Constructor)
            rdma_ep_ = std::make_shared<dlslime::RDMAEndpoint>();

            // 2. Alloc and Register Local Buffer (4MB)
            rdma_buf_size_       = 1 * 1024 * 1024;  // Test small buf
            local_rdma_buf_      = new char[rdma_buf_size_];
            uintptr_t local_addr = (uintptr_t)local_rdma_buf_;
            rdma_ep_->registerOrAccessMemoryRegion(local_addr, local_addr, 0, rdma_buf_size_);

            // 3. Get Local Info
            auto local_info            = rdma_ep_->endpointInfo();
            local_info["buffer_addr"]  = local_addr;
            local_info["buffer_size"]  = rdma_buf_size_;
            std::string local_info_str = local_info.dump();

            // 4. Handshake
            auto        fut = callRemote<std::string, std::string>(actor_id, Action::kInitRDMA, local_info_str);
            std::string remote_info_str = fut.get();

            if (remote_info_str.empty()) {
                return false;
            }

            // 5. Parse Remote Info
            auto remote_json = dlslime::json::parse(remote_info_str);

            // 6. Connect
            rdma_ep_->connect(remote_json);

            // 7. Store Remote Buffer Info
            remote_addr_     = remote_json["buffer_addr"].get<uintptr_t>();
            remote_buf_size_ = remote_json["buffer_size"].get<size_t>();

            rdma_ep_->registerOrAccessRemoteMemoryRegion(remote_addr_,
                                                         remote_json["mr_info"][std::to_string(remote_addr_)]);

            rdma_ready_ = true;
            std::cout << "[Client] RDMA Channel Established! Buffer: " << (rdma_buf_size_ / 1024 / 1024) << "MB"
                      << std::endl;

            // Start RDMA Receiver Thread
            rdma_receiver_thread_ = std::thread(&Client::rdmaRecvLoop, this);

            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "[Client] InitRDMA Exception: " << e.what() << std::endl;
            return false;
        }
    }

    std::shared_ptr<dlslime::RDMAEndpoint> rdma_ep_;
    char*                                  local_rdma_buf_  = nullptr;
    size_t                                 rdma_buf_size_   = 0;
    uintptr_t                              remote_addr_     = 0;
    size_t                                 remote_buf_size_ = 0;
    bool                                   rdma_ready_      = false;

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

            {
                std::lock_guard<std::mutex> lk(map_mtx_);
                if (response_handlers_.count(meta.seq_id)) {
                    // std::cout << "[Client] Handling response for Seq: " << meta.seq_id << std::endl;
                    response_handlers_[meta.seq_id](body);
                    response_handlers_.erase(meta.seq_id);
                }
                else {
                    std::cerr << "[Client] Warning: No handler for Seq: " << meta.seq_id << std::endl;
                }
            }
        }
    }

    void rdmaRecvLoop()
    {
        while (running_ && rdma_ready_) {
            try {
                auto fut = rdma_ep_->immRecv();
                fut->wait();
                if (!running_)
                    break;

                // Response is written to local_rdma_buf_ by Actor
                uint32_t* ptr        = (uint32_t*)local_rdma_buf_;
                uint32_t  action_val = ptr[0];  // Action
                uint32_t  seq_id     = ptr[1];  // Seq ID
                uint32_t  data_size  = ptr[2];  // Body Size

                // Zero Copy Body (offset 12)
                std::string body(local_rdma_buf_ + 12, data_size);

                std::lock_guard<std::mutex> lk(map_mtx_);
                if (response_handlers_.count(seq_id)) {
                    response_handlers_[seq_id](body);
                    response_handlers_.erase(seq_id);
                }
            }
            catch (...) {
                if (!running_)
                    break;
            }
        }
    }

    int                                                         sock_ = -1;
    std::atomic<uint32_t>                                       seq_{1};
    std::atomic<bool>                                           running_{false};
    std::mutex                                                  sock_mtx_, map_mtx_;
    std::thread                                                 receiver_thread_;
    std::thread                                                 rdma_receiver_thread_;
    bool                                                        use_hub_;
    std::string                                                 hub_ip_;
    int                                                         hub_port_;
    std::string                                                 target_ip_;
    int                                                         target_port_;
    std::map<uint32_t, std::function<void(const std::string&)>> response_handlers_;

};  // class Client

}  // namespace spoke
