#pragma once
#include <cstdint>
#include <vector>

namespace spoke {

enum class Action : uint32_t {
    kInit            = 0x01,
    kStop            = 0xFF,
    kNetSpawn        = 0x04,
    kHubRegisterNode = 0x30,
    kHubFindNode     = 0x31,
    kUserActionStart = 0x10
};

// 网络包头 (Header)
struct NetHeader {
    uint32_t magic = 0x504F4B45;
    uint32_t meta_size;
    uint32_t data_size;
};

// 请求元数据 (Meta)
struct NetMeta {
    Action   action;
    uint32_t seq_id;
    char     actor_id[32];
    char     actor_type[32];
};

// 响应元数据 (RespMeta)
struct NetRespMeta {
    uint32_t seq_id;
    int      status;
};

// IPC 管道消息头
struct PipeHeader {
    Action   action;
    uint32_t seq_id;
    uint32_t data_size;
};

}  // namespace spoke
