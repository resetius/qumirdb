cmake_minimum_required(VERSION 3.30)
project(qumir)
enable_testing()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(qdb)
add_subdirectory(test)
add_subdirectory(bin)
add_subdirectory(externals)
