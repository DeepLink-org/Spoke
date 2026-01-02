#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "spoke/csrc/client.h"

// === 必须与 Agent 端保持一致的定义 ===
struct ComplexTask {
    int                 id;
    std::string         name;
    std::vector<double> values;
};

namespace spoke {
template<>
struct Serializer<ComplexTask> {
    static std::string pack(const ComplexTask& t)
    {
        std::stringstream ss;
        ss << t.id << "|" << t.name << "|";
        for (double v : t.values)
            ss << v << ",";
        return ss.str();
    }
    static ComplexTask unpack(const std::string& s)
    {
        // Client 端其实只需要 pack，不需要 unpack request
        // 但为了完整性写上
        return {};
    }
};
}  // namespace spoke
// ===================================

const spoke::Action kProcessTask = (spoke::Action)0x60;

int main()
{
    std::string hub_ip   = "127.0.0.1";
    int         hub_port = 8888;

    std::cout << "[Smoke v5] Connecting to Hub..." << std::endl;
    spoke::Client client(hub_ip, hub_port, true);

    std::cout << "[Smoke v5] Spawning AdvancedActor..." << std::endl;
    client.spawnRemote("AdvancedActor", "adv_1");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 构造复杂对象
    ComplexTask task;
    task.id     = 999;
    task.name   = "ProjectX";
    task.values = {1.1, 2.2, 3.3, 4.4};

    std::cout << "[Smoke v5] Calling remote with Complex Object (Custom Serializer)..." << std::endl;

    // 泛型调用，自动使用我们定义的特化 Serializer
    auto fut = client.callRemote<ComplexTask, std::string>("adv_1", kProcessTask, task);

    std::cout << "[Smoke v5] Result: " << fut.get() << std::endl;

    return 0;
}
