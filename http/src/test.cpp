#include <iostream>
#include <memory>
#include <string>

#include "HttpServer.h"
#include "router/Router.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "observer/IHttpObserver.h"
#include "observer/LoggingObserver.h"

namespace {
class PrintObserver : public IHttpObserver {
public:
  void OnMessage(const IHttpMessage& msg) override {
    std::cout << "[Observer] 收到消息，类型="
              << (msg.IsRequest() ? "Request" : "Response") << '\n';
  }
};

// ========== 路由处理器 ==========

// 1. ECHO路由处理器（演示多个查询参数）
bool EchoHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理ECHO路由\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
        
        // 处理单个查询参数（返回第一个值）
        auto name = params.GetQueryParam("name");
        if (name.has_value()) {
            std::cout << "  查询参数 name (第一个值): " << name.value() << '\n';
        }
        
        // 处理多个同名查询参数
        auto tags = params.GetQueryParams("tag");
        if (!tags.empty()) {
            std::cout << "  查询参数 tag (所有值): ";
            for (size_t i = 0; i < tags.size(); ++i) {
                std::cout << tags[i];
                if (i < tags.size() - 1) std::cout << ", ";
            }
            std::cout << '\n';
        }
        
        // 获取所有查询参数
        const auto& allParams = params.GetAllQueryParams();
        if (!allParams.empty()) {
            std::cout << "  所有查询参数:\n";
            for (const auto& pair : allParams) {
                std::cout << "    " << pair.first << ": ";
                for (size_t i = 0; i < pair.second.size(); ++i) {
                    std::cout << pair.second[i];
                    if (i < pair.second.size() - 1) std::cout << ", ";
                }
                std::cout << '\n';
            }
        }
    }
    return true;
}

// 2. 用户路由处理器（带单个路径参数）
bool UserHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理用户路由\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
        
        // 提取路径参数
        auto userId = params.GetParam("id");
        if (userId.has_value()) {
            std::cout << "  用户ID: " << userId.value() << '\n';
        }
    }
    return true;
}

// 3. 用户文章路由处理器（带多个路径参数）
bool UserArticleHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理用户文章路由\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
        
        auto userId = params.GetParam("userId");
        auto articleId = params.GetParam("articleId");
        if (userId.has_value()) {
            std::cout << "  用户ID: " << userId.value() << '\n';
        }
        if (articleId.has_value()) {
            std::cout << "  文章ID: " << articleId.value() << '\n';
        }
    }
    return true;
}

// 4. 静态文件路由处理器（通配符）
bool StaticFileHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理静态文件路由\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
        
        // 获取通配符匹配的部分
        std::string wildcard = params.GetWildcard();
        if (!wildcard.empty()) {
            std::cout << "  文件路径: " << wildcard << '\n';
        }
    }
    return true;
}

// 5. API路由处理器
bool ApiHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理API路由\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
    }
    return true;
}

// 6. POST数据处理器
bool PostDataHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理POST数据\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
        std::cout << "  Body长度: " << req->GetBodyLength() << '\n';
        if (req->GetBodyLength() > 0) {
            std::cout << "  Body内容: " << req->GetBody() << '\n';
        }
    }
    return true;
}

// 7. PUT数据处理器
bool PutDataHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理PUT数据\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
    }
    return true;
}

// 8. DELETE数据处理器
bool DeleteDataHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理DELETE请求\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
    }
    return true;
}

// 9. PATCH数据处理器
bool PatchDataHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 处理PATCH请求\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Method: " << req->GetMethodString() << '\n';
    }
    return true;
}

// 10. 404错误处理器
bool NotFoundHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 404 - 路由未找到\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  请求路径: " << req->GetPath() << '\n';
        std::cout << "  请求方法: " << req->GetMethodString() << '\n';
    }
    return true;
}

// 11. 405错误处理器
bool MethodNotAllowedHandler(IHttpMessage& message, const RouteParams& params) {
    std::cout << "[Router] 405 - 方法不允许\n";
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "  请求路径: " << req->GetPath() << '\n';
        std::cout << "  请求方法: " << req->GetMethodString() << '\n';
    }
    return true;
}

// ========== 中间件 ==========

