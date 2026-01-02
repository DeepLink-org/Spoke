#include <iostream>

#include "spoke/csrc/actor.h"
#include "spoke/csrc/serializer.h"

// 自定义数据结构
struct Vec3 {
    double x, y, z;
};

struct PhysicsReq {
    int  id;
    Vec3 pos;
    Vec3 velocity;
};

struct PhysicsResp {
    int    id;
    Vec3   new_pos;
    double energy;
};

// Action Definitions
const spoke::Action kUpdatePhysics = (spoke::Action)0x50;

class PhysicsActor: public spoke::Actor {
public:
    using spoke::Actor::Actor;

    // 使用新宏：自动处理 PhysicsReq -> PhysicsResp 的转换
    SPOKE_METHOD(PhysicsActor, update, kUpdatePhysics, PhysicsReq, PhysicsResp)
    {
        // 业务逻辑：pos = pos + velocity * dt (假设 dt=1.0)
        PhysicsResp resp;
        resp.id        = val.id;
        resp.new_pos.x = val.pos.x + val.velocity.x;
        resp.new_pos.y = val.pos.y + val.velocity.y;
        resp.new_pos.z = val.pos.z + val.velocity.z;

        // 计算动能 E = 0.5 * v^2
        double v2 = val.velocity.x * val.velocity.x + val.velocity.y * val.velocity.y + val.velocity.z * val.velocity.z;
        resp.energy = 0.5 * v2;

        return resp;
    }
};

SPOKE_REGISTER_ACTOR("PhysicsActor", PhysicsActor);
