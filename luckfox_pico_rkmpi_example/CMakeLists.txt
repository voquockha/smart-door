cmake_minimum_required(VERSION 3.10)

#Processing Parameters
if(DEFINED LIBC_TYPE)
    message(STATUS "Selected libc type: ${LIBC_TYPE}")
else()
    message(FATAL_ERROR "No libc type selected!")
    return()
endif()

set(SDK_PATH $ENV{LUCKFOX_SDK_PATH})
set(GLIBC_COMPILER $ENV{GLIBC_COMPILER})
if(${LIBC_TYPE} STREQUAL "uclibc")
    if (NOT SDK_PATH)
        message(WARNING "Please Set Luckfox-pico SDK Path. Such as:export LUCKFOX_SDK_PATH=/home/user/luckfox-pico")
        return()
    else()
        set(CMAKE_C_COMPILER "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
        set(CMAKE_CXX_COMPILER "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")
    endif()
elseif(${LIBC_TYPE} STREQUAL "glibc")
    if (NOT GLIBC_COMPILER)
        message(WARNING "Please Set arm32 glibc compiler Path. Such as:export GLIBC_COMPILER=/opt/arm-linux-gnueabihf/bin/arm-linux-gnueabihf-")
        return()
    else()
        set(CMAKE_C_COMPILER    "${GLIBC_COMPILER}gcc")
        set(CMAKE_CXX_COMPILER  "${GLIBC_COMPILER}g++")
    endif()
endif()

if(DEFINED EXAMPLE_DIR)
    message(STATUS "Selected source directory: ${EXAMPLE_DIR}")
else()
    message(FATAL_ERROR "No source directory selected!")
    return()
endif()

if(DEFINED EXAMPLE_NAME)
    message(STATUS "Set project name: ${EXAMPLE_NAME}")
else()
    message(FATAL_ERROR "Project name not specified!")
    return()
endif()

project(${EXAMPLE_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
add_definitions(-DRV1106_1103)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib/${LIBC_TYPE})

#Opencv 4
set(OpenCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/${LIBC_TYPE}/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)
#Thread
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(LUCKFOX_BUILDROOT_SYSROOT
    "${SDK_PATH}/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot")
find_path(CURL_INCLUDE_DIR
          NAMES curl/curl.h
          PATHS
              "${LUCKFOX_BUILDROOT_SYSROOT}/usr/include"
              "${CMAKE_CURRENT_SOURCE_DIR}/include"
          NO_DEFAULT_PATH)
find_library(CURL_LIBRARY
             NAMES curl libcurl
             PATHS
                 "${LUCKFOX_BUILDROOT_SYSROOT}/usr/lib"
                 "${CMAKE_CURRENT_SOURCE_DIR}/lib/${LIBC_TYPE}"
             NO_DEFAULT_PATH)
foreach(CURL_DEP ssl crypto z atomic)
    string(TOUPPER "${CURL_DEP}" CURL_DEP_UPPER)
    find_library(CURL_${CURL_DEP_UPPER}_LIBRARY
                 NAMES ${CURL_DEP}
                 PATHS "${LUCKFOX_BUILDROOT_SYSROOT}/usr/lib"
                 NO_DEFAULT_PATH)
    if(CURL_${CURL_DEP_UPPER}_LIBRARY)
        list(APPEND CURL_EXTRA_LIBS "${CURL_${CURL_DEP_UPPER}_LIBRARY}")
    endif()
endforeach()
if(NOT CURL_INCLUDE_DIR OR NOT CURL_LIBRARY OR NOT CURL_EXTRA_LIBS)
    message(FATAL_ERROR "libcurl is required for TelegramClient. "
                        "Missing curl/curl.h, libcurl.so, or curl dependencies")
endif()
message(STATUS "Found libcurl include: ${CURL_INCLUDE_DIR}")
message(STATUS "Found libcurl library: ${CURL_LIBRARY}")
message(STATUS "Found libcurl deps: ${CURL_EXTRA_LIBS}")

find_path(JPEG_INCLUDE_DIR
          NAMES jpeglib.h
          PATHS
              "${SDK_PATH}/project/app/component/third_libs/include/libjpeg9"
              "${SDK_PATH}/project/app/out/include/libjpeg9"
              "${SDK_PATH}/output/out/app_out/include/libjpeg9"
              "${LUCKFOX_BUILDROOT_SYSROOT}/usr/include"
          NO_DEFAULT_PATH)
find_library(JPEG_LIBRARY
             NAMES jpeg libjpeg
             PATHS
                 "${SDK_PATH}/project/app/component/third_libs/lib/arm-rockchip830-linux-uclibcgnueabihf"
                 "${SDK_PATH}/project/app/out/lib"
                 "${SDK_PATH}/output/out/app_out/lib"
                 "${LUCKFOX_BUILDROOT_SYSROOT}/usr/lib"
             NO_DEFAULT_PATH)
if(NOT JPEG_INCLUDE_DIR OR NOT JPEG_LIBRARY)
    message(FATAL_ERROR "libjpeg is required for software JPEG evidence images. "
                        "Missing jpeglib.h or libjpeg.so")
endif()
message(STATUS "Found libjpeg include: ${JPEG_INCLUDE_DIR}")
message(STATUS "Found libjpeg library: ${JPEG_LIBRARY}")

set(SRC_DIR "${EXAMPLE_DIR}/src")
file(GLOB SRC_FILES "${SRC_DIR}/*.cc")
if(${EXAMPLE_NAME} STREQUAL "luckfox_pico_rtsp_best_facenet")
    list(FILTER SRC_FILES EXCLUDE REGEX "/mqtt_client\\.cc$")
endif()
add_executable(${PROJECT_NAME} ${SRC_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib")

add_compile_options(-g -Wall
                    -DISP_HW_V30 -DRKPLATFORM=ON -DARCH64=OFF
                    -DROCKIVA -DUAPI2
                    -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 
                    )

set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${CMAKE_CURRENT_SOURCE_DIR}/lib:/usr/lib")
if(CURL_LIBRARY)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${LUCKFOX_BUILDROOT_SYSROOT}/usr/lib")
endif()

if(${LIBC_TYPE} STREQUAL "uclibc")
    target_link_libraries(${PROJECT_NAME}
                        ${OpenCV_LIBS}
                        rknnmrt
                        Threads::Threads
                        rockiva
                        sample_comm
                        rockit
                        rockchip_mpp
                        rkaiq
                        pthread
                        rtsp
                        ${CURL_LIBRARY}
                        ${CURL_EXTRA_LIBS}
                        ${JPEG_LIBRARY}
                        rga
                        )
elseif(${LIBC_TYPE} STREQUAL "glibc")
    target_link_libraries(${PROJECT_NAME}
                        ${OpenCV_LIBS}
                        rknnmrt
                        Threads::Threads
                        sample_comm
                        rockit
                        rockchip_mpp
                        rkaiq
                        pthread
                        rtsp
                        stdc++
                        func_none
                        c
                        m
                        rt
                        dl
                        ${CURL_LIBRARY}
                        ${CURL_EXTRA_LIBS}
                        ${JPEG_LIBRARY}
                        rga
                        )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE                        
                            ${OpenCV_INCLUDE_DIRS}
                            ${CURL_INCLUDE_DIR}
                            ${JPEG_INCLUDE_DIR}
                            ${EXAMPLE_DIR}/include
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${CMAKE_CURRENT_SOURCE_DIR}/utils
                            ${CMAKE_CURRENT_SOURCE_DIR}/common 
                            ${CMAKE_CURRENT_SOURCE_DIR}/common/isp3.x   
                            ${CMAKE_CURRENT_SOURCE_DIR}/include
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rknn
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/librga 
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/uAPI2
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/common
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/xcore
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/algos
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/iq_parser
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/iq_parser_v2
                            ${CMAKE_CURRENT_SOURCE_DIR}/include/rkaiq/smartIr
                          )
message(STATUS "model dir PATH: ${EXAMPLE_DIR}/model")

set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install/${LIBC_TYPE}/${PROJECT_NAME}_demo")
install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX})
SET(MODEL_DIR "${EXAMPLE_DIR}/model")
file(GLOB MODEL_FILES "${MODEL_DIR}/*")  
install(FILES ${MODEL_FILES} DESTINATION ${CMAKE_INSTALL_PREFIX}/model)

SET(SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${EXAMPLE_DIR}/scripts")
if(EXISTS "${SCRIPT_DIR}/x68-face-app" AND
   EXISTS "${SCRIPT_DIR}/S99zzx68face" AND
   EXISTS "${SCRIPT_DIR}/S99usb0config-stable")
    install(PROGRAMS
            "${SCRIPT_DIR}/x68-face-app"
            "${SCRIPT_DIR}/S99zzx68face"
            "${SCRIPT_DIR}/S99usb0config-stable"
            DESTINATION ${CMAKE_INSTALL_PREFIX}/scripts)
endif()
