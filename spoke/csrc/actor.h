#pragma once

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <unordered_map>

#include "serializer.h"
#include "types.h"

namespace spoke {

using MethodHandler = std::function<std::string(const std::string&)>;

class Actor {
public:
    Actor(const std::string& id, int rx_fd, int tx_fd): actor_id_(id), rx_fd_(rx_fd), tx_fd_(tx_fd) {}
    virtual ~Actor() = default;

    void run()
    {
        while (is_active_) {
            PipeHeader hdr;
            if (read(rx_fd_, &hdr, sizeof(PipeHeader)) <= 0)
                break;

            std::string body;
            body.resize(hdr.data_size);
            if (hdr.data_size > 0) {
                size_t total = 0;
                while (total < hdr.data_size) {
                    int r = read(rx_fd_, body.data() + total, hdr.data_size - total);
                    if (r <= 0)
                        break;
                    total += r;
                }
            }
            onMessage(hdr, body);
        }
    }

protected:
    void registerMethod(Action action, MethodHandler handler)
    {
        handlers_[action] = std::move(handler);
    }

private:
    void onMessage(const PipeHeader& hdr, const std::string& body)
    {
        if (hdr.action == Action::kStop) {
            is_active_ = false;
            return;
        }

        auto it = handlers_.find(hdr.action);
        if (it != handlers_.end()) {
            std::string res = it->second(body);

            PipeHeader resp_hdr{Action::kInit, hdr.seq_id, (uint32_t)res.size()};
            write(tx_fd_, &resp_hdr, sizeof(PipeHeader));
            if (!res.empty())
                write(tx_fd_, res.data(), res.size());
        }
        else {
            // Error handling
        }
    }

    std::string                               actor_id_;
    int                                       rx_fd_, tx_fd_;
    bool                                      is_active_ = true;
    std::unordered_map<Action, MethodHandler> handlers_;
};

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
        if (creators_.count(type))
            return creators_[type](id, rx, tx);
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
                Type, [](std::string id, int rx, int tx) { return std::make_unique<ClassName>(id, rx, tx); });         \
        }                                                                                                              \
    } reg_##ClassName##_inst_;

// [关键更新] 使用 Pack/Unpack 接口
#define SPOKE_METHOD(ClassName, MethodName, ActionID, ReqType, RespType)                                               \
    struct Reg_##MethodName {                                                                                          \
        Reg_##MethodName(ClassName* obj)                                                                               \
        {                                                                                                              \
            obj->registerMethod(ActionID, [obj](const std::string& raw) -> std::string {                               \
                ReqType  req  = spoke::Unpack<ReqType>(raw);                                                           \
                RespType resp = obj->MethodName(req);                                                                  \
                return spoke::Pack(resp);                                                                              \
            });                                                                                                        \
        }                                                                                                              \
    } reg_##MethodName##_inst_{this};                                                                                  \
    RespType MethodName(ReqType val)

}  // namespace spoke
