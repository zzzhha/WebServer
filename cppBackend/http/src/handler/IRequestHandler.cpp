#include "handler/IRequestHandler.h"
#include "core/IHttpMessage.h"

IRequestHandler::~IRequestHandler() = default;

bool IRequestHandler::CallNext(IHttpMessage& message) {
  if (next_) {
    return next_->Handle(message);
  }
  return true;
}


