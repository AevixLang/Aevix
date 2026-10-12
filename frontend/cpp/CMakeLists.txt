cmake_minimum_required(VERSION 3.16)
project(aevix-frontend CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(aevix-frontend main.cpp)
target_compile_features(aevix-frontend PRIVATE cxx_std_20)
