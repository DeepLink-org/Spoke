#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "hub.h"
#include "spoke/csrc/types.h"

spoke::Hub g_hub;

void handle_connection(int fd)
{
    using namespace spoke;
    NetHeader hdr;
    if (recv(fd, &hdr, sizeof(NetHeader), MSG_WAITALL) <= 0) {
        close(fd);
        return;
    }
    NetMeta  meta;
    uint32_t mlen = (hdr.meta_size > 0) ? hdr.meta_size : sizeof(NetMeta);
    if (recv(fd, &meta, mlen, MSG_WAITALL) <= 0) {
        close(fd);
        return;
    }
    std::string req_body;
    if (hdr.data_size > 0) {
        req_body.resize(hdr.data_size);
        recv(fd, req_body.data(), hdr.data_size, MSG_WAITALL);
    }

    std::string resp_body;
    int         status = 0;
    if (meta.action == Action::kHubRegisterNode) {
        char ip[64];
        int  port;
        if (sscanf(req_body.c_str(), "%[^:]:%d", ip, &port) == 2) {
            g_hub.registerNode(ip, port);
            status = 1;
            std::cout << "[Hub] Node Registered: " << ip << ":" << port << std::endl;
        }
    }
    else if (meta.action == Action::kHubFindNode) {
        NodeInfo node;
        if (g_hub.scheduleNode(&node)) {
            status    = 1;
            resp_body = node.ip + ":" + std::to_string(node.port);
            std::cout << "[Hub] Assigned " << resp_body << std::endl;
        }
        else {
            status = -1;
        }
    }

    NetHeader   rh{0x504F4B45, sizeof(NetRespMeta), (uint32_t)resp_body.size()};
    NetRespMeta rm{meta.seq_id, status};
    send(fd, &rh, sizeof(rh), 0);
    send(fd, &rm, sizeof(rm), 0);
    if (!resp_body.empty())
        send(fd, resp_body.data(), resp_body.size(), 0);
    close(fd);
}

int main(int argc, char** argv)
{
    int port = 8888;
    if (argc > 1)
        port = atoi(argv[1]);
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    (void)bind(sfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sfd, 5);
    std::cout << "[Hub] Listening on " << port << "..." << std::endl;
    while (true) {
        int cfd = accept(sfd, nullptr, nullptr);
        if (cfd >= 0)
            std::thread(handle_connection, cfd).detach();
    }
    return 0;
}
