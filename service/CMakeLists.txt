find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(LLVM_LIBS Support)

add_executable(qdb_web server.cpp)
target_compile_features(qdb_web PUBLIC cxx_std_23)
target_link_libraries(qdb_web PUBLIC coroio qumirdb ${LLVM_LIBS})
target_include_directories(qdb_web PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(qdb_web PRIVATE ${LLVM_DEFINITIONS})
target_compile_definitions(qdb_web PRIVATE
    QDB_SERVICE_STATIC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/static"
    QDB_BUILD_BIN_DIR="$<TARGET_FILE_DIR:qdb_plan_export>"
)
