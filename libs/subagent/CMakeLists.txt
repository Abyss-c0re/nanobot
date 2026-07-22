add_library(nanobot_subagent STATIC subagent.c)
add_library(nanobot::subagent ALIAS nanobot_subagent)
target_include_directories(nanobot_subagent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nanobot_subagent PUBLIC nanobot::provider)
