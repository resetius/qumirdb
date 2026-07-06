add_executable(qdb cli.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(READLINE REQUIRED IMPORTED_TARGET readline)
find_package(LLVM REQUIRED CONFIG)

target_link_libraries(qdb PUBLIC qumirdb PkgConfig::READLINE)

add_executable(qdb_plan_export plan_export.cpp)
target_compile_features(qdb_plan_export PUBLIC cxx_std_23)
target_link_libraries(qdb_plan_export PUBLIC qumirdb)
target_include_directories(qdb_plan_export PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(qdb_plan_export PRIVATE ${LLVM_DEFINITIONS})
