# Unit tests
# test_ping_pong.c still documents the historical behavior. It is temporarily
# excluded from CI because PING_PONG_NOTIFY_RX_TIMEOUT was removed from the
# public notification API. Keep focused coverage in test_ping_pong_extensions.
add_executable(test_ping_pong_extensions test_ping_pong_extensions.c)
target_link_libraries(test_ping_pong_extensions PRIVATE pingpong)

add_test(NAME test_ping_pong_extensions COMMAND test_ping_pong_extensions)
