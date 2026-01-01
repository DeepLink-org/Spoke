#pragma once

#include "types.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace spoke {

class Client {
public:
    // ==========================================
    // 构造函数 1: 直连模式 (Legacy / Direct)
    // 直接连接到一个已知的 Daemon IP:Port
    // ==========================================
    Client(const std::string& ip, int port): target_ip_(ip), target_port_(port), use_hub_(false)
    {
        connectToDaemon(ip, port);
    }

    // ==========================================
    // 构造函数 2: Hub 模式 (Distributed)
    // 连接 Hub，Spawn 时请求资源，再自动连接到分配的 Daemon
    // ==========================================
    Client(const std::string& hub_ip, int hub_port, bool is_hub_mode):
        hub_ip_(hub_ip), hub_port_(hub_port), use_hub_(is_hub_mode)
    {
        // Hub 模式下，初始化时不建立长连接
        // 等到 spawnRemote 时再根据 Hub 返回的地址建立连接
    }

    ~Client()
    {
        running_ = false;

        // [关键修复]
        // 显式 shutdown socket 通道，强制打断 receiveLoop 中的阻塞 recv
        if (sock_ != -1) {
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
        }

        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    // ==========================================
    // 核心功能：在集群中拉起 Actor
    // ==========================================
    void spawnRemote(const std::string& type, const std::string& id)
    {
        // 如果开启了 Hub 模式，先去问 Hub 要节点
        if (use_hub_) {
            std::string node_addr = queryHubForNode();
            if (node_addr.empty()) {
                std::cerr << "[Client] Error: Cluster full or Hub unreachable!" << std::endl;
                return;
            }

            // 解析 Hub 返回的 "IP:Port"
            char ip_buf[64];
            int  port = 0;
            // 简单解析
            if (sscanf(node_addr.c_str(), "%[^:]:%d", ip_buf, &port) == 2) {
                std::cout << "[Client] Hub assigned node: " << ip_buf << ":" << port << std::endl;
                // 连接到目标 Daemon (如果之前连着别的，这里会切换连接)
                connectToDaemon(ip_buf, port);
            }
            else {
                std::cerr << "[Client] Error: Invalid address format from Hub: " << node_addr << std::endl;
                return;
            }
        }

        // 此时 sock_ 应该已经连接到了正确的 Daemon
        if (sock_ == -1) {
            std::cerr << "[Client] Error: Not connected to any daemon." << std::endl;
            return;
        }

        // 发送 Spawn 请求
        NetRequest req;
        req.action = Action::kNetSpawn;
        req.seq_id = 0;
        strncpy(req.actor_id, id.c_str(), 31);
        strncpy(req.actor_type, type.c_str(), 31);

        sendRequest(req);
    }

    // 兼容旧接口命名
    void spawnInCluster(const std::string& type, const std::string& id)
    {
        spawnRemote(type, id);
    }

    // ==========================================
    // 核心功能：调用远程 Actor 方法
    // ==========================================
    std::future<double> callRemote(const std::string& id, Action act, double val)
    {
        uint32_t sid  = seq_++;
        auto     prom = std::make_shared<std::promise<double>>();
        auto     fut  = prom->get_future();

        if (sock_ == -1) {
            // 如果未连接，设置异常或默认值 (这里简单处理)
            std::cerr << "[Client] Call failed: No connection." << std::endl;
            return fut;
        }

        {
            std::lock_guard<std::mutex> lock(map_mtx_);
            promises_[sid] = prom;
        }

        NetRequest req;
        req.action = act;
        req.seq_id = sid;
        strncpy(req.actor_id, id.c_str(), 31);
        req.payload.value_f64 = val;

        sendRequest(req);
        return fut;
    }

private:
    // 内部 helper: 连接到具体的 Daemon 计算节点
    void connectToDaemon(const std::string& ip, int port)
    {
        // 如果之前有连接，先清理
        if (sock_ != -1) {
            running_ = false;
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
            if (receiver_thread_.joinable())
                receiver_thread_.join();
        }

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port   = htons(port);
        inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr);

        if (connect(sock_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "[Client] Connection to Daemon " << ip << ":" << port << " failed" << std::endl;
            sock_ = -1;
            return;
        }

        // 启动接收线程
        running_         = true;
        receiver_thread_ = std::thread(&Client::receiveLoop, this);
    }

    // 内部 helper: 短连接询问 Hub
    std::string queryHubForNode()
    {
        int                hub_sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port   = htons(hub_port_);
        inet_pton(AF_INET, hub_ip_.c_str(), &serv_addr.sin_addr);

        if (connect(hub_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "[Client] Failed to connect to Hub." << std::endl;
            close(hub_sock);
            return "";
        }

        NetRequest req;
        req.action = Action::kHubFindNode;
        req.seq_id = 999;  // 临时 ID

        NetHeader hdr;
        hdr.payload_size = sizeof(NetRequest);

        // 发送查询
        send(hub_sock, &hdr, sizeof(hdr), 0);
        send(hub_sock, &req, sizeof(req), 0);

        // 接收结果
        NetHeader resp_hdr;
        if (recv(hub_sock, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL) <= 0) {
            close(hub_sock);
            return "";
        }

        NetResponse resp;
        if (recv(hub_sock, &resp, sizeof(resp), MSG_WAITALL) <= 0) {
            close(hub_sock);
            return "";
        }

        close(hub_sock);

        if (resp.result > 0) {
            // result > 0 表示找到了节点，extra_info 包含 "IP:Port"
            return std::string(resp.extra_info);
        }
        return "";
    }

    void sendRequest(const NetRequest& req)
    {
        NetHeader hdr;
        hdr.payload_size = sizeof(NetRequest);

        std::lock_guard<std::mutex> lock(sock_mtx_);
        if (sock_ != -1) {
            send(sock_, &hdr, sizeof(hdr), 0);
            send(sock_, &req, sizeof(req), 0);
        }
    }

    void receiveLoop()
    {
        while (running_) {
            NetHeader hdr;
            // 阻塞读 Header
            int n = recv(sock_, &hdr, sizeof(hdr), MSG_WAITALL);
            if (n <= 0)
                break;  // 连接断开或被 shutdown

            NetResponse resp;
            // 阻塞读 Body
            n = recv(sock_, &resp, sizeof(resp), MSG_WAITALL);
            if (n <= 0)
                break;

            {
                std::lock_guard<std::mutex> lock(map_mtx_);
                if (promises_.count(resp.seq_id)) {
                    promises_[resp.seq_id]->set_value(resp.result);
                    promises_.erase(resp.seq_id);
                }
            }
        }
    }

    // 成员变量
    int                   sock_ = -1;
    std::atomic<uint32_t> seq_{1};
    std::atomic<bool>     running_{false};

    std::mutex sock_mtx_;
    std::mutex map_mtx_;

    std::map<uint32_t, std::shared_ptr<std::promise<double>>> promises_;
    std::thread                                               receiver_thread_;

    // 配置信息
    bool        use_hub_ = false;
    std::string hub_ip_;
    int         hub_port_ = 0;
    std::string target_ip_;
    int         target_port_ = 0;
};

}  // namespace spoke
