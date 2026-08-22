/*
#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<arpa/inet.h>
#include<sys/fcntl.h>
#include<sys/epoll.h>
#include<netinet/tcp.h>   //TCP_NODELAY 需要包含此头文件
#include"InetAddress.h"
#include"Socket.h"
#include"Epoll.h"
#include"Channel.h"
#include"Eventloop.h"
*/
#include"HttpServer.h"
#include<signal.h>
#include<filesystem>
#include<cstdlib>
#include<cctype>
#include<vector>
#include<string>

#include"../logger/xconfig.h"
#include"../logger/log_fac.h"
#include"../net/NetFactory.h"

namespace {
std::string ResolveStaticPath() {
  namespace fs = std::filesystem;
  const std::vector<fs::path> candidates = {
    fs::path("./html"),
    fs::path("../html"),
    fs::path("../../html")
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    fs::path index_file = candidate / "index.html";
    if (fs::exists(index_file, ec) && fs::is_regular_file(index_file, ec)) {
      return fs::weakly_canonical(candidate, ec).string();
    }
  }
  return "./html";
}

std::string ReadNetMode() {
  if (const char* env_mode = std::getenv("WEBSERVER_NET_MODE"); env_mode && env_mode[0] != '\0') {
    return std::string(env_mode);
  }

  XConfig conf;
  const std::vector<std::string> config_candidates = {
      "./webserver.conf",
      "../webserver.conf",
      "../../webserver.conf",
      "/etc/webserver.conf"};
  for (const auto& file : config_candidates) {
    if (conf.Read(file)) {
      return conf.Get("net.mode", "reactor");
    }
  }
  return "reactor";
}

Xlog ReadLogLevel() {
  if (const char* env_level = std::getenv("WEBSERVER_LOG_LEVEL"); env_level && env_level[0] != '\0') {
    std::string level(env_level);
    for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (level == "debug") return Xlog::DEBUG;
    if (level == "info") return Xlog::INFO;
    if (level == "warning" || level == "warn") return Xlog::WARNING;
    if (level == "error") return Xlog::ERROR;
    if (level == "fatal") return Xlog::FATAL;
  }
  return Xlog::ERROR;
}
}

HttpServer *httpserver;
void Stop(int sig){
  //调用EchoServer::stop函数停止服务
  httpserver->Stop();
  delete httpserver;
  exit(0);
}
void OnDumpSignal(int) {
  // 诊断：请求转储连接状态（由 HttpServer 内轮询线程执行，避免信号上下文直接操作）
  HttpServer::dump_requested.store(true, std::memory_order_relaxed);
}
int main(int argc ,const char*argv[]){
  if(argc<3){
    printf("usage: ip port\n");
    return -1;
  }

  LogFac::Instance().Init(false);
  LogFac::Instance().logger().SetLevel(ReadLogLevel());

  signal(SIGTERM,Stop);
  signal(SIGINT,Stop);
  signal(SIGUSR1,OnDumpSignal);

  const std::string net_mode = ReadNetMode();
  NetServerOptions net_options;
  net_options.ip = argv[1];
  net_options.port = static_cast<uint16_t>(atoi(argv[2]));
  net_options.io_threads = 6;
  net_options.tcp_timeout_seconds = 360;
  net_options.opt_linger = true;

  if (const char* env_ce = std::getenv("WEBSERVER_URING_ENTRIES")) {
    net_options.uring_entries = std::atoi(env_ce);
  }
  if (const char* env_aff = std::getenv("WEBSERVER_CPU_AFFINITY")) {
    net_options.cpu_affinity = (std::string(env_aff) == "1" || std::string(env_aff) == "true");
  }
  if (const char* env_sqp = std::getenv("WEBSERVER_SQPOLL")) {
    net_options.sqpoll = (std::string(env_sqp) == "1" || std::string(env_sqp) == "true");
  }
  if (const char* env_ctr = std::getenv("WEBSERVER_COOP_TASKRUN")) {
    net_options.coop_taskrun = (std::string(env_ctr) == "1" || std::string(env_ctr) == "true");
  }

  auto net_server = NetFactory::CreateServer(net_mode, net_options);
  httpserver=new HttpServer(std::move(net_server),3306,"webuser","12589777","webserver",4,12,ResolveStaticPath());
  httpserver->start();
  
  return 0;
}
