find_package(PkgConfig REQUIRED)
pkg_check_modules(ARROW REQUIRED IMPORTED_TARGET arrow)
pkg_check_modules(PARQUET REQUIRED IMPORTED_TARGET parquet)

add_library(qumirdb
    stub.cpp
    modules/qumirdb.cpp
    modules/qumirdb_runtime.cpp
    io/parquet/source.cpp
    io/text/sink.cpp
    ops/source.cpp
    ops/filter.cpp
    ops/project.cpp
    ops/aggregate.cpp
    ops/parse_kernel.cpp
    sexp/printer.cpp
    sexp/parser.cpp
    pipeline/unbound_vars.cpp
    pipeline/typing.cpp
    pipeline/column_pruning.cpp
    kernel/gen.cpp
    kernel/lib.cpp
    kernel/compiler.cpp
    exec/source_exec.cpp
    exec/filter_exec.cpp
    exec/project_exec.cpp
    exec/aggregate_exec.cpp
    exec/planner.cpp
)
target_include_directories(qumirdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(qumirdb PUBLIC cxx_std_23)
if(UNIX AND NOT APPLE)
    target_link_libraries(qumirdb PUBLIC
        "$<LINK_GROUP:RESCAN,qumir,qumir_runtime,qumir_codegen_llvm>"
    )
else()
    target_link_libraries(qumirdb PUBLIC
        qumir
        qumir_runtime
        qumir_codegen_llvm
    )
endif()
target_link_libraries(qumirdb PUBLIC
    PkgConfig::ARROW
    PkgConfig::PARQUET
)
