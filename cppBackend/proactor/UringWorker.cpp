#include "UringWorker.h"
#include "UringConnection.h"
#include "../logger/log_fac.h"
#include "../reactor/Buffer.h"
#include "concurrencpp/concurrencpp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>

UringWorker::UringWorker(int worker_id, const UringWorkerCfg& cfg)
    : worker_id_(worker_id), cfg_(cfg), eventfd_(-1) {
    tcp_timeout_s_ = cfg_.tcp_timeout_seconds;
    tick_timeout_ts_.tv_sec = 1;
    tick_timeout_ts_.tv_nsec = 0;
}

UringWorker::~UringWorker() {
    Stop();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (eventfd_ >= 0) {
        ::close(eventfd_);
        eventfd_ = -1;
    }
}

bool UringWorker::InitListenSocket(const std::string& ip, uint16_t port, bool opt_linger) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        LOGERROR("UringWorker socket() failed worker=" +
                 std::to_string(worker_id_) + " err=" + std::strerror(errno));
        return false;
    }

    int optval = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

    if (opt_linger) {
        struct linger ling{1, 30};
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
    }

    int tcp_nodelay = 1;
    ::setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof(tcp_nodelay));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGERROR("UringWorker bind() failed worker=" +
                 std::to_string(worker_id_) + " err=" + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        LOGERROR("UringWorker listen() failed worker=" +
                 std::to_string(worker_id_) + " err=" + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    LOGINFO("UringWorker listen socket ready worker=" + std::to_string(worker_id_) +
            " fd=" + std::to_string(listen_fd_) +
            " ip=" + ip + ":" + std::to_string(port));
    return true;
}

UringWorker::IoTimeWheel::IoTimeWheel(int slot_interval_s, int slots)
    : slot_interval_s_(slot_interval_s), slots_(slots), current_slot_(0) {
    wheel_.resize(slots_);
}

void UringWorker::IoTimeWheel::AddOrRefresh(int fd, uint64_t generation, int timeout_s) {
    if (timeout_s <= 0) {
        Remove(fd);
        return;
    }
    AddOrRefreshUnsafe(fd, generation, timeout_s);
}

void UringWorker::IoTimeWheel::Remove(int fd, uint64_t generation) {
    auto it = timer_map_.find(fd);
    if (it == timer_map_.end()) return;
    // generation != 0 时校验代次：fd 可能已被新连接复用，不能误删新连接的定时器
    if (generation != 0 && it->second->generation != generation) return;
    const int slot = it->second->slot;
    wheel_[slot].erase(it->second);
    timer_map_.erase(it);
}

void UringWorker::IoTimeWheel::Tick(std::vector<ExpiredTimer>* expired_out) {
    if (!expired_out) return;

    auto& current_list = wheel_[current_slot_];
    auto it = current_list.begin();
    while (it != current_list.end()) {
        if (it->rotation > 0) {
            it->rotation--;
            ++it;
            continue;
        }
        expired_out->push_back(ExpiredTimer{it->fd, it->generation});
        timer_map_.erase(it->fd);
        it = current_list.erase(it);
    }
    current_slot_ = (current_slot_ + 1) % slots_;
}

void UringWorker::IoTimeWheel::AddOrRefreshUnsafe(int fd, uint64_t generation, int timeout_s) {
    auto it = timer_map_.find(fd);
    if (it != timer_map_.end()) {
        const int slot = it->second->slot;
        wheel_[slot].erase(it->second);
        timer_map_.erase(it);
    }

    int ticks = timeout_s / slot_interval_s_;
    if (timeout_s % slot_interval_s_ != 0) ticks++;
    if (ticks <= 0) ticks = 1;

    const int rotation = ticks / slots_;
    const int slot = (current_slot_ + (ticks % slots_)) % slots_;

    TimerNode node;
    node.fd = fd;
    node.rotation = rotation;
    node.slot = slot;
    node.generation = generation;

    wheel_[slot].push_front(node);
    timer_map_[fd] = wheel_[slot].begin();
}

void UringWorker::Start() {
    if (running_.load()) return;

    struct io_uring_params params{};
    unsigned flags = 0;

    if (cfg_.coop_taskrun) {
        flags |= IORING_SETUP_COOP_TASKRUN;
    }
    if (cfg_.cpu_affinity) {
        flags |= IORING_SETUP_SINGLE_ISSUER;
    }
    if (cfg_.sqpoll) {
        flags |= IORING_SETUP_SQPOLL;
        if (cfg_.sqpoll_idle_ms > 0) {
            params.sq_thread_idle = static_cast<unsigned>(cfg_.sqpoll_idle_ms);
        }
    }

    params.flags = flags;

    int ret = io_uring_queue_init_params(cfg_.uring_entries, &ring_, &params);
    if (ret < 0) {
        LOGERROR("UringWorker io_uring_queue_init_params failed worker=" +
                 std::to_string(worker_id_) + " err=" + std::to_string(-ret) +
                 " " + std::strerror(-ret));
        return;
    }

    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd_ < 0) {
        LOGERROR("UringWorker eventfd failed worker=" +
                 std::to_string(worker_id_));
        io_uring_queue_exit(&ring_);
        return;
    }

    SubmitEventFdRead();

    if (listen_fd_ >= 0) {
        for (int i = 0; i < kAcceptBatch; i++) {
            SubmitAccept();
        }
    }

    SubmitTickTimeout();

    running_.store(true);
    io_thread_ = std::thread([this]() {
        if (cfg_.cpu_affinity) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(worker_id_, &cpuset);
            int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
            if (rc != 0) {
                LOGWARNING("UringWorker CPU affinity bind failed worker=" +
                           std::to_string(worker_id_) + " err=" + std::strerror(rc));
            } else {
                LOGINFO("UringWorker CPU affinity bound worker=" +
                        std::to_string(worker_id_) + " to core=" +
                        std::to_string(worker_id_));
            }
        }
        IoLoop();
    });

    LOGINFO("UringWorker started worker=" + std::to_string(worker_id_) +
            " sqpoll=" + (cfg_.sqpoll ? "true" : "false") +
            " coop_taskrun=" + (cfg_.coop_taskrun ? "true" : "false"));
}

