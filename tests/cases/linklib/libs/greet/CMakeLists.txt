cmake_minimum_required(VERSION 3.20)
project(greet LANGUAGES C)
add_library(greet STATIC greet.c)
target_include_directories(greet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
