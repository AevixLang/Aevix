cmake_minimum_required(VERSION 3.10)
project(AevixBackend)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_executable(aevix-backend
    src/main.cpp
    src/json_reader.cpp
    src/codegen.cpp
)

llvm_map_components_to_libnames(llvm_libs core executionengine orcjit native)
target_link_libraries(aevix-backend ${llvm_libs})

target_include_directories(aevix-backend PRIVATE include)