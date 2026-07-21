cmake_minimum_required(VERSION 3.16)
project(nanobot VERSION 0.5.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NanobotCompiler)

# Feature switches (defaults ON). Compile definitions gate CLI/help and optional paths.
# Sources stay linked so unit of deploy is one binary; slim builds can set OFF and use
# corresponding #if NANOBOT_ENABLE_* == 0 stubs (see apps/nanobot/main.c).
option(NANOBOT_BUILD_TESTS "unit tests" ON)
option(NANOBOT_STATIC_LIBS "static libs" ON)
option(NANOBOT_ENABLE_MCP "stdio MCP server" ON)
option(NANOBOT_ENABLE_AUTH "device-code provider auth + sealed session" ON)
option(NANOBOT_ENABLE_PEER "HTTP peer/API server" ON)
option(NANOBOT_ENABLE_HUB "hub dual-port OUT" ON)
option(NANOBOT_ENABLE_SHELL "shell / @! execution" ON)
option(NANOBOT_ENABLE_PROVIDERS "LLM backends (Grok + OpenAI-compatible)" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_subdirectory(libs/crypto)
add_subdirectory(libs/os)
add_subdirectory(libs/json)

add_library(nanobot_legacy STATIC
  src/util.c
  src/shell.c
  src/shell_gate.c
  src/auth.c
  src/memory.c
  src/task.c
  src/agent.c
  src/http.c
  src/mcp.c
  src/mcp_remote.c
  src/improve.c
  src/hub.c
)
add_library(nanobot::legacy ALIAS nanobot_legacy)
target_include_directories(nanobot_legacy
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(nanobot_legacy
  PUBLIC
    nanobot::crypto
    nanobot::os
    nanobot::json
)

foreach(_f MCP AUTH PEER HUB SHELL PROVIDERS)
  if(NANOBOT_ENABLE_${_f})
    target_compile_definitions(nanobot_legacy PUBLIC NANOBOT_ENABLE_${_f}=1)
  else()
    target_compile_definitions(nanobot_legacy PUBLIC NANOBOT_ENABLE_${_f}=0)
  endif()
endforeach()

add_subdirectory(apps/nanobot)

if(NANOBOT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/crypto)
endif()

message(STATUS "nanobot: MCP=${NANOBOT_ENABLE_MCP} AUTH=${NANOBOT_ENABLE_AUTH} PEER=${NANOBOT_ENABLE_PEER} HUB=${NANOBOT_ENABLE_HUB} SHELL=${NANOBOT_ENABLE_SHELL} PROVIDERS=${NANOBOT_ENABLE_PROVIDERS}")
