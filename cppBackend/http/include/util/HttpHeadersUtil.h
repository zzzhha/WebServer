#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

#include "core/HttpRequest.h"
#include "core/HttpResponse.h"

inline void ApplyCommonResponseHeaders(HttpResponse& resp, std::string_view request_id) {
  if (!request_id.empty() && !resp.HasHeader("X-Request-Id")) {
    resp.SetHeader("X-Request-Id", std::string(request_id));
  }
}

// H11 修复：CORS Origin 白名单校验。
// 仅允许本机回环来源（http/https://localhost|127.0.0.1|[::1]，任意端口）
// 及环境变量 CORS_ALLOWED_ORIGINS（逗号分隔）显式配置的来源。
// 不再反射任意 Origin，杜绝恶意站点构造 Origin 获取带凭据的跨域读权限。
inline bool IsCorsOriginAllowed(const std::string& origin) {
  static const std::string_view kLoopbackPrefixes[] = {
      "http://localhost",  "https://localhost",
      "http://127.0.0.1",  "https://127.0.0.1",
      "http://[::1]",      "https://[::1]",
  };
  for (std::string_view prefix : kLoopbackPrefixes) {
    if (origin.rfind(prefix, 0) != 0) continue;
    std::string_view rest(origin.data() + prefix.size(), origin.size() - prefix.size());
    if (rest.empty()) return true;  // 无端口，直接放行
    if (rest[0] != ':' || rest.size() == 1) continue;  // 必须形如 ":<端口>"
    for (char c : rest.substr(1)) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        return false;  // 端口含非数字字符（如附加路径）→ 不是合法回环 Origin
      }
    }
    return true;
  }

  if (const char* env = std::getenv("CORS_ALLOWED_ORIGINS")) {
    std::string_view v(env);
    size_t pos = 0;
    while (pos <= v.size()) {
      size_t comma = v.find(',', pos);
      std::string_view item = v.substr(pos, comma == std::string_view::npos ? v.size() - pos : comma - pos);
      while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
      while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
      if (item == origin) return true;
      if (comma == std::string_view::npos) break;
      pos = comma + 1;
    }
  }
  return false;
}

inline void ApplyCorsHeaders(HttpResponse& resp, const HttpRequest* req) {
  if (!req) return;
  auto origin = req->GetHeader("Origin");
  if (!origin) return;

  resp.SetHeader("Vary", "Origin");
  // H11 修复：仅白名单内的 Origin 才反射并允许携带凭据
  if (!IsCorsOriginAllowed(*origin)) {
    return;
  }

  resp.SetHeader("Access-Control-Allow-Origin", *origin);
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

