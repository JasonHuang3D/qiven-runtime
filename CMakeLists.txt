cmake_minimum_required(VERSION 3.28)
project(qiven-runtime VERSION 0.1.0 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(MSVC)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")
endif()

option(QIVEN_RUNTIME_BUILD_TESTS "Build qiven-runtime tests" ${PROJECT_IS_TOP_LEVEL})

# Bootstrap-only: the library starts INTERFACE; choose the final structure
# and linkage after domain design is approved (reference: qiven-foundation).
add_library(qiven-runtime INTERFACE)
add_library(qiven::runtime ALIAS qiven-runtime)
target_compile_features(qiven-runtime INTERFACE cxx_std_20)

add_executable(qiven-runtime-app app/main.cpp)
add_executable(qiven::runtime_app ALIAS qiven-runtime-app)
set_target_properties(
    qiven-runtime-app
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "Apps"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:qiven-runtime-app>"
)
target_link_libraries(qiven-runtime-app PRIVATE qiven::runtime)
target_compile_features(qiven-runtime-app PRIVATE cxx_std_20)

if(QIVEN_RUNTIME_BUILD_TESTS)
    enable_testing()
endif()
