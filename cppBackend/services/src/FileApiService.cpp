#include "FileApiService.h"

#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/error/HttpErrorUtil.h"
#include "../../http/include/handler/AppHandlers.h"
#include "../../logger/log_fac.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>

static bool IsSafeFileName(const std::string& v) {
  if (v.empty()) return false;
  if (v.find('/') != std::string::npos) return false;
  if (v.find('\\') != std::string::npos) return false;
  if (v.find("..") != std::string::npos) return false;
  if (v.find('\0') != std::string::npos) return false;
  return true;
}

static std::string ToIso8601Utc(time_t t) {
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

static std::string GetLowerExt(const std::string& name) {
  auto pos = name.find_last_of('.');
  if (pos == std::string::npos) return "";
  std::string ext = name.substr(pos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return ext;
}

static bool IsAllowedInFolder(const std::string& folder, const std::string& name) {
  const std::string ext = GetLowerExt(name);
  if (folder == "images") {
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "svg";
  }
  if (folder == "video") {
    return ext == "mp4" || ext == "webm" || ext == "avi";
  }
  if (folder == "uploads") {
    return ext == "pdf" || ext == "txt" || ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
           ext == "svg" || ext == "mp4" || ext == "webm" || ext == "avi";
  }
  return false;
}

bool FileApiService::HandleListFiles(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
  if (!request) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "Bad Request");
    return true;
  }

  if (request->GetMethod() != HttpMethod::GET) {
    response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Method Not Allowed");
    return true;
  }

  std::string folder = request->GetQueryParam("folder");
  if (folder.empty()) folder = "images";
  if (folder != "images" && folder != "video" && folder != "uploads") {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法folder参数");
    return true;
  }

  const std::string dir_path = static_path + "/" + folder;
  DIR* dir = opendir(dir_path.c_str());
  if (!dir) {
    SetJsonErrorResponse(response, HttpStatusCode::NOT_FOUND, "目录不存在");
    return true;
  }

  std::ostringstream data;
  data << "{\"files\":[";

  bool first = true;
  while (auto* ent = readdir(dir)) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    if (!IsSafeFileName(name)) continue;
    if (!IsAllowedInFolder(folder, name)) continue;

    const std::string full = dir_path + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;

    if (!first) data << ",";
    first = false;

    const std::string url = "/" + folder + "/" + name;
    const std::string downloadUrl = "/download/" + folder + "/" + name;
    data << "{";
    data << "\"folder\":\"" << JsonEscape(folder) << "\"";
    data << ",\"name\":\"" << JsonEscape(name) << "\"";
    data << ",\"size\":" << static_cast<long long>(st.st_size);
    data << ",\"mimeType\":\"" << JsonEscape(GetContentType(full)) << "\"";
    data << ",\"updatedAt\":\"" << JsonEscape(ToIso8601Utc(st.st_mtime)) << "\"";
    data << ",\"url\":\"" << JsonEscape(url) << "\"";
    data << ",\"downloadUrl\":\"" << JsonEscape(downloadUrl) << "\"";
    data << "}";
  }

  closedir(dir);

  data << "]}";
  SetJsonSuccessResponseWithData(response, data.str(), "操作成功");
  return true;
}

bool FileApiService::HandlePreview(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
  if (!request) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "Bad Request");
    return true;
  }
  if (request->GetMethod() != HttpMethod::GET) {
    response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Method Not Allowed");
    return true;
  }

  std::string folder = request->GetQueryParam("folder");
  std::string name = request->GetQueryParam("name");
  if (folder.empty()) folder = "images";
  if (folder != "images" && folder != "video" && folder != "uploads") {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法folder参数");
    return true;
  }
  if (!IsSafeFileName(name) || !IsAllowedInFolder(folder, name)) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法name参数");
    return true;
  }

  const std::string full = static_path + "/" + folder + "/" + name;
  struct stat st;
  if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    SetJsonErrorResponse(response, HttpStatusCode::NOT_FOUND, "文件不存在");
    return true;
  }

  const std::string url = "/" + folder + "/" + name;
  const std::string downloadUrl = "/download/" + folder + "/" + name;
  std::ostringstream data;
  data << "{";
  data << "\"folder\":\"" << JsonEscape(folder) << "\"";
  data << ",\"name\":\"" << JsonEscape(name) << "\"";
  data << ",\"size\":" << static_cast<long long>(st.st_size);
  data << ",\"mimeType\":\"" << JsonEscape(GetContentType(full)) << "\"";
  data << ",\"updatedAt\":\"" << JsonEscape(ToIso8601Utc(st.st_mtime)) << "\"";
  data << ",\"url\":\"" << JsonEscape(url) << "\"";
  data << ",\"downloadUrl\":\"" << JsonEscape(downloadUrl) << "\"";
  data << ",\"supportRange\":true";
  data << "}";

  SetJsonSuccessResponseWithData(response, data.str(), "操作成功");
  return true;
}
