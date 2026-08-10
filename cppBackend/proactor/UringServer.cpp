#include "UringServer.h"
#include "UringWorker.h"
#include "UringConnection.h"
#include "../logger/log_fac.h"
#include "concurrencpp/concurrencpp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/tcp.h>
#include <thread>
#include <unistd.h>

UringServer::UringServer(const std::string& ip, uint16_t port,
                         const UringWorkerCfg& cfg, bool opt_linger)
    : ip_(ip), port_(port), cfg_(cfg), opt_linger_(opt_linger), num_workers_(0) {
    LOGINFO("UringServer constructing ip=" + ip_ +
            " port=" + std::to_string(port_) +
            " uring_entries=" + std::to_string(cfg_.uring_entries) +
            " cpu_affinity=" + (cfg_.cpu_affinity ? "true" : "false") +
            " sqpoll=" + (cfg_.sqpoll ? "true" : "false") +
            " coop_taskrun=" + (cfg_.coop_taskrun ? "true" : "false"));
}

UringServer::~UringServer() {
    Stop();
}

void UringServer::start() {
    if (running_.load()) return;

    int cpu_count = static_cast<int>(std::thread::hardware_concurrency());
    if (cpu_count <= 0) cpu_count = 4;
    if (num_workers_ <= 0) num_workers_ = cpu_count;

    LOGINFO("UringServer starting with " + std::to_string(num_workers_) +
            " workers (SO_REUSEPORT multi-listener)");

    for (int i = 0; i < num_workers_; i++) {
        auto worker = std::make_unique<UringWorker>(i, cfg_);
        worker->SetCallbacks(
            [this](spIConnection conn) { HandleWorkerNewConnection(conn); },
            [this](spIConnection conn) { HandleCloseConnection(conn); },
            [this](spIConnection conn) { HandleErrorConnection(conn); },
            [this](spIConnection conn) { HandleMessage(conn); },
            [this](spIConnection conn) { HandleSendComplete(conn); }
        );

        if (coroutine_executor_) {
            worker->SetCoroutineExecutor(coroutine_executor_);
        }

        if (!worker->InitListenSocket(ip_, port_, opt_linger_)) {
            LOGERROR("UringServer worker " + std::to_string(i) +
                     " failed to init listen socket");
            return;
        }

        worker->Start();
        workers_.push_back(std::move(worker));
    }

    running_.store(true);

    LOGINFO("UringServer started with " + std::to_string(num_workers_) +
            " SO_REUSEPORT listeners");

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void UringServer::Stop() {
    if (!running_.exchange(false)) return;

    // 低危修复：无论从哪个线程调用都停止并回收 worker。
    // 旧实现担心"从 start 线程调用"会跳过停止，导致从 start 线程 Stop 时
    // worker 线程仍持有 ring_，随后析构（运行在同一线程上）销毁 ring_ → UAF。
    // worker->Stop() 内部 join 的是各自 io_thread_，从任意线程调用均安全。
    for (auto& worker : workers_) {
        worker->Stop();
    }
    workers_.clear();

    LOGINFO("UringServer stopped");
}

void UringServer::HandleWorkerNewConnection(spIConnection conn) {
    if (!conn) return;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_[conn->fd()] = std::dynamic_pointer_cast<UringConnection>(conn);
    }
    if (newconnectioncb_) {
        newconnectioncb_(conn);
    }
}

void UringServer::HandleCloseConnection(spIConnection conn) {
    if (!conn) return;

    if (closeconnectioncb_) {
        closeconnectioncb_(conn);
    }

    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_.erase(conn->fd());
    }

    auto uring_conn = std::dynamic_pointer_cast<UringConnection>(conn);
    if (uring_conn) {
        uring_conn->PostIoTask([uring_conn]() {
            uring_conn->ClearSendFile();
        });
    }

    LOGINFO("UringServer connection closed fd=" + std::to_string(conn->fd()));
}

void UringServer::HandleErrorConnection(spIConnection conn) {
    if (!conn) return;

    if (errorconnectioncb_) {
        errorconnectioncb_(conn);
    }

    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_.erase(conn->fd());
    }

    LOGWARNING("UringServer connection error fd=" + std::to_string(conn->fd()));
}

void UringServer::HandleMessage(spIConnection conn) {
    if (onmessagecb_) {
        onmessagecb_(conn);
    }
}

void UringServer::HandleSendComplete(spIConnection conn) {
    if (sendcompletecb_) {
        sendcompletecb_(conn);
    }
}

void UringServer::setnewconnectioncb(ConnCallback cb) {
    newconnectioncb_ = std::move(cb);
}

void UringServer::setcloseconnectioncb(ConnCallback cb) {
    closeconnectioncb_ = std::move(cb);
}

void UringServer::seterrorconnectioncb(ConnCallback cb) {
    errorconnectioncb_ = std::move(cb);
}

void UringServer::setonmessagecb(ConnCallback cb) {
    onmessagecb_ = std::move(cb);
}

void UringServer::setsendcompletecb(ConnCallback cb) {
    sendcompletecb_ = std::move(cb);
}

void UringServer::SetCoroutineExecutor(
    std::shared_ptr<concurrencpp::thread_pool_executor> exec) {
    coroutine_executor_ = std::move(exec);
    for (auto& worker : workers_) {
        worker->SetCoroutineExecutor(coroutine_executor_);
    }
}
