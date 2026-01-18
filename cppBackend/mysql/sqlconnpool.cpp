#include "sqlconnpool.h"
#include<iostream>
using namespace std;
//mysql服务器为接入到web服务器中，先完成http解析，在做此事;
SqlConnPool* SqlConnPool::Instance(){
  static SqlConnPool connPool;
  return &connPool;
}

void SqlConnPool::Init(const char* host,int port,const char* user,const char* pwd, const char* dbName, int connSize){
  assert(connSize > 0);
  for(int i=0; i< connSize ;i++){
    MYSQL *sql = nullptr;
    sql = mysql_init(sql);
    if(!sql){
      LOGERROR("mySql init error!");
      assert(sql);
    }
    //sql =mysql_real_connect(sql,"localhost","webuser","1258977","webserver",3306,nullptr,0);
    sql = mysql_real_connect(sql,host,user,pwd,dbName,port,nullptr,0);

    if(!sql){
      LOGERROR("MySql Connection error!");
    }
    connque_.push(sql);
  }
  MAX_CONN_ = connSize;
  sem_init(&semId_,0,MAX_CONN_);

}

MYSQL* SqlConnPool::GetConn(){
  MYSQL *sql = nullptr;
  if(connque_.empty()){
    LOGWARNING("SqlConnPool busy!");
    return nullptr;
  }
  sem_wait(&semId_);
  {
    lock_guard<mutex> lock(mutex_);
    sql = connque_.front();
    connque_.pop();
  }
  return sql;
}
void SqlConnPool::FreeConn(MYSQL *sql){
  assert(sql);
  lock_guard<mutex> lock(mutex_);
  connque_.push(sql);
  sem_post(&semId_);
}
int SqlConnPool::GetFreeConnCount(){
  lock_guard<mutex> lock(mutex_);
  return connque_.size();
}


void SqlConnPool::ClosePool(){
  lock_guard<mutex> lock(mutex_);
  while(!connque_.empty()){
    auto item = connque_.front();
    connque_.pop();
    mysql_close(item);
  }
  mysql_library_end(); 
}


SqlConnPool::SqlConnPool(){
  useCount_ = 0;
  freeCount_ = 0;
}

SqlConnPool::~SqlConnPool(){
  ClosePool();
}