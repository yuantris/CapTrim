cmake_minimum_required(VERSION 3.16)
project(captrim_napi LANGUAGES CXX)

# HarmonyOS requires prebuilt .so files to be produced by the HarmonyOS NDK
# toolchain and to carry a SONAME. build-rust.sh stages them here.
set(CAPTRIM_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH}")

add_library(captrim SHARED IMPORTED)
set_target_properties(captrim PROPERTIES
    IMPORTED_LOCATION "${CAPTRIM_PREBUILT}/libcaptrim.so")

add_library(captrim_napi SHARED
    napi_bridge.cpp
    presenter.cpp)
target_include_directories(captrim_napi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(captrim_napi PRIVATE cxx_std_17)

# No C++ runtime on purpose. Both sources avoid std::string, new/delete and use
# -fno-exceptions/-fno-rtti, so `-nostdlib++` keeps libc++_shared.so out of the
# DT_NEEDED list. That matters because HarmonyOS requires OHOS_STL to be
# identical across every native library in the app: depending on libc++_shared
# would make this HAR fail to load in any app that uses a different STL flavour.
# Verified: llvm-readelf -d shows only libcaptrim.so, libace_napi.z.so, libc.so,
# libhilog_ndk.z.so and the media/window system libraries.
target_compile_options(captrim_napi PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(captrim_napi PRIVATE -nostdlib++)

# Decoding and presentation are platform work: the Rust core only says which
# source instant belongs on screen. These are the system libraries that do it.
target_link_libraries(captrim_napi
    captrim
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_media_codecbase.so
    libnative_media_core.so
    libnative_media_vdec.so
    libnative_media_avsource.so
    libnative_media_avdemuxer.so
    libnative_window.so)
