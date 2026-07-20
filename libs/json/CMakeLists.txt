add_library(nanobot_json STATIC json.c)
add_library(nanobot::json ALIAS nanobot_json)
target_include_directories(nanobot_json
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(nanobot_json PRIVATE _GNU_SOURCE)