void UringWorker::Stop() {
    if (!running_.exchange(false)) return;

    if (eventfd_ >= 0) {
        uint64_t val = 1;
        ssize_t n = ::write(eventfd_, &val, sizeof(val));
        (void)n;
    }
    io_uring_submit(&ring_);

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    io_uring_queue_exit(&ring_);
    LOGINFO("UringWorker stopped worker=" + std::to_string(worker_id_));
}

void UringWorker::AddConnection(std::shared_ptr<UringConnection> conn) {
    if (!conn) return;

    int fd = conn->fd();
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        connections_[fd] = conn;
    }

    LOGINFO("UringWorker AddConnection worker=" + std::to_string(worker_id_) +
            " fd=" + std::to_string(fd));

    AddConnTimer(conn);

    if (new_conn_cb_) {
        new_conn_cb_(conn);
    }

    QueueTask([conn]() { conn->SubmitRecv(); });
}

void UringWorker::RemoveConnection(std::shared_ptr<UringConnection> conn) {
    if (!conn) return;
    const int fd = conn->fd();  // 提前捕获 fd：延迟任务执行时 fd_ 可能已被置 -1
    QueueTask([this, fd, conn]() { DoRemoveConnection(fd, conn); });
}

void UringWorker::DoRemoveConnection(int fd, std::shared_ptr<UringConnection> conn) {
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        auto it = connections_.find(fd);
        // 校验连接身份：close 后延迟任务执行前 fd 可能已被 accept 复用，
        // 只删除仍指向同一连接对象的条目，避免误删新连接（审计 H12）。
        if (it != connections_.end() && it->second == conn) {
            connections_.erase(it);
        }
    }
    time_wheel_.Remove(fd, conn ? conn->GetTimerGeneration() : 0);
}

