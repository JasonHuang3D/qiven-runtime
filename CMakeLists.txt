cmake_minimum_required(VERSION 3.28)

project(qiven-runtime VERSION 0.1.0 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(MSVC)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")
endif()

option(QIVEN_RUNTIME_BUILD_TESTS "Build qiven-runtime tests" ${PROJECT_IS_TOP_LEVEL})

# --- dependency checkouts (sibling layout, JasonPC default) ---------------
set(QIVEN_FOUNDATION_ROOT "" CACHE PATH "Path to the qiven-foundation checkout")
if(NOT QIVEN_FOUNDATION_ROOT)
    set(QIVEN_FOUNDATION_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../qiven-foundation")
    if(EXISTS "${QIVEN_FOUNDATION_DEFAULT}/CMakeLists.txt")
        set(QIVEN_FOUNDATION_ROOT "${QIVEN_FOUNDATION_DEFAULT}")
    endif()
endif()
if(NOT EXISTS "${QIVEN_FOUNDATION_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "qiven-foundation checkout not found; set QIVEN_FOUNDATION_ROOT to its path")
endif()

# --- frozen semantic library pin (layer contract §2) ----------------------
# qiven-context-draft is consumed as a FROZEN semantic library at an exact
# revision; upgrades are an explicit re-pin with re-validation.
# Semantic baseline: frozen v4 @ 4cbc995. Consumption pin: 31fdf505
# (descendant; v4 semantics unchanged by the later housekeeping commits).
set(QIVEN_DRAFT_ROOT "" CACHE PATH "Path to the pinned qiven-context-draft checkout")
if(NOT QIVEN_DRAFT_ROOT)
    set(QIVEN_DRAFT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../qiven-context-draft")
    if(EXISTS "${QIVEN_DRAFT_DEFAULT}/CMakeLists.txt")
        set(QIVEN_DRAFT_ROOT "${QIVEN_DRAFT_DEFAULT}")
    endif()
endif()
if(NOT EXISTS "${QIVEN_DRAFT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "qiven-context-draft checkout not found; set QIVEN_DRAFT_ROOT to its path")
endif()

set(QIVEN_DRAFT_PINNED_SHA "d1e50a86061ee83ae311fdaa467e7d50aaa68317")
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${QIVEN_DRAFT_ROOT}"
    RESULT_VARIABLE QIVEN_DRAFT_SHA_RESULT
    OUTPUT_VARIABLE QIVEN_DRAFT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QIVEN_DRAFT_SHA_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read the qiven-context-draft revision at ${QIVEN_DRAFT_ROOT}")
endif()
if(NOT QIVEN_DRAFT_SHA STREQUAL QIVEN_DRAFT_PINNED_SHA)
    message(
        FATAL_ERROR
            "qiven-context-draft is pinned at ${QIVEN_DRAFT_PINNED_SHA} but the checkout is at ${QIVEN_DRAFT_SHA}; "
            "re-pin explicitly in CMakeLists.txt and re-validate (layer contract §2)"
    )
endif()

add_subdirectory("${QIVEN_FOUNDATION_ROOT}" "${CMAKE_BINARY_DIR}/qiven-foundation" EXCLUDE_FROM_ALL)
add_subdirectory("${QIVEN_DRAFT_ROOT}" "${CMAKE_BINARY_DIR}/qiven-context-draft" EXCLUDE_FROM_ALL)

# --- control-plane library (design §2) ------------------------------------
add_library(qiven-runtime STATIC)
add_library(qiven::runtime ALIAS qiven-runtime)

set_target_properties(
    qiven-runtime
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Runtime"
)

target_compile_features(qiven-runtime PUBLIC cxx_std_20)

target_include_directories(
    qiven-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set(
    QIVEN_RUNTIME_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/adapter/handshake.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/adapter/manifest.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/claims.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/generation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/intent.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/observed_action.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/identity.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/cognition_port.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/profile.hpp
)

set(
    QIVEN_RUNTIME_PRIVATE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter/handshake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter/manifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/claims.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cognition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/generation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/observed_action.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/identity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/profile.cpp
)

target_sources(
    qiven-runtime
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
        FILES ${QIVEN_RUNTIME_PUBLIC_HEADERS}
    PRIVATE
        ${QIVEN_RUNTIME_PRIVATE_SOURCES}
)

target_link_libraries(qiven-runtime PUBLIC qiven::foundation qiven::context_draft)

if(MSVC)
    target_compile_options(qiven-runtime PRIVATE /W4 /permissive- /utf-8 /MP)
else()
    target_compile_options(qiven-runtime PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

# Build-performance policy (devkit docs/conventions/build-performance.md):
# the heavy stable include stack (the frozen draft monolith + foundation
# vocabulary + the common std set) is precompiled once for the library and
# reused by the test targets below. The header-check target is EXEMPT by
# design: precompiled headers would inject includes the checked headers
# never asked for and mask a self-containment defect (never weaken the
# detector).
target_precompile_headers(
    qiven-runtime
    PRIVATE
        <qiven/context/cognition.hpp>
        <qiven/context/runtime.hpp>
        <qiven/error.hpp>
        <qiven/hashing_sha256.hpp>
        <qiven/result.hpp>
        <algorithm>
        <array>
        <cstdint>
        <memory>
        <optional>
        <span>
        <string>
        <utility>
        <vector>
)

if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    source_group(
        TREE "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PREFIX "Header Files"
        FILES ${QIVEN_RUNTIME_PUBLIC_HEADERS}
    )

    source_group(
        TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        PREFIX "Source Files"
        FILES ${QIVEN_RUNTIME_PRIVATE_SOURCES}
    )
endif()

add_executable(qiven-runtime-app app/main.cpp)
add_executable(qiven::runtime_app ALIAS qiven-runtime-app)
set_target_properties(
    qiven-runtime-app
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Apps"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(qiven-runtime-app PRIVATE qiven::runtime)
target_compile_features(qiven-runtime-app PRIVATE cxx_std_20)

if(QIVEN_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
