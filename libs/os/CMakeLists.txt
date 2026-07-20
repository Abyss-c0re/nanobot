add_library(nanobot_os STATIC os.c)
add_library(nanobot::os ALIAS nanobot_os)
target_include_directories(nanobot_os
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(nanobot_os PRIVATE _GNU_SOURCE)
target_link_libraries(nanobot_os PUBLIC nanobot::crypto)
