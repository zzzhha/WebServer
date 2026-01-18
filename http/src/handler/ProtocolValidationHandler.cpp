#include "handler/ProtocolValidationHandler.h"

#include "core/HttpRequest.h"
#include "core/IHttpMessage.h"

bool ProtocolValidationHandler::Handle(IHttpMessage& message) {
  auto contentLength = message.GetHeader("Content-Length");
  auto transferEncoding = message.GetHeader("Transfer-Encoding");

  // Content-Length 与 Transfer-Encoding 不应同时出现
  if (contentLength && transferEncoding) {
    return false;
  }

  // Content-Length 必须为合法数字，且与已累积的 body 长度一致
  if (contentLength) {
    size_t len = 0;
    try {
      len = std::stoull(*contentLength);
    } catch (...) {
      return false;
    }
    if (message.GetBodyLength() > 0 && message.GetBodyLength() != len) {
      return false;
    }
  }

  // HTTP/1.1 请求必须携带 Host，方法不可未知，路径不能为空
  if (auto* request = dynamic_cast<HttpRequest*>(&message)) {
    if (request->GetVersion() == HttpVersion::HTTP_1_1 &&
        !request->HasHeader("Host")) {
      return false;
    }
    if (request->GetMethod() == HttpMethod::UNKNOWN) {
      return false;
    }
    if (request->GetPath().empty()) {
      return false;
    }
  }

  return CallNext(message);
}

