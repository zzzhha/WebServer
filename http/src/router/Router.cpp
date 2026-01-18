#include "router/Router.h"

#include <algorithm>
#include <sstream>
#include <cctype>
#include <unordered_set>

// ========== 工具函数实现 ==========

std::string Router::NormalizePath(const std::string& path, bool removeTrailingSlash) {
  if (path.empty()) {
    return "/";
  }
  
  std::string normalized;
  normalized.reserve(path.size());
  
  bool lastWasSlash = false;
  for (char c : path) {
    // 【改进】合并连续的斜杠，提高路径匹配的鲁棒性
    if (c == '/') {
      if (!lastWasSlash) {
        normalized += c;
        lastWasSlash = true;
      }
      // 跳过连续的斜杠
    } else {
      normalized += c;
      lastWasSlash = false;
    }
  }
  
  // 确保路径以/开头
  if (normalized.empty() || normalized[0] != '/') {
    normalized = "/" + normalized;
  }
  
  // 【改进】可选地去除尾部斜杠（除非路径就是"/"）
  // 这使得 /api/user 和 /api/user/ 能够匹配同一个路由
  if (removeTrailingSlash && normalized.length() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  
  return normalized;
}

bool Router::IsStaticPath(const std::string& path) {
  // 【优化】检查路径中是否包含参数标记(:)或通配符(*)
  // 使用范围for循环和早期返回优化性能
  for (char c : path) {
    if (c == ':' || c == '*') {
      return false;
    }
  }
  return true;
}

// ========== RouteParams 实现 ==========

std::optional<std::string> RouteParams::GetParam(const std::string& key) const {
  auto it = params_.find(key);
  if (it != params_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::string RouteParams::GetWildcard() const {
  if (wildcards_.empty()) {
    return "";
  }
  return wildcards_[0];
}

std::optional<std::string> RouteParams::GetQueryParam(const std::string& key) const {
  auto it = queryParams_.find(key);
  if (it != queryParams_.end() && !it->second.empty()) {
    return it->second[0];  // 返回第一个值
  }
  return std::nullopt;
}

std::vector<std::string> RouteParams::GetQueryParams(const std::string& key) const {
  auto it = queryParams_.find(key);
  if (it != queryParams_.end()) {
    return it->second;
  }
  return {};
}

// HasQueryParam 和 GetAllQueryParams 已内联到头文件中

void RouteParams::Clear() {
  params_.clear();
  wildcards_.clear();
  queryParams_.clear();
}

// ========== RouteNode 实现 ==========

RouteNode::RouteNode() = default;

void RouteNode::SplitPathToViews(std::string_view path, std::vector<std::string_view>& segments) {
  segments.clear();
  segments.reserve(8);  // 预分配空间
  
  if (path.empty() || path == "/") {
    segments.push_back("/");
    return;
  }
  
  size_t start = 0;
  for (size_t i = 0; i < path.length(); ++i) {
    if (path[i] == '/') {
      if (i > start) {
        segments.push_back(path.substr(start, i - start));
      }
      start = i + 1;
    }
  }
  
  // 处理最后一段
  if (start < path.length()) {
    segments.push_back(path.substr(start));
  }
}

void RouteNode::AddRoute(HttpMethod method, const std::string& path, RouteHandler handler) {
  std::vector<std::string_view> segments;
  SplitPathToViews(path, segments);
  if (segments.empty()) {
    return;
  }
  
  RouteNode* current = this;
  std::vector<std::string> paramNames; // 用于收集参数名
  
  for (size_t i = 0; i < segments.size(); ++i) {
    std::string_view segment = segments[i];
    
    // 检查是否是参数段 (:param)
    if (!segment.empty() && segment[0] == ':') {
      std::string paramName(segment.substr(1));
      if (paramName.empty()) {
        #ifdef DEBUG_ROUTER
        fprintf(stderr, "[Router Error] Invalid parameter name in path: '%s'\n", path.c_str());
        #endif
        return;  // 无效参数名
      }
      paramNames.push_back(paramName); // 收集参数名
      
      if (!current->paramNode_) {
        current->paramNode_ = std::make_unique<RouteNode>();
      }
      // 不再存储 paramName_ 在 RouteNode 中
      current = current->paramNode_.get();
    }
    // 检查是否是通配符 (*)
    else if (segment == "*") {
      if (i != segments.size() - 1) {
        // 通配符只能在最后
        #ifdef DEBUG_ROUTER
        fprintf(stderr, "[Router Error] Wildcard must be at the end of path: '%s'\n", path.c_str());
        #endif
        return;
      }
      
      if (!current->wildcardNode_) {
        current->wildcardNode_ = std::make_unique<RouteNode>();
      }
      current = current->wildcardNode_.get();
      
      // 【改进】重复路由检测：检查该方法是否已注册
      if (current->wildcardRoutes_.count(method)) { // 使用 count 代替 find != end
        #ifdef DEBUG_ROUTER
        fprintf(stderr, "[Router Warning] Route already exists: %s %s (overwriting)\n",
                HttpMethodToString(method).c_str(), path.c_str());
        #endif
      }
      
      current->wildcardRoutes_[method] = {handler, paramNames}; // 存储 handler 和 paramNames
      return;
    }
    // 精确匹配
    else {
      std::string segmentStr(segment); // 转换为 std::string 用于查找和存储
      if (current->children_.find(segmentStr) == current->children_.end()) {
        current->children_[segmentStr] = std::make_unique<RouteNode>();
      }
      current = current->children_[segmentStr].get();
    }
  }
  
  // 在最后一个节点注册路由
  // 【改进】重复路由检测：检查该路径和方法是否已注册
  if (current->exactRoutes_.count("/")) { // 使用 count 代替 find != end
    auto& methodMap = current->exactRoutes_["/"];
    if (methodMap.count(method)) {
      #ifdef DEBUG_ROUTER
      fprintf(stderr, "[Router Warning] Route already exists: %s %s (overwriting)\n",
              HttpMethodToString(method).c_str(), path.c_str());
      #endif
    }
  }
  
  current->exactRoutes_["/"][method] = {handler, paramNames}; // 存储 handler 和 paramNames
}

// 递归匹配辅助函数（优化版：一次性收集所有允许的方法）
RouteResult RouteNode::MatchRouteRecursive(
    HttpMethod method, const std::vector<std::string_view>& segments, 
    size_t segmentIndex, RouteParams& params, 
    std::vector<std::string>& currentParamValues) const {
  
  RouteResult result;
  
  // 如果已经处理完所有段，检查当前节点是否有匹配的路由
  if (segmentIndex >= segments.size()) {
    auto exactIt = exactRoutes_.find("/");
    if (exactIt != exactRoutes_.end()) {
      // 收集该节点支持的所有方法
      result.allowedMethods.reserve(exactIt->second.size());
      for (const auto& methodPair : exactIt->second) {
        result.allowedMethods.push_back(methodPair.first);
      }
      result.pathMatched = true;
      
      // 查找匹配的方法
      auto methodIt = exactIt->second.find(method);
      if (methodIt != exactIt->second.end()) {
        result.handler = methodIt->second.first; // 获取 handler
        // 根据存储的参数名列表填充params
        const auto& storedParamNames = methodIt->second.second;
        for (size_t i = 0; i < storedParamNames.size() && i < currentParamValues.size(); ++i) {
          result.params.params_[storedParamNames[i]] = currentParamValues[i];
        }
        result.params.queryParams_ = params.queryParams_; // 复制查询参数
      }
    }
    return result;
  }
  
  std::string_view segment = segments[segmentIndex];
  
  // 1. 优先尝试精确匹配（更具体的路由优先）
  std::string segmentStr(segment); // 转换为 std::string 用于查找
  auto childIt = children_.find(segmentStr);
  if (childIt != children_.end()) {
    auto childResult = childIt->second->MatchRouteRecursive(method, segments, segmentIndex + 1, params, currentParamValues);
    if (childResult.IsSuccess()) {
      return childResult;
    }
    // 即使处理器未匹配，如果路径匹配了，也要保留该信息和允许的方法
    if (childResult.pathMatched) {
      result.pathMatched = true;
      result.allowedMethods = std::move(childResult.allowedMethods);
    }
  }
  
  // 2. 尝试参数匹配
  if (paramNode_) {
    // 暂存当前路径段值（转换为 std::string）
    currentParamValues.push_back(std::string(segment));
    
    auto paramResult = paramNode_->MatchRouteRecursive(method, segments, segmentIndex + 1, params, currentParamValues);
    if (paramResult.IsSuccess()) {
      return paramResult;
    }
    
    // 回溯：移除暂存的参数值
    currentParamValues.pop_back();
    
    // 合并路径匹配信息和支持的方法（避免重复）
    if (paramResult.pathMatched) {
      result.pathMatched = true;
      // 如果result还没有方法列表，直接移动
      if (result.allowedMethods.empty()) {
        result.allowedMethods = std::move(paramResult.allowedMethods);
      } else {
        // 否则合并（去重）
        for (const auto& m : paramResult.allowedMethods) {
          if (std::find(result.allowedMethods.begin(), result.allowedMethods.end(), m) == result.allowedMethods.end()) {
            result.allowedMethods.push_back(m);
          }
        }
      }
    }
  }
  
  // 3. 尝试通配符匹配（最后尝试）
  if (wildcardNode_) {
    // 通配符匹配所有剩余的段
    std::vector<std::string> wildcardParts;
    wildcardParts.reserve(segments.size() - segmentIndex);
    for (size_t j = segmentIndex; j < segments.size(); ++j) {
      wildcardParts.push_back(std::string(segments[j]));
    }
    params.wildcards_ = wildcardParts;
    
    result.pathMatched = true;
    
    // 收集通配符节点支持的所有方法
    if (result.allowedMethods.empty()) {
      result.allowedMethods.reserve(wildcardNode_->wildcardRoutes_.size());
      for (const auto& methodPair : wildcardNode_->wildcardRoutes_) {
        result.allowedMethods.push_back(methodPair.first);
      }
    } else {
      // 合并方法列表（去重）
      for (const auto& methodPair : wildcardNode_->wildcardRoutes_) {
        if (std::find(result.allowedMethods.begin(), result.allowedMethods.end(), methodPair.first) == result.allowedMethods.end()) {
          result.allowedMethods.push_back(methodPair.first);
        }
      }
    }
    
    auto wildcardIt = wildcardNode_->wildcardRoutes_.find(method);
    if (wildcardIt != wildcardNode_->wildcardRoutes_.end()) {
      result.handler = wildcardIt->second.first; // 获取 handler
      // 根据存储的参数名列表填充params
      const auto& storedParamNames = wildcardIt->second.second;
      for (size_t i = 0; i < storedParamNames.size() && i < currentParamValues.size(); ++i) {
        result.params.params_[storedParamNames[i]] = currentParamValues[i];
      }
      result.params.wildcards_ = params.wildcards_; // 复制通配符
      result.params.queryParams_ = params.queryParams_; // 复制查询参数
    }
  }
  
  return result;
}

RouteResult RouteNode::MatchRoute(HttpMethod method, const std::string& path) const {
  RouteParams params; // 注意：params 在 MatchRouteRecursive 中会被修改
  std::vector<std::string_view> segments;
  SplitPathToViews(path, segments);
  if (segments.empty()) {
    return RouteResult{};
  }
  
  std::vector<std::string> currentParamValues; // 用于收集匹配到的参数值
  RouteResult result = MatchRouteRecursive(method, segments, 0, params, currentParamValues);
  
  // 注意：查询参数不在 MatchRoute 中提取，而是在 Router::Handle 中通过 ExtractQueryParams 提取
  // 因为查询参数需要从 HttpRequest 中获取，而不是从路径字符串中解析

  return result;
}

std::vector<HttpMethod> RouteNode::GetAllowedMethods(const std::string& path) const {
  std::vector<HttpMethod> methods;
  std::vector<std::string_view> segments;
  SplitPathToViews(path, segments);
  if (segments.empty()) {
    return methods;
  }
  
  const RouteNode* current = this;
  std::vector<std::string> tempParamValues; // 临时参数值，不用于填充
  RouteParams tempParams; // 临时参数，不用于填充
  
  RouteResult result = MatchRouteRecursive(HttpMethod::UNKNOWN, segments, 0, tempParams, tempParamValues); // 使用 UNKNOWN 方法来收集所有允许的方法
  return result.allowedMethods;
}

// ========== RouteGroup 实现 ==========

RouteGroup::RouteGroup(const std::string& prefix) : prefix_(prefix) {
  // 确保前缀以/开头
  if (!prefix_.empty() && prefix_[0] != '/') {
    prefix_ = "/" + prefix_;
  }
}

void RouteGroup::SetPrefix(const std::string& prefix) {
  prefix_ = prefix;
  if (!prefix_.empty() && prefix_[0] != '/') {
    prefix_ = "/" + prefix_;
  }
}

void RouteGroup::AddMiddleware(Middleware middleware) {
  if (middleware) {
    middlewares_.push_back(middleware);
  }
}

// ========== Router 实现 ==========

Router::Router() {
  rootNode_ = std::make_unique<RouteNode>();
}

// 路由匹配：返回匹配结果信息（不执行处理器）
RouteMatchInfo Router::MatchRoute(HttpRequest& request) {
  RouteMatchInfo matchInfo;
  matchInfo.result = RouteMatchResult::NOT_FOUND;
  
  std::string rawPath = ExtractPath(request);
  std::string path = NormalizePath(rawPath, true);  // 规范化路径
  HttpMethod method = request.GetMethod();
  
  // 验证路径
  if (!ValidatePath(path)) {
    matchInfo.result = RouteMatchResult::VALIDATION_FAILED;
    return matchInfo;
  }
  
  // 获取中间件列表
  std::vector<Middleware> globalMws;
  std::vector<Middleware> pathMws;
  
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    globalMws = globalMiddlewares_;
    auto pathIt = pathMiddlewares_.find(path);
    if (pathIt != pathMiddlewares_.end()) {
      pathMws = pathIt->second;
    }
  }
  
  // 执行中间件（调用ExecuteMiddlewares）
  IHttpMessage& message = request;
  if (!ExecuteMiddlewares(message, globalMws)) {
    matchInfo.result = RouteMatchResult::MIDDLEWARE_REJECTED;
    return matchInfo;
  }
  
  if (!ExecuteMiddlewares(message, pathMws)) {
    matchInfo.result = RouteMatchResult::MIDDLEWARE_REJECTED;
    return matchInfo;
  }
  
  // 【优化】获取一次读锁，覆盖整个查找过程
  std::shared_lock<std::shared_mutex> lock(mutex_);
  
  // 先尝试静态路由快速查找（O(1)）
  RouteParams matchedParams;
  std::vector<HttpMethod> allowedMethods;
  bool pathMatched = false;
  RouteHandler matchedHandler = nullptr;
  
  auto staticIt = staticRoutes_.find(path);
  if (staticIt != staticRoutes_.end()) {
    pathMatched = true;
    // 收集允许的方法
    allowedMethods.reserve(staticIt->second.size());
    for (const auto& methodPair : staticIt->second) {
      allowedMethods.push_back(methodPair.first);
    }
    
    auto methodIt = staticIt->second.find(method);
    if (methodIt != staticIt->second.end()) {
      matchedHandler = methodIt->second.first; // 获取静态路由的 handler
      // 静态路由没有路径参数和通配符，只需提取查询参数
      ExtractQueryParams(request, matchedParams);
    }
  } else {
    // 静态路由未找到，尝试Trie树匹配（支持参数和通配符）
    RouteResult result = rootNode_->MatchRoute(method, path);
    
    if (result.IsSuccess() || result.pathMatched) {
      pathMatched = result.pathMatched;
      allowedMethods = std::move(result.allowedMethods);
      
      if (result.IsSuccess()) {
        matchedHandler = result.handler;
        matchedParams = std::move(result.params); // 将 result.params 移动到 matchedParams
        ExtractQueryParams(request, matchedParams); // 提取查询参数
      }
    }
  }
  
  lock.unlock();
  
  // 根据匹配结果设置RouteMatchInfo
  if (matchedHandler) {
    matchInfo.result = RouteMatchResult::SUCCESS;
    matchInfo.handler = matchedHandler;
    matchInfo.params = std::move(matchedParams);
  } else if (pathMatched && !allowedMethods.empty()) {
    matchInfo.result = RouteMatchResult::METHOD_NOT_ALLOWED;
    matchInfo.allowedMethods = std::move(allowedMethods);
  } else {
    matchInfo.result = RouteMatchResult::NOT_FOUND;
  }
  
  return matchInfo;
}

bool Router::Handle(IHttpMessage& message) {
  if (!message.IsRequest()) {
    return CallNext(message);
  }
  
  auto* request = dynamic_cast<HttpRequest*>(&message);
  if (!request) {
    return false;
  }
  
  HttpResponse response;
  response.SetVersion(request->GetVersion());
  
  // 调用MatchRoute进行路由匹配（MatchRoute内部已经执行了中间件）
  RouteMatchInfo matchInfo = MatchRoute(*request);
  
  // 根据匹配结果处理
  if (matchInfo.result == RouteMatchResult::SUCCESS && matchInfo.handler) {
    // 匹配成功，执行处理器
    return matchInfo.handler(message, response, matchInfo.params);
  }
  
  // 其他情况返回false，让上层根据MatchRoute结果进行错误处理
  return false;
}

void Router::AddRoute(HttpMethod method, const std::string& path, RouteHandler handler) {
  if (!handler || !ValidatePath(path)) {
    return;
  }
  
  // 路径规范化
  std::string normalizedPath = NormalizePath(path, true);
  
  std::unique_lock<std::shared_mutex> lock(mutex_);
  
  // 如果是静态路径（不含参数和通配符），添加到快速查找表
  if (IsStaticPath(normalizedPath)) {
    staticRoutes_[normalizedPath][method] = {handler, {}}; // 静态路由没有参数名
  }
  
  // 同时添加到Trie树（作为备份和兼容性保证）
  rootNode_->AddRoute(method, normalizedPath, handler);
}

void Router::Get(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::GET, path, handler);
}

void Router::Post(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::POST, path, handler);
}

