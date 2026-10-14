add_executable(test_filter test_filter.c ${CMAKE_SOURCE_DIR}/src/stream_filter.c)
target_include_directories(test_filter PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME filter COMMAND test_filter)
