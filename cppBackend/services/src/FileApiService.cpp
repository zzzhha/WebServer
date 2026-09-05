#include "FileApiService.h"

#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/error/HttpErrorUtil.h"
#include "../../http/include/handler/AppHandlers.h"
#include "../../logger/log_fac.h"
#include "../../mysql/dao/FileDao.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>
#include <vector>

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
  (void)static_path;
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

  // 分页 / 排序 / 搜索参数
  int page = 1;
  {
    std::string pv = request->GetQueryParam("page");
    if (!pv.empty()) page = atoi(pv.c_str());
    if (page < 1) page = 1;
  }
  int pageSize = 50;
  {
    std::string sv = request->GetQueryParam("pageSize");
    if (!sv.empty()) pageSize = atoi(sv.c_str());
    if (pageSize < 1) pageSize = 1;
    if (pageSize > 200) pageSize = 200;
  }
  std::string sort = request->GetQueryParam("sort");   // name|size|mtime
  std::string order = request->GetQueryParam("order"); // asc|desc
  std::string q = request->GetQueryParam("q");         // 名称搜索（包含）

  std::vector<FileMeta> files;
  long long total = 0;
  if (!FileDao::QueryPage(folder, q, sort, order, page, pageSize, files, total)) {
    SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "查询文件列表失败");
    return true;
  }

  long long totalPages = (pageSize > 0) ? (total + pageSize - 1) / pageSize : 0;

  std::ostringstream data;
  data << "{\"files\":[";
  bool first = true;
  for (const auto& f : files) {
    // url / downloadUrl 由代码用 folder+name 拼接（不入库，路径规则随代码走）
    const std::string url = "/" + f.folder + "/" + f.name;
    const std::string downloadUrl = "/download/" + f.folder + "/" + f.name;
    if (!first) data << ",";
    first = false;
    data << "{";
    data << "\"folder\":\"" << JsonEscape(f.folder) << "\"";
    data << ",\"name\":\"" << JsonEscape(f.name) << "\"";
    data << ",\"size\":" << f.size;
    data << ",\"mimeType\":\"" << JsonEscape(f.mime_type) << "\"";
    data << ",\"updatedAt\":\"" << JsonEscape(ToIso8601Utc((time_t)f.updated_at)) << "\"";
    data << ",\"url\":\"" << JsonEscape(url) << "\"";
    data << ",\"downloadUrl\":\"" << JsonEscape(downloadUrl) << "\"";
    data << ",\"thumbState\":\"" << JsonEscape(f.thumb_state) << "\"";
    data << ",\"thumbWidth\":" << f.thumb_width;
    data << ",\"posterState\":\"" << JsonEscape(f.poster_state) << "\"";
    data << "}";
  }
  data << "],\"total\":" << total
       << ",\"page\":" << page
       << ",\"pageSize\":" << pageSize
       << ",\"totalPages\":" << totalPages << "}";
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
