#include "UringConnection.h"
#include "UringWorker.h"
#include "../logger/log_fac.h"
#include "../reactor/TlsContext.h"
#include "../reactor/TlsSession.h"
#include "concurrencpp/concurrencpp.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

UringConnection::UringConnection(int fd, const std::string& ip, uint16_t port,
                                 UringWorker* owner)
    : fd_(fd), ip_(ip), port_(port), owner_(owner),
      recv_buf_(std::make_unique<char[]>(kRecvBufSize)) {
    LOGINFO("UringConnection created fd=" + std::to_string(fd_) +
            " ip=" + ip_ + " port=" + std::to_string(port_));
}

UringConnection::~UringConnection() {
    ClearSendFile();
    tls_session_.reset();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    LOGDEBUG("UringConnection析构");
}

size_t UringConnection::GetBufferedInputSize() const {
    std::lock_guard<std::mutex> lk(input_buffer_mutex_);
    return inputbuffer_.readableBytes();
}

std::string UringConnection::ConsumeBufferedInput() {
    std::lock_guard<std::mutex> lk(input_buffer_mutex_);
    std::string data = inputbuffer_.bufferToString();
    inputbuffer_.consumeBytes(data.size());
    return data;
}

void UringConnection::AppendInputBytes(const char* data, size_t size) {
    std::lock_guard<std::mutex> lk(input_buffer_mutex_);
    inputbuffer_.append(data, size);
}

bool UringConnection::TryResumeRecvAwaiterIfReady() {
    if (!IsDisconnected() && GetBufferedInputSize() == 0) {
        return false;
    }

    std::optional<ResumeEntry> entry;
    {
        std::lock_guard<std::mutex> lk(resume_mutex_);
        if (!recv_resume_.has_value()) {
            return false;
        }
        entry = std::move(recv_resume_);
        recv_resume_.reset();
    }

    entry->executor->post([h = entry->handle]() { h.resume(); });
    return true;
}

uint64_t UringConnection::ReserveWriteAwaitId() {
    std::lock_guard<std::mutex> lk(resume_mutex_);
    return next_write_await_id_++;
}

void UringConnection::MarkWriteAwaitStarted(uint64_t await_id) {
    std::lock_guard<std::mutex> lk(resume_mutex_);
    active_write_await_id_ = await_id;
}

void UringConnection::FinishWriteChain() {
    std::optional<ResumeEntry> entry;
    bool notify_send_complete = false;
    {
        std::lock_guard<std::mutex> lk(resume_mutex_);
        if (active_write_await_id_ != 0) {
            completed_write_await_id_ = std::max(completed_write_await_id_, active_write_await_id_);
            active_write_await_id_ = 0;
        }
        if (write_resume_.has_value()) {
            entry = std::move(write_resume_);
            write_resume_.reset();
        } else {
            notify_send_complete = true;
        }
    }

    if (entry.has_value()) {
        entry->executor->post([h = entry->handle]() { h.resume(); });
    } else if (notify_send_complete) {
        owner_->NotifySendComplete(shared_from_this());
    }

    if (close_on_send_complete_ && !disconnected_.load()) {
        DoClose();
    }
}

void UringConnection::send() {
    if (disconnected_.load()) return;

    if (outputbuffer_.readableBytes() == 0 && !sendfile_.active) return;

    if (IsTlsActive() && tls_session_->HandshakeDone() && !tls_session_->KtlsTx()) {
        if (tls_poll_submitted_) return;
        SubmitTlsPoll(POLLIN | POLLOUT);
        return;
    }

    if (write_submitted_) return;

    write_submitted_ = true;
    owner_->SubmitWrite(SharedFromThis());
}

void UringConnection::setCloseOnSendComplete(bool close) {
    close_on_send_complete_ = close;
}

