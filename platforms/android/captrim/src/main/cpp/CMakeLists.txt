cmake_minimum_required(VERSION 3.22)
project(captrim_jni LANGUAGES CXX)

# libcaptrim.so is produced by scripts/build-rust.sh and staged here.
set(CAPTRIM_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}")

add_library(captrim SHARED IMPORTED)
set_target_properties(captrim PROPERTIES
    IMPORTED_LOCATION "${CAPTRIM_PREBUILT}/libcaptrim.so")

add_library(captrim_jni SHARED jni_bridge.cpp)
target_include_directories(captrim_jni PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(captrim_jni PRIVATE cxx_std_17)
# jni_bridge.cpp uses no C++ standard library and no exceptions. The STL is
# linked statically (see ANDROID_STL in build.gradle.kts) so the AAR does not
# ship a second libc++_shared.so that could clash with the host app's copy.
target_compile_options(captrim_jni PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_libraries(captrim_jni captrim android log)
