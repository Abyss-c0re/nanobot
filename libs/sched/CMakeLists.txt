add_library(nanobot_sched STATIC sched.c)
add_library(nanobot::sched ALIAS nanobot_sched)
target_include_directories(nanobot_sched PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
