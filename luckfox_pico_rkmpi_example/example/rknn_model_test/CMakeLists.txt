cmake_minimum_required(VERSION 3.10)

project(rknn_model_test)

set(EXAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(SRC_DIR "${EXAMPLE_DIR}/src")
file(GLOB SRC_FILES "${SRC_DIR}/*.cc")

add_executable(${PROJECT_NAME} ${SRC_FILES})
target_include_directories(${PROJECT_NAME} PRIVATE
    ${EXAMPLE_DIR}/include
    ${EXAMPLE_DIR}/../../include
    ${EXAMPLE_DIR}/../../include/rknn
)
include_directories(
    ${EXAMPLE_DIR}/include
    ${EXAMPLE_DIR}/../../include
)

link_directories(
    ${EXAMPLE_DIR}/../../lib/uclibc
)
link_directories(${EXAMPLE_DIR}/../../lib/${LIBC_TYPE})
target_link_libraries(${PROJECT_NAME}
    ${EXAMPLE_DIR}/../../lib/${LIBC_TYPE}/librknnmrt.so
    pthread
    dl
    m
)
