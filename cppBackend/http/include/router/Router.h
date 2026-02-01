#pragma once

#include "handler/IRequestHandler.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <shared_mutex>
#include <optional>

// 路由参数：从URL路径中提取的参数和查询参数
struct RouteParams {
  std::unordered_map<std::string, std::string> params_;  // 路径参数，如 /user/:id -> {"id": "123"}
  std::vector<std::string> wildcards_;                    // 通配符匹配的部分，如 /static/* -> ["file.css"]
  std::unordered_map<std::string, std::vector<std::string>> queryParams_;  // 查询参数，如 ?name=value&age=20
  
  // 获取路径参数值
  std::optional<std::string> GetParam(const std::string& key) const;
  
  // 获取通配符部分
  std::string GetWildcard() const;
  
  // 获取查询参数值（单个值，如果有多个同名参数则返回第一个）
  std::optional<std::string> GetQueryParam(const std::string& key) const;
  
  // 获取查询参数的所有值（支持同名参数）
  std::vector<std::string> GetQueryParams(const std::string& key) const;
  
  // 检查是否存在查询参数
  inline bool HasQueryParam(const std::string& key) const {
    return queryParams_.find(key) != queryParams_.end();
  }
  
  // 获取所有查询参数
  inline const std::unordered_map<std::string, std::vector<std::string>>& GetAllQueryParams() const {
    return queryParams_;
  }
  
  // 清空所有参数
  void Clear();
};

// 路由处理器类型：接收请求、响应和参数，返回是否继续处理
using RouteHandler = std::function<bool(IHttpMessage&, HttpResponse&, const RouteParams&)>;

// 中间件类型：接收请求，返回是否继续处理
using Middleware = std::function<bool(IHttpMessage&)>;

// 路由匹配结果：包含处理器、参数和支持的方法列表
struct RouteResult {
  RouteHandler handler = nullptr;                // 匹配到的处理器
  RouteParams params;                             // 提取的参数
  std::vector<HttpMethod> allowedMethods;        // 该路径支持的所有方法（用于405处理）
  bool pathMatched = false;                       // 路径是否匹配（无论方法是否支持）
  
  // 检查是否成功匹配
  bool IsSuccess() const { return handler != nullptr; }
};

// 路由匹配结果枚举
enum class RouteMatchResult {
  SUCCESS,                    // 匹配成功
  NOT_FOUND,                  // 路径不匹配
  METHOD_NOT_ALLOWED,         // 路径匹配但方法不允许
  VALIDATION_FAILED,          // 路径验证失败
  MIDDLEWARE_REJECTED         // 中间件拒绝
};

// 路由匹配信息结构体
struct RouteMatchInfo {
  RouteMatchResult result;                        // 匹配结果
  RouteHandler handler = nullptr;                 // 匹配到的处理器（如果成功）
  RouteParams params;                             // 提取的参数（如果成功）
  std::vector<HttpMethod> allowedMethods;         // 允许的方法列表（用于405处理）
};

// 路由节点：Trie树节点，用于高效路由匹配
class RouteNode {
public:
  RouteNode();
  ~RouteNode() = default;
  
  // 添加路由：path可以是精确路径、参数路径(:param)或通配符(*)
  void AddRoute(HttpMethod method, const std::string& path, RouteHandler handler);
  
  // 匹配路由：返回匹配的处理器和参数（优化版本，一次返回完整结果）
  RouteResult MatchRoute(HttpMethod method, const std::string& path) const;
  
  // 获取所有支持的方法（用于405错误）
  std::vector<HttpMethod> GetAllowedMethods(const std::string& path) const;

private:
  // 递归匹配辅助函数（支持回溯，优先匹配更具体的路由）
  RouteResult MatchRouteRecursive(
      HttpMethod method, const std::vector<std::string_view>& segments,
      size_t segmentIndex, RouteParams& params,
      std::vector<std::string>& currentParamValues) const;

private:
  // 精确路径匹配：path -> method -> handler
  // 精确路径匹配：path -> method -> handler和参数名列表
  std::unordered_map<std::string, std::unordered_map<HttpMethod, std::pair<RouteHandler, std::vector<std::string>>>> exactRoutes_;
  
  // 通配符路径匹配：method -> handler和参数名列表
  std::unordered_map<HttpMethod, std::pair<RouteHandler, std::vector<std::string>>> wildcardRoutes_;
  