// 全局日志中间件
bool LoggingMiddleware(IHttpMessage& message) {
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "[Middleware] 记录请求: " 
                  << req->GetMethodString() << " " 
                  << req->GetPath() << '\n';
    }
    return true;
}

// API认证中间件（示例）
bool ApiAuthMiddleware(IHttpMessage& message) {
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "[Middleware] API认证检查: " << req->GetPath() << '\n';
        // 这里可以检查API密钥、JWT令牌等
        auto apiKey = req->GetHeader("X-API-Key");
        if (!apiKey.has_value()) {
            std::cout << "  警告: 缺少API密钥\n";
        } else {
            std::cout << "  API密钥: " << apiKey.value() << '\n';
        }
    }
    return true;
}

// 请求时间记录中间件
bool TimingMiddleware(IHttpMessage& message) {
    if (auto* req = dynamic_cast<HttpRequest*>(&message)) {
        std::cout << "[Middleware] 请求时间记录: " << req->GetPath() << '\n';
    }
    return true;
}
}  // namespace

// 测试函数：处理单个HTTP请求
bool TestRequest(HttpServer& server, const std::string& raw, const std::string& testName) {
    std::cout << "\n========== 测试: " << testName << " ==========\n";
    
    std::unique_ptr<IHttpMessage> message;
    HttpServerResult result = server.Process(raw, message);
    
    if (result != HttpServerResult::SUCCESS) {
        std::cerr << "HTTP处理失败，错误码=" << static_cast<int>(result) << '\n';
        return false;
    }
    
    // 输出解析结果
    if (auto* req = dynamic_cast<HttpRequest*>(message.get())) {
        std::cout << "解析结果:\n";
        std::cout << "  Method: " << req->GetMethodString() << '\n';
        std::cout << "  Path: " << req->GetPath() << '\n';
        std::cout << "  Version: " << req->GetVersionStr() << '\n';
    }
    
    std::cout << "序列化输出：\n" << message->Serialize() << '\n';
    return true;
}

