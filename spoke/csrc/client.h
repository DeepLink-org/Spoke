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
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <unistd.h>

#include "dlslime/engine/rdma/rdma_endpoint.h"
#include "dlslime/engine/rdma/rdma_future.h"
#include "dlslime/json.hpp"

namespace spoke {

struct RdmaChannel {
    std::shared_ptr<dlslime::RDMAEndpoint> ep;
    char*                                  local_buf       = nullptr;
    size_t                                 local_buf_size  = 0;
    uintptr_t                              remote_addr     = 0;
    size_t                                 remote_buf_size = 0;
    std::thread                            receiver_thread;
    std::atomic<bool>                      running{true};

    ~RdmaChannel() {
        running = false;
        if (ep) ep->shutdown();
        if (receiver_thread.joinable()) {
            receiver_thread.detach(); // Safer in destructor
        }
        if (local_buf) delete[] local_buf;
    }
};

class Client {
public:
    Client(const std::string& ip, int port): target_ip_(ip), target_port_(port), use_hub_(false)
    {
        connectToDaemon(ip, port);
    }

    Client(const std::string& ip, int port, bool is_hub_mode):
        use_hub_(is_hub_mode)
    {
        if (is_hub_mode) {
            hub_ip_ = ip;
            hub_port_ = port;
            // In Hub mode, we don't connect to a daemon until spawnRemote or gangAllocate is called
        } else {
            target_ip_ = ip;
            target_port_ = port;
            connectToDaemon(ip, port);
        }
    }

