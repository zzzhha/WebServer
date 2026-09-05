#include "ThumbService.h"

#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/handler/AppHandlers.h"   // GetContentType
#include "../../http/include/util/HttpStringUtil.h"   // 若需要小写/解码工具
#include "../../logger/log_fac.h"
#include "../../mysql/dao/FileDao.h"
#include "FileServeUtil.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {

bool IsSafeName(const std::string& v) {
  if (v.empty()) return false;
  if (v.find('/') != std::string::npos) return false;
  if (v.find('\\') != std::string::npos) return false;
  if (v.find("..") != std::string::npos) return false;
  if (v.find('\0') != std::string::npos) return false;
  return true;
}

std::string LowerExt(const std::string& name) {
  auto pos = name.find_last_of('.');
  if (pos == std::string::npos) return "";
  std::string e = name.substr(pos + 1);
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return e;
}

bool IsAllowedInFolder(const std::string& folder, const std::string& name) {
  const std::string ext = LowerExt(name);
  if (folder == "images")
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "svg";
  if (folder == "video")
    return ext == "mp4" || ext == "webm" || ext == "avi";
  return false;
}

bool IsVideo(const std::string& folder) { return folder == "video"; }

// 简单的 %XX 解码（文件名字面量，用于路径还原）。
std::string PercentDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

// 单引号包裹以规避 shell 注入/空格问题。
std::string ShellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

int RunCommand(const std::string& cmd) {
  int rc = system(cmd.c_str());
  if (rc == -1) return -1;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return -1;
}

}  // namespace

bool ThumbService::HandleThumb(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
  if (!request) {
    response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Bad Request");
    return true;
  }

  // 解析路径 /thumb/<folder>/<name>
  std::string path = request->GetPath();
  const std::string prefix = "/thumb/";
  if (path.compare(0, prefix.size(), prefix) != 0) {
    response.SetStatusCode(HttpStatusCode::NOT_FOUND);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Not Found");
    return true;
  }
  std::string rest = path.substr(prefix.size());
  size_t slash = rest.find('/');
  if (slash == std::string::npos) {
    response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Bad Request");
    return true;
  }
  std::string folder = rest.substr(0, slash);
  std::string name = PercentDecode(rest.substr(slash + 1));

  if (folder != "images" && folder != "video") {
    response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Bad Request");
    return true;
  }
  if (!IsSafeName(name) || !IsAllowedInFolder(folder, name)) {
    response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Bad Request");
    return true;
  }

  // 宽度
  int width = 320;
  std::string wstr = request->GetQueryParam("w");
  if (!wstr.empty()) width = atoi(wstr.c_str());
  if (width < 16) width = 16;
  if (width > 2000) width = 2000;

  // 源文件
  std::string src_full;
  if (!FileServeUtil::ResolvePathUnderRoot(static_path, folder + "/" + name, src_full)) {
    response.SetStatusCode(HttpStatusCode::FORBIDDEN);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Forbidden");
    return true;
  }
  struct stat st;
  if (stat(src_full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    response.SetStatusCode(HttpStatusCode::NOT_FOUND);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Not Found");
    return true;
  }
  const long long mtime = static_cast<long long>(st.st_mtime);

  // 缓存路径：_thumbs/<folder>/<name>@<mtime>_w<width>.jpg （mtime 变化自动失效）
  std::error_code ec;
  std::filesystem::path thumb_dir = std::filesystem::path(static_path) / "_thumbs" / folder;
  std::filesystem::create_directories(thumb_dir, ec);
  // 统一裁剪到固定宽高比：图片 4:3，视频封面 16:9（让所有卡片视觉一致）。
  const bool is_video = IsVideo(folder);
  const int height = std::max(1, (int)std::lround(width * (is_video ? 9.0 / 16.0 : 3.0 / 4.0)));
  // 缓存文件名带高度，旧版本缓存（无高度）会自动失效并重新生成。
  std::filesystem::path thumb_file = thumb_dir / (name + "@" + std::to_string(mtime) + "_w" + std::to_string(width) + "_h" + std::to_string(height) + ".jpg");
  const std::string thumb_path = thumb_file.string();

  bool exists = false;
  {
    struct stat ts;
    exists = (stat(thumb_path.c_str(), &ts) == 0 && S_ISREG(ts.st_mode));
  }

  if (!exists) {
    // 用 ffmpeg 生成，缩放铺满 + 居中裁剪到目标宽高比
    std::string cmd;
    const std::string src_q = ShellQuote(src_full);
    const std::string dst_q = ShellQuote(thumb_path);
    std::string vf = "scale=" + std::to_string(width) + ":" + std::to_string(height) +
                     ":force_original_aspect_ratio=increase,crop=" + std::to_string(width) + ":" + std::to_string(height);
    if (is_video) vf = "thumbnail=300," + vf;
    cmd = "ffmpeg -y -loglevel error -i " + src_q + " -vf '" + vf + "' -frames:v 1 " + dst_q;
    int rc = RunCommand(cmd);
    struct stat ts2;
    bool created = (rc == 0 && stat(thumb_path.c_str(), &ts2) == 0 && S_ISREG(ts2.st_mode));
    if (!created) {
      LOGWARNING("ThumbService: generate failed folder=" + folder + " name=" + name + " rc=" + std::to_string(rc));
      // 记录失败状态，但允许下次重试。
      if (is_video) FileDao::SetPosterState(folder, name, "fail");
      else FileDao::SetThumbState(folder, name, "fail", 0);
      response.SetStatusCode(HttpStatusCode::NOT_FOUND);
      response.SetHeader("Content-Type", "text/plain");
      response.SetBody("Not Found");
      return true;
    }
    // 回写状态
    if (is_video) FileDao::SetPosterState(folder, name, "ok");
    else FileDao::SetThumbState(folder, name, "ok", width);
  }

  // 直接发送文件（带缓存头）
  std::string etag = FileServeUtil::BuildWeakEtag(st.st_mtime, 0);
  response.SetStatusCode(HttpStatusCode::OK);
  response.SetHeader("Content-Type", "image/jpeg");
  response.SetHeader("Cache-Control", "public, max-age=3600");
  response.SetHeader("ETag", etag);
  response.SetHeader("Accept-Ranges", "bytes");
  uint64_t fsize = 0;
  FileServeUtil::GetFileSize(thumb_path, fsize);
  response.SetHeader("Content-Length", std::to_string(fsize));
  response.SetBody("");
  response.SetSendFile(thumb_path, 0, fsize);
  return true;
}
