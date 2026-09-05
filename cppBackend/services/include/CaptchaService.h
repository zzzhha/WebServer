#pragma once

#include <string>

struct CaptchaChallenge {
  std::string token;
  std::string prompt;
};

/**
 * CaptchaService：自研算术验证码。
 * - Create()：生成一道加法题（如 "7 + 5 = ?"）与一次性 token，带 TTL，服务端只存答案。
 * - Verify(token, answer)：校验并一次性消费；错/已用/过期即失败。
 * 纯内存存储（mutex 保护），无外部依赖，用 openssl RAND_bytes 生成 token。
 */
class CaptchaService {
 public:
  static CaptchaChallenge Create();

  // 校验答案：token 有效且未过期、answer 数字正确 → 成功并删除该 token（一次性）。
  static bool Verify(const std::string& token, const std::string& answer);

  static void SetTtlSeconds(int seconds);

 private:
  static void CleanupExpired();
};
