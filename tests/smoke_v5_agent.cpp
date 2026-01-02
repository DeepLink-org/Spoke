#include <sstream>
#include <vector>

#include "spoke/csrc/actor.h"

// 1. 定义复杂业务对象
struct ComplexTask {
    int                 id;
    std::string         name;
    std::vector<double> values;
};

// 2. 自定义序列化逻辑 (演示用，简单的逗号分隔字符串)
// 只要客户端和服务器端的特化逻辑一致即可
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
        ComplexTask       t;
        std::stringstream ss(s);
        std::string       seg;

        std::getline(ss, seg, '|');
        t.id = std::stoi(seg);
        std::getline(ss, t.name, '|');
        std::string vals;
        std::getline(ss, vals);
        std::stringstream vs(vals);
        std::string       v;
        while (std::getline(vs, v, ','))
            t.values.push_back(std::stod(v));
        return t;
    }
};
}  // namespace spoke

const spoke::Action kProcessTask = (spoke::Action)0x60;

class AdvancedActor: public spoke::Actor {
public:
    using spoke::Actor::Actor;

    // 使用宏，自动调用上面的特化 Serializer
    SPOKE_METHOD(AdvancedActor, process, kProcessTask, ComplexTask, std::string)
    {
        double sum = 0;
        for (auto v : val.values)
            sum += v;

        std::string res = "Processed " + val.name + " (ID:" + std::to_string(val.id) + ") Sum=" + std::to_string(sum);
        return res;
    }
};

SPOKE_REGISTER_ACTOR("AdvancedActor", AdvancedActor);
