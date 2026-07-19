#ifndef NANOBOT_MCP_H
#define NANOBOT_MCP_H
#include "agent.h"

/* Stdio MCP server (JSON-RPC 2.0, Content-Length framing OR newline NDJSON).
 * Implements: initialize, tools/list, tools/call, ping.
 * Tools: run_terminal_command, nanobot_ask (full agent turn).
 */
int ng_mcp_stdio_run(ng_agent_cfg *agent);

#endif
