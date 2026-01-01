// sdk/daemon.cpp
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../csrc/spoke/actor.h"
#include "../csrc/spoke/agent.h"
#include "../csrc/spoke/types.h"

// --- Daemon 主逻辑 ---

void handle_client(int client_fd, spoke::Agent* agent)
{
    using namespace spoke;
    std::cout << "[Daemon] Client connected!" << std::endl;

    while (true) {
        NetHeader header;
        // 1. 读包头
        int n = recv(client_fd, &header, sizeof(NetHeader), MSG_WAITALL);
        if (n <= 0)
            break;

        // 2. 读包体
        NetRequest req;
        recv(client_fd, &req, sizeof(NetRequest), MSG_WAITALL);

        if (req.action == Action::kNetSpawn) {
            std::string type = req.actor_type;
            std::string id   = req.actor_id;

            std::cout << "[Daemon] Spawning actor type='" << type << "' id='" << id << "'" << std::endl;

            // 通过工厂模式动态生成，不再硬编码类名
            agent->spawnActor(type, id);
        }
        else {
            // 这是一个计算请求转发
            // std::cout << "[Daemon] Action " << (uint32_t)req.action << " -> " << req.actor_id << std::endl;

            // 调用本地 Agent，获取 Future
            auto future = agent->callRemote(req.actor_id, req.action, req.payload.value_f64);

            // 等待结果 (简单版：阻塞式)
            double result = future.get();

            // 回传结果给 Client
            NetHeader resp_header;
            resp_header.payload_size = sizeof(NetResponse);
            NetResponse resp;
            resp.seq_id = req.seq_id;
            resp.result = result;

            send(client_fd, &resp_header, sizeof(NetHeader), 0);
            send(client_fd, &resp, sizeof(NetResponse), 0);
        }
    }
    close(client_fd);
    std::cout << "[Daemon] Client disconnected." << std::endl;
}

int main(int argc, char** argv)
{
    int port = 9000;
    if (argc > 1)
        port = atoi(argv[1]);

    spoke::Agent local_agent;  // 管理本机的子进程

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt       = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return -1;
    }
    listen(server_fd, 3);

    std::cout << "[Daemon] Spoke Daemon listening on port " << port << "..." << std::endl;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            // 为每个 Client 启动一个线程处理
            std::thread(handle_client, client_fd, &local_agent).detach();
        }
    }
    return 0;
}
