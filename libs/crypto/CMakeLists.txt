add_library(nanobot_crypto STATIC
  random.c
  ct.c
  hex.c
  secret.c
  vendor/monocypher.c
)
add_library(nanobot::crypto ALIAS nanobot_crypto)
target_include_directories(nanobot_crypto
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(nanobot_crypto PRIVATE _GNU_SOURCE)
# monocypher is pure C; silence unused if any
