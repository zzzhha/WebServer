cmake_minimum_required(VERSION 3.10)
project(httpserver)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 前端资源目录
set(FRONTEND_DIR ${CMAKE_SOURCE_DIR}/Frontend)
set(HTML_DIR ${FRONTEND_DIR}/html)

# 安装前端资源到构建目录
add_custom_target(copy_frontend ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${HTML_DIR} ${CMAKE_BINARY_DIR}/html
    COMMENT "Copying frontend resources to build directory"
)

# 添加后端子目录
add_subdirectory(cppBackend)

# 打印信息
message(STATUS "Frontend directory: ${FRONTEND_DIR}")
message(STATUS "HTML directory: ${HTML_DIR}")
message(STATUS "Build directory: ${CMAKE_BINARY_DIR}")