#include "UploadService.h"

#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/error/HttpErrorUtil.h"
#include "../../http/include/handler/AppHandlers.h"
#include "../../http/include/router/Router.h"
#include "../../logger/log_fac.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/stat.h>

static bool EnsureDir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) == 0) return true;
  if (errno == EEXIST) return true;
  return false;
}

static bool IsSafeFileName(const std::string& v) {
  if (v.empty()) return false;
  if (v.find('/') != std::string::npos) return false;
  if (v.find('\\') != std::string::npos) return false;
  if (v.find("..") != std::string::npos) return false;
  if (v.find('\0') != std::string::npos) return false;
  return true;
}

// H8 修复：uploadId 白名单校验 —— 仅允许字母数字、下划线、连字符，长度 1~64。
// uploadId 会被拼接进目录路径（uploads_tmp/<uploadId>），若不校验可路径遍历逃逸。
static bool IsSafeUploadId(const std::string& v) {
  if (v.empty() || v.size() > 64) return false;
  for (unsigned char c : v) {
    if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
  }
  return true;
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
    // H9 修复：移除 svg（可内嵌 <script>，匿名上传覆盖后构成存储型 XSS）
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif";
  }
  if (folder == "video") {
    return ext == "mp4" || ext == "webm" || ext == "avi";
  }
  if (folder == "uploads") {
    // H9 修复：移除 svg（同上，防存储型 XSS）
    return ext == "pdf" || ext == "txt" || ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
           ext == "mp4" || ext == "webm" || ext == "avi";
  }
  return false;
}

static std::string NewUploadId() {
  // 中危修复：原实现 std::rand()+时间戳可预测，可被猜到他人会话；
  // 改用系统随机源生成 32 位十六进制（128 bit）
  std::random_device rd;
  std::string id;
  id.reserve(32);
  const char* hex = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    const unsigned char b = static_cast<unsigned char>(rd() & 0xff);
    id += hex[b >> 4];
    id += hex[b & 0x0f];
  }
  return id;
}

// 中危修复：删除上传会话临时目录（分片 + meta），成功后清理防止临时文件堆积
static void RemoveSessionDir(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (auto* ent = readdir(d)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    ::remove((dir + "/" + name).c_str());
  }
  closedir(d);
  ::rmdir(dir.c_str());
}

static bool ReadMeta(const std::string& dir, std::string& fileName, long long& fileSize, int& chunkSize, std::string& folder) {
  std::ifstream in(dir + "/meta.txt");
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    const std::string k = line.substr(0, pos);
    const std::string v = line.substr(pos + 1);
    if (k == "fileName") fileName = v;
    if (k == "fileSize") fileSize = std::atoll(v.c_str());
    if (k == "chunkSize") chunkSize = std::atoi(v.c_str());
    if (k == "folder") folder = v;
  }
  if (fileName.empty() || fileSize <= 0 || chunkSize <= 0 || folder.empty()) return false;
  return true;
}

static std::string PartPath(const std::string& dir, int partNo) {
  return dir + "/part_" + std::to_string(partNo) + ".bin";
}

static std::string ListUploadedPartsJson(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return "[]";
  std::vector<int> parts;
  while (auto* ent = readdir(d)) {
    std::string name = ent->d_name;
    if (name.rfind("part_", 0) != 0) continue;
    if (name.size() < 9) continue;
    if (name.find(".bin") == std::string::npos) continue;
    const std::string mid = name.substr(5, name.size() - 5 - 4);
    int n = std::atoi(mid.c_str());
    if (n >= 0) parts.push_back(n);
  }
  closedir(d);
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < parts.size(); i += 1) {
    if (i) oss << ",";
    oss << parts[i];
  }
  oss << "]";
  return oss.str();
}

