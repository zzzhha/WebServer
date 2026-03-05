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

static std::string NewUploadId() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  unsigned r = static_cast<unsigned>(std::rand());
  std::ostringstream oss;
  oss << ms << "-" << std::hex << r;
  return oss.str();
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
  const int partNo = std::atoi(partNoOpt->c_str());
  if (uploadId.empty() || partNo < 0) {
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

  const std::string partPath = PartPath(dir, partNo);
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

  const long long partCount = (fileSize + chunkSize - 1) / chunkSize;
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
