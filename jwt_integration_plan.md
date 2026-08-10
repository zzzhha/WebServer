# JWT集成规划文档

## 1. 项目概述

本项目是一个C++ HTTP服务器，位于`/home/zsy/WebServer`目录下。目前已实现用户认证功能，包括注册和登录，但尚未集成JWT（JSON Web Token）进行身份验证。

### 现有认证架构

- **UserDao**：数据访问层，负责数据库操作
- **AuthService**：业务逻辑层，处理注册和登录逻辑
- **Http模块**：处理HTTP请求和响应

## 2. JWT集成目标

1. **实现无状态认证**：使用JWT替代传统的会话管理
2. **提高安全性**：通过加密的token保护用户身份
3. **支持跨服务认证**：便于未来微服务架构扩展
4. **简化认证流程**：减少服务器端状态管理

## 3. JWT库选择

**jwt-cpp** (0.7.0+)：轻量级、现代C++、头文件库，已在本地`/usr/local/include`中安装，无需额外配置。

## 4. 集成方案

### 4.1 目录结构

```
WebServer/
├── cppBackend/
│   ├── auth/           # 新增：认证相关模块
│   │   ├── jwt/        # JWT实现
│   │   │   ├── JwtUtil.h    # JWT工具类头文件
│   │   │   └── JwtUtil.cpp  # JWT工具类实现
│   ├── mysql/          # 数据库访问模块
│   ├── services/       # 业务逻辑模块
│   │   ├── include/    # 头文件
│   │   │   └── AuthService.h  # 认证服务
│   │   └── AuthService.cpp     # 认证服务实现
│   └── http/           # HTTP模块
```

### 4.2 依赖管理

**jwt-cpp库**：已在本地`/usr/local/include`中安装，无需额外配置。编译时会自动找到头文件。

### 4.3 认证实现层次

**推荐实现层次**：
1. **auth模块**：实现核心JWT功能（JwtUtil类）
2. **Service层**：AuthService使用JwtUtil处理业务逻辑
3. **HTTP层**：集成认证中间件，使用AuthService进行验证

**优势**：
- 职责分离，每个模块只负责自己的功能
- 代码复用，JWT工具类可以被多个服务使用
- 易于维护，修改认证逻辑时只需修改相应层级

## 5. 核心功能设计

### 5.1 JWT工具类

**JwtUtil.h**

| 方法名 | 参数 | 返回值 | 描述 |
|--------|------|--------|------|
| `GenerateToken` | username: string<br>user_id: string | string | 生成JWT token |
| `ValidateToken` | token: string | optional<Claims> | 验证token并返回claims |
| `RefreshToken` | token: string | optional<string> | 刷新token |
| `ExtractUsername` | token: string | optional<string> | 从token中提取用户名 |

### 5.2 认证服务扩展

**AuthService.h**

| 方法名 | 参数 | 返回值 | 描述 |
|--------|------|--------|------|
| `HandleLogin` | username: string<br>password: string | optional<string> | 登录并返回token |
| `ValidateToken` | token: string | bool | 验证token有效性 |
| `GetUserFromToken` | token: string | optional<UserInfo> | 从token获取用户信息 |

### 5.3 HTTP模块集成

**认证中间件**
- 实现JWT验证中间件
- 处理Authorization头
- 保护需要认证的路由

## 6. 数据结构设计

### 6.1 JWT Claims结构

```json
{
  "sub": "user_id",           # 主题（用户ID）
  "username": "user123",      # 用户名
  "iat": 1620000000,          # 签发时间
  "exp": 1620086400,          # 过期时间
  "jti": "uuid-v4-string"     # JWT ID
}
```

**JWT ID生成**：
- 复用UserDao中已实现的UUID生成方法
- 或使用与UserDao中相同的UUID生成逻辑
- 确保每个JWT都有唯一的标识符，防止token重放攻击

### 6.2 错误处理

| 错误类型 | 状态码 | 消息 |
|----------|--------|------|
| 无效token | 401 | Invalid or expired token |
| 缺少token | 401 | Authorization header required |
| 格式错误 | 401 | Invalid authorization header format |
| 权限不足 | 403 | Insufficient permissions |

## 7. 安全性考虑

### 7.1 密钥管理

**当前实现**：
- **使用固定的开发环境密钥**：直接在代码中使用固定的密钥
- **暂不考虑生产环境密钥管理**：专注于功能实现

**实现细节**：
- 在JwtUtil类中使用固定的测试密钥
- 密钥长度至少32字符，使用强随机字符串
- 示例密钥：`your-super-secret-key-for-jwt-token-generation-2026`

### 7.2 Token安全

- **过期时间**：
  - **访问令牌**：24小时（平衡安全性和用户体验）
  - **刷新令牌**：7天（用于长期登录）
- **刷新机制**：实现token刷新流程，支持长期登录
- **HTTPS传输**：确保token通过HTTPS传输
- **防篡改**：使用签名防止token被篡改

**长期登录实现方案**：
1. **双令牌机制**：
   - 生成访问令牌（24小时过期）
   - 生成刷新令牌（7天过期）
