#include "mcp.h"
#include "shell.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* Read one MCP message: prefer Content-Length framing; fallback newline JSON */
static char *mcp_read_message(void) {
  char header[4096];
  size_t hlen = 0;
  int have_cl = 0;
  long content_length = -1;

  /* peek first byte */
  int c = fgetc(stdin);
  if (c == EOF) return NULL;
  ungetc(c, stdin);

  /* If starts with '{', treat as NDJSON line mode */
  if (c == '{') {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (fgets(buf + len, (int)(cap - len), stdin)) {
      len += strlen(buf + len);
      if (len && buf[len-1] == '\n') { buf[len-1] = 0; return buf; }
      if (len + 2 >= cap) {
        cap *= 2;
        char *n = realloc(buf, cap);
        if (!n) { free(buf); return NULL; }
        buf = n;
      }
    }
    if (len) { buf[len] = 0; return buf; }
    free(buf);
    return NULL;
  }

  /* Content-Length framing */
  while (fgets(header + hlen, (int)(sizeof header - hlen), stdin)) {
    size_t line_len = strlen(header + hlen);
    if (line_len == 0) break;
    char *line = header + hlen;
    if (strncmp(line, "Content-Length:", 15) == 0) {
      content_length = strtol(line + 15, NULL, 10);
      have_cl = 1;
    }
    if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
    hlen += line_len;
    if (hlen + 2 >= sizeof header) break;
  }
  if (!have_cl || content_length < 0 || content_length > 8 * 1024 * 1024) return NULL;
  char *body = malloc((size_t)content_length + 1);
  if (!body) return NULL;
  size_t got = 0;
  while (got < (size_t)content_length) {
    size_t r = fread(body + got, 1, (size_t)content_length - got, stdin);
    if (r == 0) { free(body); return NULL; }
    got += r;
  }
  body[content_length] = 0;
  return body;
}

static void mcp_write_message(const char *json) {
  size_t n = strlen(json);
  /* Content-Length framing (MCP standard) */
  fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", n, json);
  fflush(stdout);
}

static void mcp_reply_result(const char *id_json, const char *result_json) {
  /* id_json is already JSON token (number or "string") */
  char *msg = NULL;
  asprintf(&msg, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id_json, result_json);
  if (msg) { mcp_write_message(msg); free(msg); }
}

static void mcp_reply_error(const char *id_json, int code, const char *message) {
  char *esc = ng_json_escape(message);
  char *msg = NULL;
  asprintf(&msg,
    "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
    id_json ? id_json : "null", code, esc ? esc : "error");
  if (msg) { mcp_write_message(msg); free(msg); }
  free(esc);
}

static char *extract_id_token(const char *json) {
  const char *p = strstr(json, "\"id\"");
  if (!p) return strdup("null");
  p += 4;
  while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
  if (*p == '"') {
    const char *s = p;
    p++;
    while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
    if (*p == '"') p++;
    size_t n = (size_t)(p - s);
    char *o = malloc(n + 1);
    memcpy(o, s, n); o[n] = 0;
    return o;
  }
  if (isdigit((unsigned char)*p) || *p == '-') {
    const char *s = p;
    if (*p == '-') p++;
    while (isdigit((unsigned char)*p)) p++;
    size_t n = (size_t)(p - s);
    char *o = malloc(n + 1);
    memcpy(o, s, n); o[n] = 0;
    return o;
  }
  if (strncmp(p, "null", 4) == 0) return strdup("null");
  return strdup("null");
}

static char *extract_method(const char *json) {
  return ng_json_get_string(json, "method");
}

