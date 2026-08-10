#include "sqlconnpool.h"
#include<iostream>
using namespace std;
//mysql服务器为接入到web服务器中，先完成http解析，在做此事;
SqlConnPool* SqlConnPool::Instance(){
  static SqlConnPool connPool;
  return &connPool;
}

// 依据 Init 保存的参数新建一个 MySQL 连接
MYSQL *SqlConnPool::CreateNewConnection(){
  MYSQL *sql = mysql_init(nullptr);
  if(!sql){
    LOGERROR("mySql init error!");
    return nullptr;
  }
  if(!mysql_real_connect(sql, host_.c_str(), user_.c_str(), pwd_.c_str(), dbName_.c_str(), port_, nullptr, 0)){
    char buf[256];
    snprintf(buf, sizeof(buf), "MySql Connection error: %s", mysql_error(sql));
    LOGERROR(buf);
    mysql_close(sql);  // 释放失败句柄，防止泄漏（审计 H15）
    return nullptr;
  }
  return sql;
}

void SqlConnPool::Init(const char* host,int port,const char* user,const char* pwd, const char* dbName, int connSize){
  assert(connSize > 0);
  std::lock_guard<std::mutex> lock(init_mutex_);
  // 幂等初始化：重复 Init 先完整关闭旧池，避免 sem 重复初始化 / 连接泄漏（审计 H15）
  ClosePoolLocked();
  closed_.store(false);
  host_ = host ? host : "";
  port_ = port;
  user_ = user ? user : "";
  pwd_ = pwd ? pwd : "";
  dbName_ = dbName ? dbName : "";

  int successCount = 0;
  for(int i=0; i< connSize ;i++){
    MYSQL *sql = CreateNewConnection();
    if(!sql) continue;
    connque_.push(sql);
    successCount++;
  }
  if(successCount == 0){
    LOGERROR("Failed to create any MySQL connections!");
    MAX_CONN_ = 0;
    borrowed_ = 0;
    sem_init(&semId_,0,0);
    initialized_.store(true);
    return;
  }
  MAX_CONN_ = successCount;
  borrowed_ = 0;
  sem_init(&semId_,0,MAX_CONN_);
  initialized_.store(true);
  LOGINFO("SqlConnPool initialized with " + std::to_string(MAX_CONN_) + " connections");
}

MYSQL* SqlConnPool::GetConn(){
  if (closed_.load() || MAX_CONN_ <= 0){
    LOGWARNING("SqlConnPool not initialized or unavailable!");
    return nullptr;
  }
  MYSQL *sql = nullptr;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 3; // 3秒限时等待

  if (sem_timedwait(&semId_, &ts) != 0) {
    // 超时：可能是死连接被清空导致容量丢失（借用数 < 上限）。
    // 此时懒重建一个连接，使池在 DB 重启后能自愈（审计 H15 健康检查）。
    if (!closed_.load()) {
      bool capacity_lost = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_lost = (borrowed_ < MAX_CONN_);
      }
      if (capacity_lost) {
        sql = CreateNewConnection();
        if (sql) {
          std::lock_guard<std::mutex> lock(mutex_);
          borrowed_++;
          return sql;
        }
      }
    }
    LOGWARNING("SqlConnPool busy or timeout!");
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(connque_.empty()){
      LOGWARNING("SqlConnPool busy!");
      sem_post(&semId_);
      return nullptr;
    }
    sql = connque_.front();
    connque_.pop();
    borrowed_++;
  }

  // 健康检查：ping 失败说明连接已死（如 DB 重启），关闭并尝试替换（审计 H15）
  if (mysql_ping(sql) != 0) {
    LOGWARNING("SqlConnPool dead connection discarded, reconnecting...");
    mysql_close(sql);
    sql = CreateNewConnection();
    if (sql) return sql;
    // 替换失败：该槽位作废（borrowed_ 已计数），后续由 sem 超时分支懒重建恢复容量
    return nullptr;
  }
  return sql;
}
void SqlConnPool::FreeConn(MYSQL *sql){
  if(!sql) return;
  bool should_close = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load()) {
      // 池已关闭：连接直接关闭，绝不塞回队列，避免借出期间 ClosePool 后
      // 死连接回流，下一手 GetConn 拿到已关闭句柄（审计 H15）
      should_close = true;
    } else {
      connque_.push(sql);
      if (borrowed_ > 0) borrowed_--;
    }
  }
  if (should_close) {
    mysql_close(sql);
    return;
  }
  sem_post(&semId_);
}
int SqlConnPool::GetFreeConnCount(){
  lock_guard<mutex> lock(mutex_);
  return connque_.size();
}


void SqlConnPool::ClosePool(){
  std::lock_guard<std::mutex> lock(init_mutex_);
  ClosePoolLocked();
}

// 已持有 init_mutex_ 时的内部实现（Init 复用，避免重复加锁/死锁）
void SqlConnPool::ClosePoolLocked(){
  bool was_initialized = initialized_.exchange(false);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // 先置 closed：与 FreeConn 的加锁判定串行化，保证归还的连接要么被池回收、
    // 要么被直接关闭，不会泄漏到已关闭的队列里（审计 H15）
    closed_.store(true);
    while(!connque_.empty()){
      auto item = connque_.front();
      connque_.pop();
      mysql_close(item);
    }
  }
  // 只在 sem 成功初始化过后才销毁，保证 ClosePool 幂等（重复调用/重复 Init 安全）
  if (was_initialized) {
    sem_destroy(&semId_);
  }
  MAX_CONN_ = 0;
  borrowed_ = 0;
  freeCount_ = 0;
}


SqlConnPool::SqlConnPool(){
  MAX_CONN_ = 0;
  useCount_ = 0;
  freeCount_ = 0;
}

SqlConnPool::~SqlConnPool(){
  ClosePool();
  mysql_library_end();
}