bool UploadService::HandleInit(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
  if (!request) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "Bad Request");
    return true;
  }
  if (request->GetMethod() != HttpMethod::POST) {
    response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Method Not Allowed");
    return true;
  }

  auto form = ParseFormData(request->GetBody());
  std::string fileName = form.count("fileName") ? form.at("fileName") : "";
  std::string folder = form.count("folder") ? form.at("folder") : "uploads";
  long long fileSize = form.count("fileSize") ? std::atoll(form.at("fileSize").c_str()) : 0;
  int chunkSize = form.count("chunkSize") ? std::atoi(form.at("chunkSize").c_str()) : 0;
  std::string uploadId = form.count("uploadId") ? form.at("uploadId") : "";

  if (folder.empty()) folder = "uploads";
  if (folder != "images" && folder != "video" && folder != "uploads") {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法folder参数");
    return true;
  }
  if (!IsSafeFileName(fileName) || !IsAllowedInFolder(folder, fileName)) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法文件名或类型");
    return true;
  }
  if (fileSize <= 0 || fileSize > 200LL * 1024 * 1024) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "文件大小不合法");
    return true;
  }
  if (chunkSize <= 0 || chunkSize > 8 * 1024 * 1024) chunkSize = 1024 * 1024;

  const std::string root = static_path + "/uploads_tmp";
  if (!EnsureDir(root)) {
    SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法创建上传目录");
    return true;
  }

  if (uploadId.empty()) uploadId = NewUploadId();
  // H8 修复：客户端可指定 uploadId（断点续传），必须白名单校验，防路径遍历逃逸 uploads_tmp
  if (!IsSafeUploadId(uploadId)) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法uploadId参数");
    return true;
  }
  const std::string dir = root + "/" + uploadId;
  if (!EnsureDir(dir)) {
    SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法创建上传会话");
    return true;
  }

  const std::string metaPath = dir + "/meta.txt";
  {
    std::ofstream out(metaPath, std::ios::trunc);
    if (!out) {
      SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法写入上传元信息");
      return true;
    }
    out << "fileName=" << fileName << "\n";
    out << "fileSize=" << fileSize << "\n";
    out << "chunkSize=" << chunkSize << "\n";
    out << "folder=" << folder << "\n";
  }

  std::ostringstream data;
  data << "{";
  data << "\"uploadId\":\"" << JsonEscape(uploadId) << "\"";
  data << ",\"chunkSize\":" << chunkSize;
  data << ",\"uploadedParts\":" << ListUploadedPartsJson(dir);
  data << "}";

  SetJsonSuccessResponseWithData(response, data.str(), "操作成功");
  return true;
}

bool UploadService::HandleUploadPart(HttpRequest* request, HttpResponse& response, const RouteParams& params, const std::string& static_path) {
  if (!request) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "Bad Request");
    return true;
  }
  if (request->GetMethod() != HttpMethod::PUT) {
    response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Method Not Allowed");
    return true;
  }

  const auto uploadIdOpt = params.GetParam("uploadId");
  const auto partNoOpt = params.GetParam("partNo");
  if (!uploadIdOpt || !partNoOpt) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "缺少参数");
    return true;
  }
  const std::string uploadId = *uploadIdOpt;
  // 中危修复：partNo 全量数字校验 + 上限（防 atoi 部分解析与磁盘耗尽 DoS）
  const std::string partNoStr = *partNoOpt;
  char* end = nullptr;
  long partNo = std::strtol(partNoStr.c_str(), &end, 10);
  constexpr long kMaxPartNo = 1000000;
  if (end == partNoStr.c_str() || *end != '\0' || partNo < 0 || partNo > kMaxPartNo) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法参数");
    return true;
  }
  // H8 修复：uploadId 白名单校验，防路径遍历逃逸 uploads_tmp
  if (!IsSafeUploadId(uploadId)) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法参数");
    return true;
  }

  const std::string dir = static_path + "/uploads_tmp/" + uploadId;
  std::string fileName;
  long long fileSize = 0;
  int chunkSize = 0;
  std::string folder;
  if (!ReadMeta(dir, fileName, fileSize, chunkSize, folder)) {
    SetJsonErrorResponse(response, HttpStatusCode::NOT_FOUND, "上传会话不存在");
    return true;
  }

  const std::string& body = request->GetBody();
  if (body.empty()) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "分片为空");
    return true;
  }
  if (static_cast<int>(body.size()) > chunkSize) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "分片过大");
    return true;
  }

  const std::string partPath = PartPath(dir, static_cast<int>(partNo));
  std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法写入分片");
    return true;
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  out.close();

  std::ostringstream data;
  data << "{";
  data << "\"partNo\":" << partNo;
  data << ",\"size\":" << static_cast<long long>(body.size());
  data << "}";
  SetJsonSuccessResponseWithData(response, data.str(), "操作成功");
  return true;
}

