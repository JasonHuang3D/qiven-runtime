cmake_minimum_required(VERSION 3.28)

# C is enabled for vendored third-party C targets (TP law; design 17):
# without it .c sources classify as <None> and never compile.
project(qiven-runtime VERSION 0.1.0 LANGUAGES CXX C)

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

# Foundation pin (cross-repo-cmake.md law; owner direction 2026-09-23:
# full SHA-pin coverage - the v2 standard's recorded gap is closed, not
# deferred; upgrades are an explicit re-pin with re-validation).
set(QIVEN_FOUNDATION_PINNED_SHA "424a9a8225f10d72a3a40d7c5cbd863382d32536")
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${QIVEN_FOUNDATION_ROOT}"
    RESULT_VARIABLE QIVEN_FOUNDATION_SHA_RESULT
    OUTPUT_VARIABLE QIVEN_FOUNDATION_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QIVEN_FOUNDATION_SHA_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read the qiven-foundation revision at ${QIVEN_FOUNDATION_ROOT}")
endif()
if(NOT QIVEN_FOUNDATION_SHA STREQUAL QIVEN_FOUNDATION_PINNED_SHA)
    message(FATAL_ERROR "qiven-foundation is pinned at ${QIVEN_FOUNDATION_PINNED_SHA} but the checkout is at ${QIVEN_FOUNDATION_SHA}; re-pin explicitly in CMakeLists.txt and re-validate")
endif()

# --- frozen semantic library pin (layer contract §2) ----------------------
# qiven-context-draft is consumed as a FROZEN semantic library at an exact
# revision; upgrades are an explicit re-pin with re-validation.
# Semantic baseline: frozen v4 @ 4cbc995. Consumption pin: 611255b
# (descendant chain 31fdf505 -> 73a4bfa -> 41d32ff -> de7f0e1 -> 611255b;
# v4 semantics unchanged - the later commits are engineering-docs managed
# syncs, template manual patches and the foundation-pin build glue).
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

set(QIVEN_DRAFT_PINNED_SHA "611255b8dba1a2318177a559dc827c5cb83c0ad6")
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

# --- third-party singleton (Devkit standard v2; cross-repo-cmake.md) ----
# One workspace copy: qiven-third-party-win. Root resolution: env override
# -> sibling default -> fail. Exact-SHA pin, validated at configure (same
# discipline as the draft pin); moving the pin is an explicit re-pin batch.
set(QIVEN_THIRD_PARTY_ROOT "" CACHE PATH "Path to the qiven-third-party-win checkout")
if(NOT QIVEN_THIRD_PARTY_ROOT)
    set(QIVEN_THIRD_PARTY_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../qiven-third-party-win")
    if(EXISTS "${QIVEN_THIRD_PARTY_DEFAULT}/CMakeLists.txt")
        set(QIVEN_THIRD_PARTY_ROOT "${QIVEN_THIRD_PARTY_DEFAULT}")
    endif()
endif()
if(NOT EXISTS "${QIVEN_THIRD_PARTY_ROOT}/packages/sqlite3/CMakeLists.txt")
    message(FATAL_ERROR "qiven-third-party-win checkout not found; set QIVEN_THIRD_PARTY_ROOT to its path")
endif()

set(QIVEN_THIRD_PARTY_PIN "318453834315a68fa85374dffa9a77ff73d6e71c")
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${QIVEN_THIRD_PARTY_ROOT}"
    RESULT_VARIABLE QIVEN_TP_SHA_RESULT
    OUTPUT_VARIABLE QIVEN_TP_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QIVEN_TP_SHA_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read the qiven-third-party-win revision at ${QIVEN_THIRD_PARTY_ROOT}")
endif()
if(NOT QIVEN_TP_SHA STREQUAL QIVEN_THIRD_PARTY_PIN)
    message(FATAL_ERROR "qiven-third-party-win is pinned at ${QIVEN_THIRD_PARTY_PIN} but the checkout is at ${QIVEN_TP_SHA}; re-pin explicitly in CMakeLists.txt and re-run the third-party-verify gate")
endif()

# Consumption law (standard section 5; conventions cross-repo-cmake.md):
# per-package add_subdirectory into the CONSUMER build tree; NO directory-
# level EXCLUDE_FROM_ALL (VS generator drops ProjectReferences -> LNK1104).
add_subdirectory("${QIVEN_THIRD_PARTY_ROOT}/packages/sqlite3" "${CMAKE_BINARY_DIR}/tp/sqlite3")

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
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/auth.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/claims.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/generation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/intent.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/observed_action.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/identity.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/cognition_bundle.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/cognition_port.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/deployment_profile.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/runtime_journal.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/profile.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/reconciliation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/requirement.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/resolver.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/scope.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/transaction.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/state_store.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/decision.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/executor.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/failure.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/ingress.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/outcome.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/admission.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/execution_authority.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/activation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/claims.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/port/interception.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qiven/runtime/adapter/bridge.hpp
)

set(
    QIVEN_RUNTIME_PRIVATE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter/handshake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port/activation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port/execution_authority.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port/interception.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter/manifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter/loopback.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter/bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/claims.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cognition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/generation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/observed_action.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/identity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/profile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/reconciliation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/requirement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transaction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/state_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decision.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/failure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ingress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/outcome.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/admission.cpp
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

add_executable(qiven-adapter-bridge apps/adapter_bridge_main.cpp)
add_executable(qiven::adapter_bridge ALIAS qiven-adapter-bridge)
set_target_properties(
    qiven-adapter-bridge
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Apps"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(qiven-adapter-bridge PRIVATE qiven::runtime)
target_compile_features(qiven-adapter-bridge PRIVATE cxx_std_20)

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
