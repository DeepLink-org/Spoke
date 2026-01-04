#include "daemon.h"
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "spoke/csrc/agent.h"
#include "spoke/csrc/types.h"

namespace spoke {

void register_to_hub(const std::string& hub_ip, int hub_port, int my_port)
{
    int                sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(hub_port);
    inet_pton(AF_INET, hub_ip.c_str(), &sa.sin_addr);
    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(sock);
        return;
    }

    std::string my_addr = "127.0.0.1:" + std::to_string(my_port);
    NetMeta     meta{Action::kHubRegisterNode, 0, "", ""};
    NetHeader   hdr{0x504F4B45, sizeof(NetMeta), (uint32_t)my_addr.size()};
    send(sock, &hdr, sizeof(hdr), 0);
    send(sock, &meta, sizeof(meta), 0);
    send(sock, my_addr.data(), my_addr.size(), 0);

    NetHeader rh;
    recv(sock, &rh, sizeof(rh), MSG_WAITALL);
    NetRespMeta rm;
    recv(sock, &rm, sizeof(rm), MSG_WAITALL);
    if (rm.status > 0)
        std::cout << "[Daemon] Registered to Hub." << std::endl;
    close(sock);
}

void handle_client(int client_fd, Agent* agent)
{
    while (true) {
        NetHeader hdr;
        if (recv(client_fd, &hdr, sizeof(NetHeader), MSG_WAITALL) <= 0)
            break;
        NetMeta meta;
        if (hdr.meta_size > 0)
            recv(client_fd, &meta, hdr.meta_size, MSG_WAITALL);
        std::string body;
        body.resize(hdr.data_size);
        if (hdr.data_size > 0)
            recv(client_fd, body.data(), hdr.data_size, MSG_WAITALL);

        if (meta.action == Action::kNetSpawn) {
            std::cout << "[Daemon] Spawning: " << meta.actor_type << std::endl;
            agent->spawnActor(meta.actor_type, meta.actor_id);
            std::cout << "[Daemon] Spawned actor: " << meta.actor_id << std::endl;

            // Send Ack
            NetHeader   rh{0x504F4B45, sizeof(NetRespMeta), 0};
            NetRespMeta rm{meta.seq_id, 1};  // Success
            send(client_fd, &rh, sizeof(rh), 0);
            send(client_fd, &rm, sizeof(rm), 0);
        }
        else {
            auto        future = agent->callRemoteRaw(meta.actor_id, meta.action, body);
            std::string res    = future.get();
            std::cout << "[Daemon] Received result from actor. Size: " << res.size() << ". Sending to client..."
                      << std::endl;

            NetHeader   rh{0x504F4B45, sizeof(NetRespMeta), (uint32_t)res.size()};
            NetRespMeta rm{meta.seq_id, 1};
            if (send(client_fd, &rh, sizeof(rh), 0) < 0)
                perror("[Daemon] Failed to send header");
            if (send(client_fd, &rm, sizeof(rm), 0) < 0)
                perror("[Daemon] Failed to send meta");
            if (!res.empty())
                if (send(client_fd, res.data(), res.size(), 0) < 0)
                    perror("[Daemon] Failed to send body");
            std::cout << "[Daemon] Response sent to client." << std::endl;
        }
    }
    close(client_fd);
}

int run_daemon(int argc, char** argv)
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

    Agent local_agent;
    int   server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int   opt       = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(my_port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Daemon] Bind failed on port " << my_port << std::endl;
        return 1;
    }
    listen(server_fd, 5);
    std::cout << "[Daemon] Listening on " << my_port << "..." << std::endl;

    if (!hub_ip.empty())
        register_to_hub(hub_ip, hub_port, my_port);

    // Setup signal handling for graceful shutdown
    static bool g_running = true;
    signal(SIGPIPE, SIG_IGN);  // Prevent crash on write to dead child/client
    signal(SIGINT, [](int) {
        std::cout << "\n[Daemon] Shutting down..." << std::endl;
        g_running = false;
    });

    while (g_running) {
        // Use select/poll with timeout to allow checking g_running
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv{1, 0};  // 1s timeout

        int ret = select(server_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(server_fd, &fds)) {
            int cfd = accept(server_fd, nullptr, nullptr);
            if (cfd >= 0)
                std::thread(handle_client, cfd, &local_agent).detach();
        }
    }
    return 0;
}

}  // namespace spoke
