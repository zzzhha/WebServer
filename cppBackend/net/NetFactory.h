#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "INetServer.h"

struct NetServerOptions {
    std::string ip;
    uint16_t port{0};
    int io_threads{6};
    int tcp_timeout_seconds{360};
    bool opt_linger{true};

    int uring_entries{256};
    bool cpu_affinity{false};
    bool sqpoll{false};
    int sqpoll_idle_ms{1000};
    bool coop_taskrun{false};
};

class NetFactory {
public:
    static std::unique_ptr<INetServer> CreateServer(
        const std::string& mode,
        const NetServerOptions& options);
};
