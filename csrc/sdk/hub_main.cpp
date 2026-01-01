#include <cstring>
#include <iostream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spoke/types.h"

#include "hub.h"

spoke::Hub g_hub;

void handle_connection(int fd)
{
    using namespace spoke;
    NetHeader header;
    if (recv(fd, &header, sizeof(NetHeader), MSG_WAITALL) <= 0) {
        close(fd);
        return;
    }
    NetRequest req;
    if (recv(fd, &req, sizeof(NetRequest), MSG_WAITALL) <= 0) {
        close(fd);
        return;
    }

    NetResponse resp;
    resp.seq_id = req.seq_id;
    memset(resp.extra_info, 0, 64);

    if (req.action == Action::kHubRegisterNode) {
        char ip_buf[32];
        int  port;
        sscanf(req.payload.raw, "%[^:]:%d", ip_buf, &port);
        g_hub.registerNode(ip_buf, port);
        resp.result = 1.0;
    }
    else if (req.action == Action::kHubFindNode) {
        NodeInfo node;
        if (g_hub.scheduleNode(&node)) {
            resp.result = 1.0;  // Found
            // 返回 "IP:Port"
            snprintf(resp.extra_info, 64, "%s:%d", node.ip.c_str(), node.port);
            std::cout << "[Hub] Assigned " << resp.extra_info << " to client." << std::endl;
        }
        else {
            resp.result = -1.0;  // No resources
            std::cout << "[Hub] No nodes available!" << std::endl;
        }
    }

    NetHeader resp_hdr;
    resp_hdr.payload_size = sizeof(NetResponse);
    send(fd, &resp_hdr, sizeof(NetHeader), 0);
    send(fd, &resp, sizeof(NetResponse), 0);

    close(fd);
}

int main(int argc, char** argv)
{
    int port      = 8888;  // Hub 默认端口
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt       = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

    (void)bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 5);

    std::cout << "[Hub] Spoke Hub Registry listening on " << port << "..." << std::endl;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            std::thread(handle_connection, client_fd).detach();
        }
    }
    return 0;
}
