add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto PRIVATE nanobot::crypto)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_secret test_secret.c)
target_link_libraries(test_secret PRIVATE nanobot::crypto)
add_test(NAME secret COMMAND test_secret)
