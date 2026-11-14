cmake_minimum_required(VERSION 3.16)
project(captrim_napi LANGUAGES CXX)

# HarmonyOS requires prebuilt .so files to be produced by the HarmonyOS NDK
# toolchain and to carry a SONAME. build-rust.sh stages them here.
set(CAPTRIM_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH}")

add_library(captrim SHARED IMPORTED)
set_target_properties(captrim PROPERTIES
    IMPORTED_LOCATION "${CAPTRIM_PREBUILT}/libcaptrim.so")

add_library(captrim_napi SHARED napi_bridge.cpp)
target_include_directories(captrim_napi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(captrim_napi PRIVATE cxx_std_17)

# No C++ runtime on purpose. napi_bridge.cpp avoids std::string and uses
# -fno-exceptions/-fno-rtti, so `-nostdlib++` keeps libc++_shared.so out of the
# DT_NEEDED list. That matters because HarmonyOS requires OHOS_STL to be
# identical across every native library in the app: depending on libc++_shared
# would make this HAR fail to load in any app that uses a different STL flavour.
# Verified: llvm-readelf -d shows only libcaptrim.so, libace_napi.z.so, libc.so.
target_compile_options(captrim_napi PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(captrim_napi PRIVATE -nostdlib++)

target_link_libraries(captrim_napi captrim libace_napi.z.so)
