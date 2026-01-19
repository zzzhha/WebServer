#pragma once
#include"tcpserver.h"
#include"Eventloop.h"
#include"Connection.h"
#include"ThreadPool.h"
#include"../logger/log_fac.h"
#include"Buffer.h"
#include"../http/include/core/HttpRequest.h"
#include"../http/include/core/HttpResponse.h"
#include"../http/include/core/IHttpMessage.h"
#include"../http/include/router/Router.h"
#include"../http/include/HttpFacade.h"
#include"../http/include/handler/AppHandlers.h"
#include"../views/include/NotFoundHandler.h"
#include"../views/include/MethodNotAllowedHandler.h"
#include"../views/include/BadRequestHandler.h"
#include"../views/include/ForbiddenHandler.h"
#include"../views/include/WelcomePageHandler.h"
#include"../views/include/IndexPageHandler.h"
#include"../views/include/LoginPageHandler.h"
#include"../views/include/RegisterPageHandler.h"
#include"../views/include/PicturePageHandler.h"
#include"../views/include/VideoPageHandler.h"
#include"../views/include/IPageHandler.h"
#include"../services/include/AuthService.h"
#include"../services/include/DownloadService.h"
#include <memory>
#include <fstream>
#include <sys/stat.h>
#include <vector>

/**
 * HttpServer类：HTTP服务器核心类
 * 职责：处理HTTP请求的接收、路由、处理和响应
 * 这是一个业务类，与底层网络类(TcpServer等)不同，它会根据业务需求改变
 * 继承和使用了TcpServer的接口来处理网络事件
 */
class HttpServer{
private:
  TcpServer tcpserver_;                   // TCP服务器实例
  ThreadPool threadpool_;                 // 工作线程池
  std::string static_path_;               // 静态资源路径
  std::shared_ptr<Router> router_;        // 路由管理器
  std::shared_ptr<HttpFacade> http_facade_; // HTTP处理门面
  
  // 错误处理器
  std::shared_ptr<NotFoundHandler> notFoundHandler_;                 // 404处理器
  std::shared_ptr<MethodNotAllowedHandler> methodNotAllowedHandler_; // 405处理器
  std::shared_ptr<BadRequestHandler> badRequestHandler_;             // 400处理器
  std::shared_ptr<ForbiddenHandler> forbiddenHandler_;               // 403处理器
  
  // 页面处理器集合
  std::unordered_map<std::string, std::shared_ptr<IPageHandler>> pageHandlers_;  // 路径到页面处理器的映射
  
public:
  /**
   * HttpServer构造函数
   * @param ip 服务器监听IP地址
   * @param port 服务器监听端口
   * @param timeoutMS 连接超时时间(毫秒)
   * @param OptLinger 是否启用SO_LINGER选项
   * @param sqlPort MySQL数据库端口
   * @param sqlUser MySQL用户名
   * @param sqlPwd MySQL密码
   * @param dbName 数据库名称
   * @param subthreadnum IO子线程数量
   * @param workthreadnum 工作线程数量
   * @param connpoolnum 数据库连接池大小
   * @param static_path 静态资源根路径
   */
  HttpServer(const std::string &ip,uint16_t port,int timeoutMS,bool OptLinger=true,
int sqlPort=3306,const char*sqlUser="webuser",const char*sqlPwd="12589777",const char*dbName="webserver",
int subthreadnum=6,int workthreadnum=0,int connpoolnum=12,const std::string&static_path="./html");
  
  /**
   * 析构函数
   */
  ~HttpServer();

  /**
   * 启动HTTP服务器
   */
  void start();
  
  /**
   * 停止HTTP服务器
   */
  void Stop();
  
  /**
   * 处理新客户端连接请求
   * @param conn 新连接对象
   */
  void HandleNewConnection(spConnection conn);
  
  /**
   * 处理客户端连接关闭
   * @param conn 关闭的连接对象
   */
  void HandleClose(spConnection conn);
  
  /**
   * 处理客户端连接错误
   * @param conn 错误的连接对象
   */
  void HandleError(spConnection conn);
  
  /**
   * 处理客户端请求报文
   * @param conn 连接对象(包含请求数据)
   */
  void HandleMessage(spConnection conn/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock& buffer*/);
  
  /**
   * 处理数据发送完成事件
   * @param conn 发送完成的连接对象
   */
  void HandleSendComplete(spConnection conn);
  //void HandleTimeOut(EventLoop*loop);  //epoll_wait()超时处理

private:
  /**
   * 处理HTTP请求
   * @param request HTTP请求对象
   * @param response HTTP响应对象
   */
  void ProcessRequest(HttpRequest* request, HttpResponse& response);
  
  /**
   * 设置路由规则
   */
  void SetupRoutes();
  
  /**
   * 根据文件扩展名获取Content-Type
   * @param path 文件路径
   * @return Content-Type字符串
   */
  std::string GetContentType(const std::string& path);
  
  /**
   * 处理404错误
   * @param request HTTP请求对象
   * @param response HTTP响应对象
   */
  void HandleNotFound(HttpRequest* request, HttpResponse& response);
  
  /**
   * 处理405错误
   * @param request HTTP请求对象
   * @param response HTTP响应对象
   * @param allowedMethods 允许的HTTP方法列表
   */
  void HandleMethodNotAllowed(HttpRequest* request, HttpResponse& response, const std::vector<HttpMethod>& allowedMethods);
  
  /**
   * 处理400错误
   * @param request HTTP请求对象
   * @param response HTTP响应对象
   */
  void HandleBadRequest(HttpRequest* request, HttpResponse& response);
  
  /**
   * 处理403错误
   * @param request HTTP请求对象
   * @param response HTTP响应对象
   */
  void HandleForbidden(HttpRequest* request, HttpResponse& response);
};