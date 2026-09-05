#include "RegisterThrottle.h"

#include "../../logger/log_fac.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {

const std::chrono::seconds kWindow(3600);  // 1 小时窗口

std::mutex g_mutex;
std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> g_recent;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_submit;

int g_per_hour = 5;
int g_cooldown_seconds = 30;

std::chrono::steady_clock::time_point Now() { return std::chrono::steady_clock::now(); }

}  // namespace

void RegisterThrottle::SetLimits(int perHour, int cooldownSeconds) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_per_hour = perHour > 0 ? perHour : 5;
  g_cooldown_seconds = cooldownSeconds > 0 ? cooldownSeconds : 30;
}

void RegisterThrottle::Cleanup(const std::string& ip) {
  auto it = g_recent.find(ip);
  if (it == g_recent.end()) return;
  auto& q = it->second;
  const auto cutoff = Now() - kWindow;
  while (!q.empty() && q.front() < cutoff) q.pop_front();
  if (q.empty()) g_recent.erase(it);
}

ThrottleResult RegisterThrottle::Allow(const std::string& ip) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Now();

  auto it = g_recent.find(ip);
  if (it != g_recent.end()) {
    auto& q = it->second;
    const auto cutoff = now - kWindow;
    while (!q.empty() && q.front() < cutoff) q.pop_front();
    if (q.size() >= static_cast<size_t>(g_per_hour)) return ThrottleResult::HOURLY_LIMIT;
  }

  auto ls = g_last_submit.find(ip);
  if (ls != g_last_submit.end()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ls->second).count();
    if (elapsed < g_cooldown_seconds) return ThrottleResult::COOLDOWN;
  }

  return ThrottleResult::ALLOWED;
}

void RegisterThrottle::Record(const std::string& ip) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Now();
  auto& q = g_recent[ip];
  q.push_back(now);
  const auto cutoff = now - kWindow;
  while (!q.empty() && q.front() < cutoff) q.pop_front();
  g_last_submit[ip] = now;
}

int RegisterThrottle::RemainingCooldownSeconds(const std::string& ip) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto ls = g_last_submit.find(ip);
  if (ls == g_last_submit.end()) return 0;
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now() - ls->second).count();
  const int remain = g_cooldown_seconds - static_cast<int>(elapsed);
  return remain > 0 ? remain : 0;
}
