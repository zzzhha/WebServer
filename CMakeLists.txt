cmake_minimum_required(VERSION 3.10)
project(httpserver)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译器标志
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")

# 手动查找MySQL头文件和库
find_path(MYSQL_INCLUDE_DIR
  NAMES mysql.h
  PATHS
    /usr/include/mysql
    /usr/local/include/mysql
    /opt/mysql/include
    /usr/include/mariadb
    /usr/local/include/mariadb
  DOC "MySQL include directory"
)

# 查找MySQL客户端库
find_library(MYSQL_LIBRARY
  NAMES mysqlclient mysqlclient_r
  PATHS
    /usr/lib/mysql
    /usr/local/lib/mysql
    /usr/lib/x86_64-linux-gnu
    /usr/lib/x86_64-linux-gnu/mysql
    /usr/lib64/mysql
  DOC "MySQL library"
)

if(NOT MYSQL_INCLUDE_DIR)
    message(FATAL_ERROR "MySQL include directory not found. Please install MySQL development package.")
endif()

if(NOT MYSQL_LIBRARY)
    message(FATAL_ERROR "MySQL library not found. Please install MySQL development package.")
endif()

message(STATUS "Found MySQL include dir: ${MYSQL_INCLUDE_DIR}")
message(STATUS "Found MySQL library: ${MYSQL_LIBRARY}")

# 添加 http 子目录（这会构建 http 库）
add_subdirectory(http)

# 包含目录
include_directories(
    ${MYSQL_INCLUDE_DIR}
    .
    http
    logger
    memory_pool
    mysql
    reactor
    timer
    http/include
    services/include
    views/include
)

# HTTP模块源文件
#set(HTTP_SOURCES
#    http/httprequest.cpp
#    http/httprequest.h
#    http/httpresponse.cpp  # 注意：文件名可能是httpresponse.cpp而不是httpssponse.cpp
#    http/httpresponse.h
#)

# Logger模块源文件
set(LOGGER_SOURCES
    logger/log_fac.cpp
    logger/log_fac.h
    logger/log_console_output.cpp
    logger/log_console_output.h
    logger/log_file_output.cpp
    logger/log_file_output.h
    logger/log_format.h
    logger/log_output.h
    logger/logger.cpp
    logger/logger.h
    logger/user_format.cpp
    logger/user_format.h
    logger/xconfig.cpp
    logger/xconfig.h
    logger/xlog_format.cpp
    logger/xlog_format.h
)

# Memory Pool模块源文件
#set(MEMORY_POOL1_SOURCES
 #   memory_pool1/CentralCache.cpp
  #  memory_pool1/CentralCache.h
   # memory_pool1/Common.h
    #memory_pool1/ConcurrentPool.h
    #memory_pool1/ObjectPool.h
    #memory_pool1/PageMap.h
    #memory_pool1/PageCache.cpp
    #memory_pool1/PageCache.h
    #memory_pool1/ThreadCache.cpp
    #memory_pool1/ThreadCache.h
#)
set(MEMORY_POOL_SOURCES
    memory_pool2/centralCache.cpp
    memory_pool2/centralCache.h
    memory_pool2/common.h
    memory_pool2/MemoryPool.h
    memory_pool2/pageCache.cpp
    memory_pool2/pageCache.h
    memory_pool2/threadcache.cpp
    memory_pool2/threadcache.h
)
# MySQL模块源文件
set(MYSQL_SOURCES
    mysql/User.h
    mysql/User.cpp
    mysql/sqlconnpool.cpp
    mysql/sqlconnpool.h
    mysql/sqlConnRAII.h
)

# Services模块源文件（业务处理层）
set(SERVICES_SOURCES
    services/src/AuthService.cpp
    services/src/DownloadService.cpp
    services/src/StaticFileService.cpp
)

# Views模块源文件（界面跳转层）
set(VIEWS_SOURCES
    views/src/ErrorHandler.cpp
    views/src/NotFoundHandler.cpp
    views/src/MethodNotAllowedHandler.cpp
    views/src/BadRequestHandler.cpp
    views/src/ForbiddenHandler.cpp
    views/src/WelcomePageHandler.cpp
    views/src/IndexPageHandler.cpp
    views/src/LoginPageHandler.cpp
    views/src/RegisterPageHandler.cpp
    views/src/PicturePageHandler.cpp
    views/src/VideoPageHandler.cpp
)

# Reactor模块源文件
set(REACTOR_SOURCES
    reactor/Acceptor.cpp
    reactor/Acceptor.h
    reactor/Buffer.cpp
    reactor/Buffer.h
    reactor/Channel.cpp
    reactor/Channel.h
    reactor/Connection.cpp
    reactor/Connection.h
    reactor/Epoll.cpp
    reactor/Epoll.h
    reactor/Eventloop.cpp
    reactor/Eventloop.h
    reactor/HttpServer.cpp
    reactor/HttpServer.h
    reactor/InetAddress.cpp
    reactor/InetAddress.h
    reactor/Socket.cpp
    reactor/Socket.h
    reactor/tcpserver.cpp
    reactor/tcpserver.h
    reactor/ThreadPool.cpp
    reactor/ThreadPool.h
)

# Timer模块源文件
set(TIMER_SOURCES
    timer/TimeWheel.h
    timer/TimeWheel.cpp
    # 注意：如果存在timer.cpp，请添加在这里
)

# 添加可执行文件
add_executable(${PROJECT_NAME}
    reactor/main.cpp  # 注意：main.cpp在reactor目录下
    ${HTTP_SOURCES}
    ${LOGGER_SOURCES}
    ${MEMORY_POOL_SOURCES}
    ${MYSQL_SOURCES}
    ${REACTOR_SOURCES}
    ${TIMER_SOURCES}
    ${SERVICES_SOURCES}
    ${VIEWS_SOURCES}
)

# 链接库
target_link_libraries(${PROJECT_NAME}
    http  # http 库
    ${MYSQL_LIBRARY}
    pthread
    m
    rt
)

# 添加 AuthService 单元测试
execute_process(COMMAND mkdir -p ${CMAKE_CURRENT_BINARY_DIR}/services/test
    RESULT_VARIABLE MKDIR_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
)

add_executable(AuthServiceTest
    services/src/AuthServiceTest.cpp
    ${SERVICES_SOURCES}
    ${MYSQL_SOURCES}
    ${LOGGER_SOURCES}
    ${MEMORY_POOL_SOURCES}
)

# 链接测试所需的库
target_link_libraries(AuthServiceTest
    http  # http 库
    ${MYSQL_LIBRARY}
    pthread
    m
    rt
)
