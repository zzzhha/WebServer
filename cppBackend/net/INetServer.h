#pragma once

#include <functional>
#include <memory>

#include "IConnection.h"

class INetServer {
public:
    using ConnCallback = std::function<void(spIConnection)>;

    virtual ~INetServer() = default;

    // Lifecycle.
    virtual void start() = 0;
    virtual void Stop() = 0;

    // Callback registration.
    virtual void setnewconnectioncb(ConnCallback cb) = 0;
    virtual void setcloseconnectioncb(ConnCallback cb) = 0;
    virtual void seterrorconnectioncb(ConnCallback cb) = 0;
    virtual void setonmessagecb(ConnCallback cb) = 0;
    virtual void setsendcompletecb(ConnCallback cb) = 0;
};