void UringConnection::StartSendFile(int file_fd, off_t offset, size_t length, bool auto_close) {
    ClearSendFile();
    sendfile_.file_fd = file_fd;
    sendfile_.offset = offset;
    sendfile_.remaining = length;
    sendfile_.auto_close = auto_close;
    sendfile_.active = (file_fd >= 0);

    if (sendfile_.active) {
        if (::pipe2(sendfile_.pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
            LOGERROR("UringConnection pipe2 failed: " + std::string(std::strerror(errno)));
            sendfile_.active = false;
            return;
        }
        sendfile_.pipe_open = true;

        int pipe_sz = ::fcntl(sendfile_.pipe_fds[0], F_SETPIPE_SZ, 1048576);
        if (pipe_sz > 0) {
            LOGDEBUG("UringConnection pipe capacity set to " + std::to_string(pipe_sz));
        }
    }

    LOGINFO("UringConnection StartSendFile fd=" + std::to_string(fd_) +
            " file_fd=" + std::to_string(file_fd) +
            " offset=" + std::to_string(offset) +
            " length=" + std::to_string(length) +
            " pipe=" + std::to_string(sendfile_.pipe_open));
}

void UringConnection::ClearSendFile() {
    sendfile_.ClosePipe();
    if (sendfile_.file_fd >= 0 && sendfile_.auto_close) {
        ::close(sendfile_.file_fd);
    }
    sendfile_ = SendFileState{};
}

void UringConnection::PostIoTask(std::function<void()> task) {
    if (owner_) {
        owner_->QueueTask(std::move(task));
    }
}

void UringConnection::SetTlsContext(std::shared_ptr<TlsContext> ctx) {
    tls_ctx_ = std::move(ctx);
    if (tls_ctx_ && fd_ >= 0) {
        tls_session_.reset(new TlsSession(tls_ctx_, fd_));
        tls_poll_state_ = TlsPollState::HANDSHAKE;
        LOGINFO("UringConnection TLS enabled fd=" + std::to_string(fd_));
    }
}

void UringConnection::SubmitRecv() {
    if (disconnected_.load()) return;
    if (recv_submitted_) return;

    recv_submitted_ = true;

    if (IsTlsActive()) {
        SubmitTlsPoll(POLLIN | POLLOUT);
    } else {
        owner_->SubmitRead(SharedFromThis());
    }
}

void UringConnection::HandleRecvComplete(int result) {
    recv_submitted_ = false;

    if (disconnected_.load()) return;

    if (result <= 0) {
        if (result == 0) {
            LOGINFO("UringConnection peer closed fd=" + std::to_string(fd_));
            DoClose();
            return;
        }
        int err = -result;
        // ENOTSOCK (88): fd 已经关闭或不是 socket，直接清理连接
        if (err == ENOTSOCK || err == EBADF) {
            LOGINFO("UringConnection recv on invalid fd=" + std::to_string(fd_) +
                    " err=" + std::to_string(err));
            DoClose();
            return;
        }
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            SubmitRecv();
            return;
        }
        LOGERROR("UringConnection recv error fd=" + std::to_string(fd_) +
                 " err=" + std::to_string(err) + " " + std::strerror(err));
        DoClose();
        return;
    }

    AppendInputBytes(recv_buf_.get(), static_cast<size_t>(result));
    LOGDEBUG("UringConnection recv fd=" + std::to_string(fd_) +
             " bytes=" + std::to_string(result));

    owner_->RefreshConnTimer(SharedFromThis());

    // 协程路径：直接恢复挂起协程，不走路由消息回调
    if (!TryResumeRecvAwaiterIfReady()) {
        owner_->NotifyMessage(shared_from_this());
    }

    SubmitRecv();
}

void UringConnection::HandleWriteComplete(int result) {
    write_submitted_ = false;

    if (disconnected_.load()) return;

    if (result < 0) {
        int err = -result;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            write_submitted_ = true;
            owner_->SubmitWrite(SharedFromThis());
            return;
        }
        LOGERROR("UringConnection write error fd=" + std::to_string(fd_) +
                 " err=" + std::to_string(err) + " " + std::strerror(err));
        DoClose();
        return;
    }

    outputbuffer_.consumeBytes(static_cast<size_t>(result));
    LOGDEBUG("UringConnection write complete fd=" + std::to_string(fd_) +
             " bytes=" + std::to_string(result));

    // 按回调路径驱动尾部逻辑：
    // 1) 如果 outputbuffer_ 还有未发数据 → 续写
    // 2) 否则如果 sendfile_ 活跃 → 启动 splice 链（协程由 HandleSplicePipe2Sock 恢复）
    // 3) 否则写真正完成 → 恢复协程或通知回调
    if (outputbuffer_.readableBytes() > 0) {
        // 仍有未发数据，续写；协程保持挂起等待整条写链完成
        write_submitted_ = true;
        owner_->SubmitWrite(SharedFromThis());
        return;
    }

    if (sendfile_.active) {
        // 启动文件体 splice 链；若 file body 已经为空，SubmitSpliceChain 会直接走完成态
        SubmitSpliceChain();
        return;
    }

    // outputbuffer_ 为空且无 sendfile：写真正完成
    FinishWriteChain();
}

