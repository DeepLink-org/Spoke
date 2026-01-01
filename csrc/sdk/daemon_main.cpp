#include <cstring>
#include <iostream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spoke/actor.h"
#include "spoke/agent.h"
#include "spoke/types.h"

void register_to_hub(const std::string& hub_ip, int hub_port, const std::string& my_ip, int my_port)
{
    using namespace spoke;
    int                sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(hub_port);
    inet_pton(AF_INET, hub_ip.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "[Daemon] Failed to connect to Hub at " << hub_ip << ":" << hub_port << std::endl;
        return;
    }

    NetRequest req;
    req.action = Action::kHubRegisterNode;
    req.seq_id = 0;

    snprintf(req.payload.raw, 32, "%s:%d", my_ip.c_str(), my_port);

    NetHeader hdr;
    hdr.payload_size = sizeof(NetRequest);

    send(sock, &hdr, sizeof(hdr), 0);
    send(sock, &req, sizeof(req), 0);

    NetHeader resp_hdr;
    recv(sock, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL);
    NetResponse resp;
    recv(sock, &resp, sizeof(resp), MSG_WAITALL);

    if (resp.result > 0) {
        std::cout << "[Daemon] Successfully registered to Hub!" << std::endl;
    }
    close(sock);
}

void handle_client(int client_fd, spoke::Agent* agent)
{
    using namespace spoke;
    std::cout << "[Daemon] Client connected!" << std::endl;

    while (true) {
        NetHeader header;
        int       n = recv(client_fd, &header, sizeof(NetHeader), MSG_WAITALL);
        if (n <= 0)
            break;

        NetRequest req;
        recv(client_fd, &req, sizeof(NetRequest), MSG_WAITALL);

        if (req.action == Action::kNetSpawn) {
            std::string type = req.actor_type;
            std::string id   = req.actor_id;

            std::cout << "[Daemon] Spawning actor type='" << type << "' id='" << id << "'" << std::endl;

            agent->spawnActor(type, id);
        }
        else {
            auto future = agent->callRemote(req.actor_id, req.action, req.payload.value_f64);

            double result = future.get();

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
    int         my_port  = 9000;
    std::string hub_ip   = "";
    int         hub_port = 8888;

    if (argc > 1)
        my_port = atoi(argv[1]);
    if (argc > 3) {
        hub_ip   = argv[2];
        hub_port = atoi(argv[3]);
    }

    spoke::Agent local_agent;
    int          server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int          opt       = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(my_port);
    (void)bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);

    std::cout << "[Daemon] Listening on port " << my_port << "..." << std::endl;

    if (!hub_ip.empty()) {
        register_to_hub(hub_ip, hub_port, "127.0.0.1", my_port);
    }

    // 3. Loop Accept
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            std::thread(handle_client, client_fd, &local_agent).detach();
        }
    }
    return 0;
}
