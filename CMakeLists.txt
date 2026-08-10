cmake_minimum_required(VERSION 3.10)
project(httpserver)

# Step 1: 为 concurrencpp 和后续协程预留升级到 C++20。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

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
add_subdirectory(test)

# 打印信息
message(STATUS "Frontend directory: ${FRONTEND_DIR}")
message(STATUS "HTML directory: ${HTML_DIR}")
message(STATUS "Build directory: ${CMAKE_BINARY_DIR}")
