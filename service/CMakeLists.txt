find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(LLVM_LIBS Support)

add_executable(qdb_web server.cpp)
target_compile_features(qdb_web PUBLIC cxx_std_23)
target_link_libraries(qdb_web PUBLIC coroio qumirdb)
target_include_directories(qdb_web PRIVATE ${LLVM_INCLUDE_DIRS}) # json
target_compile_definitions(qdb_web PRIVATE
    QDB_SERVICE_STATIC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/static"
    QDB_BUILD_BIN_DIR="$<TARGET_FILE_DIR:qdb>"
    QDB_VERSION_STRING="${QDB_VERSION_STRING}"
)

install(PROGRAMS $<TARGET_FILE:qdb_web>
    DESTINATION bin
    RENAME qumirdb-service
    COMPONENT service)

install(DIRECTORY static/
    DESTINATION share/qumirdb-service/static
    COMPONENT service)

configure_file(qumirdb-service.service.in qumirdb-service.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qumirdb-service.service
    DESTINATION lib/systemd/system
    COMPONENT service)
