#pragma once

#include <memory>
#include <string>

#include <openssl/ssl.h>

class TlsContext {
public:
  static std::shared_ptr<TlsContext> CreateFromEnv();

  SSL_CTX* Get() const { return ctx_.get(); }
  bool Strict() const { return strict_; }

private:
  struct CtxDeleter {
    void operator()(SSL_CTX* p) const noexcept { SSL_CTX_free(p); }
  };

  explicit TlsContext(SSL_CTX* ctx, bool strict);

  std::unique_ptr<SSL_CTX, CtxDeleter> ctx_;
  bool strict_{false};
};

