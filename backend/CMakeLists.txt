cmake_minimum_required(VERSION 3.10)
project(AevixBackend C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Locate LLVM ---
# Priority: user-provided LLVM_DIR > llvm-config > env var LLVM_ROOT > known paths

if(NOT DEFINED LLVM_DIR)
    # 1) Try llvm-config --cmakedir (works everywhere LLVM is properly installed)
    find_program(_LLVM_CONFIG llvm-config)
    if(_LLVM_CONFIG)
        execute_process(
            COMMAND ${_LLVM_CONFIG} --cmakedir
            OUTPUT_VARIABLE _LLVM_CMAKEDIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_LLVM_CMAKEDIR AND EXISTS "${_LLVM_CMAKEDIR}/LLVMConfig.cmake")
            set(LLVM_DIR "${_LLVM_CMAKEDIR}")
        endif()
    endif()
endif()

if(NOT DEFINED LLVM_DIR)
    # 2) Check LLVM_ROOT / LLVM_DIR environment variables
    if(DEFINED ENV{LLVM_DIR})
        set(LLVM_DIR "$ENV{LLVM_DIR}")
    elseif(DEFINED ENV{LLVM_ROOT})
        set(LLVM_DIR "$ENV{LLVM_ROOT}/lib/cmake/llvm")
    endif()
endif()

if(NOT DEFINED LLVM_DIR OR NOT EXISTS "${LLVM_DIR}/LLVMConfig.cmake")
    # 3) Search known installation paths
    set(_LLVM_SEARCH_PATHS "")

    if(APPLE)
        list(APPEND _LLVM_SEARCH_PATHS
            "/opt/homebrew/opt/llvm/lib/cmake/llvm"       # macOS Homebrew ARM
            "/usr/local/opt/llvm/lib/cmake/llvm"          # macOS Homebrew Intel
            "/opt/local/libexec/llvm-devel/lib/cmake/llvm" # macOS MacPorts
        )
    elseif(UNIX)
        file(GLOB _LINUX_LLVM_DIRS /usr/lib/llvm-*/lib/cmake/llvm /usr/lib64/cmake/llvm /usr/lib/cmake/llvm)
        list(APPEND _LLVM_SEARCH_PATHS ${_LINUX_LLVM_DIRS})
    elseif(WIN32)
        list(APPEND _LLVM_SEARCH_PATHS
            "C:/Program Files/LLVM/lib/cmake/llvm"
            "C:/LLVM/lib/cmake/llvm"
            "$ENV{PROGRAMFILES}/LLVM/lib/cmake/llvm"
        )
    endif()

    foreach(_PATH IN LISTS _LLVM_SEARCH_PATHS)
        if(EXISTS "${_PATH}/LLVMConfig.cmake")
            set(LLVM_DIR "${_PATH}")
            break()
        endif()
    endforeach()
endif()

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

# Arena runtime object, linked into every final Aevix binary (tests + Go CLI).
set(RUNTIME_OBJ ${CMAKE_BINARY_DIR}/runtime.o)
add_custom_command(
    OUTPUT ${RUNTIME_OBJ}
    COMMAND ${CMAKE_C_COMPILER} -c ${CMAKE_CURRENT_SOURCE_DIR}/src/aevix_runtime.c -o ${RUNTIME_OBJ}
    DEPENDS src/aevix_runtime.c
)
add_custom_target(aevix-runtime ALL DEPENDS ${RUNTIME_OBJ})

llvm_map_components_to_libnames(llvm_libs core executionengine orcjit native)
target_link_libraries(aevix-backend ${llvm_libs})

target_include_directories(aevix-backend PRIVATE include)