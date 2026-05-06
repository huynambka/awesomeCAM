cmake_minimum_required(VERSION 3.22.1)
project(awesomeCAM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(shadowhook REQUIRED CONFIG)
find_package(ffmpeg REQUIRED CONFIG)

add_executable(injector_helper
    injector.cpp
    injector_main.cpp
    ptrace_utils.cpp
    remote_call.cpp
    proc_maps.cpp
    elf_parser.cpp
)
set_target_properties(injector_helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../assets"
)

add_executable(awesomecam_player
    awesomecam_player.cpp
    video2camera_ndk.cpp
)
set_target_properties(awesomecam_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../assets"
)

add_library(hook SHARED
    hook.cpp
    libyuv_runtime.cpp
    ready_frame_cache.cpp
    video2camera_ndk.cpp
    video2camera_service.cpp
)
set_target_properties(hook PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../assets"
)

add_library(streamclient SHARED streamclient.cpp video2camera_ndk.cpp)

find_library(log-lib log)
find_library(dl-lib dl)
find_library(m-lib m)
find_library(android-lib android)

target_include_directories(injector_helper PRIVATE "${CMAKE_SOURCE_DIR}")
target_include_directories(awesomecam_player PRIVATE "${CMAKE_SOURCE_DIR}")
target_include_directories(hook PRIVATE "${CMAKE_SOURCE_DIR}")
target_include_directories(streamclient PRIVATE "${CMAKE_SOURCE_DIR}")

target_compile_options(injector_helper PRIVATE -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti)
target_link_options(injector_helper PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)

target_link_options(hook PRIVATE -Wl,-z,max-page-size=16384)
target_link_options(streamclient PRIVATE -Wl,-z,max-page-size=16384)
target_link_options(awesomecam_player PRIVATE -Wl,-z,max-page-size=16384 -Wl,-rpath,/data/camera)

target_link_libraries(injector_helper ${log-lib} ${dl-lib} ${m-lib})
target_link_libraries(awesomecam_player ffmpeg::ffmpeg ${log-lib} ${dl-lib} ${android-lib})
target_link_libraries(hook shadowhook::shadowhook ${log-lib} ${dl-lib})
target_link_libraries(streamclient ${log-lib} ${dl-lib})
