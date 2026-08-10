#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>

#include <poll.h>

#include "../net/IConnection.h"
#include "../reactor/Buffer.h"

namespace concurrencpp {
    class thread_pool_executor;
}

class UringWorker;
class TlsContext;
class TlsSession;

class UringConnection : public IConnection {
public:
    UringConnection(int fd, const std::string& ip, uint16_t port,
                    UringWorker* owner);
    ~UringConnection() override;

    int fd() const override { return fd_; }
    std::string ip() const override { return ip_; }
    uint16_t port() const override { return port_; }
    bool IsDisconnected() const override { return disconnected_.load(); }

    BufferBlock& getInputBuffer() override { return inputbuffer_; }
    BufferBlock& getOutputBuffer() override { return outputbuffer_; }

    void send() override;
    void setCloseOnSendComplete(bool close) override;
    void StartSendFile(int file_fd, off_t offset, size_t length, bool auto_close) override;
    void ClearSendFile() override;

    void PostIoTask(std::function<void()> task) override;

    UringConnection* AsUringConnection() override { return this; }
    bool IsProactorMode() const override { return true; }

    void SetTlsContext(std::shared_ptr<TlsContext> ctx) override;

    void SubmitRecv();
    void HandleRecvComplete(int result);
    void HandleWriteComplete(int result);

    void HandleSpliceFile2Pipe(size_t nspliced);
    void HandleSplicePipe2Sock(ssize_t nsent);
    void SubmitSpliceChain();

    void SubmitTlsPoll(short poll_mask);
    void HandleTlsPollCQE(short revents);
    short GetTlsPollMask() const { return tls_poll_mask_; }
    bool IsTlsActive() const {
        return tls_session_ != nullptr && tls_poll_state_ != TlsPollState::NONE;
    }

    std::shared_ptr<UringConnection> SharedFromThis() {
        return std::static_pointer_cast<UringConnection>(shared_from_this());
    }

    bool GetCloseOnSendComplete() const { return close_on_send_complete_; }
    bool HasSendFile() const { return sendfile_.active; }
    int GetSendFileFd() const { return sendfile_.file_fd; }
    off_t GetSendFileOffset() const { return sendfile_.offset; }
    size_t GetSendFileRemaining() const { return sendfile_.remaining; }
    bool HasSendFileRemaining() const { return sendfile_.remaining > 0; }
    int GetSendFilePipeReadFd() const { return sendfile_.pipe_fds[0]; }
    int GetSendFilePipeWriteFd() const { return sendfile_.pipe_fds[1]; }
    bool IsSendFilePipeOpen() const { return sendfile_.pipe_open; }
    void AdvanceSendFile(size_t n) {
        sendfile_.offset += static_cast<off_t>(n);
        sendfile_.remaining -= n;
    }
    void CompleteSendFile() {
        sendfile_.ClosePipe();
        ClearSendFile();
    }

    char* GetRecvBuf() { return recv_buf_.get(); }
    static constexpr size_t kRecvBufSize = 8192;
    bool IsRecvSubmitted() const { return recv_submitted_; }
    bool IsWriteSubmitted() const { return write_submitted_; }

    void DoClose();
    size_t GetBufferedInputSize() const;
    std::string ConsumeBufferedInput();

    UringWorker* owner() { return owner_; }

    uint64_t BumpTimerGeneration() { return ++timer_generation_; }
    uint64_t GetTimerGeneration() const { return timer_generation_.load(); }

    // ===== 协程化 Awaitable 接口 (Phase 1) =====

    struct RecvReadyAwaiter {
        UringConnection* conn;
        std::shared_ptr<concurrencpp::thread_pool_executor> executor;

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> h);
        size_t await_resume() noexcept;
    };

    struct WriteResponseAwaiter {
        UringConnection* conn;
        std::string data;
        bool close_after;
        std::shared_ptr<concurrencpp::thread_pool_executor> executor;
        bool success_{true};

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> h);
        bool await_resume() noexcept;
    };

    struct SendFileAwaiter {
        UringConnection* conn;
        std::string header_data;
        int file_fd;
        off_t offset;
        size_t length;
        bool close_after;
        std::shared_ptr<concurrencpp::thread_pool_executor> executor;
        bool success_{true};

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> h);
        bool await_resume() noexcept;
    };

    // 等待 inputbuffer_ 有可读数据，返回可读字节数（0 表示连接断开）
    RecvReadyAwaiter AsyncRecvReady();

    // 异步写响应数据，挂起等待 IO 线程完成写操作
    WriteResponseAwaiter AsyncWriteResponse(std::string data, bool close_after = false);

    // 异步 sendfile（响应头 writev + 文件体 splice 零拷贝）
    SendFileAwaiter AsyncSendFile(std::string header_data, int file_fd,
                                  off_t offset, size_t length, bool close_after = false);

    // 注册/恢复协程句柄
    void RegisterRecvResumeHandle(std::coroutine_handle<> h,
                                   std::shared_ptr<concurrencpp::thread_pool_executor> exec);
    void RegisterWriteResumeHandle(std::coroutine_handle<> h,
                                    std::shared_ptr<concurrencpp::thread_pool_executor> exec,
                                    uint64_t await_id);
    void ClearAllResumeHandles();

private:
    void AppendInputBytes(const char* data, size_t size);
    bool TryResumeRecvAwaiterIfReady();
    uint64_t ReserveWriteAwaitId();
    void MarkWriteAwaitStarted(uint64_t await_id);
    void FinishWriteChain();

    int fd_;
    std::string ip_;
    uint16_t port_;
    std::atomic<bool> disconnected_{false};
    UringWorker* owner_;

    BufferBlock inputbuffer_;
    BufferBlock outputbuffer_;

    bool close_on_send_complete_{false};
    bool recv_submitted_{false};
    bool write_submitted_{false};
    std::atomic<uint64_t> timer_generation_{0};

    struct SendFileState {
        int file_fd{-1};
        off_t offset{0};
        size_t remaining{0};
        bool auto_close{false};
        bool active{false};
        int pipe_fds[2]{-1, -1};
        bool pipe_open{false};
        void ClosePipe() {
            if (pipe_fds[0] >= 0) { ::close(pipe_fds[0]); pipe_fds[0] = -1; }
            if (pipe_fds[1] >= 0) { ::close(pipe_fds[1]); pipe_fds[1] = -1; }
            pipe_open = false;
        }
    } sendfile_;

    std::unique_ptr<char[]> recv_buf_;

    std::shared_ptr<TlsContext> tls_ctx_;
    std::unique_ptr<TlsSession> tls_session_;

    enum class TlsPollState { NONE, HANDSHAKE, ACTIVE };
    TlsPollState tls_poll_state_{TlsPollState::NONE};
    bool tls_poll_submitted_{false};
    short tls_poll_mask_{0};

    std::string tls_out_pending_;

    // 协程恢复上下文
    struct ResumeEntry {
        std::coroutine_handle<> handle;
        std::shared_ptr<concurrencpp::thread_pool_executor> executor;
    };
    std::optional<ResumeEntry> recv_resume_;
    std::optional<ResumeEntry> write_resume_;
    mutable std::mutex resume_mutex_;  // 保护 recv_resume_/write_resume_ 的跨线程并发访问
    mutable std::mutex input_buffer_mutex_;
    uint64_t next_write_await_id_{1};
    uint64_t active_write_await_id_{0};
    uint64_t completed_write_await_id_{0};
};
