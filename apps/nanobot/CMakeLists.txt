add_executable(nanobot main.c)
target_include_directories(nanobot PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(nanobot PRIVATE nanobot::legacy)
# convenience symlink name for MCP
add_custom_command(TARGET nanobot POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink nanobot
          $<TARGET_FILE_DIR:nanobot>/nanobot-mcp
  COMMENT "symlink nanobot-mcp"
)
