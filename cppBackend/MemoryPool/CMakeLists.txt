file(GLOB MEMORY_POOL_SOURCES "*.cpp")

file(GLOB MEMORY_POOL_HEADERS "*.h")

set(CMAKE_CXX_STANDARD 14)
add_library(memory_pool STATIC ${MEMORY_POOL_SOURCES} ${MEMORY_POOL_HEADERS})


target_include_directories(memory_pool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(memory_pool PUBLIC Threads::Threads)

install(TARGETS memory_pool
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${MEMORY_POOL_HEADERS} DESTINATION include/memory_pool)