void Router::Put(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::PUT, path, handler);
}

void Router::Delete(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::DELETE, path, handler);
}

void Router::Patch(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::PATCH, path, handler);
}

void Router::Head(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::HEAD, path, handler);
}

void Router::Options(const std::string& path, RouteHandler handler) {
  AddRoute(HttpMethod::OPTIONS, path, handler);
}

std::shared_ptr<RouteGroup> Router::CreateGroup(const std::string& prefix) {
  return std::make_shared<RouteGroup>(prefix);
}

void Router::AddRouteInGroup(std::shared_ptr<RouteGroup> group, HttpMethod method,
                             const std::string& path, RouteHandler handler) {
  if (!group || !handler) {
    return;
  }
  
  std::string fullPath = group->GetPrefix();
  if (!path.empty() && path[0] == '/') {
    fullPath += path;
  } else if (!path.empty()) {
    fullPath += "/" + path;
  } else {
    fullPath += path;
  }
  
  // 先执行组的中间件，再执行路由处理器
  auto wrappedHandler = [group, handler](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    // 执行组的中间件
    for (const auto& middleware : group->GetMiddlewares()) {
      if (!middleware(message)) {
        return false;
      }
    }
    // 执行路由处理器
    return handler(message, response, params);
  };
  
  AddRoute(method, fullPath, wrappedHandler);
}

