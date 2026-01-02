#pragma once

#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "spoke/csrc/types.h"

namespace spoke {

struct NodeInfo {
    std::string ip;
    int         port;
    int         load;  // 简单的负载计数 (跑了多少个 Actor)
};

class Hub {
public:
    void registerNode(const std::string& ip, int port)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // 简单去重
        for (auto& node : nodes_) {
            if (node.ip == ip && node.port == port)
                return;
        }
        nodes_.push_back({ip, port, 0});
        std::cout << "[Hub] Registered Node: " << ip << ":" << port << std::endl;
    }

    // 简单的调度策略：Round Robin 或 Least Connections
    bool scheduleNode(NodeInfo* out_node)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (nodes_.empty())
            return false;

        // 找负载最小的
        auto it = std::min_element(
            nodes_.begin(), nodes_.end(), [](const NodeInfo& a, const NodeInfo& b) { return a.load < b.load; });

        if (it != nodes_.end()) {
            *out_node = *it;
            it->load++;  // 假定调用者会去这个节点 Spawn
            return true;
        }
        return false;
    }

private:
    std::vector<NodeInfo> nodes_;
    std::mutex            mtx_;
};

}  // namespace spoke
