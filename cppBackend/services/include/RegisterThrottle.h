#pragma once

#include <string>

enum class ThrottleResult {
  ALLOWED,
  HOURLY_LIMIT,  // 已达每小时上限
  COOLDOWN,      // 两次提交间隔过短（冷却）
};

/**
 * RegisterThrottle：注册接口限流（按真实源 IP）。
 * - 每 IP 1 小时内最多允许 N 次注册提交。
 * - 同一 IP 两次提交间隔需 >= 冷却秒数。
 * 纯内存存储（mutex 保护）。key 使用 socket 源 IP，忽略 X-Forwarded-For 等可伪造头。
 */
class RegisterThrottle {
 public:
  static ThrottleResult Allow(const std::string& ip);
  static void Record(const std::string& ip);

  // 冷却剩余秒数（用于提示“请稍后再试”）。
  static int RemainingCooldownSeconds(const std::string& ip);

  static void SetLimits(int perHour, int cooldownSeconds);

 private:
  static void Cleanup(const std::string& ip);
};
