#include"HttpServer.h"
#include"../mysql/sqlconnpool.h"
#include"../http/include/handler/AppHandlers.h"
#include"../services/include/AuthService.h"
#include"../services/include/DownloadService.h"
#include"../services/include/StaticFileService.h"
#include<algorithm>
#include<sstream>

HttpServer::HttpServer(const std::string &ip,uint16_t port,int timeoutS,bool OptLinger,
                       int sqlPort,const char*sqlUser,const char*sqlPwd,const char*dbName,
                       int subthreadnum,int workthreadnum,int connpoolnum,const std::string&static_path)
      :tcpserver_(ip,port,subthreadnum,timeoutS,OptLinger),threadpool_(workthreadnum,"WORKS"),static_path_(static_path)
{
  // 以下代码不是必须的，业务关心什么事件，就指定相应的回调函数。
  tcpserver_.setnewconnection(std::bind(&HttpServer::HandleNewConnection, this, std::placeholders::_1));
  tcpserver_.setcloseconnection(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
  tcpserver_.seterrorconnection(std::bind(&HttpServer::HandleError, this, std::placeholders::_1));
  tcpserver_.setonmessage(std::bind(&HttpServer::HandleMessage, this, std::placeholders::_1/*, std::placeholders::_2*/));
  tcpserver_.setsendcomplete(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
  //tcpserver_.settimeout(std::bind(&HttpServer::HandleTimeOut, this, std::placeholders::_1));
LOGINFO("尝试连接数据库");
  SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connpoolnum);
LOGINFO("数据库连接成功");
  
  // 初始化HttpFacade
  http_facade_ = std::make_shared<HttpFacade>();
  
  // 初始化路由器并设置路由
  router_ = std::make_shared<Router>();
  SetupRoutes();
}
HttpServer::~HttpServer(){

}
void HttpServer::start(){
  LOGINFO("Http服务器启动");
  tcpserver_.start();
}

void HttpServer::Stop(){
  LOGINFO("Http服务器关闭");
  SqlConnPool::Instance()->ClosePool();
  //停止工作线程
  threadpool_.stop();
  //停止IO线程
  tcpserver_.stop();
}
void HttpServer::HandleNewConnection(spConnection conn){
  LOGINFO("new connection(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")ok.");
}
void HttpServer::HandleClose(spConnection conn){
  LOGINFO("connection close(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")");
}
void HttpServer::HandleError(spConnection conn){
  LOGERROR("connection error(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")");
}
void HttpServer::HandleMessage(spConnection conn/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock& buffer*/){
  LOGINFO("处理了(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")的数据.");
  
  // 获取连接的输入缓冲区和输出缓冲区
  BufferBlock& inputbuffer = conn->getInputBuffer();
  BufferBlock& outputbuffer = conn->getOutputBuffer();
  
  // 将缓冲区数据转换为字符串供解析器使用
  std::string request_data(inputbuffer.peek(), inputbuffer.readableBytes());
  
  // 使用HttpFacade处理HTTP请求
  std::unique_ptr<IHttpMessage> message;
  HttpServerResult result = http_facade_->Process(request_data, message);
  
  if (result == HttpServerResult::SUCCESS && message) {
    // 解析成功，消费已解析的字节数
    size_t consumed_bytes = http_facade_->GetConsumedBytes();
    inputbuffer.consumeBytes(consumed_bytes);
    
    // 确保这是一个请求消息
    if (!message->IsRequest()) {
      LOGERROR("收到的不是HTTP请求消息");
      return;
    }
    
    // 转换为 HttpRequest 对象
    HttpRequest* request = dynamic_cast<HttpRequest*>(message.get());
    if (!request) {
      LOGERROR("无法将消息转换为HttpRequest");
      return;
    }
    
    // 创建响应对象
    HttpResponse response;
    
    // 获取请求路径和方法
    std::string path = request->GetPath();
    std::string method = request->GetMethodString();
    
    LOGINFO("请求方法: " + method + ", 路径: " + path);
    
    // 判断是否为 keep-alive 连接
    bool keep_alive = false;
    auto connection_header = request->GetHeader("Connection");
    if (connection_header.has_value()) {
      std::string conn_value = connection_header.value();
      std::transform(conn_value.begin(), conn_value.end(), conn_value.begin(), ::tolower);
      keep_alive = (conn_value == "keep-alive");
    }
    
    // 处理请求并生成响应
    ProcessRequest(request, response);
    
    // 设置响应头
    response.SetVersion(request->GetVersion());
    if (keep_alive) {
      response.SetHeader("Connection", "keep-alive");
    } else {
      response.SetHeader("Connection", "close");
    }
    
    // 序列化响应
    std::string response_data = response.Serialize();
    
    // 将响应数据写入输出缓冲区
    outputbuffer.append(response_data.c_str(), response_data.size());
    
    // 发送响应
    conn->send();
    
    // 如果不是 keep-alive，关闭连接
    if (!keep_alive) {
      conn->closeConnection();
    }
    
    LOGINFO("HTTP响应已发送 - 状态码: " + std::to_string(response.getStatusCodeInt()));
    
  } else if (result == HttpServerResult::NEED_MORE_DATA) {
    // 需要更多数据，等待下次接收
    LOGINFO("HTTP请求数据不完整，等待更多数据");
  } else {
    // 解析错误
    LOGERROR("HTTP请求处理失败，错误码: " + std::to_string(static_cast<int>(result)));
    
    // 发送400 Bad Request响应
    HttpResponse error_response(HttpStatusCode::BAD_REQUEST);
    error_response.SetHeader("Content-Type", "text/plain");
    error_response.SetHeader("Connection", "close");
    error_response.SetBody("Bad Request");
    
    std::string error_data = error_response.Serialize();
    outputbuffer.append(error_data.c_str(), error_data.size());
    conn->send();
    
    // 关闭连接
    conn->closeConnection();
  }
}

/**
 * 路由处理器函数实现
 * 
 * 职责：作为Router和业务服务层之间的适配器
 * - 解析HTTP请求参数
 * - 调用对应的业务服务（AuthService、DownloadService等）
 * - 生成HTTP响应
 */
namespace {
  // 通用页面路由处理器：根据请求路径查找并执行对应的页面处理器
  using PageHandlersMap = std::unordered_map<std::string, std::shared_ptr<IPageHandler>>;
  
  std::function<bool(IHttpMessage&, HttpResponse&, const RouteParams&)> CreatePageRouteHandler(PageHandlersMap& pageHandlers) {
    return [&pageHandlers](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
      auto* request = dynamic_cast<HttpRequest*>(&message);
      if (!request || request->GetMethod() != HttpMethod::GET) {
        return false;
      }
      
      std::string path = request->GetPath();
      if (path == "/") path = "/index.html"; // 根路径映射到 index.html
      
      auto it = pageHandlers.find(path);
      if (it != pageHandlers.end() && it->second) {
        it->second->Handle(request, response);
        return true;
      }
      
      return false;
    };
  }
  
  // 注册路由处理器：处理POST /register请求
  bool RegisterRouteHandler(IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::POST) {
      return false;
    }
    
    // 解析POST表单数据
    std::string body = request->GetBody();
    auto form_data = ParseFormData(body);
    
    // 提取用户名和密码（使用更简洁的方式）
    std::string username = form_data.count("username") > 0 ? form_data.at("username") : "";
    std::string password = form_data.count("password") > 0 ? form_data.at("password") : "";
    
    // 调用AuthService处理注册
    bool success = AuthService::HandleRegister(username, password);
    
    // 生成JSON响应
    if (success) {
      SetJsonResponse(response, true, "注册成功", HttpStatusCode::OK);
    } else {
      SetJsonResponse(response, false, "注册失败，用户名可能已存在", HttpStatusCode::BAD_REQUEST);
    }
    
    return true;
  }
  
  // 登录路由处理器：处理POST /login请求
  bool LoginRouteHandler(IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::POST) {
      return false;
    }
    
    // 解析POST表单数据
    std::string body = request->GetBody();
    auto form_data = ParseFormData(body);
    
    // 提取用户名和密码（使用更简洁的方式）
    std::string username = form_data.count("username") > 0 ? form_data.at("username") : "";
    std::string password = form_data.count("password") > 0 ? form_data.at("password") : "";
    
    // 调用AuthService处理登录
    bool success = AuthService::HandleLogin(username, password);
    
    // 生成JSON响应
    if (success) {
      SetJsonResponse(response, true, "登录成功", HttpStatusCode::OK);
    } else {
      SetJsonResponse(response, false, "登录失败，用户名或密码错误", HttpStatusCode::UNAUTHORIZED);
    }
    
    return true;
  }
  
  // 下载路由处理器：处理GET /download/*请求
  bool DownloadRouteHandler(IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::GET) {
      return false;
    }
    
    // 调用DownloadService处理下载，使用params传递静态路径
    std::string static_path = params.params_.at("static_path");
    bool success = DownloadService::HandleDownload(request, response, static_path);
    
    return success;
  }
}