void UringConnection::SubmitSpliceChain() {
    if (disconnected_.load()) return;
    if (!sendfile_.active || !sendfile_.pipe_open) return;
    if (sendfile_.remaining == 0) {
        CompleteSendFile();
        FinishWriteChain();
        return;
    }

    owner_->SubmitSendFile(SharedFromThis());
}

void UringConnection::HandleSpliceFile2Pipe(size_t nspliced) {
    if (disconnected_.load()) return;
    if (!sendfile_.active) return;

    // 由于使用 IOSQE_IO_LINK，第一个 splice 的结果会在第二个 splice 之前到达
    // 如果第一个 splice 失败（nspliced == 0 或错误），第二个 splice 会被取消
    // 这里我们只记录日志，实际错误处理在 HandleSplicePipe2Sock 中进行
    if (nspliced == 0) {
        LOGINFO("UringConnection splice file2pipe reached EOF fd=" + std::to_string(fd_));
        return;
    }

    LOGDEBUG("UringConnection splice file2pipe fd=" + std::to_string(fd_) +
             " bytes=" + std::to_string(nspliced));
}

void UringConnection::HandleSplicePipe2Sock(ssize_t nsent) {
    if (disconnected_.load()) return;
    if (!sendfile_.active) return;

    if (nsent < 0) {
        int err = static_cast<int>(-nsent);
        // ECANCELED (125): 由于 IOSQE_IO_LINK，前一个操作失败导致此操作被取消
        if (err == ECANCELED) {
            LOGINFO("UringConnection splice canceled (linked op failed) fd=" + std::to_string(fd_));
            CompleteSendFile();
            FinishWriteChain();
            return;
        }
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            SubmitSpliceChain();
            return;
        }
        LOGERROR("UringConnection splice error fd=" + std::to_string(fd_) +
                 " err=" + std::to_string(err) + " " + std::strerror(err));
        DoClose();
        return;
    }

    if (nsent == 0) {
        CompleteSendFile();
        FinishWriteChain();
        return;
    }

    AdvanceSendFile(static_cast<size_t>(nsent));
    LOGINFO("UringConnection splice sent fd=" + std::to_string(fd_) +
            " bytes=" + std::to_string(nsent) +
            " remaining=" + std::to_string(sendfile_.remaining));

    if (sendfile_.remaining > 0) {
        SubmitSpliceChain();
    } else {
        CompleteSendFile();
        FinishWriteChain();
    }
}

void UringConnection::SubmitTlsPoll(short poll_mask) {
    if (disconnected_.load()) return;
    if (tls_poll_submitted_) return;

    tls_poll_submitted_ = true;
    tls_poll_mask_ = poll_mask;
    owner_->SubmitPollAdd(SharedFromThis(), poll_mask);
}

