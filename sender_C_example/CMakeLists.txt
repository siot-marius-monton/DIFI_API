cmake_minimum_required(VERSION 3.10)
project(sender_C_example C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

add_executable(sender_C_example src/sender_C_example.c)
target_include_directories(sender_C_example PRIVATE include ${DPDK_INCLUDE_DIRS})
target_compile_options(sender_C_example PRIVATE ${DPDK_CFLAGS} -O2 -g)
target_link_libraries(sender_C_example ${DPDK_LDFLAGS} rt)
