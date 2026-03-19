#include "TlsSession.h"

#include "TlsContext.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "../logger/log_fac.h"

#include <openssl/bio.h>
#include <openssl/err.h>

TlsSession::TlsSession(std::shared_ptr<TlsContext> ctx, int fd) : ctx_(std::move(ctx)), fd_(fd) {
  if (!ctx_ || !ctx_->Get() || fd_ < 0) return;

  ssl_ = SSL_new(ctx_->Get());
  if (!ssl_) return;

  SSL_set_accept_state(ssl_);
  SSL_set_fd(ssl_, fd_);

#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
  SSL_set_options(ssl_, SSL_OP_ENABLE_KTLS);
#endif
}

TlsSession::~TlsSession() {
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
}

void TlsSession::RefreshKtlsState() {
#if !defined(OPENSSL_NO_KTLS)
  if (!ssl_) return;
  BIO* rb = SSL_get_rbio(ssl_);
  BIO* wb = SSL_get_wbio(ssl_);
  if (rb) {
    ktls_rx_ = (BIO_get_ktls_recv(rb) == 1);
  }
  if (wb) {
    ktls_tx_ = (BIO_get_ktls_send(wb) == 1);
  }
#endif
}

TlsIoResult TlsSession::DriveHandshake() {
  if (!ssl_) return TlsIoResult::ERROR;
  if (handshake_done_) return TlsIoResult::OK;

  int rc = SSL_accept(ssl_);
  if (rc == 1) {
    handshake_done_ = true;
    RefreshKtlsState();
    return TlsIoResult::OK;
  }

  int err = SSL_get_error(ssl_, rc);
  if (err == SSL_ERROR_WANT_READ) return TlsIoResult::WANT_READ;
  if (err == SSL_ERROR_WANT_WRITE) return TlsIoResult::WANT_WRITE;

  unsigned long e = ERR_get_error();
  if (e != 0) {
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    LOGERROR("SSL_accept failed: " + std::string(buf));
  } else {
    LOGERROR("SSL_accept failed");
  }
  return TlsIoResult::ERROR;
}

TlsIoResult TlsSession::ReadPlain(char* out, size_t cap, size_t& nread) {
  nread = 0;
  if (!ssl_ || !handshake_done_) return TlsIoResult::ERROR;

  if (ktls_rx_) {
    ssize_t n = ::read(fd_, out, cap);
    if (n > 0) {
      nread = static_cast<size_t>(n);
      return TlsIoResult::OK;
    }
    if (n == 0) return TlsIoResult::CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return TlsIoResult::WANT_READ;
    return TlsIoResult::ERROR;
  }

  int n = SSL_read(ssl_, out, static_cast<int>(cap));
  if (n > 0) {
    nread = static_cast<size_t>(n);
    return TlsIoResult::OK;
  }
  int err = SSL_get_error(ssl_, n);
  if (err == SSL_ERROR_ZERO_RETURN) return TlsIoResult::CLOSED;
  if (err == SSL_ERROR_WANT_READ) return TlsIoResult::WANT_READ;
  if (err == SSL_ERROR_WANT_WRITE) return TlsIoResult::WANT_WRITE;
  return TlsIoResult::ERROR;
}

TlsIoResult TlsSession::WritePlain(const char* data, size_t len, size_t& nwritten) {
  nwritten = 0;
  if (!ssl_ || !handshake_done_) return TlsIoResult::ERROR;
  if (ktls_tx_) return TlsIoResult::ERROR;

  int n = SSL_write(ssl_, data, static_cast<int>(len));
  if (n > 0) {
    nwritten = static_cast<size_t>(n);
    return TlsIoResult::OK;
  }
  int err = SSL_get_error(ssl_, n);
  if (err == SSL_ERROR_ZERO_RETURN) return TlsIoResult::CLOSED;
  if (err == SSL_ERROR_WANT_READ) return TlsIoResult::WANT_READ;
  if (err == SSL_ERROR_WANT_WRITE) return TlsIoResult::WANT_WRITE;
  return TlsIoResult::ERROR;
}
