find_package(PkgConfig REQUIRED)

pkg_check_modules(GTEST gtest REQUIRED)

macro(ut name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} qumirdb ${GTEST_LIBRARIES})
    target_include_directories(${name} PRIVATE ${GTEST_INCLUDE_DIRS})
    target_link_directories(${name} PRIVATE ${GTEST_LIBRARY_DIRS})
    add_test(NAME ${name} COMMAND ${name} ${ARGN} --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/${name}.xml)
endmacro()

ut(test_io test_io.cpp)
ut(test_sexp test_sexp.cpp)
ut(test_aggregation test_aggregation.cpp ${CMAKE_SOURCE_DIR}/qdb/kernel/aggregation)
