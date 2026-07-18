find_package(PkgConfig REQUIRED)
pkg_check_modules(ARROW REQUIRED IMPORTED_TARGET arrow)
pkg_check_modules(PARQUET REQUIRED IMPORTED_TARGET parquet)
find_package(LLVM REQUIRED CONFIG)

add_library(qumirdb
    stub.cpp
    modules/qumirdb.cpp
    modules/qumirdb_runtime.cpp
    io/parquet/source.cpp
    io/text/sink.cpp
    plan/ops/source.cpp
    plan/ops/filter.cpp
    plan/ops/project.cpp
    plan/ops/aggregate.cpp
    plan/ops/join.cpp
    plan/ops/union.cpp
    plan/ops/sort.cpp
    plan/ops/limit.cpp
    plan/ops/parse_kernel.cpp
    sexp/printer.cpp
    sexp/parser.cpp
    sql/ast.cpp
    sql/lexer.cpp
    sql/parser.cpp
    sql/printer.cpp
    plan/passes/unbound_vars.cpp
    plan/passes/typing.cpp
    plan/passes/column_pruning.cpp
    plan/passes/estimate_stats.cpp
    plan/passes/qualify_columns.cpp
    plan/passes/equijoin.cpp
    plan/passes/join_order.cpp
    plan/passes/cbo/dpccp.cpp
    plan/passes/top_sort.cpp
    plan/passes/flatten_conjuncts.cpp
    plan/passes/flatten_disjuncts.cpp
    plan/passes/factor_conjuncts.cpp
    plan/clone_expr.cpp
    plan/pipeline.cpp
    plan/plan_print.cpp
    plan/build.cpp
    kernel/column_value.cpp
    kernel/gen.cpp
    kernel/aggregate_key.cpp
    kernel/join_key.cpp
    kernel/join_gen.cpp
    kernel/builder.cpp
    kernel/annotate_type.cpp
    kernel/spec.cpp
    kernel/lib.cpp
    kernel/compiler.cpp
    kernel/finalize.cpp
    exec/filter_exec.cpp
    exec/project_exec.cpp
    exec/aggregate_exec.cpp
    exec/join_exec.cpp
    exec/sort_exec.cpp
    exec/planner_helpers.cpp
    scheduler/connection.cpp
    scheduler/executor.cpp
    scheduler/graph.cpp
    scheduler/plan_lowerer.cpp
    scheduler/runtime_adapter.cpp
    scheduler/scan_split.cpp
    scheduler/single_threaded_scheduler.cpp
    scheduler/threaded_scheduler.cpp
)
target_include_directories(qumirdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(qumirdb PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(qumirdb PUBLIC QDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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