// 设置路由
void HttpServer::SetupRoutes() {
  // 初始化错误处理器
  notFoundHandler_ = std::make_shared<NotFoundHandler>(static_path_);
  methodNotAllowedHandler_ = std::make_shared<MethodNotAllowedHandler>(static_path_);
  badRequestHandler_ = std::make_shared<BadRequestHandler>(static_path_);
  forbiddenHandler_ = std::make_shared<ForbiddenHandler>(static_path_);
  
  // 初始化页面处理器集合
  pageHandlers_["/index.html"] = std::make_shared<IndexPageHandler>(static_path_);
  pageHandlers_["/welcome.html"] = std::make_shared<WelcomePageHandler>(static_path_);
  pageHandlers_["/login.html"] = std::make_shared<LoginPageHandler>(static_path_);
  pageHandlers_["/register.html"] = std::make_shared<RegisterPageHandler>(static_path_);
  pageHandlers_["/picture.html"] = std::make_shared<PicturePageHandler>(static_path_);
  pageHandlers_["/video.html"] = std::make_shared<VideoPageHandler>(static_path_);
  
  // 创建页面路由处理器（使用lambda捕获pageHandlers_）
  auto pageRouteHandler = CreatePageRouteHandler(pageHandlers_);
  
  // 注册业务API路由
        router_->Post("/register", RegisterRouteHandler);
        router_->Post("/login", LoginRouteHandler);
        router_->Get("/download/*", DownloadRouteHandler);
        
        // 注册静态文件路由
        router_->Get("/images/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
            auto* request = dynamic_cast<HttpRequest*>(&message);
            if (!request) return false;
            return StaticFileService::HandleStaticFile(request, response, static_path_);
        });
        
        router_->Get("/video/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
            auto* request = dynamic_cast<HttpRequest*>(&message);
            if (!request) return false;
            return StaticFileService::HandleStaticFile(request, response, static_path_);
        });
        
        // 注册页面路由（使用lambda表达式）
        router_->Get("/", pageRouteHandler);
        router_->Get("/index.html", pageRouteHandler);
        router_->Get("/welcome.html", pageRouteHandler);
        router_->Get("/login.html", pageRouteHandler);
        router_->Get("/register.html", pageRouteHandler);
        router_->Get("/picture.html", pageRouteHandler);
        router_->Get("/video.html", pageRouteHandler);
}

