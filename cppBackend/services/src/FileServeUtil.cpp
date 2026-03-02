#include "FileServeUtil.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <time.h>

#include <openssl/evp.h>

bool FileServeUtil::GetFileSize(const std::string& path, uint64_t& size) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  if (!S_ISREG(st.st_mode)) return false;
  size = static_cast<uint64_t>(st.st_size);
  return true;
}

static bool ParseUint64(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    uint64_t d = static_cast<uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  out = v;
  return true;
}

bool FileServeUtil::ParseRangeHeader(const std::string& range_value, uint64_t file_size, FileRange& out) {
  out = FileRange{};
  if (range_value.empty()) return true;

  std::string v = range_value;
  const std::string kPrefix = "bytes=";
  if (v.rfind(kPrefix, 0) != 0) return false;
  v = v.substr(kPrefix.size());

  if (v.find(',') != std::string::npos) return false;

  size_t dash = v.find('-');
  if (dash == std::string::npos) return false;

  std::string left = v.substr(0, dash);
  std::string right = v.substr(dash + 1);

  if (file_size == 0) return false;

  uint64_t start = 0;
  uint64_t end = 0;

  if (left.empty()) {
    uint64_t suffix_len = 0;
    if (!ParseUint64(right, suffix_len)) return false;
    if (suffix_len == 0) return false;
    if (suffix_len >= file_size) {
      start = 0;
    } else {
      start = file_size - suffix_len;
    }
    end = file_size - 1;
  } else {
    if (!ParseUint64(left, start)) return false;
    if (start >= file_size) return false;

    if (right.empty()) {
      end = file_size - 1;
    } else {
      if (!ParseUint64(right, end)) return false;
      if (end < start) return false;
      if (end >= file_size) end = file_size - 1;
    }
  }

  out.enabled = true;
  out.start = start;
  out.end = end;
  return true;
}

bool FileServeUtil::ReadFileRange(const std::string& path, uint64_t start, uint64_t length, std::string& out) {
  out.clear();
  if (length == 0) return true;

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;

  file.seekg(static_cast<std::streamoff>(start), std::ios::beg);
  if (!file) return false;

  out.resize(static_cast<size_t>(length));
  file.read(&out[0], static_cast<std::streamsize>(length));
  if (!file) {
    size_t got = static_cast<size_t>(file.gcount());
    out.resize(got);
    return got == static_cast<size_t>(length);
  }
  return true;
}

static std::string ToHexLower(const unsigned char* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    unsigned char b = data[i];
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

std::optional<std::string> FileServeUtil::ComputeFileMd5Hex(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return std::nullopt;

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return std::nullopt;

  const EVP_MD* md = EVP_md5();
  if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::nullopt;
  }

  char buf[64 * 1024];
  while (file) {
    file.read(buf, sizeof(buf));
    std::streamsize n = file.gcount();
    if (n > 0) {
      if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n)) != 1) {
        EVP_MD_CTX_free(ctx);
        return std::nullopt;
      }
    }
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::nullopt;
  }
  EVP_MD_CTX_free(ctx);

  return ToHexLower(digest, digest_len);
}

std::string FileServeUtil::ToHttpDate(time_t t) {
  char buf[100];
  if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", std::gmtime(&t))) {
    return std::string(buf);
  }
  return "";
}

bool FileServeUtil::ParseHttpDate(const std::string& s, time_t& out) {
  std::tm tm{};
  std::istringstream ss(s);
  ss.imbue(std::locale::classic());
  ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
  if (ss.fail()) return false;
  out = timegm(&tm);
  return true;
}

std::string FileServeUtil::BuildWeakEtag(time_t mtime, uint64_t size) {
  return "W/\"" + std::to_string(static_cast<long long>(mtime)) + "-" + std::to_string(size) + "\"";
}

bool FileServeUtil::ResolvePathUnderRoot(const std::string& root, const std::string& request_path, std::string& out_full_path) {
  out_full_path.clear();
  if (root.empty()) return false;
  if (request_path.empty()) return false;
  if (request_path.find('\\') != std::string::npos) return false;
  if (request_path.find('\0') != std::string::npos) return false;

  std::string rel = request_path;
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  if (rel.empty()) rel = "index.html";

  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path root_p = fs::weakly_canonical(fs::path(root), ec);
  if (ec) return false;
  fs::path full_p = fs::weakly_canonical(root_p / fs::path(rel), ec);
  if (ec) return false;

  auto root_it = root_p.begin();
  auto full_it = full_p.begin();
  for (; root_it != root_p.end(); ++root_it, ++full_it) {
    if (full_it == full_p.end() || *root_it != *full_it) return false;
  }

  out_full_path = full_p.string();
  return true;
}
