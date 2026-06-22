add_executable(qdb cli.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(READLINE REQUIRED IMPORTED_TARGET readline)

target_link_libraries(qdb PUBLIC qumirdb PkgConfig::READLINE)
