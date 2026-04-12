cmake_minimum_required(VERSION 3.10)
project(PingPong C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict compiler warnings
add_compile_options(-Wall -Wextra -Werror -Wpedantic)

# Core library
add_subdirectory(src)

# Unit tests
enable_testing()
add_subdirectory(test)
