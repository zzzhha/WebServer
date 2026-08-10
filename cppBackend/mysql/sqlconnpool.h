#pragma once

#include<mysql/mysql.h>
#include<string>
#include<queue>
#include<mutex>
#include<atomic>
#include<semaphore.h>
#include<thread>
#include <assert.h>
#include "../logger/log_fac.h"

class SqlConnPool{
public:
  static SqlConnPool *Instance();

  MYSQL *GetConn();
  void FreeConn(MYSQL *sql);
  int GetFreeConnCount();

  void Init(const char* host,int port,const char* user,const char* pwd, const char* dbName, int connSize);
  void ClosePool();

private:
  SqlConnPool();
  ~SqlConnPool();

  // 已持有 init_mutex_ 时的关闭实现（Init 内部复用，避免死锁）
  void ClosePoolLocked();

  // 依据保存的连接参数新建一个 MySQL 连接（失败返回 nullptr）
  MYSQL *CreateNewConnection();

  int MAX_CONN_;
  int useCount_;
  int freeCount_;
  // 当前借出的连接数（配合 MAX_CONN_ 判断容量是否因死连接丢弃而丢失）
  int borrowed_{0};

  std::string host_, user_, pwd_, dbName_;
  int port_{0};

  std::queue<MYSQL *> connque_;
  std::mutex mutex_;
  // 串行化 Init / ClosePool，防止并发初始化互相踩踏（审计低危）
  std::mutex init_mutex_;
  sem_t semId_;
  // 池是否已关闭：关闭后 FreeConn 不再把连接塞回队列，避免死连接回流（审计 H15）
  std::atomic<bool> closed_{false};
  // semId_ 是否已成功初始化：ClosePool 只在初始化过后才 sem_destroy，保证幂等（审计 H15）
  std::atomic<bool> initialized_{false};

};