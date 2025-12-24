#pragma once

#include <memory>
#include "ssl/ISslHandler.h"
#include "ssl/SslContext.h"

// SSL 工厂类：提供创建 SSL 处理器的统一接口
// 遵循工厂模式，与 HttpParseFactory 保持一致的设计风格
class SslFactory {
public:
  // 创建默认的 SSL 处理器
  static std::shared_ptr<ISslHandler> Create();

  // 使用指定的上下文创建 SSL 处理器
  static std::shared_ptr<ISslHandler> Create(std::shared_ptr<SslContext> context);

  // 创建服务端 SSL 处理器（使用默认配置）
  static std::shared_ptr<ISslHandler> CreateServer();

  // 创建客户端 SSL 处理器（使用默认配置）
  static std::shared_ptr<ISslHandler> CreateClient();

  // 使用证书和私钥文件创建 SSL 处理器
  static std::shared_ptr<ISslHandler> CreateWithCert(
      const std::string& cert_file, 
      const std::string& key_file,
      const std::string& ca_file = "");
};

