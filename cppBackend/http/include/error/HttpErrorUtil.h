#pragma once

#include <string>
#include <string_view>

#include "error/HttpError.h"

std::string ToString(HttpErrc code);
std::string ToString(HttpErrorStage stage);

std::string JsonEscape(std::string_view s);

std::string CaptureStackTrace(size_t max_frames = 64);

std::string BuildHttpErrorJson(const HttpError& err,
                               std::string_view request_id,
                               bool include_context);

