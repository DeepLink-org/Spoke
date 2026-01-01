#pragma once
#include "types.h"
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <unordered_map>

namespace spoke {

class Actor {
public:
    Actor(const std::string& id, int rx_fd, int tx_fd): actor_id_(id), rx_fd_(rx_fd), tx_fd_(tx_fd) {}
    virtual ~Actor() = default;

    void run()
    {
        while (is_active_) {
            Message msg;
            if (read(rx_fd_, &msg, sizeof(Message)) <= 0)
                break;
            onMessage(msg);
        }
    }

protected:
    void registerMethod(Action action, std::function<double(double)> handler)
    {
        handlers_[action] = std::move(handler);
    }

private:
    void onMessage(const Message& msg)
    {
        auto it = handlers_.find(msg.action);
        if (it != handlers_.end()) {
            double   res  = it->second(msg.payload.value_f64);
            Response resp = {msg.seq_id, res};
            write(tx_fd_, &resp, sizeof(Response));
            return;
        }
        // 内置控制指令
        if (msg.action == Action::kStop) {
            is_active_ = false;
        }
        else {
            // 错误或未处理
            Response err = {msg.seq_id, -999.99};
            write(tx_fd_, &err, sizeof(Response));
        }
    }

    std::string                                               actor_id_;
    int                                                       rx_fd_, tx_fd_;
    bool                                                      is_active_ = true;
    std::unordered_map<Action, std::function<double(double)>> handlers_;
};

// --- 全局工厂 (Factory) ---
using ActorFactoryFunc = std::function<std::unique_ptr<Actor>(std::string, int, int)>;

class ActorFactory {
public:
    static ActorFactory& instance()
    {
        static ActorFactory inst;
        return inst;
    }

    void registerType(const std::string& type, ActorFactoryFunc func)
    {
        creators_[type] = func;
    }

    std::unique_ptr<Actor> create(const std::string& type, const std::string& id, int rx, int tx)
    {
        if (creators_.count(type)) {
            return creators_[type](id, rx, tx);
        }
        return nullptr;
    }

private:
    std::map<std::string, ActorFactoryFunc> creators_;
};

#define SPOKE_REGISTER_ACTOR(Type, ClassName)                                                                          \
    struct Reg_##ClassName {                                                                                           \
        Reg_##ClassName()                                                                                              \
        {                                                                                                              \
            spoke::ActorFactory::instance().registerType(                                                              \
                Type, [](std::string id, int rx, int tx) -> std::unique_ptr<spoke::Actor> {                            \
                    return std::make_unique<ClassName>(id, rx, tx);                                                    \
                });                                                                                                    \
        }                                                                                                              \
    } reg_##ClassName##_inst_;

#define SPOKE_METHOD(ClassName, MethodName, ActionID)                                                                  \
    struct Reg_Method_##MethodName {                                                                                   \
        Reg_Method_##MethodName(ClassName* obj)                                                                        \
        {                                                                                                              \
            obj->registerMethod(ActionID, [obj](double v) { return obj->MethodName(v); });                             \
        }                                                                                                              \
    } reg_method_##MethodName##_inst_{this};                                                                           \
    double MethodName(double val)

}  // namespace spoke
