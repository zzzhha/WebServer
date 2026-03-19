#pragma once

#include <memory>
#include <string>

#include <openssl/ssl.h>

class TlsContext;

enum class TlsIoResult {
  OK = 0,
  WANT_READ = 1,
  WANT_WRITE = 2,
  CLOSED = 3,
  ERROR = 4
};

class TlsSession {
public:
  TlsSession(std::shared_ptr<TlsContext> ctx, int fd);
  ~TlsSession();

  bool HandshakeDone() const { return handshake_done_; }
  bool KtlsTx() const { return ktls_tx_; }
  bool KtlsRx() const { return ktls_rx_; }

  TlsIoResult DriveHandshake();
  TlsIoResult ReadPlain(char* out, size_t cap, size_t& nread);
  TlsIoResult WritePlain(const char* data, size_t len, size_t& nwritten);

private:
  std::shared_ptr<TlsContext> ctx_;
  int fd_{-1};
  SSL* ssl_{nullptr};
  bool handshake_done_{false};
  bool ktls_tx_{false};
  bool ktls_rx_{false};

  void RefreshKtlsState();
};

