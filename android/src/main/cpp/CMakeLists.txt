# SPDX-License-Identifier: Apache-2.0 OR MIT
# liblatai_rn.so: the JSI glue + the JNI adapter, linked against the prebuilt liblatai.so.
cmake_minimum_required(VERSION 3.22)
project(latai_rn LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

# include/latai.h: the package's own copy (android/src/main/cpp -> four levels up), kept by check_bindings.py.
set(LATAI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../include"
    CACHE PATH "directory with latai.h and latai_errors.h")
set(LATAI_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}"
    CACHE PATH "directory with liblatai.so for this ABI")

find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)

add_library(latai SHARED IMPORTED)
set_target_properties(latai PROPERTIES IMPORTED_LOCATION "${LATAI_LIB_DIR}/liblatai.so")

add_library(latai_rn SHARED
    ../../../../cpp/LataiJsi.cpp
    cpp-adapter.cpp)
target_include_directories(latai_rn PRIVATE
    ../../../../cpp
    "${LATAI_INCLUDE_DIR}")
target_compile_options(latai_rn PRIVATE -Wall -Wextra -fexceptions -frtti)
target_link_libraries(latai_rn PRIVATE
    latai
    ReactAndroid::jsi
    ReactAndroid::react_nativemodule_core
    fbjni::fbjni
    android
    log)
target_link_options(latai_rn PRIVATE "-Wl,-z,max-page-size=16384")
