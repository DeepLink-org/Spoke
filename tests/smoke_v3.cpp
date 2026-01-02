#include <iostream>
#include <thread>
#include <vector>

#include "spoke/csrc/client.h"

// 定义动作 (与 WorkerActor 保持一致)
const spoke::Action kAddTen = (spoke::Action)0x11;
const spoke::Action kSquare = (spoke::Action)0x12;

int main(int argc, char** argv)
{
    std::string hub_ip   = "127.0.0.1";
    int         hub_port = 8888;

    std::cout << "[Orchestrator] Connecting to Hub at " << hub_ip << ":" << hub_port << "..." << std::endl;

    // --- 步骤 1: 创建两个独立的客户端句柄 ---
    // client_alpha 负责控制第一个节点
    // client_beta  负责控制第二个节点
    spoke::Client client_alpha(hub_ip, hub_port, true);
    spoke::Client client_beta(hub_ip, hub_port, true);

    // --- 步骤 2: 在集群中申请资源 (Spawn) ---
    // [修复 1] spawnInCluster 已重命名为 spawnRemote
    std::cout << "\n[Orchestrator] Spawning Agent Alpha..." << std::endl;
    client_alpha.spawnRemote("WorkerActor", "agent_alpha");

    std::cout << "[Orchestrator] Spawning Agent Beta..." << std::endl;
    client_beta.spawnRemote("WorkerActor", "agent_beta");

    // 给一点时间让进程拉起
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // --- 步骤 3: 执行流水线编排 (Pipeline) ---
    // 逻辑：Result = (Initial + 10)^2

    double initial_value = 50.0;
    std::cout << "\n[Orchestrator] Starting Pipeline: (" << initial_value << " + 10)^2" << std::endl;

    // [修复 2] 显式指定模板参数 <double, double>
    // 3.1 调用 Alpha 做加法
    std::cout << "  -> Sending " << initial_value << " to Agent Alpha (AddTen)..." << std::endl;
    auto f1 = client_alpha.callRemote<double, double>("agent_alpha", kAddTen, initial_value);

    double mid_result = f1.get();  // 获取中间结果
    std::cout << "  <- Alpha returned: " << mid_result << std::endl;

    // 3.2 将中间结果传给 Beta 做平方
    std::cout << "  -> Sending " << mid_result << " to Agent Beta (Square)..." << std::endl;
    auto f2 = client_beta.callRemote<double, double>("agent_beta", kSquare, mid_result);

    double final_result = f2.get();  // 获取最终结果
    std::cout << "  <- Beta returned: " << final_result << std::endl;

    // --- 步骤 4: 并行测试 (Parallelism) ---
    std::cout << "\n[Orchestrator] Starting Parallel Stress Test..." << std::endl;

    // [修复 2] 显式指定模板参数
    auto p1 = client_alpha.callRemote<double, double>("agent_alpha", kAddTen, 100.0);
    auto p2 = client_beta.callRemote<double, double>("agent_beta", kSquare, 5.0);

    std::cout << "  -> Tasks sent to both agents simultaneously." << std::endl;

    double r1 = p1.get();
    double r2 = p2.get();

    std::cout << "  <- Parallel Results: " << r1 << " & " << r2 << std::endl;

    return 0;
}
