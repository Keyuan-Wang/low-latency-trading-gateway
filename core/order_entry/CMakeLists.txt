add_library(order_entry INTERFACE)
target_include_directories(order_entry INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(order_entry_epoll_server examples/epoll_server.cpp)
target_link_libraries(order_entry_epoll_server PRIVATE order_entry spsc)

add_executable(order_entry_epoll_bench_client examples/epoll_bench_client.cpp)
target_link_libraries(order_entry_epoll_bench_client PRIVATE order_entry)
