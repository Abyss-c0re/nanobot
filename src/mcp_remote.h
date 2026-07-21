#ifndef NANOBOT_MCP_REMOTE_H
#define NANOBOT_MCP_REMOTE_H

/* Outbound MCP client: this process connects TO remote MCP servers.
 * Config: $NANOBOT_HOME/mcp_servers.json
 * Transports: HTTP JSON-RPC (POST) + optional SSE body.
 */

/* Full servers array JSON for clients (malloc'd). */
char *ng_mcp_servers_list_json(void);

/* Replace entire config from JSON body. Returns 0 ok. */
int ng_mcp_servers_save_raw(const char *json_body);

/* Probe one server (by id) or ad-hoc url. Returns JSON status (malloc'd). */
char *ng_mcp_server_probe(const char *id, const char *url_override, const char *auth_header);

/* Extra OpenAI tools JSON array elements (leading commas ok via fragment).
 * Returns malloc'd string starting with ',' or "" if none. */
char *ng_mcp_openai_tools_fragment(void);

/* If tool name is mcp_list or mcp_call (or mcp__*), run it. Returns malloc'd
 * text result, or NULL if not an MCP tool. */
char *ng_mcp_try_tool(const char *name, const char *args_json);

#endif
