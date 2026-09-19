cmake_minimum_required(VERSION 3.28)
project(qiven-runtime VERSION 0.1.0 LANGUAGES CXX)

option(QIVEN_RUNTIME_BUILD_TESTS "Build qiven-runtime tests" ON)

# Bootstrap-only: choose the final library structure and linkage after domain design is approved.
add_library(qiven-runtime INTERFACE)
add_library(qiven::runtime ALIAS qiven-runtime)
target_compile_features(qiven-runtime INTERFACE cxx_std_20)

if(QIVEN_RUNTIME_BUILD_TESTS)
    enable_testing()
endif()
