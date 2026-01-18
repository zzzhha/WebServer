file(GLOB LOGGER_SOURCES "*.cpp")

file(GLOB LOGGER_HEADERS "*.h")

set(CMAKE_CXX_STANDARD 14)
add_library(logger STATIC ${LOGGER_SOURCES} ${LOGGER_HEADERS})

target_include_directories(logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(logger PUBLIC Threads::Threads)


install(TARGETS logger
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${LOGGER_HEADERS} DESTINATION include/logger)