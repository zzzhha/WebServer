#include "CaptchaService.h"

#include "../../logger/log_fac.h"
#include <openssl/rand.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {

struct Entry {
  int answer;
  std::chrono::steady_clock::time_point expires;
};

std::mutex g_mutex;
std::unordered_map<std::string, Entry> g_store;
std::atomic<int> g_ttl_seconds{60};

int RandomInt(int lo, int hi) {
  unsigned int v = 0;
  if (RAND_bytes(reinterpret_cast<unsigned char*>(&v), sizeof(v)) != 1) {
    return lo;  // 降级：返回确定值，避免注册失败
  }
  return lo + static_cast<int>(v % static_cast<unsigned int>(hi - lo + 1));
}

std::string RandomToken() {
  const int N = 16;
  unsigned char buf[N];
  if (RAND_bytes(buf, N) != 1) {
    // 极低概率失败：用时间戳兜底（Token 仅作一次性会话标识）
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  }
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (int i = 0; i < N; ++i) ss << std::setw(2) << static_cast<unsigned int>(buf[i]);
  return ss.str();
}

std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

}  // namespace

void CaptchaService::CleanupExpired() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = g_store.begin(); it != g_store.end();) {
    if (it->second.expires < now) it = g_store.erase(it);
    else ++it;
  }
}

void CaptchaService::SetTtlSeconds(int seconds) {
  g_ttl_seconds.store(seconds > 0 ? seconds : 60, std::memory_order_relaxed);
}

CaptchaChallenge CaptchaService::Create() {
  const int a = RandomInt(1, 20);
  const int b = RandomInt(1, 20);
  const int answer = a + b;
  std::string token = RandomToken();
  const int ttl = g_ttl_seconds.load(std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    CleanupExpired();
    g_store[token] = Entry{answer, std::chrono::steady_clock::now() + std::chrono::seconds(ttl)};
  }

  CaptchaChallenge ch;
  ch.token = std::move(token);
  ch.prompt = std::to_string(a) + " + " + std::to_string(b) + " = ?";
  return ch;
}

bool CaptchaService::Verify(const std::string& token, const std::string& answerStr) {
  if (token.empty() || answerStr.empty()) return false;
  const std::string trimmed = Trim(answerStr);
  int answer = 0;
  try {
    answer = std::stoi(trimmed);
  } catch (...) {
    return false;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_store.find(token);
  if (it == g_store.end()) return false;
  if (it->second.expires < std::chrono::steady_clock::now()) {
    g_store.erase(it);
    return false;
  }
  const bool ok = (it->second.answer == answer);
  g_store.erase(it);  // 一次性使用
  return ok;
}
