#pragma once

#include<mysql/mysql.h>
#include<string>
#include<queue>
#include<mutex>
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

  int MAX_CONN_;
  int useCount_;
  int freeCount_;

  std::queue<MYSQL *> connque_;
  std::mutex mutex_;
  sem_t semId_;

};