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

target_compile_definitions(qdb PRIVATE QDB_VERSION_STRING="${QDB_VERSION_STRING}")
target_compile_definitions(qdb_plan_export PRIVATE QDB_VERSION_STRING="${QDB_VERSION_STRING}")

install(TARGETS qdb qdb_plan_export
    RUNTIME DESTINATION ${QDB_PRIVATE_BINDIR}
    COMPONENT engine)

# The real binaries stay in the versioned tree; the executable path resolves through
# these links, so qdb still finds the oz modules relative to itself.
install(CODE "
    set(_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
    file(MAKE_DIRECTORY \"\${_bindir}\")
    file(CREATE_LINK \"../${QDB_PRIVATE_BINDIR}/qdb\" \"\${_bindir}/qdb\" SYMBOLIC)
    file(CREATE_LINK \"../${QDB_PRIVATE_BINDIR}/qdb_plan_export\" \"\${_bindir}/qdb_plan_export\" SYMBOLIC)
" COMPONENT engine)