void UringWorker::QueueTask(std::function<void()> task) {
    // 修复：MPSC 队列并发入队丢任务（高并发下连接挂死的根因）。
    // 原实现先 exchange 发布新 head、再设置 node->next：若线程 A 在
    // exchange 之后、给 node->next 赋值之前被抢占，线程 B 入队并触发
    // drain 时，drain 走到 A 后 next 仍为 nullptr，A 之前的节点全部丢失。
    // 标准 Treiber 栈：先设置 next 再 CAS 发布。
    TaskNode* node = new TaskNode{std::move(task), nullptr};
    TaskNode* prev = mpsc_head_.load(std::memory_order_relaxed);
    for (;;) {
        node->next = prev;
        if (mpsc_head_.compare_exchange_weak(prev, node,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
        // CAS 失败：prev 已被更新为最新 head，重试
    }

    uint64_t val = 1;
    ssize_t n = ::write(eventfd_, &val, sizeof(val));
    (void)n;
}

void UringWorker::SubmitRead(std::shared_ptr<UringConnection> conn) {
    if (!conn || conn->IsDisconnected()) return;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            LOGWARNING("UringWorker SubmitRead: no SQE available fd=" +
                       std::to_string(conn->fd()) + ", queue retry");
            // 不能静默丢弃：recv 不再武装会导致连接僵死（审计 H13）。
            // 重试任务在 CQE 被消费（SQE 槽释放）后的任务队列里执行。
            conn->PostIoTask([conn]() {
                UringWorker* w = conn->owner();
                if (w && !conn->IsDisconnected()) {
                    w->SubmitRead(conn);
                }
            });
            return;
        }
    }

    UringUserData* ud = AllocUserData(UringUserData::OpType::RECV, conn);
    io_uring_prep_recv(sqe, conn->fd(), conn->GetRecvBuf(),
                       UringConnection::kRecvBufSize, 0);
    sqe->user_data = reinterpret_cast<uint64_t>(ud);

    io_uring_submit(&ring_);
}

void UringWorker::SubmitWrite(std::shared_ptr<UringConnection> conn) {
    if (!conn || conn->IsDisconnected()) return;

    BufferBlock& output = conn->getOutputBuffer();
    if (output.readableBytes() == 0) return;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            LOGWARNING("UringWorker SubmitWrite: no SQE available fd=" +
                       std::to_string(conn->fd()) + ", queue retry");
            // 不能静默丢弃：send() 已置 write_submitted_，输出缓冲将永远发不出去
            // （审计 H13）。入队重试，待 CQE 释放 SQE 槽后再提交。
            conn->PostIoTask([conn]() {
                UringWorker* w = conn->owner();
                if (w && !conn->IsDisconnected()) {
                    w->SubmitWrite(conn);
                }
            });
            return;
        }
    }

    constexpr size_t kMaxIovs = 16;
    struct iovec iovs[kMaxIovs];
    size_t iov_count = output.getIOVecs(iovs, kMaxIovs, output.read_pos_);

    if (iov_count == 0) return;

    UringUserData* ud = AllocUserData(UringUserData::OpType::WRITEV, conn);
    io_uring_prep_writev(sqe, conn->fd(), iovs, static_cast<unsigned>(iov_count), 0);
    sqe->user_data = reinterpret_cast<uint64_t>(ud);

    io_uring_submit(&ring_);
}

