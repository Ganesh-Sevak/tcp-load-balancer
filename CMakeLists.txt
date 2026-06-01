cmake_minimum_required(VERSION 3.16)

project(tcp_load_balancer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lb_core
    src/config.cpp
    src/scheduler.cpp
)
target_include_directories(lb_core PUBLIC include)
target_compile_options(lb_core PRIVATE -Wall -Wextra -Wpedantic)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tcp-load-balancer
        src/main.cpp
        src/server.cpp
    )
    target_link_libraries(tcp-load-balancer PRIVATE lb_core pthread)
    target_compile_options(tcp-load-balancer PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(STATUS "tcp-load-balancer server target is Linux-only because it uses epoll")
endif()

enable_testing()

add_executable(scheduler_tests tests/scheduler_tests.cpp)
target_link_libraries(scheduler_tests PRIVATE lb_core)
target_compile_options(scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME scheduler_tests COMMAND scheduler_tests)

add_executable(config_tests tests/config_tests.cpp)
target_link_libraries(config_tests PRIVATE lb_core)
target_compile_options(config_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME config_tests COMMAND config_tests)
