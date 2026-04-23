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
#include"../services/include/AuthService.h"
#include"../services/include/DownloadService.h"
#include <memory>
#include <fstream>
#include <sys/stat.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <map>
#include <chrono>
#include <unordered_map>

class TlsContext;

/**
 * HttpServer类：HTTP服务器核心类
 * 职责：处理HTTP请求的接收、路由、处理和响应
 * 这是一个业务类，与底层网络类(TcpServer等)不同，它会根据业务需求改变
 * 继承和使用了TcpServer的接口来处理网络事件
 */
class HttpServer{
private:
  struct WorkResult;

  struct PendingChunk {
    std::string data;
    uint64_t enqueue_seq{0};
    std::chrono::steady_clock::time_point enqueue_tp;
  };

  struct ConnectionWorkContext {
    std::shared_ptr<HttpFacade> facade;             //连接级的 HTTP 协议处理对象，每个连接独立实例
    std::mutex mutex;                               //保护队列和状态变量的线程安全
    std::deque<PendingChunk> queued_chunks;         //待处理的 HTTP 请求数据块队列
    size_t queued_bytes{0};                         //连接级待处理字节数，用于背压控制
    bool worker_running{false};                     //标记是否有 worker 线程正在处理该连接的数据
    uint64_t next_enqueue_seq{1};                   //保证入队顺序的序列号生成器
    uint64_t next_response_seq{1};                  //保证响应顺序的序列号生成器
    uint64_t last_applied_response_seq{0};          //记录最后应用的响应序列号
    std::map<uint64_t, WorkResult> pending_results; //按序列号存储的待回写响应结果
  };

  struct WorkResult {
    uint64_t response_seq{0};                          // 响应序列号
    std::string request_id;                            // 请求ID（调试用）
    std::string method;                                // HTTP方法
    std::string path;                                  // 请求路径
    std::string route_bucket;                          // 路由桶（用于指标统计）
    bool has_response{false};                          // 是否有响应数据
    std::string response_data;                         // 响应数据
    bool close_after_send{false};                      // 发送后是否关闭连接
    bool has_sendfile{false};                          // 是否包含文件发送
    int sendfile_fd{-1};                               // 文件描述符
    off_t sendfile_offset{0};                          // 文件偏移量
    size_t sendfile_length{0};                         // 文件长度
    bool is_error{false};                              // 是否是错误响应
    bool is_download{false};                           // 是否是下载请求
    size_t sendfile_bytes{0};                          // 已发送文件字节数
    long queue_wait_ms{-1};                            // 队列等待时间（毫秒）
    long worker_exec_ms{0};                            // worker执行时间（毫秒）
    long parse_route_ms{0};                            // 解析路由时间（毫秒）
    long business_ms{0};                               // 业务处理时间（毫秒）
    long serialize_ms{0};                              // 序列化时间（毫秒）
    std::chrono::steady_clock::time_point io_enqueue_tp;// IO入队时间点
  };

  struct RouteMetric {
    uint64_t requests{0};                            // 请求总数
    uint64_t errors{0};                                // 错误数量
    uint64_t total_pipeline_ms{0};                     // 总流水线时间（毫秒）
    uint64_t max_pipeline_ms{0};                       // 最大流水线时间（毫秒）
    uint64_t total_queue_wait_ms{0};                   // 总队列等待时间（毫秒）
    uint64_t total_worker_exec_ms{0};                  // 总worker执行时间（毫秒）
    uint64_t total_io_flush_ms{0};                     // 总IO刷新时间（毫秒）
    uint64_t total_parse_route_ms{0};                  // 总解析路由时间（毫秒）
    uint64_t total_business_ms{0};                     // 总业务处理时间（毫秒）
    uint64_t total_serialize_ms{0};                    // 总序列化时间（毫秒）
    uint64_t sendfile_requests{0};                     // 文件发送请求数
    uint64_t sendfile_bytes{0};                        // 文件发送总字节数
  };

  TcpServer tcpserver_;                   // TCP服务器实例
  ThreadPool threadpool_;                 // 工作线程池
  std::string static_path_;               // 静态资源路径
  std::shared_ptr<Router> router_;
  std::shared_ptr<TlsContext> tls_ctx_;
  std::atomic<uint64_t> request_seq_{0};
  std::mutex metrics_mutex_;
  std::unordered_map<std::string, RouteMetric> route_metrics_;
  std::atomic<uint64_t> metrics_observed_{0};
  size_t metrics_snapshot_every_{200};
  long slow_request_ms_threshold_{300};
  size_t max_work_queue_depth_{4096};
  size_t max_conn_pending_bytes_{512 * 1024};
  
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
  void HandleMessageInWorker(std::weak_ptr<Connection> weak_conn, std::shared_ptr<ConnectionWorkContext> ctx);
  void PostResultToIoLoop(std::weak_ptr<Connection> weak_conn, std::shared_ptr<ConnectionWorkContext> ctx, WorkResult result);
  void SendServiceUnavailable(spConnection conn, const std::string& reason);
  void RecordPhase3Metrics(const WorkResult& result, long io_flush_ms, long pipeline_ms);
  void MaybeLogPhase3Snapshot();
  
  /**
   * 设置路由规则
   * @param router 路由管理器实例
   */
  void SetupRoutes(Router& router);
  

};
