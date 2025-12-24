#include "ssl/SslContext.h"

#include <algorithm>
#include <cctype>

namespace SslVersionUtil {
  std::string ToString(SslVersion version) {
    switch (version) {
      case SslVersion::TLS_1_0:
        return "TLSv1.0";
      case SslVersion::TLS_1_1:
        return "TLSv1.1";
      case SslVersion::TLS_1_2:
        return "TLSv1.2";
      case SslVersion::TLS_1_3:
        return "TLSv1.3";
      case SslVersion::AUTO:
        return "AUTO";
      default:
        return "TLSv1.2";
    }
  }

  SslVersion FromString(const std::string& version_str) {
    if (version_str.empty()) {
      return SslVersion::TLS_1_2;  // 默认值
    }

    // 转换为小写以便比较
    std::string lower_str = version_str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // 移除可能的空格
    lower_str.erase(std::remove_if(lower_str.begin(), lower_str.end(),
                                   [](unsigned char c) { return std::isspace(c); }),
                    lower_str.end());

    // 匹配版本字符串
    if (lower_str == "tlsv1.0" || lower_str == "tls1.0" || lower_str == "tls_1_0") {
      return SslVersion::TLS_1_0;
    } else if (lower_str == "tlsv1.1" || lower_str == "tls1.1" || lower_str == "tls_1_1") {
      return SslVersion::TLS_1_1;
    } else if (lower_str == "tlsv1.2" || lower_str == "tls1.2" || lower_str == "tls_1_2") {
      return SslVersion::TLS_1_2;
    } else if (lower_str == "tlsv1.3" || lower_str == "tls1.3" || lower_str == "tls_1_3") {
      return SslVersion::TLS_1_3;
    } else if (lower_str == "auto" || lower_str == "automatic") {
      return SslVersion::AUTO;
    }

    // 默认返回 TLS 1.2
    return SslVersion::TLS_1_2;
  }

  bool IsValidVersion(const std::string& version_str) {
    if (version_str.empty()) {
      return false;
    }

    std::string lower_str = version_str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    lower_str.erase(std::remove_if(lower_str.begin(), lower_str.end(),
                                   [](unsigned char c) { return std::isspace(c); }),
                    lower_str.end());

    return (lower_str == "tlsv1.0" || lower_str == "tls1.0" || lower_str == "tls_1_0" ||
            lower_str == "tlsv1.1" || lower_str == "tls1.1" || lower_str == "tls_1_1" ||
            lower_str == "tlsv1.2" || lower_str == "tls1.2" || lower_str == "tls_1_2" ||
            lower_str == "tlsv1.3" || lower_str == "tls1.3" || lower_str == "tls_1_3" ||
            lower_str == "auto" || lower_str == "automatic");
  }
}  // namespace SslVersionUtil

void SslContext::SetSslVersion(const std::string& version_str) {
  ssl_version_ = SslVersionUtil::FromString(version_str);
}

std::string SslContext::GetSslVersionStr() const {
  return SslVersionUtil::ToString(ssl_version_);
}