2. **刷新流程**：
   - 访问令牌过期时，使用刷新令牌获取新的访问令牌
   - 刷新令牌也过期时，需要重新登录
3. **客户端存储**：
   - 访问令牌存储在内存中
   - 刷新令牌存储在安全的地方（如HttpOnly cookie）

### 7.3 最佳实践

- 不在token中存储敏感信息
- 使用强密钥并定期更换
- 实现token黑名单机制
- 监控异常的认证尝试

## 8. 实现步骤

### 步骤1：准备工作

1. 确认jwt-cpp库已在`/usr/local/include`中安装
2. 检查编译环境，确保能正确包含jwt-cpp头文件

### 步骤2：实现JwtUtil工具类

1. 创建`JwtUtil.h`和`JwtUtil.cpp`
2. 实现token生成、验证和刷新功能
3. 配置密钥管理

### 步骤3：扩展AuthService

1. 修改`AuthService.h`，添加JWT相关方法
2. 实现登录返回token的逻辑
3. 实现token验证和用户信息获取

### 步骤4：HTTP模块集成

1. 添加认证中间件
2. 处理Authorization头
3. 保护需要认证的路由

### 步骤5：前端集成

1. 登录成功后存储token
2. 请求时在Authorization头中携带token
3. 处理token过期和刷新

## 9. 测试计划

### 单元测试

- **JwtUtil测试**：测试token生成、验证、刷新
- **AuthService测试**：测试登录、注册、token验证
- **HTTP中间件测试**：测试认证流程

### 集成测试

- **端到端测试**：完整的认证流程测试
- **安全性测试**：测试token篡改、过期处理
- **性能测试**：测试JWT验证性能

## 10. 配置管理

### 10.1 配置项

| 配置项 | 类型 | 默认值 | 描述 |
|--------|------|--------|------|
| JWT_SECRET | string | 随机生成 | JWT签名密钥 |
| JWT_EXPIRATION | int | 86400 | token过期时间（秒） |
| JWT_ALGORITHM | string | HS256 | 签名算法 |
| JWT_ISSUER | string | webserver | token签发者 |

### 10.2 HTTP认证头

**客户端请求时需要在HTTP头中包含**：

```
Authorization: Bearer <your-token-here>
```

**示例**：

```
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c
```

**实现说明**：
- 客户端在登录成功后获取token
- 后续请求时在Authorization头中携带token
- 服务器端解析并验证token
- 验证失败返回401状态码

## 11. 部署考虑

### 11.1 生产环境

- 使用环境变量存储JWT密钥
- 配置合理的token过期时间
- 实现密钥轮换机制
- 启用HTTPS

### 11.2 开发环境

- 使用固定的测试密钥
- 可适当延长token过期时间
- 开启详细的日志

## 12. 监控与维护

### 12.1 日志记录

- 记录token生成和验证事件
- 记录认证失败的原因
- 记录token过期和刷新

### 12.2 监控指标

- 认证成功率
- token验证时间
- 异常认证尝试次数

## 13. 扩展可能性

### 13.1 未来功能

- **多因素认证**：集成MFA
- **权限管理**：基于JWT的细粒度权限
- **社交登录**：支持OAuth2.0集成
- **单点登录**：实现SSO系统

### 13.2 微服务支持

- **服务间认证**：使用JWT进行服务间通信
- **统一认证中心**：独立的认证服务

## 14. 风险评估

### 14.1 潜在风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 密钥泄露 | 身份伪造 | 定期轮换密钥、使用环境变量 |
| token劫持 | 会话劫持 | 使用HTTPS、合理的过期时间 |
| 性能问题 | 验证延迟 | 优化验证逻辑、缓存验证结果 |
| 兼容性问题 | 客户端错误 | 充分测试、提供清晰的错误信息 |

### 14.2 风险等级

| 风险 | 等级 | 优先级 |
|------|------|--------|
| 密钥泄露 | 高 | 高 |
| token劫持 | 高 | 高 |
| 性能问题 | 中 | 中 |
| 兼容性问题 | 低 | 低 |

## 15. 结论

集成JWT到现有C++ HTTP服务器是一个合理且必要的改进，可以提高系统的安全性、可扩展性和维护性。推荐使用jwt-cpp库，通过分步骤实现，可以平滑过渡到基于token的认证系统。

### 关键成功因素

1. **正确的库选择**：选择适合项目的JWT库
2. **安全的密钥管理**：确保JWT密钥的安全存储和轮换
3. **合理的token设计**：设计适合业务需求的token结构
4. **完整的测试**：确保认证流程的正确性和安全性
5. **良好的文档**：为后续维护和扩展提供清晰的文档

## 16. 参考资料

1. [jwt-cpp GitHub仓库](https://github.com/Thalhammer/jwt-cpp)
2. [RFC 7519 - JSON Web Token (JWT)](https://tools.ietf.org/html/rfc7519)
3. [JWT最佳实践](https://auth0.com/blog/a-look-at-the-latest-draft-for-jwt-bcp/)
4. [C++ HTTP服务器项目文档](https://github.com/your-project/documentation)

---

**文档版本**：1.1
**创建日期**：2026-02-06
**最后更新**：2026-02-06
**作者**：系统生成