// 处理HTTP请求的辅助函数
void HttpServer::ProcessRequest(HttpRequest* request, HttpResponse& response) {
  // 设置响应版本
  response.SetVersion(request->GetVersion());
  
  if (!router_) {
    HandleBadRequest(request, response);
    return;
  }
  
  // 调用Router::MatchRoute()获取匹配结果
  RouteMatchInfo matchInfo = router_->MatchRoute(*request);
  
  // 根据匹配结果进行switch分发
  switch (matchInfo.result) {
    case RouteMatchResult::SUCCESS:
      // 匹配成功，执行处理器
      if (matchInfo.handler) {
        // 添加静态路径到路由参数中，供处理器使用
        matchInfo.params.params_["static_path"] = static_path_;
        // 执行处理器，直接传递response
        matchInfo.handler(*request, response, matchInfo.params);
        // response已经在handler中设置
      } else {
        // 如果没有handler，返回404
        HandleNotFound(request, response);
      }
      break;
      
    case RouteMatchResult::NOT_FOUND:
      HandleNotFound(request, response);
      break;
      
    case RouteMatchResult::METHOD_NOT_ALLOWED:
      HandleMethodNotAllowed(request, response, matchInfo.allowedMethods);
      break;
      
    case RouteMatchResult::VALIDATION_FAILED:
      HandleBadRequest(request, response);
      break;
      
    case RouteMatchResult::MIDDLEWARE_REJECTED:
      HandleForbidden(request, response);
      break;
      
    default:
      HandleBadRequest(request, response);
      break;
  }
}

// 错误处理方法实现
void HttpServer::HandleNotFound(HttpRequest* request, HttpResponse& response) {
  if (notFoundHandler_) {
    notFoundHandler_->Handle(request, response);
  }
}

void HttpServer::HandleMethodNotAllowed(HttpRequest* request, HttpResponse& response, 
                                        const std::vector<HttpMethod>& allowedMethods) {
  if (methodNotAllowedHandler_) {
    methodNotAllowedHandler_->Handle(request, response, allowedMethods);
  }
}

void HttpServer::HandleBadRequest(HttpRequest* request, HttpResponse& response) {
  if (badRequestHandler_) {
    badRequestHandler_->Handle(request, response);
  }
}

void HttpServer::HandleForbidden(HttpRequest* request, HttpResponse& response) {
  if (forbiddenHandler_) {
    forbiddenHandler_->Handle(request, response);
  }
}

// 根据文件扩展名返回 Content-Type（已移动到 AppHandlers，这里保留作为备用）
std::string HttpServer::GetContentType(const std::string& path) {
  return ::GetContentType(path);  // 使用 AppHandlers 中的全局函数
}

void HttpServer::HandleSendComplete(spConnection conn){

  LOGINFO("Message send complete.");

}
/*
void HttpServer::HandleTimeOut(EventLoop*loop){
  std::cout<<"EchoServer timeout."<<std::endl;

}   
*/