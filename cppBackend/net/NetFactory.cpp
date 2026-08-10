#include "NetFactory.h"

#include <algorithm>
#include <cctype>
#include <iostream>

#include "../reactor/tcpserver.h"
#include "../proactor/UringServer.h"
#include "../proactor/UringWorker.h"

namespace {
std::string NormalizeMode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return mode;
}

std::unique_ptr<INetServer> CreateReactorServer(const NetServerOptions& options) {
    return std::make_unique<TcpServer>(
        options.ip,
        options.port,
        options.io_threads,
        options.tcp_timeout_seconds,
        options.opt_linger);
}

std::unique_ptr<INetServer> CreateProactorServer(const NetServerOptions& options) {
    UringWorkerCfg cfg;
    cfg.uring_entries = options.uring_entries;
    cfg.cpu_affinity = options.cpu_affinity;
    cfg.sqpoll = options.sqpoll;
    cfg.sqpoll_idle_ms = options.sqpoll_idle_ms;
    cfg.coop_taskrun = options.coop_taskrun;
    cfg.tcp_timeout_seconds = options.tcp_timeout_seconds;

    return std::make_unique<UringServer>(
        options.ip,
        options.port,
        cfg,
        options.opt_linger);
}
}  // namespace

std::unique_ptr<INetServer> NetFactory::CreateServer(
    const std::string& mode,
    const NetServerOptions& options) {
    const std::string normalized_mode = NormalizeMode(mode);

    if (normalized_mode == "proactor") {
        return CreateProactorServer(options);
    }

    if (normalized_mode.empty() || normalized_mode == "reactor") {
        return CreateReactorServer(options);
    }

    std::cerr << "[NET-WARN] 未知 net.mode=" << mode
              << "，自动回退到 reactor 后端。" << std::endl;
    return CreateReactorServer(options);
}