void UringConnection::HandleTlsPollCQE(short revents) {
    tls_poll_submitted_ = false;

    if (disconnected_.load()) return;

    if (!tls_session_) {
        DoClose();
        return;
    }

    bool did_work = true;
    short next_mask = 0;

    while (did_work) {
        did_work = false;

        if (!tls_session_->HandshakeDone()) {
            TlsIoResult hr = tls_session_->DriveHandshake();
            if (hr == TlsIoResult::OK) {
                tls_poll_state_ = TlsPollState::ACTIVE;
                did_work = true;
                LOGINFO("UringConnection TLS handshake done fd=" + std::to_string(fd_));
                continue;
            }
            if (hr == TlsIoResult::WANT_READ) {
                next_mask |= POLLIN;
                break;
            }
            if (hr == TlsIoResult::WANT_WRITE) {
                next_mask |= POLLOUT;
                break;
            }
            LOGERROR("UringConnection TLS handshake failed fd=" + std::to_string(fd_));
            DoClose();
            return;
        }

        if (revents & POLLIN) {
            revents &= ~POLLIN;

            if (tls_session_->KtlsRx()) {
                ssize_t n = ::read(fd_, recv_buf_.get(), kRecvBufSize);
                if (n > 0) {
                    AppendInputBytes(recv_buf_.get(), static_cast<size_t>(n));
                    did_work = true;
                    owner_->RefreshConnTimer(SharedFromThis());
                    if (!TryResumeRecvAwaiterIfReady()) {
                        owner_->NotifyMessage(shared_from_this());
                    }
                } else if (n == 0) {
                    DoClose();
                    return;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOGERROR("UringConnection ktls read error fd=" + std::to_string(fd_) +
                             " err=" + std::strerror(errno));
                    DoClose();
                    return;
                }
            } else {
                while (true) {
                    size_t nread = 0;
                    TlsIoResult rr = tls_session_->ReadPlain(recv_buf_.get(), kRecvBufSize, nread);
                    if (rr == TlsIoResult::OK && nread > 0) {
                        AppendInputBytes(recv_buf_.get(), nread);
                        did_work = true;
                        owner_->RefreshConnTimer(SharedFromThis());
                        if (!TryResumeRecvAwaiterIfReady()) {
                            owner_->NotifyMessage(shared_from_this());
                        }
                        continue;
                    }
                    if (rr == TlsIoResult::CLOSED) {
                        DoClose();
                        return;
                    }
                    if (rr == TlsIoResult::WANT_READ) {
                        next_mask |= POLLIN;
                        break;
                    }
                    if (rr == TlsIoResult::WANT_WRITE) {
                        next_mask |= POLLOUT;
                        break;
                    }
                    LOGERROR("UringConnection TLS read error fd=" + std::to_string(fd_));
                    DoClose();
                    return;
                }
            }
        }

        if (revents & POLLOUT) {
            revents &= ~POLLOUT;

            if (tls_session_->KtlsTx()) {
                break;
            }

            const size_t kMaxTlsWrites = 4;
            size_t write_count = 0;

            while (true) {
                if (!tls_out_pending_.empty()) {
                    size_t nwritten = 0;
                    TlsIoResult wr = tls_session_->WritePlain(
                        tls_out_pending_.data(), tls_out_pending_.size(), nwritten);
                    if (wr == TlsIoResult::OK && nwritten > 0) {
                        tls_out_pending_.erase(0, nwritten);
                        did_work = true;
                        write_count++;
                        continue;
                    }
                    if (wr == TlsIoResult::WANT_WRITE) {
                        next_mask |= POLLOUT;
                        break;
                    }
                    if (wr == TlsIoResult::WANT_READ) {
                        next_mask |= POLLIN;
                        break;
                    }
                    if (wr == TlsIoResult::CLOSED) {
                        DoClose();
                        return;
                    }
                    LOGERROR("UringConnection TLS write error fd=" + std::to_string(fd_));
                    DoClose();
                    return;
                }

                if (outputbuffer_.readableBytes() > 0) {
                    if (write_count >= kMaxTlsWrites) {
                        next_mask |= POLLOUT;
                        break;
                    }
                    size_t take = std::min<size_t>(16384, outputbuffer_.readableBytes());
                    tls_out_pending_.assign(take, '\0');
                    outputbuffer_.peekFromBlock(&tls_out_pending_[0], take);
                    outputbuffer_.consumeBytes(take);
                    continue;
                }

                if (sendfile_.active) {
                    if (sendfile_.remaining == 0) {
                        CompleteSendFile();
                        did_work = true;
                        continue;
                    }
                    if (write_count >= kMaxTlsWrites) {
                        next_mask |= POLLOUT;
                        break;
                    }
                    const size_t take = std::min<size_t>(16384, sendfile_.remaining);
                    std::string file_chunk(take, '\0');
                    ssize_t nread = 0;
                    while (true) {
                        nread = ::pread(sendfile_.file_fd, file_chunk.data(), take, sendfile_.offset);
                        if (nread >= 0 || errno != EINTR) {
                            break;
                        }
                    }
                    if (nread > 0) {
                        outputbuffer_.append(file_chunk.data(), static_cast<size_t>(nread));
                        AdvanceSendFile(static_cast<size_t>(nread));
                        did_work = true;
                        continue;
                    }
                    if (nread == 0) {
                        CompleteSendFile();
                        did_work = true;
                        continue;
                    }
                    LOGERROR("UringConnection TLS sendfile pread error fd=" +
                             std::to_string(fd_) + " err=" + std::strerror(errno));
                    DoClose();
                    return;
                }

                break;
            }

            if (tls_out_pending_.empty() && outputbuffer_.readableBytes() == 0) {
                FinishWriteChain();
                if (disconnected_.load()) {
                    return;
                }
            }
        }
    }

    // 合并重挂分支：
    // 1) 处理过程中产生的 WANT_READ/WANT_WRITE 需求（next_mask）
    // 2) 空闲连接的常态化重挂
    // 关键：POLLOUT 只在确有待写数据时挂载。one-shot POLL_ADD 在 socket 可写时立即完成，
    // 若空闲连接无条件挂 POLLOUT，会陷入"完成→无事可做→重挂"的 100% CPU 忙轮询（审计 H14）。
    // kTLS 发送走普通 writev 路径，无需（也不能）用 TLS poll 的 POLLOUT。
    if (tls_poll_state_ == TlsPollState::ACTIVE && tls_session_->HandshakeDone()) {
        if (!tls_session_->KtlsRx() && !(next_mask & POLLIN)) {
            next_mask |= POLLIN;
        }
        if (!tls_session_->KtlsTx() &&
            (!tls_out_pending_.empty() || outputbuffer_.readableBytes() > 0 || sendfile_.active)) {
            next_mask |= POLLOUT;
        }
    }

    if (next_mask) {
        SubmitTlsPoll(next_mask);
    }
}

