// spoke/registry.h
#pragma once
#include "actor.h"
#include <functional>
#include <iostream>
#include <map>
#include <string>

namespace spoke {

// 定义工厂函数类型：接收 ID 和 文件描述符，返回 Actor 指针
using ActorCreator = std::function<Actor*(std::string, int, int)>;

class Registry {
public:
    static std::map<std::string, ActorCreator>& GetMap()
    {
        static std::map<std::string, ActorCreator> map;
        return map;
    }

    static void Register(std::string name, ActorCreator creator)
    {
        GetMap()[name] = creator;
        // std::cout << "[Registry] Registered class: " << name << std::endl;
    }

    static Actor* Create(const std::string& name, const std::string& id, int rx, int tx)
    {
        auto& map = GetMap();
        if (map.find(name) == map.end())
            return nullptr;
        return map[name](id, rx, tx);
    }
};

// 辅助类，利用构造函数在 main 之前执行注册
struct RegisterHelper {
    RegisterHelper(std::string name, ActorCreator creator)
    {
        Registry::Register(name, creator);
    }
};

// --- 用户使用的宏 ---
// 原理：生成一个静态的 RegisterHelper 变量
#define SPOKE_REGISTER_ACTOR(CLASS_NAME)                                                                               \
    static spoke::RegisterHelper global_reg_##CLASS_NAME(                                                              \
        #CLASS_NAME, [](std::string id, int rx, int tx) -> spoke::Actor* { return new CLASS_NAME(id, rx, tx); })

}  // namespace spoke
