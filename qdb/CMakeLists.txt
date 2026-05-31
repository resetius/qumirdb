find_package(PkgConfig REQUIRED)
pkg_check_modules(ARROW REQUIRED IMPORTED_TARGET arrow)
pkg_check_modules(PARQUET REQUIRED IMPORTED_TARGET parquet)

add_library(qumirdb
    stub.cpp
    io/parquet/source.cpp
    io/text/sink.cpp
    ops/source.cpp
    ops/filter.cpp
    ops/project.cpp
    ops/parse_kernel.cpp
    kernel/gen.cpp
    kernel/compiler.cpp
    exec/source_exec.cpp
    exec/filter_exec.cpp
    exec/project_exec.cpp
    exec/planner.cpp
)
target_include_directories(qumirdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(qumirdb PUBLIC cxx_std_23)
target_link_libraries(qumirdb PUBLIC
    qumir
    qumir_runtime
    qumir_codegen_llvm
    PkgConfig::ARROW
    PkgConfig::PARQUET
)
