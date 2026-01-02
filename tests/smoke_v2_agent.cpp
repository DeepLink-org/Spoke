// tests/smoke_v2_agent.cpp
#include <cmath>

#include "spoke/csrc/actor.h"
#include "spoke/csrc/serializer.h"

// 定义 Action ID
const spoke::Action kAddTen = (spoke::Action)0x11;
const spoke::Action kSquare = (spoke::Action)0x12;

class WorkerActor: public spoke::Actor {
public:
    WorkerActor(const std::string& id, int rx, int tx): spoke::Actor(id, rx, tx) {}

    // [修复] 宏现在需要 5 个参数: (类名, 方法名, ActionID, 请求类型, 响应类型)
    // 这里我们处理的是简单的 double -> double 计算
    SPOKE_METHOD(WorkerActor, addTen, kAddTen, double, double)
    {
        return val + 10.0;
    }

    SPOKE_METHOD(WorkerActor, square, kSquare, double, double)
    {
        return val * val;
    }
};

SPOKE_REGISTER_ACTOR("WorkerActor", WorkerActor);