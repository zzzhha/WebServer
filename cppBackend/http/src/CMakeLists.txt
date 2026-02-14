set(CORE_SOURCES 
  core/HttpRequest.cpp
  core/HttpResponse.cpp
)

set(PARSER_SOURCES
  parser/Http1Parser.cpp
  parser/Http2Parser.cpp
)

set(BUILDER_SOURCES
  builder/RequestBuilder.cpp
  builder/ResponseBuilder.cpp
)

set(HANDLER_SOURCES
  handler/HandlerChain.cpp
  handler/IRequestHandler.cpp
  handler/ProtocolValidationHandler.cpp
  handler/SecurityValidationHandler.cpp
  handler/AppHandlers.cpp
)

set(FACTORY_SOURCES
  factory/ParseFactory.cpp
  factory/ResponseFactory.cpp
)

set(OBSERVER_SOURCES
  observer/LoggingObserver.cpp
)

set(STRATEGY_SOURCES
  strategy/ParseStrategy.cpp
)

set(SSL_SOURCES
  ssl/SslHandler.cpp
  ssl/SslFactory.cpp
  ssl/SslContext.cpp
)

set(ROUTER_SOURCES
  router/Router.cpp
)

set(SERVER_SOURCES
  HttpFacade.cpp
)

set(HTTP_SOURCES
  ${CORE_SOURCES}
  ${PARSER_SOURCES}
  ${BUILDER_SOURCES}
  ${HANDLER_SOURCES}
  ${FACTORY_SOURCES}
  ${OBSERVER_SOURCES}
  ${STRATEGY_SOURCES}
  ${SSL_SOURCES}
  ${ROUTER_SOURCES}
  ${SERVER_SOURCES}
)

# 查找 OpenSSL 库
find_package(OpenSSL REQUIRED)

# 获取项目根目录（http 的父目录）
get_filename_component(PROJECT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(http STATIC ${HTTP_SOURCES})

# 设置包含目录
# http/include 目录已经通过父 CMakeLists.txt 的 include_directories 添加
# 这里添加项目根目录，以便能找到 mysql 和 logger 目录
target_include_directories(http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include  # http/include
    ${PROJECT_ROOT_DIR}  # 项目根目录，用于访问 mysql 和 logger
)

# 链接 OpenSSL 库到静态库
target_link_libraries(http PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# 演示主程序
# add_executable(http_demo main.cpp)
# target_link_libraries(http_demo PRIVATE http_new)