void UringWorker::SubmitSendFile(std::shared_ptr<UringConnection> conn) {
    if (!conn || conn->IsDisconnected()) return;
    if (!conn->HasSendFile() || !conn->IsSendFilePipeOpen()) return;
    if (conn->GetSendFileRemaining() == 0) return;

    int file_fd = conn->GetSendFileFd();
    int pipe_wr = conn->GetSendFilePipeWriteFd();
    int pipe_rd = conn->GetSendFilePipeReadFd();
    int sock_fd = conn->fd();
    size_t remaining = conn->GetSendFileRemaining();
    off_t offset = conn->GetSendFileOffset();

    static constexpr size_t kMaxSplice = 1048576;
    size_t chunk = std::min(remaining, kMaxSplice);

    struct io_uring_sqe* sqe1 = io_uring_get_sqe(&ring_);
    struct io_uring_sqe* sqe2 = io_uring_get_sqe(&ring_);
    if (!sqe1 || !sqe2) {
        io_uring_submit(&ring_);
        sqe1 = io_uring_get_sqe(&ring_);
        sqe2 = io_uring_get_sqe(&ring_);
        if (!sqe1 || !sqe2) {
            LOGERROR("UringWorker SubmitSendFile: not enough SQEs fd=" +
                     std::to_string(sock_fd));
            conn->PostIoTask([conn]() {
                UringWorker* w = conn->owner();
                if (w && !conn->IsDisconnected()) {
                    w->SubmitSendFile(conn);
                }
            });
            return;
        }
    }

    UringUserData* ud1 = AllocUserData(UringUserData::OpType::SPLICE_FILE2PIPE, conn);
    ud1->splice_expected = chunk;

    io_uring_prep_splice(sqe1, file_fd, offset, pipe_wr, -1,
                         static_cast<unsigned int>(chunk), 0);
    sqe1->flags |= IOSQE_IO_LINK;
    sqe1->user_data = reinterpret_cast<uint64_t>(ud1);

    UringUserData* ud2 = AllocUserData(UringUserData::OpType::SPLICE_PIPE2SOCK, conn);
    ud2->splice_expected = chunk;

    io_uring_prep_splice(sqe2, pipe_rd, -1, sock_fd, -1,
                         static_cast<unsigned int>(chunk), 0);
    sqe2->user_data = reinterpret_cast<uint64_t>(ud2);

    io_uring_submit(&ring_);

    LOGINFO("UringWorker SubmitSendFile worker=" + std::to_string(worker_id_) +
            " fd=" + std::to_string(sock_fd) +
            " chunk=" + std::to_string(chunk) +
            " remaining=" + std::to_string(remaining));
}

void UringWorker::SubmitPollAdd(std::shared_ptr<UringConnection> conn, short poll_mask) {
    if (!conn || conn->IsDisconnected()) return;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            LOGWARNING("UringWorker SubmitPollAdd: no SQE available fd=" +
                       std::to_string(conn->fd()) + ", queue retry");
            // TLS 连接的读写都依赖 POLL_ADD 驱动，静默丢弃会导致 TLS 连接僵死
            // （审计 H13）。入队重试，待 CQE 释放 SQE 槽后再提交。
            conn->PostIoTask([conn]() {
                UringWorker* w = conn->owner();
                if (w && !conn->IsDisconnected()) {
                    w->SubmitPollAdd(conn, conn->GetTlsPollMask());
                }
            });
            return;
        }
    }

    UringUserData* ud = AllocUserData(UringUserData::OpType::POLL_ADD, conn);
    ud->poll_mask = poll_mask;

    io_uring_prep_poll_add(sqe, conn->fd(), static_cast<unsigned int>(poll_mask));
    sqe->user_data = reinterpret_cast<uint64_t>(ud);

    io_uring_submit(&ring_);

    LOGDEBUG("UringWorker SubmitPollAdd worker=" + std::to_string(worker_id_) +
             " fd=" + std::to_string(conn->fd()) +
             " mask=" + std::to_string(poll_mask));
}

void UringWorker::SubmitAccept() {
    if (listen_fd_ < 0) return;

    int slot = -1;
    for (int i = 0; i < kAcceptBatch; i++) {
        if (!accept_slots_[i].in_flight) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return;
    }

    accept_slots_[slot].client_addr_len = sizeof(accept_slots_[slot].client_addr);
    accept_slots_[slot].in_flight = true;

    UringUserData* ud = AllocUserData(UringUserData::OpType::ACCEPT, nullptr);
    ud->accept_idx = slot;

    io_uring_prep_accept(sqe, listen_fd_,
                         reinterpret_cast<struct sockaddr*>(&accept_slots_[slot].client_addr),
                         &accept_slots_[slot].client_addr_len, SOCK_NONBLOCK);
    sqe->user_data = reinterpret_cast<uint64_t>(ud);

    io_uring_submit(&ring_);
}

