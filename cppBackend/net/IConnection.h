#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

class BufferBlock;
class TlsContext;

class IConnection : public std::enable_shared_from_this<IConnection> {
public:
    virtual ~IConnection() = default;

    // Connection attributes.
    virtual int fd() const = 0;
    virtual std::string ip() const = 0;
    virtual uint16_t port() const = 0;
    virtual bool IsDisconnected() const = 0;

    // Buffer access.
    virtual BufferBlock& getInputBuffer() = 0;
    virtual BufferBlock& getOutputBuffer() = 0;

    // Send and close control.
    virtual void send() = 0;
    virtual void setCloseOnSendComplete(bool close) = 0;
    virtual void StartSendFile(int fd, off_t offset, size_t length, bool auto_close) = 0;
    virtual void ClearSendFile() = 0;

    // Schedule a task on this connection's IO thread.
    virtual void PostIoTask(std::function<void()> task) = 0;

    // Connection-level context storage.
    template <typename T>
    void SetContext(const T& ctx) {
        context_ = ctx;
    }

    template <typename T>
    T* GetContext() {
        return std::any_cast<T>(&context_);
    }

    // TLS related settings.
    virtual void SetTlsContext(std::shared_ptr<TlsContext> ctx) = 0;

    // 协程化支持：Proactor 连接返回自身的 UringConnection*，其他后端返回 nullptr
    virtual class UringConnection* AsUringConnection() { return nullptr; }
    virtual bool IsProactorMode() const { return false; }

protected:
    std::any context_;
};

using spIConnection = std::shared_ptr<IConnection>;
