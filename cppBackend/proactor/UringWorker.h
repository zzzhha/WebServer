#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <liburing.h>
#include <netinet/in.h>

#include "../net/IConnection.h"

namespace concurrencpp {
    class thread_pool_executor;
}

class UringConnection;

struct UringUserData {
    enum class OpType : uint32_t {
        RECV,
        WRITEV,
        SENDFILE_READ,
        EVENTFD_READ,
        ACCEPT,
        SPLICE_FILE2PIPE,
        SPLICE_PIPE2SOCK,
        POLL_ADD,
        TIMEOUT_TICK,
    };

    OpType op;
    std::shared_ptr<UringConnection> conn;

    size_t splice_expected{0};
    short poll_mask{0};
    int accept_idx{-1};
};

struct UringWorkerCfg {
    int uring_entries{256};
    bool cpu_affinity{false};
    bool sqpoll{false};
    int sqpoll_idle_ms{1000};
    bool coop_taskrun{false};
    int tcp_timeout_seconds{360};
};

class UringWorker {
public:
    UringWorker(int worker_id, const UringWorkerCfg& cfg);
    ~UringWorker();

    void Start();
    void Stop();

    bool InitListenSocket(const std::string& ip, uint16_t port, bool opt_linger);

    void AddConnection(std::shared_ptr<UringConnection> conn);
    void RemoveConnection(std::shared_ptr<UringConnection> conn);

    void QueueTask(std::function<void()> task);

    void SubmitRead(std::shared_ptr<UringConnection> conn);
    void SubmitWrite(std::shared_ptr<UringConnection> conn);
    void SubmitSendFile(std::shared_ptr<UringConnection> conn);
    void SubmitPollAdd(std::shared_ptr<UringConnection> conn, short poll_mask);

    void RefreshConnTimer(const std::shared_ptr<UringConnection>& conn);

    void NotifyMessage(spIConnection conn);
    void NotifySendComplete(spIConnection conn);
    void NotifyClose(spIConnection conn);

    void SetCallbacks(
        std::function<void(spIConnection)> new_conn_cb,
        std::function<void(spIConnection)> close_cb,
        std::function<void(spIConnection)> error_cb,
        std::function<void(spIConnection)> message_cb,
        std::function<void(spIConnection)> send_complete_cb);

    int worker_id() const { return worker_id_; }
    io_uring* uring() { return &ring_; }

    // 协程化：设置/获取 coroutine executor（由 HttpServer 注入 works_executor_）
    void SetCoroutineExecutor(std::shared_ptr<concurrencpp::thread_pool_executor> exec);
    std::shared_ptr<concurrencpp::thread_pool_executor> GetCoroutineExecutor() const;

    // 诊断：转储本 worker 所有连接状态（排查高并发挂死）
    void DumpConnections(FILE* f);

private:
    struct ExpiredTimer {
        int fd;
        uint64_t generation;
    };

    class IoTimeWheel {
    public:
        IoTimeWheel(int slot_interval_s, int slots);

        void AddOrRefresh(int fd, uint64_t generation, int timeout_s);
        void Remove(int fd, uint64_t generation = 0);
        void Tick(std::vector<ExpiredTimer>* expired_out);

    private:
        struct TimerNode {
            int fd;
            int rotation;
            int slot;
            uint64_t generation;
        };

        void AddOrRefreshUnsafe(int fd, uint64_t generation, int timeout_s);

        int slot_interval_s_;
        int slots_;
        int current_slot_{0};

        std::vector<std::list<TimerNode>> wheel_;
        std::unordered_map<int, std::list<TimerNode>::iterator> timer_map_;
    };

    int worker_id_;
    io_uring ring_;
    UringWorkerCfg cfg_;
    int eventfd_;

    int listen_fd_{-1};

    static constexpr int kAcceptBatch = 64;
    struct AcceptSlot {
        struct sockaddr_in client_addr{};
        socklen_t client_addr_len{0};
        bool in_flight{false};
    };
    AcceptSlot accept_slots_[64]{};

    std::thread io_thread_;
    std::atomic<bool> running_{false};

    std::mutex conns_mutex_;
    std::unordered_map<int, std::shared_ptr<UringConnection>> connections_;

    bool eventfd_armed_{false};   // eventfd 读是否已武装（IoLoop 顶部自愈重武装）

    struct TaskNode {
        std::function<void()> task;
        TaskNode* next{nullptr};
    };
    std::atomic<TaskNode*> mpsc_head_{nullptr};

    std::function<void(spIConnection)> new_conn_cb_;
    std::function<void(spIConnection)> close_cb_;
    std::function<void(spIConnection)> error_cb_;
    std::function<void(spIConnection)> message_cb_;
    std::function<void(spIConnection)> send_complete_cb_;

    uint64_t eventfd_read_buf_{0};

    IoTimeWheel time_wheel_{1, 60};
    int tcp_timeout_s_{360};
    __kernel_timespec tick_timeout_ts_{};
    bool tick_submitted_{false};

    std::shared_ptr<concurrencpp::thread_pool_executor> coroutine_executor_;

    void IoLoop();
    void ProcessCQE(io_uring_cqe* cqe);
    void DrainTaskQueue();
    void SubmitEventFdRead();
    void SubmitAccept();
    void HandleAccept(int conn_fd, struct sockaddr_in& addr);

    void DoRemoveConnection(int fd, std::shared_ptr<UringConnection> conn);

    void SubmitTickTimeout();
    void HandleTickTimeout(int result);
    void AddConnTimer(const std::shared_ptr<UringConnection>& conn);

    UringUserData* AllocUserData(UringUserData::OpType op,
                                  std::shared_ptr<UringConnection> conn);
    void FreeUserData(UringUserData* ud);
};