void UringConnection::DoClose() {
    if (disconnected_.exchange(true)) return;

    // 协程化：清理所有挂起的协程恢复句柄
    ClearAllResumeHandles();

    const int closing_fd = fd_;
    LOGINFO("UringConnection closing fd=" + std::to_string(closing_fd));
    auto self = SharedFromThis();
    if (closing_fd >= 0) {
        ::shutdown(closing_fd, SHUT_RDWR);
        ::close(closing_fd);
    }
    // 注意：fd_ 必须等 NotifyClose / RemoveConnection 之后才置 -1。
    // 若提前置 -1，UringServer::HandleCloseConnection 会用 conn->fd() == -1
    // 执行 conns_.erase(-1)，真实 fd 条目永不删除 → 连接对象泄漏（审计 H12）。
    if (owner_) {
        owner_->NotifyClose(self);
        owner_->RemoveConnection(self);
    }
    fd_ = -1;
}

// ===== 协程化 Awaitable 实现 =====

void UringConnection::RegisterRecvResumeHandle(
    std::coroutine_handle<> h,
    std::shared_ptr<concurrencpp::thread_pool_executor> exec) {
    std::lock_guard<std::mutex> lk(resume_mutex_);
    recv_resume_ = ResumeEntry{h, std::move(exec)};
}

void UringConnection::RegisterWriteResumeHandle(
    std::coroutine_handle<> h,
    std::shared_ptr<concurrencpp::thread_pool_executor> exec,
    uint64_t await_id) {
    std::optional<ResumeEntry> ready_entry;
    {
        std::lock_guard<std::mutex> lk(resume_mutex_);
        if (completed_write_await_id_ >= await_id) {
            ready_entry = ResumeEntry{h, std::move(exec)};
        } else {
            write_resume_ = ResumeEntry{h, std::move(exec)};
        }
    }
    if (ready_entry.has_value()) {
        ready_entry->executor->post([resume = ready_entry->handle]() { resume.resume(); });
    }
}

