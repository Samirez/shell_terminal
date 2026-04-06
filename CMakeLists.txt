cmake_minimum_required(VERSION 3.13)

project(shell_terminal)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(shell ${SOURCE_FILES})