void UringWorker::HandleAccept(int conn_fd, struct sockaddr_in& addr) {
    char ip_str[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
    uint16_t client_port = ntohs(addr.sin_port);

    auto conn = std::make_shared<UringConnection>(conn_fd, std::string(ip_str),
                                                    client_port, this);
    AddConnection(conn);

    LOGINFO("UringWorker accept worker=" + std::to_string(worker_id_) +
            " fd=" + std::to_string(conn_fd) +
            " ip=" + std::string(ip_str) + ":" + std::to_string(client_port));
}

void UringWorker::NotifyMessage(spIConnection conn) {
    if (message_cb_) {
        message_cb_(conn);
    }
}

void UringWorker::NotifySendComplete(spIConnection conn) {
    if (send_complete_cb_) {
        send_complete_cb_(conn);
    }
}

void UringWorker::NotifyClose(spIConnection conn) {
    if (close_cb_) {
        close_cb_(conn);
    }
}

void UringWorker::SetCallbacks(
    std::function<void(spIConnection)> new_conn_cb,
    std::function<void(spIConnection)> close_cb,
    std::function<void(spIConnection)> error_cb,
    std::function<void(spIConnection)> message_cb,
    std::function<void(spIConnection)> send_complete_cb) {
    new_conn_cb_ = std::move(new_conn_cb);
    close_cb_ = std::move(close_cb);
    error_cb_ = std::move(error_cb);
    message_cb_ = std::move(message_cb);
    send_complete_cb_ = std::move(send_complete_cb);
}

void UringWorker::IoLoop() {
    LOGINFO("UringWorker IoLoop started worker=" + std::to_string(worker_id_) +
            " listen_fd=" + std::to_string(listen_fd_));

    while (true) {
        // 唤醒通道自愈（修复高并发下连接挂死）：
        // 1) 进入等待前确保 eventfd 读已武装；若之前武装失败（SQE 不足 / submit 失败），
        //    顶部重试，避免“任务已入队但 io 线程永远睡死”的丢失唤醒。
        // 2) 同样确保 tick 超时已武装（每秒兜底唤醒）。
        // 3) 等待前先排空任务队列：任务不会因为 eventfd 唤醒丢失而滞留。
        if (!eventfd_armed_) {
            SubmitEventFdRead();
        }
        if (!tick_submitted_ && tcp_timeout_s_ > 0) {
            SubmitTickTimeout();
        }
        DrainTaskQueue();

        if (!running_.load()) {
            break;
        }

        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);

        if (ret < 0) {
            if (ret == -EINTR) continue;
            LOGERROR("UringWorker io_uring_wait_cqe error worker=" +
                     std::to_string(worker_id_) + " ret=" + std::to_string(ret));
            continue;
        }

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            count++;
            ProcessCQE(cqe);
        }
        if (count > 0) {
            io_uring_cq_advance(&ring_, count);
        }

        DrainTaskQueue();

        if (!running_.load()) {
            break;
        }
    }

    LOGINFO("UringWorker IoLoop exited worker=" + std::to_string(worker_id_));
}

void UringWorker::ProcessCQE(io_uring_cqe* cqe) {
    UringUserData* ud = reinterpret_cast<UringUserData*>(cqe->user_data);
    if (!ud) return;

    int result = cqe->res;

    switch (ud->op) {
    case UringUserData::OpType::RECV: {
        auto conn = ud->conn;
        if (conn && !conn->IsDisconnected()) {
            conn->HandleRecvComplete(result);
        }
        break;
    }
    case UringUserData::OpType::WRITEV: {
        auto conn = ud->conn;
        if (conn && !conn->IsDisconnected()) {
            conn->HandleWriteComplete(result);
        }
        break;
    }
    case UringUserData::OpType::SENDFILE_READ: {
        break;
    }
    case UringUserData::OpType::EVENTFD_READ: {
        eventfd_read_buf_ = 0;
        // CQE 已消费：标记未武装，由 IoLoop 顶部重新武装（自愈路径）
        eventfd_armed_ = false;
        break;
    }
    case UringUserData::OpType::ACCEPT: {
        int slot = ud->accept_idx;
        if (slot >= 0 && slot < kAcceptBatch) {
            accept_slots_[slot].in_flight = false;
            int conn_fd = result;
            if (conn_fd >= 0) {
                HandleAccept(conn_fd, accept_slots_[slot].client_addr);
            } else {
                if (result != -EAGAIN && result != -EWOULDBLOCK) {
                    LOGWARNING("UringWorker accept error worker=" +
                               std::to_string(worker_id_) + " err=" + std::to_string(-result));
                }
            }
            accept_slots_[slot].client_addr_len = 0;
        }
        if (running_.load()) {
            SubmitAccept();
        }
        break;
    }
    case UringUserData::OpType::SPLICE_PIPE2SOCK: {
        auto conn = ud->conn;
        if (conn && !conn->IsDisconnected()) {
            conn->HandleSplicePipe2Sock(static_cast<ssize_t>(result));
        }
        break;
    }
    case UringUserData::OpType::SPLICE_FILE2PIPE: {
        auto conn = ud->conn;
        if (conn && !conn->IsDisconnected()) {
            conn->HandleSpliceFile2Pipe(static_cast<size_t>(result));
        }
        break;
    }
    case UringUserData::OpType::POLL_ADD: {
        auto conn = ud->conn;
        if (conn && !conn->IsDisconnected()) {
            short revents = static_cast<short>(result);
            conn->HandleTlsPollCQE(revents);
        }
        break;
    }
    case UringUserData::OpType::TIMEOUT_TICK: {
        HandleTickTimeout(result);
        break;
    }
    }

    FreeUserData(ud);
}

