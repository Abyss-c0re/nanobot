add_executable(test_wire test_wire.c ${CMAKE_SOURCE_DIR}/src/wire_compat.c)
target_include_directories(test_wire PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME wire COMMAND test_wire)
