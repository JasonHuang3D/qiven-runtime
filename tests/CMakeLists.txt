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
        headers/identity.cpp
        headers/profile.cpp
)

target_link_libraries(qiven-runtime-header-check PRIVATE qiven::runtime)
qiven_enable_test_warnings(qiven-runtime-header-check)

set_target_properties(
    qiven-runtime-header-check
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Tests/Header Checks"
)
