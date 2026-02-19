cmake_minimum_required(VERSION 3.10)
project(difi_dpdk_receiver C)

set(CMAKE_C_STANDARD 11)

# DIFI_C_Lib: sibling of DIFI_API (parent of difi_dpdk_receiver)
set(DIFI_C_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../DIFI_C_Lib")
if(NOT EXISTS "${DIFI_C_LIB_DIR}/CMakeLists.txt")
  set(DIFI_C_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../DIFI_C_Lib")
endif()
add_subdirectory(${DIFI_C_LIB_DIR} ${CMAKE_CURRENT_BINARY_DIR}/difi_c_lib)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

add_executable(difi_dpdk_receiver src/difi_dpdk_receiver.c)
target_include_directories(difi_dpdk_receiver PRIVATE
  include
  ${DPDK_INCLUDE_DIRS}
  ${DIFI_C_LIB_DIR}/include
)
target_compile_options(difi_dpdk_receiver PRIVATE ${DPDK_CFLAGS} -O3)
target_link_libraries(difi_dpdk_receiver PRIVATE difi ${DPDK_LDFLAGS} rt)