  // 子节点：用于路径前缀匹配
  std::unordered_map<std::string, std::unique_ptr<RouteNode>> children_;
  
  // 参数节点：仅表示存在一个参数段，不存储参数名
  std::unique_ptr<RouteNode> paramNode_;
  
  // 通配符节点
  std::unique_ptr<RouteNode> wildcardNode_;
  
  // 解析路径段
  static void SplitPathToViews(std::string_view path, std::vector<std::string_view>& segments);
};

// 路由组：用于组织具有公共前缀或中间件的路由
class RouteGroup {
public:
  RouteGroup(const std::string& prefix = "");
  
  // 设置前缀
  void SetPrefix(const std::string& prefix);
  
  // 添加中间件
  void AddMiddleware(Middleware middleware);
  
  // 获取前缀
  std::string GetPrefix() const { return prefix_; }
  
  // 获取中间件列表
  const std::vector<Middleware>& GetMiddlewares() const { return middlewares_; }

private:
  std::string prefix_;
  std::vector<Middleware> middlewares_;
};

/**
 * Router类：高级路由系统
 * 职责：路由匹配和分发，不负责错误处理
 * - 支持参数、通配符、分组、中间件等
 * - 提供路由匹配功能（MatchRoute）
 * - 错误处理由HttpServer层负责
 */
class Router {
public:
  Router();
  ~Router() = default;
  
  // 处理请求
  bool Handle(IHttpMessage& message, HttpResponse& response);
  
  // 路由匹配：返回匹配结果信息（不执行处理器）
  RouteMatchInfo MatchRoute(HttpRequest& request);
  
  // ========== 路由注册接口 ==========
  
  // 注册路由（支持所有HTTP方法）
  void AddRoute(HttpMethod method, const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册GET路由
  void Get(const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册POST路由
  void Post(const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册PUT路由
  void Put(const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册DELETE路由
  void Delete(const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册PATCH路由
  void Patch(const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册HEAD路由
  void Head(const std::string& path, RouteHandler handler);
  
  // 便捷方法：注册OPTIONS路由
  void Options(const std::string& path, RouteHandler handler);
  
  // ========== 路由分组接口 ==========
  
  // 创建路由组
  std::shared_ptr<RouteGroup> CreateGroup(const std::string& prefix = "");
  
  // 在路由组中注册路由
  void AddRouteInGroup(std::shared_ptr<RouteGroup> group, HttpMethod method,
                       const std::string& path, RouteHandler handler);
  
  // ========== 中间件接口 ==========
  
  // 添加全局中间件
  void AddMiddleware(Middleware middleware);
  
  // 为特定路径添加中间件
  void AddMiddlewareForPath(const std::string& path, Middleware middleware);
  
  // ========== 工具接口 ==========
  
  // 清空所有路由
  void Clear();
  
  // 验证路径格式
  static bool ValidatePath(const std::string& path);

private:
  // 路由根节点
  std::unique_ptr<RouteNode> rootNode_;
  
  // 静态路由快速查找表（不含参数和通配符的路由）
  // 存储格式：path -> method -> (handler, paramNames)
  // 注意：静态路由的 paramNames 应该为空，但为了与动态路由保持一致，使用相同的存储格式
  std::unordered_map<std::string, std::unordered_map<HttpMethod, std::pair<RouteHandler, std::vector<std::string>>>> staticRoutes_;
  
  // 全局中间件
  std::vector<Middleware> globalMiddlewares_;
  
  // 路径特定中间件：path -> middlewares
  std::unordered_map<std::string, std::vector<Middleware>> pathMiddlewares_;
  
  // 线程安全锁
  mutable std::shared_mutex mutex_;
  
  // 执行中间件链
  bool ExecuteMiddlewares(IHttpMessage& message, const std::vector<Middleware>& middlewares) const;
  
  // 从请求中提取路径（去除查询字符串）
  static std::string ExtractPath(const HttpRequest& request);
  
  // 从请求中提取查询参数并填充到RouteParams中
  static void ExtractQueryParams(const HttpRequest& request, RouteParams& params);
  
  // 路径规范化：去除重复的斜杠，可选地去除尾部斜杠
  static std::string NormalizePath(const std::string& path, bool removeTrailingSlash = true);
  
  // 检查路径是否包含参数或通配符
  static bool IsStaticPath(const std::string& path);
  
  // 【新增】将HttpMethod转换为字符串（用于调试和日志）
  static std::string HttpMethodToString(HttpMethod method);
};