void UringWorker::DrainTaskQueue() {
    TaskNode* head = mpsc_head_.exchange(nullptr, std::memory_order_acq_rel);
    if (!head) return;

    std::vector<std::function<void()>> tasks;
    while (head) {
        TaskNode* next = head->next;
        if (head->task) {
            tasks.push_back(std::move(head->task));
        }
        delete head;
        head = next;
    }

    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
        if (*it) {
            (*it)();
        }
    }
}

void UringWorker::SubmitEventFdRead() {
    if (eventfd_armed_) return;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // 修复：不静默丢弃。IoLoop 顶部会在下一轮重试武装。
            eventfd_armed_ = false;
            LOGWARNING("UringWorker SubmitEventFdRead: no SQE available, retry at loop top");
            return;
        }
    }

    UringUserData* ud = AllocUserData(UringUserData::OpType::EVENTFD_READ, nullptr);
    io_uring_prep_read(sqe, eventfd_, &eventfd_read_buf_, sizeof(eventfd_read_buf_), 0);
    sqe->user_data = reinterpret_cast<uint64_t>(ud);

    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        if (ret == -EINTR) {
            // SQE 仍在 SQ 中未提交：重试一次提交
            ret = io_uring_submit(&ring_);
        }
        if (ret < 0) {
            eventfd_armed_ = false;
            LOGERROR("UringWorker SubmitEventFdRead: submit failed ret=" +
                     std::to_string(ret) + " err=" + std::strerror(-ret));
            return;
        }
    }
    eventfd_armed_ = true;
}

void UringWorker::SubmitTickTimeout() {
    if (tick_submitted_) return;
    if (tcp_timeout_s_ <= 0) return;

    constexpr int kMaxRetries = 8;
    struct io_uring_sqe* sqe = nullptr;

    for (int i = 0; i < kMaxRetries; ++i) {
        sqe = io_uring_get_sqe(&ring_);
        if (sqe) break;
        io_uring_submit(&ring_);
        std::this_thread::yield();
    }

    if (!sqe) {
        LOGERROR("UringWorker SubmitTickTimeout: SQE exhausted after " +
                 std::to_string(kMaxRetries) + " retries, forcing submit for drain");
        io_uring_submit(&ring_);
        QueueTask([this]() { SubmitTickTimeout(); });
        return;
    }

    UringUserData* ud = AllocUserData(UringUserData::OpType::TIMEOUT_TICK, nullptr);
    io_uring_prep_timeout(sqe, &tick_timeout_ts_, 0, 0);
    sqe->user_data = reinterpret_cast<uint64_t>(ud);
    tick_submitted_ = true;

    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        if (ret == -EINTR) {
            ret = io_uring_submit(&ring_);
        }
        if (ret < 0) {
            // 修复：submit 失败时恢复未提交状态，由 IoLoop 顶部重试，
            // 避免 tick 永久丢失导致 io 线程失去每秒兜底唤醒。
            tick_submitted_ = false;
            LOGERROR("UringWorker SubmitTickTimeout: submit failed ret=" +
                     std::to_string(ret) + " err=" + std::strerror(-ret));
            return;
        }
    }
}

