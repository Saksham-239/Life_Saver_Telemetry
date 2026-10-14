cmake_minimum_required(VERSION 3.13)
project(host_tests C)

set(CMAKE_C_STANDARD 11)

add_executable(host_tests
    src/tests/host_main.c
    src/tests/protocol_vectors.c
    src/packet_codec.c
)

target_include_directories(host_tests PRIVATE include src)
