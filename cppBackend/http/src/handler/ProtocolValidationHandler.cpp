#include "handler/ProtocolValidationHandler.h"

#include "core/HttpRequest.h"
#include "core/IHttpMessage.h"
#include "error/HttpError.h"

bool ProtocolValidationHandler::Handle(IHttpMessage& message, HttpError& error) {
  auto contentLength = message.GetHeader("Content-Length");
  auto transferEncoding = message.GetHeader("Transfer-Encoding");

  // Content-Length 与 Transfer-Encoding 不应同时出现
  if (contentLength && transferEncoding) {
    error.code = HttpErrc::VALIDATION_CONFLICTING_LENGTH;
    error.status = HttpStatusCode::BAD_REQUEST;
    error.message = "Bad Request";
    error.ctx.stage = HttpErrorStage::VALIDATION;
    error.ctx.header_key = "Content-Length/Transfer-Encoding";
    error.ctx.detail = "conflicting length headers";
    return false;
  }

  // Content-Length 必须为合法数字，且与已累积的 body 长度一致
  if (contentLength) {
    size_t len = 0;
    try {
      len = std::stoull(*contentLength);
    } catch (...) {
      error.code = HttpErrc::VALIDATION_CONTENT_LENGTH_INVALID;
      error.status = HttpStatusCode::BAD_REQUEST;
      error.message = "Bad Request";
      error.ctx.stage = HttpErrorStage::VALIDATION;
      error.ctx.header_key = "Content-Length";
      error.ctx.detail = "invalid Content-Length";
      return false;
    }
    if (message.GetBodyLength() > 0 && message.GetBodyLength() != len) {
      error.code = HttpErrc::VALIDATION_CONTENT_LENGTH_MISMATCH;
      error.status = HttpStatusCode::BAD_REQUEST;
      error.message = "Bad Request";
      error.ctx.stage = HttpErrorStage::VALIDATION;
      error.ctx.header_key = "Content-Length";
      error.ctx.detail = "body length mismatch";
      return false;
    }
  }

  // HTTP/1.1 请求必须携带 Host，方法不可未知，路径不能为空
  if (auto* request = dynamic_cast<HttpRequest*>(&message)) {
    if (request->GetVersion() == HttpVersion::HTTP_1_1 &&
        !request->HasHeader("Host")) {
      error.code = HttpErrc::VALIDATION_MISSING_HOST;
      error.status = HttpStatusCode::BAD_REQUEST;
      error.message = "Bad Request";
      error.ctx.stage = HttpErrorStage::VALIDATION;
      error.ctx.header_key = "Host";
      error.ctx.detail = "missing Host for HTTP/1.1";
      return false;
    }
    if (request->GetMethod() == HttpMethod::UNKNOWN) {
      error.code = HttpErrc::VALIDATION_UNKNOWN_METHOD;
      error.status = HttpStatusCode::NOT_IMPLEMENTED;
      error.message = "Not Implemented";
      error.ctx.stage = HttpErrorStage::VALIDATION;
      error.ctx.method = request->GetMethodString();
      return false;
    }
  }

  return CallNext(message, error);
}
