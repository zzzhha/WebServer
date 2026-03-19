#include "TlsContext.h"

#include <cstdlib>
#include <string>

#include "../logger/log_fac.h"

#include <openssl/err.h>

TlsContext::TlsContext(SSL_CTX* ctx, bool strict) : ctx_(ctx), strict_(strict) {}

static bool EnvIsOn(const char* name) {
  const char* v = std::getenv(name);
  if (!v) return false;
  return std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "TRUE";
}

std::shared_ptr<TlsContext> TlsContext::CreateFromEnv() {
  const char* cert = std::getenv("WEBSERVER_TLS_CERT");
  const char* key = std::getenv("WEBSERVER_TLS_KEY");
  if (!cert || !key || std::string(cert).empty() || std::string(key).empty()) {
    return nullptr;
  }

  OPENSSL_init_ssl(0, nullptr);

  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    LOGERROR("SSL_CTX_new failed");
    return nullptr;
  }

  SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

  if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) != 1) {
    LOGERROR("SSL_CTX_set_*_proto_version failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }

  const char* cipher_list =
      "ECDHE-ECDSA-AES128-GCM-SHA256:"
      "ECDHE-RSA-AES128-GCM-SHA256:"
      "ECDHE-ECDSA-AES256-GCM-SHA384:"
      "ECDHE-RSA-AES256-GCM-SHA384";
  if (SSL_CTX_set_cipher_list(ctx, cipher_list) != 1) {
    LOGERROR("SSL_CTX_set_cipher_list failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }

  if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
    LOGERROR("SSL_CTX_use_certificate_file failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
    LOGERROR("SSL_CTX_use_PrivateKey_file failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    LOGERROR("SSL_CTX_check_private_key failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }

  bool strict = EnvIsOn("WEBSERVER_TLS_STRICT");
  return std::shared_ptr<TlsContext>(new TlsContext(ctx, strict));
}

