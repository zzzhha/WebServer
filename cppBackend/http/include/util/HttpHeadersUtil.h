#pragma once

#include <string>
#include <string_view>

#include "core/HttpRequest.h"
#include "core/HttpResponse.h"

inline void ApplyCommonResponseHeaders(HttpResponse& resp, std::string_view request_id) {
  if (!request_id.empty() && !resp.HasHeader("X-Request-Id")) {
    resp.SetHeader("X-Request-Id", std::string(request_id));
  }
}

inline void ApplyCorsHeaders(HttpResponse& resp, const HttpRequest* req) {
  if (!req) return;
  auto origin = req->GetHeader("Origin");
  if (!origin) return;

  resp.SetHeader("Access-Control-Allow-Origin", *origin);
  resp.SetHeader("Vary", "Origin");
  resp.SetHeader("Access-Control-Allow-Credentials", "true");
  resp.SetHeader("Access-Control-Expose-Headers", "X-Request-Id");
  resp.SetHeader("Access-Control-Max-Age", "600");

  auto acrm = req->GetHeader("Access-Control-Request-Method");
  if (acrm && !acrm->empty()) {
    resp.SetHeader("Access-Control-Allow-Methods", *acrm);
  } else {
    resp.SetHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,PATCH,HEAD,OPTIONS");
  }

  auto acrh = req->GetHeader("Access-Control-Request-Headers");
  if (acrh && !acrh->empty()) {
    resp.SetHeader("Access-Control-Allow-Headers", *acrh);
  } else {
    resp.SetHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With, X-Request-Id");
  }
}

