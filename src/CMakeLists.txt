# PingPong static library
add_library(pingpong STATIC ping_pong.c)
target_include_directories(pingpong PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
