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

void register_to_hub(const std::string& hub_ip, int hub_port, int my_port, const ResourceSpec& capacity)
{
    int                sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(hub_port);
    inet_pton(AF_INET, hub_ip.c_str(), &sa.sin_addr);
    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(sock);
        std::cerr << "[Daemon] Failed to connect to Hub at " << hub_ip << ":" << hub_port << std::endl;
        return;
    }

    RegisterNodeReq req;
    strncpy(req.ip, "127.0.0.1", sizeof(req.ip) - 1); // Ideally, get actual IP
    req.port = my_port;
    req.capacity = capacity;

    NetMeta     meta{Action::kHubRegisterNode, 0, "", ""};
    NetHeader   hdr{0x504F4B45, sizeof(NetMeta), sizeof(RegisterNodeReq)};

    send(sock, &hdr, sizeof(hdr), 0);
    send(sock, &meta, sizeof(meta), 0);
    send(sock, &req, sizeof(req), 0);

    NetHeader rh;
    recv(sock, &rh, sizeof(rh), MSG_WAITALL);
    NetRespMeta rm;
    recv(sock, &rm, sizeof(rm), MSG_WAITALL);
    if (rm.status > 0)
        std::cout << "[Daemon] Registered to Hub with " << capacity.num_gpus << " GPUs." << std::endl;
    else
        std::cerr << "[Daemon] Registration failed." << std::endl;
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

            // Check for V2 Spawn Body
            std::vector<int> gpu_ids;
            if (body.size() >= sizeof(SpawnActorReq)) {
                // V2 Launch (with Resource Spec)
                const auto* req = reinterpret_cast<const SpawnActorReq*>(body.data());
                for (int i=0; i<8; ++i) {
                    if (req->assigned_gpu_ids[i] >= 0) {
                        gpu_ids.push_back(req->assigned_gpu_ids[i]);
                    }
                }
                // Args are after the struct, if any (not implemented yet)
            } else {
                // Legacy V1 Spawn (Empty Body or just string args)
                // Default: no GPU isolation
            }

            agent->spawnActor(meta.actor_type, meta.actor_id, gpu_ids);
            std::cout << "[Daemon] Spawned actor: " << meta.actor_id
                      << " GPUs: " << gpu_ids.size() << std::endl;

            // Send Ack
            NetHeader   rh{0x504F4B45, sizeof(NetRespMeta), 0};
            NetRespMeta rm{meta.seq_id, 1};  // Success
            send(client_fd, &rh, sizeof(rh), 0);
            send(client_fd, &rm, sizeof(rm), 0);
        }
        else {
            // [Fix] For user actions, dispatch asynchronously and handle responses in background
            // This allows collective operations (like NVSHMEM barriers) to work correctly
            auto future = agent->callRemoteRaw(meta.actor_id, meta.action, body);
            
            // Capture by value for thread safety
            uint32_t seq_id_copy = meta.seq_id;
            int client_fd_copy = client_fd;
            
            std::thread([future = std::move(future), seq_id_copy, client_fd_copy]() mutable {
                std::string res = future.get();
                
                NetHeader   rh{0x504F4B45, sizeof(NetRespMeta), (uint32_t)res.size()};
                NetRespMeta rm{seq_id_copy, 1};
                
                // Use a static mutex to serialize sends to same client_fd
                static std::mutex send_mtx;
                std::lock_guard<std::mutex> lk(send_mtx);
                
                if (send(client_fd_copy, &rh, sizeof(rh), 0) < 0)
                    perror("[Daemon] Failed to send header");
                if (send(client_fd_copy, &rm, sizeof(rm), 0) < 0)
                    perror("[Daemon] Failed to send meta");
                if (!res.empty())
                    if (send(client_fd_copy, res.data(), res.size(), 0) < 0)
                        perror("[Daemon] Failed to send body");
            }).detach();
        }
    }
    close(client_fd);
}

int run_daemon(int argc, char** argv)
{
    int         my_port  = 9000;
    std::string hub_ip   = "";
    int         hub_port = 8888;
    ResourceSpec capacity{0, 0};

    // Argument Parsing (Simple)
    // Usage: ./agent <port> <hub_ip> <hub_port> [--gpus <n>]
    std::vector<std::string> args;
    for(int i=0; i<argc; ++i) args.push_back(argv[i]);

    if (args.size() > 1) my_port = std::stoi(args[1]);
    if (args.size() > 3) {
        hub_ip = args[2];
        hub_port = std::stoi(args[3]);
    }

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--gpus" && i + 1 < args.size()) {
            capacity.num_gpus = std::stoi(args[i+1]);
            i++;
        }
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
        register_to_hub(hub_ip, hub_port, my_port, capacity);

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
