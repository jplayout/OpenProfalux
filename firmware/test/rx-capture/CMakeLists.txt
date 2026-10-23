cmake_minimum_required(VERSION 3.16)
project(rx_capture_host_test C)

add_executable(rx_capture_host_test
    main/test_main.c
    ../../main/rx_capture.c
    ../../main/keeloq_decode.c)
target_include_directories(rx_capture_host_test PRIVATE main ../../main)
target_compile_options(rx_capture_host_test PRIVATE
    -std=gnu11 -Wall -Wextra -Wno-unused-parameter
    -fsanitize=address -fno-omit-frame-pointer -g)
target_link_options(rx_capture_host_test PRIVATE -fsanitize=address)