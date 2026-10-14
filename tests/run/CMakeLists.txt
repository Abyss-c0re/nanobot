add_executable(test_run test_run.c)
target_link_libraries(test_run PRIVATE nanobot::legacy)
add_test(NAME run COMMAND test_run)

add_executable(test_motive test_motive.c)
target_link_libraries(test_motive PRIVATE nanobot::legacy)
add_test(NAME motive COMMAND test_motive)
