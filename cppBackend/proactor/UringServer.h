#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "../net/INetServer.h"
#include "UringWorker.h"

namespace concurrencpp {
    class thread_pool_executor;
}

class UringWorker;
class UringConnection;

class UringServer : public INetServer {
public:
    UringServer(const std::string& ip, uint16_t port,
                const UringWorkerCfg& cfg, bool opt_linger);
    ~UringServer() override;

    void start() override;
    void Stop() override;

    void setnewconnectioncb(ConnCallback cb) override;
    void setcloseconnectioncb(ConnCallback cb) override;
    void seterrorconnectioncb(ConnCallback cb) override;
    void setonmessagecb(ConnCallback cb) override;
    void setsendcompletecb(ConnCallback cb) override;

    // 协程化：向所有 UringWorker 注入 coroutine executor
    void SetCoroutineExecutor(std::shared_ptr<concurrencpp::thread_pool_executor> exec);

    // 诊断：转储所有 worker 的连接状态（排查高并发挂死）
    void DumpConnections(FILE* f);

private:
    std::string ip_;
    uint16_t port_;
    UringWorkerCfg cfg_;
    bool opt_linger_;

    int num_workers_;
    std::atomic<bool> running_{false};

    std::vector<std::unique_ptr<UringWorker>> workers_;
    std::mutex conns_mutex_;
    std::map<int, std::shared_ptr<UringConnection>> conns_;

    ConnCallback newconnectioncb_;
    ConnCallback closeconnectioncb_;
    ConnCallback errorconnectioncb_;
    ConnCallback onmessagecb_;
    ConnCallback sendcompletecb_;

    std::shared_ptr<concurrencpp::thread_pool_executor> coroutine_executor_;

    void HandleWorkerNewConnection(spIConnection conn);
    void HandleCloseConnection(spIConnection conn);
    void HandleErrorConnection(spIConnection conn);
    void HandleMessage(spIConnection conn);
    void HandleSendComplete(spIConnection conn);
};
