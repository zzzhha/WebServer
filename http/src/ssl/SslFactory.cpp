#include "ssl/SslFactory.h"
#include "ssl/SslHandler.h"

std::shared_ptr<ISslHandler> SslFactory::Create() {
  return std::make_shared<SslHandler>();
}

std::shared_ptr<ISslHandler> SslFactory::Create(std::shared_ptr<SslContext> context) {
  return std::make_shared<SslHandler>(context);
}

std::shared_ptr<ISslHandler> SslFactory::CreateServer() {
  auto context = std::make_shared<SslContext>();
  context->SetServerMode(true);
  context->SetPort(443);
  return std::make_shared<SslHandler>(context);
}

std::shared_ptr<ISslHandler> SslFactory::CreateClient() {
  auto context = std::make_shared<SslContext>();
  context->SetServerMode(false);
  return std::make_shared<SslHandler>(context);
}

std::shared_ptr<ISslHandler> SslFactory::CreateWithCert(
    const std::string& cert_file,
    const std::string& key_file,
    const std::string& ca_file) {
  auto context = std::make_shared<SslContext>();
  context->SetCertificateFile(cert_file);
  context->SetPrivateKeyFile(key_file);
  if (!ca_file.empty()) {
    context->SetCaFile(ca_file);
  }
  return std::make_shared<SslHandler>(context);
}