void UringConnection::ClearAllResumeHandles() {
    std::lock_guard<std::mutex> lk(resume_mutex_);
    if (recv_resume_.has_value()) {
        auto entry = std::move(recv_resume_);
        recv_resume_.reset();
        entry->executor->post([h = entry->handle]() { h.resume(); });
    }
    if (write_resume_.has_value()) {
        auto entry = std::move(write_resume_);
        write_resume_.reset();
        entry->executor->post([h = entry->handle]() { h.resume(); });
    }
}

bool UringConnection::RecvReadyAwaiter::await_ready() const noexcept {
    return conn->IsDisconnected() || conn->GetBufferedInputSize() > 0;
}

void UringConnection::RecvReadyAwaiter::await_suspend(std::coroutine_handle<> h) {
    conn->RegisterRecvResumeHandle(h, executor);
    conn->TryResumeRecvAwaiterIfReady();
}

size_t UringConnection::RecvReadyAwaiter::await_resume() noexcept {
    return conn->IsDisconnected() ? 0 : conn->GetBufferedInputSize();
}

UringConnection::RecvReadyAwaiter UringConnection::AsyncRecvReady() {
    return RecvReadyAwaiter{this, owner_->GetCoroutineExecutor()};
}

bool UringConnection::WriteResponseAwaiter::await_ready() const noexcept {
    return false;
}

void UringConnection::WriteResponseAwaiter::await_suspend(std::coroutine_handle<> h) {
    const uint64_t await_id = conn->ReserveWriteAwaitId();
    // 先投递 IO 任务再注册 handle，避免"注册后旧 CQE 已处理完"的竞态挂死
    conn->PostIoTask([conn = this->conn, data = std::move(data),
                      close_after = this->close_after,
                      await_id]() {
        if (conn->IsDisconnected()) return;
        conn->MarkWriteAwaitStarted(await_id);
        BufferBlock& out = conn->getOutputBuffer();
        out.append(data.c_str(), data.size());
        if (close_after) conn->setCloseOnSendComplete(true);
        conn->send();
        if (!conn->IsDisconnected() &&
            !conn->IsWriteSubmitted() &&
            conn->getOutputBuffer().readableBytes() == 0 &&
            !conn->HasSendFile()) {
            conn->FinishWriteChain();
        }
    });
    conn->RegisterWriteResumeHandle(h, executor, await_id);
}

bool UringConnection::WriteResponseAwaiter::await_resume() noexcept {
    return success_;
}

UringConnection::WriteResponseAwaiter UringConnection::AsyncWriteResponse(std::string data, bool close_after) {
    return WriteResponseAwaiter{this, std::move(data), close_after,
                                owner_->GetCoroutineExecutor()};
}

bool UringConnection::SendFileAwaiter::await_ready() const noexcept {
    return false;
}

void UringConnection::SendFileAwaiter::await_suspend(std::coroutine_handle<> h) {
    const uint64_t await_id = conn->ReserveWriteAwaitId();
    // 先投递 IO 任务再注册 handle，避免"注册后旧 CQE 已处理完"的竞态挂死
    conn->PostIoTask([conn = this->conn, data = std::move(header_data),
                      file_fd = this->file_fd, offset = this->offset,
                      length = this->length,
                      close_after = this->close_after,
                      await_id]() {
        if (conn->IsDisconnected()) return;
        conn->MarkWriteAwaitStarted(await_id);
        BufferBlock& out = conn->getOutputBuffer();
        out.append(data.c_str(), data.size());
        conn->StartSendFile(file_fd, offset, length, true);
        if (close_after) conn->setCloseOnSendComplete(true);
        conn->send();
        if (!conn->IsDisconnected() &&
            !conn->IsWriteSubmitted() &&
            conn->getOutputBuffer().readableBytes() == 0 &&
            !conn->HasSendFile()) {
            conn->FinishWriteChain();
        }
    });
    conn->RegisterWriteResumeHandle(h, executor, await_id);
}

bool UringConnection::SendFileAwaiter::await_resume() noexcept {
    return success_;
}

UringConnection::SendFileAwaiter UringConnection::AsyncSendFile(
    std::string header_data,
    int file_fd,
    off_t offset,
    size_t length,
    bool close_after) {
    return SendFileAwaiter{this, std::move(header_data), file_fd, offset, length,
                           close_after, owner_->GetCoroutineExecutor()};
}