void Router::AddMiddleware(Middleware middleware) {
  if (!middleware) {
    return;
  }
  
  std::unique_lock<std::shared_mutex> lock(mutex_);
  globalMiddlewares_.push_back(middleware);
}

void Router::AddMiddlewareForPath(const std::string& path, Middleware middleware) {
  if (!middleware || !ValidatePath(path)) {
    return;
  }
  
  std::unique_lock<std::shared_mutex> lock(mutex_);
  pathMiddlewares_[path].push_back(middleware);
}


void Router::Clear() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  rootNode_ = std::make_unique<RouteNode>();
  staticRoutes_.clear();
  globalMiddlewares_.clear();
  pathMiddlewares_.clear();
}

bool Router::ValidatePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  
  // 路径必须以/开头
  if (path[0] != '/') {
    return false;
  }
  
  // 【改进】检查非法字符和路径遍历攻击
  for (size_t i = 0; i < path.length(); ++i) {
    char c = path[i];
    
    // 检查控制字符
    if (c < 32 || c == 127) {
      return false;
    }
    
    // 【安全】检查路径遍历攻击模式 (../)
    if (c == '.' && i + 1 < path.length() && path[i + 1] == '.') {
      // 检查是否是 ../ 或 ..结尾
      if (i + 2 >= path.length() || path[i + 2] == '/') {
        return false;
      }
    }
  }
  
  return true;
}