    ~Client()
    {
        // Step 1: Collect tickets to release (minimize lock holding time)
        std::vector<std::string> tickets_to_release;
        {
            std::lock_guard<std::mutex> lk(ticket_mtx_);
            tickets_to_release.assign(allocated_tickets_.begin(), allocated_tickets_.end());
            allocated_tickets_.clear();
        }

        // Step 2: Release gang allocations BEFORE stopping threads (needs network)
        for (const auto& ticket : tickets_to_release) {
            std::cerr << "[Client] Auto-releasing unreleased ticket: " << ticket << std::endl;
            try {
                gangReleaseSync(ticket);
            }
            catch (const std::exception& e) {
                std::cerr << "[Client] Failed to auto-release ticket " << ticket << ": " << e.what() << std::endl;
            }
        }

        // Step 3: Signal threads to stop
        running_ = false;

        // Step 4: Shutdown RDMA channels
        {
            std::lock_guard<std::mutex> lk(rdma_mtx_);
            rdma_channels_.clear(); // Will trigger RdmaChannel destructors
        }

        // Step 5: Shutdown connections to unblock recv loops
        if (sock_ != -1) {
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
            sock_ = -1;
        }

        // Step 6: Wait for socket receiver thread (usually exits quickly after shutdown)
        try {
            if (receiver_thread_.joinable()) {
                if (receiver_thread_.get_id() == std::this_thread::get_id()) {
                    receiver_thread_.detach();
                }
                else {
                    receiver_thread_.join();
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[Client] Exception joining receiver_thread: " << e.what() << std::endl;
            if (receiver_thread_.joinable()) {
                receiver_thread_.detach();
            }
        }
        catch (...) {
            if (receiver_thread_.joinable()) {
                receiver_thread_.detach();
            }
        }
    }

    // [New] V2 Allocation API
    std::future<AllocateResp> gangAllocate(uint32_t num_nodes, uint32_t actors_per_node,
                                                const ResourceSpec& res_per_actor, bool strict_pack = true)
    {
        auto prom = std::make_shared<std::promise<AllocateResp>>();
        auto fut  = prom->get_future();

        if (!use_hub_) {
            try { throw std::runtime_error("gangAllocate requires Hub mode"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        // Connect to Hub (Temporary connection for this request)
        // Note: For production, maintain a persistent connection to Hub
        int hs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(hub_port_);
        inet_pton(AF_INET, hub_ip_.c_str(), &sa.sin_addr);
        if (connect(hs, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(hs);
            try { throw std::runtime_error("Failed to connect to Hub"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        AllocateReq req;
        req.num_nodes = num_nodes;
        req.actors_per_node = actors_per_node;
        req.res_per_actor = res_per_actor;
        req.strict_pack = strict_pack;

        NetMeta   meta{Action::kNetAllocate, seq_++, "", ""};
        NetHeader hdr{0x504F4B45, sizeof(NetMeta), sizeof(AllocateReq)};

        send(hs, &hdr, sizeof(hdr), 0);
        send(hs, &meta, sizeof(meta), 0);
        send(hs, &req, sizeof(req), 0);

        // Receive Response
        NetHeader rh;
        recv(hs, &rh, sizeof(rh), MSG_WAITALL);
        NetRespMeta rm;
        recv(hs, &rm, sizeof(rm), MSG_WAITALL);

        if (rm.status <= 0) {
            close(hs);
            try { throw std::runtime_error("Allocation rejected by Hub"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        // Parse Body
        // Body = AllocateResp + Slots...
        std::vector<char> body(rh.data_size);
        recv(hs, body.data(), rh.data_size, MSG_WAITALL);
        close(hs);

        if (body.size() < sizeof(AllocateResp)) {
             try { throw std::runtime_error("Invalid allocation response size"); }
             catch(...) { prom->set_exception(std::current_exception()); }
             return fut;
        }

        AllocateResp resp;
        memcpy(&resp, body.data(), sizeof(AllocateResp));

        // Track ticket for auto-release on destruction
        {
            std::lock_guard<std::mutex> lk(ticket_mtx_);
            allocated_tickets_.insert(std::string(resp.ticket_id));
        }

        // [Auto-Connect] Connect to the first node in the allocation
        // This is necessary because Client (currently) maintains a single connection for callRemote
        if (resp.num_members > 0) {
            size_t slots_offset = sizeof(AllocateResp);
            size_t expected_size = slots_offset + resp.num_members * sizeof(AllocatedSlot);

            if (body.size() >= expected_size) {
                const AllocatedSlot* slots = reinterpret_cast<const AllocatedSlot*>(body.data() + slots_offset);
                // Only connect if not already connected
                if (sock_ == -1) {
                    std::string node_ip = slots[0].node_ip;
                    int node_port = slots[0].node_port;
                    std::cout << "[Client] Auto-connecting to allocated node: " << node_ip << ":" << node_port << std::endl;
                    connectToDaemon(node_ip, node_port);
                }
            }
        }

        prom->set_value(resp);
        return fut;
    }

    // [New] V2 Launch API
    std::future<bool> launchActor(const std::string& ticket_id, uint32_t global_rank,
                                  const std::string& type, const std::string& id, const std::string& args_serialized)
    {
        auto prom = std::make_shared<std::promise<bool>>();
        auto fut  = prom->get_future();

        if (!use_hub_) {
            try { throw std::runtime_error("launchActor requires Hub mode"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        int hs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(hub_port_);
        inet_pton(AF_INET, hub_ip_.c_str(), &sa.sin_addr);
        if (connect(hs, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(hs);
            try { throw std::runtime_error("Failed to connect to Hub"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        LaunchReq req;
        memset(&req, 0, sizeof(req));
        strncpy(req.ticket_id, ticket_id.c_str(), 63);
        req.global_rank = global_rank;

        // Payload = LaunchReq + Args
        std::vector<char> payload(sizeof(LaunchReq) + args_serialized.size());
        memcpy(payload.data(), &req, sizeof(LaunchReq));
        if (!args_serialized.empty()) {
            memcpy(payload.data() + sizeof(LaunchReq), args_serialized.data(), args_serialized.size());
        }

        NetMeta   meta{Action::kNetLaunch, seq_++, "", ""};
        strncpy(meta.actor_id, id.c_str(), 31);
        strncpy(meta.actor_type, type.c_str(), 31);

        NetHeader hdr{0x504F4B45, sizeof(NetMeta), (uint32_t)payload.size()};

        send(hs, &hdr, sizeof(hdr), 0);
        send(hs, &meta, sizeof(meta), 0);
        send(hs, payload.data(), payload.size(), 0);

        NetHeader rh;
        recv(hs, &rh, sizeof(rh), MSG_WAITALL);
        NetRespMeta rm;
        recv(hs, &rm, sizeof(rm), MSG_WAITALL);

        close(hs);

        prom->set_value(rm.status > 0);
        return fut;
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

    // [New] Release allocated resources (Gang Release)
    std::future<bool> gangRelease(const std::string& ticket_id)
    {
        auto prom = std::make_shared<std::promise<bool>>();
        auto fut  = prom->get_future();

        if (!use_hub_) {
            try { throw std::runtime_error("gangRelease requires Hub mode"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        int hs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(hub_port_);
        inet_pton(AF_INET, hub_ip_.c_str(), &sa.sin_addr);
        if (connect(hs, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(hs);
            try { throw std::runtime_error("Failed to connect to Hub for release"); }
            catch(...) { prom->set_exception(std::current_exception()); }
            return fut;
        }

        ReleaseReq req;
        strncpy(req.ticket_id, ticket_id.c_str(), 63);

        NetMeta   meta{Action::kNetRelease, seq_++, "", ""};
        NetHeader hdr{0x504F4B45, sizeof(NetMeta), sizeof(ReleaseReq)};

        send(hs, &hdr, sizeof(hdr), 0);
        send(hs, &meta, sizeof(meta), 0);
        send(hs, &req, sizeof(req), 0);

        NetHeader rh;
        recv(hs, &rh, sizeof(rh), MSG_WAITALL);
        NetRespMeta rm;
        recv(hs, &rm, sizeof(rm), MSG_WAITALL);

        close(hs);

        // Remove ticket from tracking on successful release
        if (rm.status > 0) {
            std::lock_guard<std::mutex> lk(ticket_mtx_);
            allocated_tickets_.erase(ticket_id);
        }

        prom->set_value(rm.status > 0);
        return fut;
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
        {
            std::lock_guard<std::mutex> rdma_lk(rdma_mtx_);
            if (rdma_channels_.count(id)) {
                auto chan = rdma_channels_[id];
                size_t body_size  = PackedSize(req_data);
                size_t total_size = 12 + body_size;

                // Realloc Remote if needed
                if (total_size > chan->remote_buf_size) {
                    size_t new_size = total_size * 3 / 2;

                    // Note: Need to release rdma_lk during recursive callRemote to avoid deadlock
                    rdma_mtx_.unlock();
                    auto realloc_fut =
                        callRemote<std::string, std::string>(id, Action::kRdmaRealloc, std::to_string(new_size));
                    std::string realloc_resp = realloc_fut.get();
                    rdma_mtx_.lock();

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

                    // Update Channel State
                    chan->remote_addr     = new_addr;
                    chan->remote_buf_size = reported_size;
                    chan->ep->registerOrAccessRemoteMemoryRegion(new_addr, json["mr_info"][std::to_string(new_addr)]);

                    std::cout << "[Client] Remote RDMA Buffer for " << id << " Expanded to " << (chan->remote_buf_size / 1024 / 1024) << "MB"
                              << std::endl;
                }

                // Realloc Local if needed
                if (total_size > chan->local_buf_size) {
                    if (chan->local_buf) delete[] chan->local_buf;
                    chan->local_buf_size  = total_size * 3 / 2;
                    chan->local_buf = new char[chan->local_buf_size];
                    uintptr_t addr  = (uintptr_t)chan->local_buf;
                    chan->ep->registerOrAccessMemoryRegion(addr, addr, 0, chan->local_buf_size);
                }

                // Proceed with Write (Direct Serialize)
                uint32_t* hdr_ptr = (uint32_t*)chan->local_buf;
                hdr_ptr[0]        = static_cast<uint32_t>(act);
                hdr_ptr[1]        = sid;
                hdr_ptr[2]        = (uint32_t)body_size;

                // DIRECT SERIALIZATION into pinned buffer!
                PackTo(req_data, chan->local_buf + 12);

                std::vector<dlslime::assign_tuple_t> chunks;
                chunks.emplace_back((uintptr_t)chan->local_buf, (uintptr_t)chan->remote_addr, 0, 0, total_size);

                auto write_fut = chan->ep->writeWithImm(chunks, sid, nullptr);
                write_fut->wait();

                return fut;
            }
        }

        // Slow Path (Socket)
        std::string body = Pack(req_data);
        sendRequest(meta, body);
        return fut;
    }

    // Initialize RDMA channel with a specific actor
    bool initRDMA(const std::string& actor_id)
    {
        {
            std::lock_guard<std::mutex> lk(rdma_mtx_);
            if (rdma_channels_.count(actor_id))
                return true;
        }

        try {
            auto chan = std::make_shared<RdmaChannel>();

            // 1. Create Local EP (Default Constructor)
            chan->ep = std::make_shared<dlslime::RDMAEndpoint>();

            // 2. Alloc and Register Local Buffer (1MB)
            chan->local_buf_size  = 1 * 1024 * 1024;
            chan->local_buf       = new char[chan->local_buf_size];
            uintptr_t local_addr  = (uintptr_t)chan->local_buf;
            chan->ep->registerOrAccessMemoryRegion(local_addr, local_addr, 0, chan->local_buf_size);

            // 3. Get Local Info
            auto local_info            = chan->ep->endpointInfo();
            local_info["buffer_addr"]  = local_addr;
            local_info["buffer_size"]  = chan->local_buf_size;
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
            chan->ep->connect(remote_json);

            // 7. Store Remote Buffer Info
            chan->remote_addr     = remote_json["buffer_addr"].get<uintptr_t>();
            chan->remote_buf_size = remote_json["buffer_size"].get<size_t>();

            chan->ep->registerOrAccessRemoteMemoryRegion(chan->remote_addr,
                                                         remote_json["mr_info"][std::to_string(chan->remote_addr)]);

            // Start RDMA Receiver Thread for this channel
            chan->receiver_thread = std::thread(&Client::rdmaRecvLoop, this, chan);

            {
                std::lock_guard<std::mutex> lk(rdma_mtx_);
                rdma_channels_[actor_id] = chan;
            }

            std::cout << "[Client] RDMA Channel Established for " << actor_id
                      << "! Buffer: " << (chan->local_buf_size / 1024) << "KB" << std::endl;

            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "[Client] InitRDMA Exception for " << actor_id << ": " << e.what() << std::endl;
            return false;
        }
    }

    std::mutex                                          rdma_mtx_;
    std::map<std::string, std::shared_ptr<RdmaChannel>> rdma_channels_;

private:
    // Synchronous version of gangRelease for use in destructor
    // Note: Does NOT modify allocated_tickets_ (caller handles it)
    void gangReleaseSync(const std::string& ticket_id)
    {
        if (!use_hub_) {
            return;
        }

        int hs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(hub_port_);
        inet_pton(AF_INET, hub_ip_.c_str(), &sa.sin_addr);
        if (connect(hs, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(hs);
            throw std::runtime_error("Failed to connect to Hub for release");
        }

        ReleaseReq req;
        strncpy(req.ticket_id, ticket_id.c_str(), 63);

        NetMeta   meta{Action::kNetRelease, seq_++, "", ""};
        NetHeader hdr{0x504F4B45, sizeof(NetMeta), sizeof(ReleaseReq)};

        send(hs, &hdr, sizeof(hdr), 0);
        send(hs, &meta, sizeof(meta), 0);
        send(hs, &req, sizeof(req), 0);

        NetHeader rh;
        recv(hs, &rh, sizeof(rh), MSG_WAITALL);
        NetRespMeta rm;
        recv(hs, &rm, sizeof(rm), MSG_WAITALL);

        close(hs);

        if (rm.status <= 0) {
            throw std::runtime_error("Release request rejected by Hub");
        }
    }

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

    void rdmaRecvLoop(std::shared_ptr<RdmaChannel> chan)
    {
        while (running_ && chan->running) {
            try {
                auto fut = chan->ep->immRecv();
                fut->wait();
                if (!running_ || !chan->running)
                    break;

                // Response is written to local_buf by Actor
                uint32_t* ptr        = (uint32_t*)chan->local_buf;
                uint32_t  action_val = ptr[0];  // Action
                uint32_t  seq_id     = ptr[1];  // Seq ID
                uint32_t  data_size  = ptr[2];  // Body Size

                // Zero Copy Body (offset 12)
                std::string body(chan->local_buf + 12, data_size);

                std::lock_guard<std::mutex> lk(map_mtx_);
                if (response_handlers_.count(seq_id)) {
                    response_handlers_[seq_id](body);
                    response_handlers_.erase(seq_id);
                }
            }
            catch (...) {
                if (!running_ || !chan->running)
                    break;
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

    // Track allocated tickets for auto-release on destruction
    std::set<std::string> allocated_tickets_;
    std::mutex            ticket_mtx_;

};  // class Client

}  // namespace spoke