void UringWorker::HandleTickTimeout(int result) {
    tick_submitted_ = false;

    if (!running_.load()) return;
    if (result == -ECANCELED) return;

    std::vector<ExpiredTimer> expired;
    time_wheel_.Tick(&expired);

    std::vector<int> leaked_fds;
    for (const auto& e : expired) {
        std::shared_ptr<UringConnection> conn;
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            auto it = connections_.find(e.fd);
            if (it != connections_.end()) {
                conn = it->second;
            } else {
                leaked_fds.push_back(e.fd);
            }
        }
        if (!conn) continue;
        if (conn->IsDisconnected()) {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            connections_.erase(e.fd);
            continue;
        }
        if (conn->GetTimerGeneration() != e.generation) continue;
        conn->DoClose();
    }

    if (!leaked_fds.empty()) {
        LOGWARNING("UringWorker tick found " + std::to_string(leaked_fds.size()) +
                   " fds in time_wheel but not in connections_map (fd reuse or leak), worker=" +
                   std::to_string(worker_id_));
    }

    SubmitTickTimeout();
}

void UringWorker::AddConnTimer(const std::shared_ptr<UringConnection>& conn) {
    if (!conn) return;
    if (tcp_timeout_s_ <= 0) return;
    const uint64_t gen = conn->BumpTimerGeneration();
    time_wheel_.AddOrRefresh(conn->fd(), gen, tcp_timeout_s_);
}

void UringWorker::RefreshConnTimer(const std::shared_ptr<UringConnection>& conn) {
    if (!conn) return;
    if (tcp_timeout_s_ <= 0) return;
    const uint64_t gen = conn->BumpTimerGeneration();
    time_wheel_.AddOrRefresh(conn->fd(), gen, tcp_timeout_s_);
}

void UringWorker::SetCoroutineExecutor(
    std::shared_ptr<concurrencpp::thread_pool_executor> exec) {
    coroutine_executor_ = std::move(exec);
}

std::shared_ptr<concurrencpp::thread_pool_executor>
UringWorker::GetCoroutineExecutor() const {
    return coroutine_executor_;
}

UringUserData* UringWorker::AllocUserData(UringUserData::OpType op,
                                            std::shared_ptr<UringConnection> conn) {
    return new UringUserData{op, std::move(conn)};
}

void UringWorker::FreeUserData(UringUserData* ud) {
    delete ud;
}

void UringWorker::DumpConnections(FILE* f) {
    if (!f) return;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        fprintf(f, "--- worker=%d conns=%zu ---\n", worker_id_, connections_.size());
        for (const auto& [fd, conn] : connections_) {
            if (!conn) continue;
            fprintf(f,
                    "  conn fd=%d recv_sub=%d write_sub=%d disc=%d "
                    "inbuf=%zu outbuf=%zu close_on_send=%d timer_gen=%llu\n",
                    conn->fd(), conn->IsRecvSubmitted() ? 1 : 0,
                    conn->IsWriteSubmitted() ? 1 : 0, conn->IsDisconnected() ? 1 : 0,
                    conn->GetBufferedInputSize(), conn->getOutputBuffer().readableBytes(),
                    conn->GetCloseOnSendComplete() ? 1 : 0,
                    static_cast<unsigned long long>(conn->GetTimerGeneration()));
        }
    }
    // 唤醒通道与任务队列状态
    const unsigned sq_head = *ring_.sq.khead;
    const unsigned sq_tail = *ring_.sq.ktail;
    const unsigned cq_head = *ring_.cq.khead;
    const unsigned cq_tail = *ring_.cq.ktail;
    fprintf(f,
            "  [wake] worker=%d eventfd_armed=%d tick_submitted=%d "
            "mpsc_pending=%d sq_entries=%u cq_entries=%u "
            "sq_inflight=%u cq_ready=%u cq_overflow=%d\n",
            worker_id_, eventfd_armed_ ? 1 : 0, tick_submitted_ ? 1 : 0,
            mpsc_head_.load(std::memory_order_acquire) != nullptr ? 1 : 0,
            ring_.sq.ring_entries, ring_.cq.ring_entries,
            sq_tail - sq_head, cq_tail - cq_head,
            (*ring_.cq.kflags & IORING_SQ_CQ_OVERFLOW) ? 1 : 0);
}
