// tests/smoke_v5_client.cpp
#include "../csrc/spoke/client.h"
#include <iostream>
#include <thread>

const spoke::Action kAddTen = (spoke::Action)0x11;
const spoke::Action kSquare = (spoke::Action)0x12;

int main(int argc, char** argv)
{
    std::string ip = "127.0.0.1";
    if (argc > 1)
        ip = argv[1];

    std::cout << "[Client] Connecting to Daemon at " << ip << "..." << std::endl;
    spoke::Client client(ip, 9000);

    // 1. 告诉远端 Daemon：请生成一个类型为 "WorkerActor" 的实例，ID叫 "gpu_node_1"
    std::cout << "[Client] Spawning remote actor..." << std::endl;
    client.spawnRemote("WorkerActor", "gpu_node_1");

    // 稍等一下让进程起来 (生产环境应用 CallBack 或 Future)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 2. 调用计算
    std::cout << "[Client] Sending tasks..." << std::endl;
    auto f1 = client.callRemote("gpu_node_1", kAddTen, 100.0);
    auto f2 = client.callRemote("gpu_node_1", kSquare, 8.0);

    std::cout << "[Client] Waiting results..." << std::endl;

    std::cout << "  100 + 10 = " << f1.get() << std::endl;
    std::cout << "  8 * 8    = " << f2.get() << std::endl;

    return 0;
}