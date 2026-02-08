#include"HttpServer.h"
#include"../mysql/sqlconnpool.h"
#include"../http/include/handler/AppHandlers.h"
#include"../http/include/router/Router.h"
#include"../services/include/AuthService.h"
#include"../services/include/DownloadService.h"
#include"../services/include/StaticFileService.h"
#include"../views/include/IndexPageHandler.h"
#include"../views/include/WelcomePageHandler.h"
#include"../views/include/LoginPageHandler.h"
#include"../views/include/RegisterPageHandler.h"
#include"../views/include/PicturePageHandler.h"
#include"../views/include/VideoPageHandler.h"
#include"../views/include/IPageHandler.h"
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
  SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connpoolnum);
  
  // 初始化HttpFacade
  http_facade_ = std::make_shared<HttpFacade>();
  
  // 初始化路由器并通过HttpFacade设置
  auto router = std::make_shared<Router>();
  http_facade_->SetRouter(router);
  
  // 注册路由
  SetupRoutes(*router);
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
  // 检查连接是否为空
  if (!conn) {
    LOGERROR("连接为空，无法处理消息");
    return;
  }
  
  LOGINFO("处理了(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+")的数据.");
  
  // 获取连接的输入缓冲区和输出缓冲区
  BufferBlock& inputbuffer = conn->getInputBuffer();
  BufferBlock& outputbuffer = conn->getOutputBuffer();
  
  // 检查缓冲区是否有可读数据
  size_t readable_bytes = inputbuffer.readableBytes();
  if (readable_bytes == 0) {
    LOGINFO("缓冲区无数据，跳过处理");
    return;
  }
  
  // 检查peek指针是否有效
  const char* peek_ptr = inputbuffer.peek();
  if (!peek_ptr) {
    LOGERROR("缓冲区peek指针无效，无法处理消息");
    return;
  }
  
  // 将缓冲区数据转换为字符串供解析器使用
  std::string request_data(peek_ptr, readable_bytes);
  
  // 检查http_facade_是否初始化
  if (!http_facade_) {
    LOGERROR("HttpFacade未初始化，无法处理HTTP请求");
    return;
  }
  
  // 创建响应对象
  HttpResponse response;
  
  // 使用HttpFacade处理HTTP请求
  std::unique_ptr<IHttpMessage> message;
  HttpServerResult result = http_facade_->Process(request_data, message, response);
  
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
    if (keep_alive) {
      response.SetHeader("Connection", "keep-alive");
    } else {
      response.SetHeader("Connection", "close");
      conn->setCloseOnSendComplete(true);
    }
    
    // 序列化响应
    std::string response_data = response.Serialize();
    
    // 检查响应数据是否为空
    if (response_data.empty()) {
      LOGERROR("响应数据为空，无法发送响应");
      // 设置默认的404响应
      response.SetStatusCode(HttpStatusCode::NOT_FOUND);
      response.SetHeader("Content-Type", "text/plain");
      response.SetHeader("Connection", "close");
      response.SetBody("Not Found");
      response_data = response.Serialize();
    }
    
    // 将响应数据写入输出缓冲区
    outputbuffer.append(response_data.c_str(), response_data.size());
    
    // 发送响应
    conn->send();
    LOGINFO("HTTP响应已发送 - 状态码: " + std::to_string(response.getStatusCodeInt()) +" 响应数据大小: " + std::to_string(response_data.size()));
    
    // 注意：不再立即关闭连接，而是等待数据发送完毕后通过回调函数关闭
    // 连接的关闭由Connection::writecallback方法在数据发送完毕后处理
    // 如果不是 keep-alive，Connection会在数据发送完毕后自动关闭
    
  } else if (result == HttpServerResult::NEED_MORE_DATA) {
    // 需要更多数据，等待下次接收
    LOGINFO("HTTP请求数据不完整，等待更多数据");
  } else {
    // 处理错误
    LOGERROR("HTTP请求处理失败，错误码: " + std::to_string(static_cast<int>(result)));
    
    // 根据错误码生成相应的错误响应
    HttpResponse error_response;
    error_response.SetHeader("Content-Type", "text/plain");
    error_response.SetHeader("Connection", "close");
    
    switch (result) {
      case HttpServerResult::SSL_HANDSHAKE_FAILED:
        error_response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        error_response.SetBody("SSL Handshake Failed");
        break;
      case HttpServerResult::PARSE_FAILED:
        error_response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        error_response.SetBody("Bad Request");
        break;
      case HttpServerResult::VALIDATION_FAILED:
        error_response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        error_response.SetBody("Validation Failed");
        break;
      case HttpServerResult::ROUTING_FAILED:
        error_response.SetStatusCode(HttpStatusCode::NOT_FOUND);
        error_response.SetBody("Not Found");
        break;
      default:
        error_response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        error_response.SetBody("Bad Request");
        break;
    }
    
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
// 设置路由
void HttpServer::SetupRoutes(Router& router) {
  // 初始化页面处理器集合
  auto pageHandlers = std::make_shared<std::unordered_map<std::string, std::shared_ptr<IPageHandler>>>();
  (*pageHandlers)["/index.html"] = std::make_shared<IndexPageHandler>(static_path_);
  (*pageHandlers)["/welcome.html"] = std::make_shared<WelcomePageHandler>(static_path_);
  (*pageHandlers)["/login.html"] = std::make_shared<LoginPageHandler>(static_path_);
  (*pageHandlers)["/register.html"] = std::make_shared<RegisterPageHandler>(static_path_);
  (*pageHandlers)["/picture.html"] = std::make_shared<PicturePageHandler>(static_path_);
  (*pageHandlers)["/video.html"] = std::make_shared<VideoPageHandler>(static_path_);
  
  // 创建页面路由处理器（使用lambda捕获pageHandlers的shared_ptr）
  auto pageRouteHandler = [pageHandlers](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::GET) {
      return false;
    }
    
    std::string path = request->GetPath();
    if (path == "/") path = "/index.html"; // 根路径映射到 index.html
    
    auto it = pageHandlers->find(path);
    if (it != pageHandlers->end() && it->second) {
      it->second->Handle(request, response);
      return true;
    }
    
    return false;
  };
  
  // 注册业务API路由
  router.Post("/register", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
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
  });
  
  router.Post("/login", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
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
    auto login_result = AuthService::HandleLogin(username, password);
    
    // 生成JSON响应
    if (login_result) {
      // 登录成功，获取token
      std::string token = login_result.value();
      // 在响应中包含token
      response.SetHeader("Content-Type", "application/json");
      response.SetStatusCode(HttpStatusCode::OK);
      std::string json_response = "{\"success\":true,\"message\":\"登录成功\",\"token\":\"" + token + "\"}";
      response.SetBody(json_response);
      response.SetHeader("Content-Length", std::to_string(response.GetBodyLength()));
    } else {
      SetJsonResponse(response, false, "登录失败，用户名或密码错误", HttpStatusCode::UNAUTHORIZED);
    }
    
    return true;
  });
  router.Get("/favicon.ico", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    response.SetStatusCode(HttpStatusCode::NO_CONTENT);
    response.SetHeader("Content-Type", "image/x-icon");
    return true;
  });
  router.Get("/download/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    // 添加静态路径到路由参数中，供处理器使用
    RouteParams new_params = params;
    new_params.params_["static_path"] = static_path_;
    return DownloadService::HandleDownload(request, response, static_path_);
  });
  
  // 注册静态文件路由
  router.Get("/images/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  
  router.Get("/video/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  
  // 注册页面路由（使用lambda表达式）
  router.Get("/", pageRouteHandler);
  router.Get("/index.html", pageRouteHandler);
  router.Get("/welcome.html", pageRouteHandler);
  router.Get("/login.html", pageRouteHandler);
  router.Get("/register.html", pageRouteHandler);
  router.Get("/picture.html", pageRouteHandler);
  router.Get("/video.html", pageRouteHandler);
}

// 处理HTTP请求的辅助函数
void HttpServer::ProcessRequest(HttpRequest* request, HttpResponse& response) {
  // 设置响应版本
  response.SetVersion(request->GetVersion());
  
  // 注意：HttpServer不再自行实现路由功能，而是通过HttpFacade提供的接口使用路由
  // 路由匹配和处理已经在HttpFacade::Process()中执行
  
  // 这里可以添加一些额外的处理逻辑（如果需要）
  
  // 检查响应是否已经设置了状态码
  if (response.getStatusCodeInt() == 0) {
    // 如果没有设置状态码，说明路由处理失败，返回404错误
    response.SetStatusCode(HttpStatusCode::NOT_FOUND);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Not Found");
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