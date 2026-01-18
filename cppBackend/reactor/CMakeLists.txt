file(GLOB REACTOR_SOURCES "*.cpp")

file(GLOB REACTOR_HEADERS "*.h")

set(CMAKE_CXX_STANDARD 14)
add_executable(tcpepoll ${REACTOR_SOURCES} ${REACTOR_HEADERS})

target_include_directories(tcpepoll 
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC 
    ${CMAKE_SOURCE_DIR}/logger
    ${CMAKE_SOURCE_DIR}/MemoryPool
)
target_link_libraries(tcpepoll 
    PUBLIC 
    logger
    MemoryPool
    Threads::Threads
)

install(TARGETS tcpepoll
    RUNTIME DESTINATION bin
)
