add_library(qumirdb stub.cpp)
target_include_directories(qumirdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(qumirdb PUBLIC cxx_std_23)
target_link_libraries(qumirdb PUBLIC qumir)