/* tools/call params: name + arguments object */
static int handle_tools_call(ng_agent_cfg *agent, const char *json, char **out_result) {
  char *name = ng_json_get_string(json, "name");
  if (!name) {
    /* nested under params */
    const char *params = strstr(json, "\"params\"");
    if (params) name = ng_json_get_string(params, "name");
  }
  const char *params = strstr(json, "\"params\"");
  if (!params) params = json;

  if (!name) {
    *out_result = strdup("{\"content\":[{\"type\":\"text\",\"text\":\"missing tool name\"}],\"isError\":true}");
    return -1;
  }

  if (strcmp(name, "run_terminal_command") == 0 || strcmp(name, "shell") == 0) {
    char *cmd = ng_json_get_string(params, "command");
    if (!cmd) {
      /* arguments.command */
      const char *args = strstr(params, "\"arguments\"");
      if (args) cmd = ng_json_get_string(args, "command");
    }
    if (!cmd) {
      free(name);
      *out_result = strdup("{\"content\":[{\"type\":\"text\",\"text\":\"missing command\"}],\"isError\":true}");
      return -1;
    }
    ng_log("mcp tools/call shell: %.200s", cmd);
    ng_cmd_result cr = ng_run_command(cmd, agent->timeout_sec);
    char *text = NULL;
    asprintf(&text, "exit=%d\n%s", cr.exit_code, cr.output ? cr.output : "");
    char *esc = ng_json_escape(text);
    asprintf(out_result,
      "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}",
      esc ? esc : "", cr.exit_code != 0 ? "true" : "false");
    free(text); free(esc); free(cmd); free(name);
    ng_cmd_result_free(&cr);
    return 0;
  }

  if (strcmp(name, "nanobot_ask") == 0) {
    char *prompt = ng_json_get_string(params, "prompt");
    if (!prompt) {
      const char *args = strstr(params, "\"arguments\"");
      if (args) prompt = ng_json_get_string(args, "prompt");
    }
    if (!prompt) {
      free(name);
      *out_result = strdup("{\"content\":[{\"type\":\"text\",\"text\":\"missing prompt\"}],\"isError\":true}");
      return -1;
    }
    ng_log("mcp tools/call nanobot_ask: %.200s", prompt);
    char *reply = ng_agent_run(agent, prompt);
    char *esc = ng_json_escape(reply ? reply : "");
    asprintf(out_result,
      "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":false}",
      esc ? esc : "");
    free(reply); free(esc); free(prompt); free(name);
    return 0;
  }

  char *esc = ng_json_escape(name);
  asprintf(out_result,
    "{\"content\":[{\"type\":\"text\",\"text\":\"unknown tool: %s\"}],\"isError\":true}",
    esc ? esc : "?");
  free(esc); free(name);
  return -1;
}

int ng_mcp_stdio_run(ng_agent_cfg *agent) {
  ng_log("mcp stdio server start version=%s", NG_VERSION);
  /* disable buffering issues */
  setvbuf(stdin, NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);

  while (1) {
    char *msg = mcp_read_message();
    if (!msg) break;
    char *id = extract_id_token(msg);
    char *method = extract_method(msg);
    ng_log("mcp method=%s", method ? method : "(null)");

    if (!method) {
      mcp_reply_error(id, -32600, "invalid request");
      free(id); free(msg);
      continue;
    }

    if (strcmp(method, "initialize") == 0) {
      char *result = NULL;
      asprintf(&result,
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"nanobot\",\"version\":\"%s\"}}",
        NG_VERSION);
      mcp_reply_result(id, result);
      free(result);
    } else if (strcmp(method, "notifications/initialized") == 0 ||
               strcmp(method, "initialized") == 0) {
      /* no response for notifications (id null) */
      if (id && strcmp(id, "null") != 0) {
        mcp_reply_result(id, "{}");
      }
    } else if (strcmp(method, "ping") == 0) {
      mcp_reply_result(id, "{}");
    } else if (strcmp(method, "tools/list") == 0) {
      const char *tools =
        "{\"tools\":["
        "{\"name\":\"run_terminal_command\","
        "\"description\":\"Run a shell command on the nanobot host machine. Returns exit code and output.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"}},"
        "\"required\":[\"command\"]}},"
        "{\"name\":\"nanobot_ask\","
        "\"description\":\"Ask Grok via nanobot agent loop (CLI tool enabled) and return the final answer.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"prompt\":{\"type\":\"string\",\"description\":\"User prompt\"}},"
        "\"required\":[\"prompt\"]}}"
        "]}";
      mcp_reply_result(id, tools);
    } else if (strcmp(method, "tools/call") == 0) {
      char *result = NULL;
      handle_tools_call(agent, msg, &result);
      mcp_reply_result(id, result ? result : "{\"content\":[]}");
      free(result);
    } else {
      /* ignore unknown notifications */
      if (strncmp(method, "notifications/", 14) == 0) {
        /* no-op */
      } else {
        mcp_reply_error(id, -32601, "method not found");
      }
    }
    free(method); free(id); free(msg);
  }
  ng_log("mcp stdio server exit");
  return 0;
}
