#pragma once
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace spoke {

// 1. 定义序列化接口模板 (默认实现：二进制 memcpy)
template<typename T>
struct Serializer {
    static std::string pack(const T& obj)
    {
        // 默认行为：简单的内存拷贝 (仅适用于 POD 类型)
        std::string s;
        s.resize(sizeof(T));
        std::memcpy(s.data(), &obj, sizeof(T));
        return s;
    }

    static T unpack(const std::string& data)
    {
        // 默认行为：内存反拷贝
        T obj;
        if (data.size() >= sizeof(T)) {
            std::memcpy(&obj, data.data(), sizeof(T));
        }
        return obj;
    }
};

// 2. 特化：针对 std::string (框架内部经常用)
template<>
struct Serializer<std::string> {
    static std::string pack(const std::string& obj)
    {
        return obj;
    }
    static std::string unpack(const std::string& data)
    {
        return data;
    }
};

// 3. 辅助函数：用户调用这些即可，不需要写 Serializer<T>::...
template<typename T>
std::string Pack(const T& obj)
{
    return Serializer<T>::pack(obj);
}

template<typename T>
T Unpack(const std::string& raw)
{
    return Serializer<T>::unpack(raw);
}

}  // namespace spoke
