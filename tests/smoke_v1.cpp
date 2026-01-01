// tests/smoke_v3.cpp
// 目的：测试单机 IPC 模式（不走网络，直接 fork 并在 main 文件内定义 Actor 类）
// 这对于快速 Debug 业务逻辑非常有用。

#include "../csrc/spoke/actor.h"
#include "../csrc/spoke/agent.h"
#include <iostream>

using namespace spoke;

const Action kAddTen = (Action)0x11;
const Action kSquare = (Action)0x12;

// 注意：这里我们没有用 SPOKE_REGISTER_ACTOR 宏
// 因为我们在 main 里直接用模板生成它
class MyActor: public Actor {
public:
    MyActor(const std::string& id, int rx, int tx): Actor(id, rx, tx) {}

    SPOKE_METHOD(MyActor, addTen, kAddTen)
    {
        return val + 10.0;
    }

    SPOKE_METHOD(MyActor, square, kSquare)
    {
        return val * val;
    }
};

int main()
{
    Agent agent;

    // 这里的模板参数 <MyActor> 告诉 Agent 直接在子进程构造这个类
    // 不需要去工厂里查表
    std::string id = "v3_engine";
    agent.spawnActor<MyActor>(id);

    std::cout << "[Smoke V3] Firing tasks to " << id << "..." << std::endl;

    auto f1 = agent.callRemote(id, kAddTen, 5.5);
    auto f2 = agent.callRemote(id, kSquare, 4.0);

    std::cout << "[Smoke V3] 5.5 + 10 = " << f1.get() << std::endl;
    std::cout << "[Smoke V3] 4.0^2    = " << f2.get() << std::endl;

    return 0;
}
