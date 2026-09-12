cmake_minimum_required(VERSION 3.20)
project(rnqjsbench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)

add_library(rnqjsbench SHARED JsiBenchBindings.cpp)
target_link_libraries(rnqjsbench PRIVATE
  ReactAndroid::jsi
  ReactAndroid::reactnative
  fbjni::fbjni
  log
)
