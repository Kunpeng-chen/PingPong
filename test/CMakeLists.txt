# Unit tests
add_executable(test_ping_pong test_ping_pong.c)
target_link_libraries(test_ping_pong PRIVATE pingpong)

add_test(NAME test_ping_pong COMMAND test_ping_pong)
