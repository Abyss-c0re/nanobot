add_library(nanobot_provider STATIC provider.c)
add_library(nanobot::provider ALIAS nanobot_provider)
target_include_directories(nanobot_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nanobot_provider PUBLIC nanobot::os)
