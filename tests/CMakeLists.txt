function(qiven_enable_test_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
endfunction()

add_executable(qiven-runtime-generation runtime_generation.cpp)

target_link_libraries(qiven-runtime-generation PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-generation)
target_precompile_headers(qiven-runtime-generation REUSE_FROM qiven-runtime)

set_target_properties(
    qiven-runtime-generation
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Tests"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(NAME qiven-runtime.generation COMMAND qiven-runtime-generation)

add_executable(qiven-runtime-adapter-handshake adapter_handshake.cpp)

target_link_libraries(qiven-runtime-adapter-handshake PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-adapter-handshake)
target_precompile_headers(qiven-runtime-adapter-handshake REUSE_FROM qiven-runtime)

set_target_properties(
    qiven-runtime-adapter-handshake
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Tests"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(NAME qiven-runtime.adapter-handshake COMMAND qiven-runtime-adapter-handshake)

add_executable(qiven-runtime-mediation-claims mediation_claims.cpp)

target_link_libraries(qiven-runtime-mediation-claims PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-mediation-claims)
target_precompile_headers(qiven-runtime-mediation-claims REUSE_FROM qiven-runtime)

set_target_properties(
    qiven-runtime-mediation-claims
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Tests"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(NAME qiven-runtime.mediation-claims COMMAND qiven-runtime-mediation-claims)

add_library(
    qiven-runtime-header-check
    OBJECT
        headers/adapter_handshake.cpp
        headers/adapter_manifest.cpp
        headers/claims.cpp
        headers/generation.cpp
        headers/cognition_port.cpp
        headers/identity.cpp
        headers/requirement.cpp
        headers/resolver.cpp
        headers/transaction.cpp
        headers/decision.cpp
        headers/executor.cpp
        headers/ingress.cpp
        headers/state.cpp
        headers/intent.cpp
        headers/observed_action.cpp
        headers/profile.cpp
)

target_link_libraries(qiven-runtime-header-check PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-header-check)

set_target_properties(
    qiven-runtime-header-check
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Tests/Header Checks"
        DISABLE_PRECOMPILE_HEADERS ON
)

add_executable(qiven-runtime-observed-action observed_action.cpp)
target_link_libraries(qiven-runtime-observed-action PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-observed-action)
target_precompile_headers(qiven-runtime-observed-action REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-observed-action PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.observed-action COMMAND qiven-runtime-observed-action)

add_executable(qiven-runtime-intent intent.cpp)
target_link_libraries(qiven-runtime-intent PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-intent)
target_precompile_headers(qiven-runtime-intent REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-intent PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.intent COMMAND qiven-runtime-intent)

add_executable(qiven-runtime-correlation correlation.cpp)
target_link_libraries(qiven-runtime-correlation PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-correlation)
target_precompile_headers(qiven-runtime-correlation REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-correlation PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.correlation COMMAND qiven-runtime-correlation)

add_executable(qiven-runtime-cognition-pin cognition_pin.cpp)
target_link_libraries(qiven-runtime-cognition-pin PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-cognition-pin)
target_precompile_headers(qiven-runtime-cognition-pin REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-cognition-pin PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.cognition-pin COMMAND qiven-runtime-cognition-pin)

add_executable(qiven-runtime-requirement-derivation requirement_derivation.cpp)
target_link_libraries(qiven-runtime-requirement-derivation PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-requirement-derivation)
target_precompile_headers(qiven-runtime-requirement-derivation REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-requirement-derivation PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.requirement-derivation COMMAND qiven-runtime-requirement-derivation)

add_executable(qiven-runtime-resolver-registry resolver_registry.cpp)
target_link_libraries(qiven-runtime-resolver-registry PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-resolver-registry)
target_precompile_headers(qiven-runtime-resolver-registry REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-resolver-registry PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.resolver-registry COMMAND qiven-runtime-resolver-registry)

add_executable(qiven-runtime-before-judgment-path before_judgment_path.cpp)
target_link_libraries(qiven-runtime-before-judgment-path PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-before-judgment-path)
target_precompile_headers(qiven-runtime-before-judgment-path REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-before-judgment-path PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.before-judgment-path COMMAND qiven-runtime-before-judgment-path)

add_executable(qiven-runtime-before-execution-path before_execution_path.cpp)
target_link_libraries(qiven-runtime-before-execution-path PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-before-execution-path)
target_precompile_headers(qiven-runtime-before-execution-path REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-before-execution-path PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.before-execution-path COMMAND qiven-runtime-before-execution-path)

add_executable(qiven-runtime-decision-binding decision_binding.cpp)
target_link_libraries(qiven-runtime-decision-binding PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-decision-binding)
target_precompile_headers(qiven-runtime-decision-binding REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-decision-binding PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.decision-binding COMMAND qiven-runtime-decision-binding)

add_executable(qiven-runtime-ingress-queue ingress_queue.cpp)
target_link_libraries(qiven-runtime-ingress-queue PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-ingress-queue)
target_precompile_headers(qiven-runtime-ingress-queue REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-ingress-queue PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.ingress-queue COMMAND qiven-runtime-ingress-queue)

add_executable(qiven-runtime-executor executor.cpp)
target_link_libraries(qiven-runtime-executor PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-executor)
target_precompile_headers(qiven-runtime-executor REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-executor PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.executor COMMAND qiven-runtime-executor)

add_executable(qiven-runtime-control-core control_core.cpp)
target_link_libraries(qiven-runtime-control-core PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-control-core)
target_precompile_headers(qiven-runtime-control-core REUSE_FROM qiven-runtime)
set_target_properties(qiven-runtime-control-core PROPERTIES CXX_EXTENSIONS OFF FOLDER "Tests" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME qiven-runtime.control-core COMMAND qiven-runtime-control-core)