bool UploadService::HandleComplete(HttpRequest* request, HttpResponse& response, const RouteParams& params, const std::string& static_path) {
  if (!request) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "Bad Request");
    return true;
  }
  if (request->GetMethod() != HttpMethod::POST) {
    response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Method Not Allowed");
    return true;
  }

  const auto uploadIdOpt = params.GetParam("uploadId");
  if (!uploadIdOpt) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "缺少参数");
    return true;
  }
  const std::string uploadId = *uploadIdOpt;
  // H8 修复：uploadId 白名单校验，防路径遍历逃逸 uploads_tmp
  if (!IsSafeUploadId(uploadId)) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法uploadId参数");
    return true;
  }

  const std::string dir = static_path + "/uploads_tmp/" + uploadId;
  std::string fileName;
  long long fileSize = 0;
  int chunkSize = 0;
  std::string folder;
  if (!ReadMeta(dir, fileName, fileSize, chunkSize, folder)) {
    SetJsonErrorResponse(response, HttpStatusCode::NOT_FOUND, "上传会话不存在");
    return true;
  }
  if (!IsSafeFileName(fileName) || !IsAllowedInFolder(folder, fileName)) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法文件名或类型");
    return true;
  }

  // 中危修复：meta.txt 可被客户端篡改，fileSize/chunkSize 不可信。
  // 校验上下限，防 chunkSize=1 时 partCount 达 2 亿次 stat（CPU DoS）与除法溢出。
  constexpr long long kMaxUploadBytes = 200LL * 1024 * 1024;
  constexpr int kMinChunkSize = 1024;
  constexpr int kMaxChunkSize = 8 * 1024 * 1024;
  if (fileSize <= 0 || fileSize > kMaxUploadBytes || chunkSize < kMinChunkSize ||
      chunkSize > kMaxChunkSize) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "非法上传参数");
    return true;
  }

  const long long partCount = (fileSize + chunkSize - 1) / chunkSize;
  // 中危修复：分片数上限（200MB / 1KB ≈ 20 万分片，1M 为安全裕量）
  constexpr long long kMaxPartCount = 1000000;
  if (partCount > kMaxPartCount) {
    SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "分片数量超限");
    return true;
  }
  for (int i = 0; i < partCount; i += 1) {
    struct stat st;
    if (stat(PartPath(dir, i).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "分片缺失");
      return true;
    }
  }

  const std::string finalDir = static_path + "/" + folder;
  if (!EnsureDir(finalDir)) {
    SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法创建目标目录");
    return true;
  }

  const std::string finalPath = finalDir + "/" + fileName;
  // H9 修复：禁止覆盖已有文件 —— 匿名上传可覆盖静态资源/同名文件，构成完整性破坏与存储型 XSS
  struct stat final_st;
  if (stat(finalPath.c_str(), &final_st) == 0) {
    SetJsonErrorResponse(response, HttpStatusCode::CONFLICT, "目标文件已存在，禁止覆盖");
    return true;
  }
  std::ofstream out(finalPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法写入目标文件");
    return true;
  }

  std::vector<char> buf(static_cast<size_t>(chunkSize));
  for (int i = 0; i < partCount; i += 1) {
    std::ifstream in(PartPath(dir, i), std::ios::binary);
    if (!in) {
      SetJsonErrorResponse(response, HttpStatusCode::INTERNAL_SERVER_ERROR, "无法读取分片");
      return true;
    }
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      std::streamsize n = in.gcount();
      if (n > 0) out.write(buf.data(), n);
    }
  }
  out.close();

  // 中危修复：合并成功后清理整个会话临时目录（分片 + meta），防止临时文件无限堆积
  RemoveSessionDir(dir);

  std::ostringstream data;
  data << "{";
  data << "\"folder\":\"" << JsonEscape(folder) << "\"";
  data << ",\"name\":\"" << JsonEscape(fileName) << "\"";
  data << ",\"url\":\"" << JsonEscape("/" + folder + "/" + fileName) << "\"";
  data << ",\"downloadUrl\":\"" << JsonEscape("/download/" + folder + "/" + fileName) << "\"";
  data << "}";
  SetJsonSuccessResponseWithData(response, data.str(), "操作成功");
  return true;
}
