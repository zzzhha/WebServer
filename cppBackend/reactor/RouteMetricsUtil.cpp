#include "RouteMetricsUtil.h"

namespace {
bool StartsWith(const std::string& s, const char* pfx) {
  return s.rfind(pfx, 0) == 0;
}
}  // namespace

bool IsDownloadRoute(const std::string& path) {
  return StartsWith(path, "/download/");
}

std::string ClassifyRouteBucket(const std::string& path) {
  if (path.empty() || path == "/") return "page";

  if (path == "/login" || path == "/register" || path == "/refresh-token") {
    return "auth";
  }
  if (StartsWith(path, "/api/")) return "api";
  if (IsDownloadRoute(path)) return "download";
  if (StartsWith(path, "/assets/") || StartsWith(path, "/images/") ||
      StartsWith(path, "/video/") || StartsWith(path, "/uploads/")) {
    return "static";
  }
  if (path == "/index.html" || path == "/welcome.html" || path == "/login.html" ||
      path == "/register.html" || path == "/picture.html" || path == "/video.html") {
    return "page";
  }
  if (StartsWith(path, "/favicon")) return "static";
  return "other";
}