bool Router::ExecuteMiddlewares(IHttpMessage& message, 
                                const std::vector<Middleware>& middlewares) const {
  for (const auto& middleware : middlewares) {
    if (!middleware(message)) {
      return false;  // 中间件拒绝
    }
  }
  return true;
}


std::string Router::ExtractPath(const HttpRequest& request) {
  std::string path = request.GetPath();
  // 路径已经由HttpRequest解析，不包含查询字符串
  return path;
}

void Router::ExtractQueryParams(const HttpRequest& request, RouteParams& params) {
  // 【优化】直接获取HttpRequest已经解析好的查询参数，避免重复解析URL字符串
  // 这大大提高了效率，特别是对于包含大量查询参数的请求
  const auto& queryParams = request.GetAllQueryParams();
  
  // 转换为unordered_map以提高后续查找效率
  params.queryParams_.reserve(queryParams.size());
  for (const auto& [key, values] : queryParams) {
    params.queryParams_[key] = values;
  }
}

// 【新增】辅助函数：将HttpMethod转换为字符串
std::string Router::HttpMethodToString(HttpMethod method) {
  switch (method) {
    case HttpMethod::GET: return "GET";
    case HttpMethod::POST: return "POST";
    case HttpMethod::PUT: return "PUT";
    case HttpMethod::DELETE: return "DELETE";
    case HttpMethod::PATCH: return "PATCH";
    case HttpMethod::HEAD: return "HEAD";
    case HttpMethod::OPTIONS: return "OPTIONS";
    case HttpMethod::TRACE: return "TRACE";
    case HttpMethod::CONNECT: return "CONNECT";
    default: return "UNKNOWN";
  }
}

