#include "handler/IRequestHandler.h"
#include "core/IHttpMessage.h"
#include "error/HttpError.h"

IRequestHandler::~IRequestHandler() = default;

bool IRequestHandler::CallNext(IHttpMessage& message, HttpError& error) {
    if (next_) {
        return next_->Handle(message, error);
    }
    return true;
}