int main() {
  // 创建HTTP服务器实例（外观模式）
  HttpServer server;

  // 配置服务器：默认不启用SSL（可根据需要启用）
  // server.ConfigureServer(true, "server.crt", "server.key");  // 启用HTTPS
  server.ConfigureServer(false);

  // 添加日志观察者：使用Logger系统记录各个处理阶段的事件
  auto logging_observer = std::make_shared<LoggingObserver>();
  server.AddObserver(logging_observer);

  // 可选：添加其他观察者（如PrintObserver）
  // auto observer = std::make_shared<PrintObserver>();
  // server.AddObserver(observer);

  // 创建新的路由器
  auto router = std::make_shared<Router>();

  // ========== 添加全局中间件 ==========
  router->AddMiddleware(LoggingMiddleware);
  router->AddMiddleware(TimingMiddleware);

  // ========== 注册基础路由 ==========
  // 注意：建议先注册更具体的路由，再注册通用路由，以确保更具体的路由优先匹配
  // 例如：先注册 /user/:userId/article/:articleId，再注册 /user/:id
  
  // GET路由
  router->Get("/demo/echo", EchoHandler);
  // 先注册更具体的路由（多个路径参数）- 这样匹配时会优先匹配更具体的路由
  router->Get("/user/:userId/article/:articleId", UserArticleHandler);
  // 再注册通用路由（单个路径参数）
  router->Get("/user/:id", UserHandler);
  router->Get("/static/*", StaticFileHandler);
  
  // POST路由
  router->Post("/api/v1/test", ApiHandler);
  router->Post("/api/v1/data", PostDataHandler);
  
  // PUT路由
  router->Put("/api/v1/resource/:id", PutDataHandler);
  
  // DELETE路由
  router->Delete("/api/v1/resource/:id", DeleteDataHandler);
  
  // PATCH路由
  router->Patch("/api/v1/resource/:id", PatchDataHandler);
  
  // HEAD路由
  router->Head("/api/v1/info", ApiHandler);
  
  // OPTIONS路由
  router->Options("/api/v1/options", ApiHandler);

  // ========== 创建路由组 ==========
  
  // API v1 路由组（带前缀和中间件）
  auto apiV1Group = router->CreateGroup("/api/v1");
  apiV1Group->AddMiddleware(ApiAuthMiddleware);
  router->AddRouteInGroup(apiV1Group, HttpMethod::GET, "/users", ApiHandler);
  router->AddRouteInGroup(apiV1Group, HttpMethod::POST, "/users", PostDataHandler);
  router->AddRouteInGroup(apiV1Group, HttpMethod::PUT, "/users/:id", PutDataHandler);
  router->AddRouteInGroup(apiV1Group, HttpMethod::DELETE, "/users/:id", DeleteDataHandler);
  
  // API v2 路由组
  auto apiV2Group = router->CreateGroup("/api/v2");
  router->AddRouteInGroup(apiV2Group, HttpMethod::GET, "/products", ApiHandler);
  router->AddRouteInGroup(apiV2Group, HttpMethod::GET, "/products/:id", ApiHandler);

  // ========== 添加路径特定中间件 ==========
  router->AddMiddlewareForPath("/api/v1/users", ApiAuthMiddleware);

  // ========== 设置错误处理器 ==========
  router->SetNotFoundHandler(NotFoundHandler);
  router->SetMethodNotAllowedHandler(MethodNotAllowedHandler);

  // 设置路由器到服务器
  server.SetRouter(router);

  // ========== 执行测试用例 ==========
  
  bool allTestsPassed = true;
  
  // 测试1: GET请求 - 基础路由（多个查询参数，包括同名参数）
  std::string test1 = 
      "GET /demo/echo?name=cursor&age=25&tag=red&tag=blue&tag=green&category=tech HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Accept: */*\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test1, "GET基础路由（多个查询参数，包括同名参数）");
  
  // 测试2: GET请求 - 路径参数
  std::string test2 = 
      "GET /user/123 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Accept: */*\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test2, "GET路径参数路由");
  
  // 测试3: GET请求 - 多个路径参数
  std::string test3 = 
      "GET /user/456/article/789 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Accept: */*\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test3, "GET多个路径参数");
  
  // 测试4: GET请求 - 通配符路由
  std::string test4 = 
      "GET /static/css/style.css HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Accept: */*\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test4, "GET通配符路由");
  
  // 测试5: POST请求
  std::string test5 = 
      "POST /api/v1/data HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 15\r\n"
      "\r\n"
      "{\"key\":\"value\"}";
  allTestsPassed &= TestRequest(server, test5, "POST请求（带Body）");
  
  // 测试6: PUT请求
  std::string test6 = 
      "PUT /api/v1/resource/100 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test6, "PUT请求");
  
  // 测试7: DELETE请求
  std::string test7 = 
      "DELETE /api/v1/resource/200 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test7, "DELETE请求");
  
  // 测试8: PATCH请求
  std::string test8 = 
      "PATCH /api/v1/resource/300 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test8, "PATCH请求");
  
  // 测试9: HEAD请求
  std::string test9 = 
      "HEAD /api/v1/info HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test9, "HEAD请求");
  
  // 测试10: OPTIONS请求
  std::string test10 = 
      "OPTIONS /api/v1/options HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test10, "OPTIONS请求");
  
  // 测试11: 路由组 - API v1
  std::string test11 = 
      "GET /api/v1/users HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "X-API-Key: test-key-12345\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test11, "路由组 - API v1 GET");
  
  // 测试12: 路由组 - API v1 POST
  std::string test12 = 
      "POST /api/v1/users HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "X-API-Key: test-key-12345\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test12, "路由组 - API v1 POST");
  
  // 测试13: 路由组 - API v2
  std::string test13 = 
      "GET /api/v2/products HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test13, "路由组 - API v2");
  
  // 测试14: 404错误 - 不存在的路由
  std::string test14 = 
      "GET /nonexistent/route HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test14, "404错误处理");
  
  // 测试15: 405错误 - 方法不允许
  std::string test15 = 
      "POST /demo/echo HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "User-Agent: http-demo/0.1\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  allTestsPassed &= TestRequest(server, test15, "405错误处理");

  // 输出测试结果
  std::cout << "\n========== 测试完成 ==========\n";
  if (allTestsPassed) {
    std::cout << "所有测试通过！\n";
    return 0;
  } else {
    std::cout << "部分测试失败！\n";
    return 1;
  }
}

