#pragma once
#include <cstdint>
#include <cstring>

namespace spoke {

enum class Action : uint32_t {
    kInit = 0x01,
    kRun  = 0x02,
    kStop = 0xFF,

    // Daemon <-> Client
    kNetSpawn = 0x04,

    kHubRegisterNode = 0x30,
    kHubFindNode     = 0x31,

    kUserMethodStart = 0x10
};

struct alignas(8) Payload {
    union {
        double   value_f64;
        uint64_t u64;
        char     raw[32];
    };
};

struct Message {
    Action   action;
    uint32_t seq_id;
    Payload  payload;
};

struct Response {
    uint32_t seq_id;
    double   result;
};

struct NetHeader {
    uint32_t magic = 0x504F4B45;
    uint32_t payload_size;
};

struct NetRequest {
    Action   action;
    uint32_t seq_id;
    char     actor_id[32];
    char     actor_type[32];
    Payload  payload;
};

struct NetResponse {
    uint32_t seq_id;
    double   result;
    char     extra_info[64];
};

}  // namespace spoke
