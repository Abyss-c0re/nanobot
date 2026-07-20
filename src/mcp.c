#include "mcp.h"
#include "improve.h"
#include "memory.h"
#include "shell.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

/* Read one MCP message: prefer Content-Length framing; fallback newline JSON */
static char *mcp_read_message(void) {
  char header[4096];
  size_t hlen = 0;
  int have_cl = 0;
  long content_length = -1;

  int c = fgetc(stdin);
  if (c == EOF) return NULL;
  ungetc(c, stdin);

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
  fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", n, json);
  fflush(stdout);
}

static void mcp_reply_result(const char *id_json, const char *result_json) {
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

/* Get tool name from tools/call body */
static char *tool_name(const char *json) {
  const char *params = strstr(json, "\"params\"");
  if (params) {
    char *n = ng_json_get_string(params, "name");
    if (n) return n;
  }
  return ng_json_get_string(json, "name");
}

static char *tool_arg(const char *json, const char *key) {
  const char *params = strstr(json, "\"params\"");
  if (!params) params = json;
  char *v = ng_json_get_string(params, key);
  if (v) return v;
  const char *args = strstr(params, "\"arguments\"");
  if (args) return ng_json_get_string(args, key);
  return NULL;
}


static int text_result(char **out, const char *text, int is_err) {
  char *esc = ng_json_escape(text ? text : "");
  int rc = asprintf(out,
    "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}",
    esc ? esc : "", is_err ? "true" : "false");
  free(esc);
  return rc < 0 ? -1 : 0;
}

static int handle_tools_call(ng_agent_cfg *agent, const char *json, char **out_result) {
  char *name = tool_name(json);
  if (!name) {
    *out_result = strdup("{\"content\":[{\"type\":\"text\",\"text\":\"missing tool name\"}],\"isError\":true}");
    return -1;
  }

  if (strcmp(name, "run_terminal_command") == 0 || strcmp(name, "shell") == 0) {
    char *cmd = tool_arg(json, "command");
    if (!cmd) {
      free(name);
      return text_result(out_result, "missing command", 1);
    }
    ng_log("mcp tools/call shell: %.200s", cmd);
    ng_cmd_result cr = ng_run_command(cmd, agent->timeout_sec > 0 ? agent->timeout_sec : 60);
    char *text = NULL;
    asprintf(&text, "exit=%d\n%s", cr.exit_code, cr.output ? cr.output : "");
    text_result(out_result, text, cr.exit_code != 0);
    free(text); free(cmd); free(name);
    ng_cmd_result_free(&cr);
    return 0;
  }

  if (strcmp(name, "nanobot_ask") == 0) {
    char *prompt = tool_arg(json, "prompt");
    if (!prompt) {
      free(name);
      return text_result(out_result, "missing prompt", 1);
    }
    ng_log("mcp tools/call nanobot_ask: %.200s", prompt);
    char *reply = ng_agent_run(agent, prompt);
    text_result(out_result, reply ? reply : "(no reply)", 0);
    free(reply); free(prompt); free(name);
    return 0;
  }

  if (strcmp(name, "memory_read") == 0) {
    char *which = tool_arg(json, "name");
    if (!which) which = strdup("core");
    /* only allow safe basenames */
    if (strchr(which, '/') || strchr(which, '\\') || strstr(which, "..")) {
      free(which); free(name);
      return text_result(out_result, "invalid memory name", 1);
    }
    char path[700];
    snprintf(path, sizeof path, "%s/memory/%s", ng_workdir(), which);
    /* allow with or without .txt */
    if (access(path, R_OK) != 0) {
      char path2[720];
      snprintf(path2, sizeof path2, "%s.txt", path);
      if (access(path2, R_OK) == 0) snprintf(path, sizeof path, "%s", path2);
    }
    size_t len = 0;
    char *body = ng_read_file(path, &len);
    if (!body) {
      free(which); free(name);
      return text_result(out_result, "(missing or empty)", 0);
    }
    text_result(out_result, body, 0);
    free(body); free(which); free(name);
    return 0;
  }

  if (strcmp(name, "memory_note") == 0) {
    char *line = tool_arg(json, "line");
    if (!line) line = tool_arg(json, "text");
    if (!line) {
      free(name);
      return text_result(out_result, "missing line", 1);
    }
    ng_memory_note_profile(line);
    text_result(out_result, "ok: profile note stored", 0);
    free(line); free(name);
    return 0;
  }

  if (strcmp(name, "home_info") == 0) {
    char *info = NULL;
    asprintf(&info,
      "nanobot %s\nworkdir=%s\nbackend=%s\nmodel=%s\nbase=%s\nlean=%s\n"
      "MCP: shell, ask, memory_*, self_improve_*, home_info\n",
      NG_VERSION, ng_workdir(),
      ng_agent_backend_kind(agent),
      agent->model ? agent->model : "?",
      agent->base_url ? agent->base_url : "?",
      ng_is_lean() ? "yes" : "no");
    text_result(out_result, info, 0);
    free(info); free(name);
    return 0;
  }

  if (strcmp(name, "self_improve_status") == 0) {
    char *st = ng_improve_status_json();
    text_result(out_result, st, 0);
    free(st); free(name);
    return 0;
  }

  if (strcmp(name, "self_improve_cycle") == 0) {
    char *focus = tool_arg(json, "focus");
    char *cycles_s = tool_arg(json, "cycles");
    int n = 1;
    if (cycles_s) { n = atoi(cycles_s); free(cycles_s); }
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    ng_log("mcp self_improve_cycle n=%d focus=%.120s", n, focus ? focus : "");
    char *reply = n == 1
      ? ng_improve_run_cycle(agent, focus)
      : ng_improve_run_n(agent, n, focus);
    text_result(out_result, reply ? reply : "(no reply)", 0);
    free(reply); free(focus); free(name);
    return 0;
  }

  char *esc = ng_json_escape(name);
  char *msg = NULL;
  asprintf(&msg, "unknown tool: %s", esc ? esc : "?");
  text_result(out_result, msg, 1);
  free(esc); free(msg); free(name);
  return -1;
}

static const char *TOOLS_JSON =
  "{\"tools\":["
  "{\"name\":\"run_terminal_command\","
  "\"description\":\"Run a shell command on the nanobot host. Returns exit code and output.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}},"
  "{\"name\":\"nanobot_ask\","
  "\"description\":\"Run a full nanobot agent turn (LLM + tools) and return the answer.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"prompt\":{\"type\":\"string\"}},\"required\":[\"prompt\"]}},"
  "{\"name\":\"memory_read\","
  "\"description\":\"Read a file under NANOBOT_HOME/memory/ (core, profile, summary, self_improve, improve_log.jsonl).\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"name\":{\"type\":\"string\",\"description\":\"basename e.g. core or self_improve.txt\"}},"
  "\"required\":[\"name\"]}},"
  "{\"name\":\"memory_note\","
  "\"description\":\"Append a short durable note to memory/profile.txt.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"line\":{\"type\":\"string\"}},\"required\":[\"line\"]}},"
  "{\"name\":\"home_info\","
  "\"description\":\"nanobot version, workdir, backend.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"self_improve_status\","
  "\"description\":\"Status of self-improvement mode (goals path, cycle count, last log).\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"self_improve_cycle\","
  "\"description\":\"Run 1–4 self-improvement agent cycles. Ships one small durable win per cycle.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"focus\":{\"type\":\"string\",\"description\":\"optional focus for this cycle\"},"
  "\"cycles\":{\"type\":\"string\",\"description\":\"1-4, default 1\"}}"
  "}}"
  "]}";

int ng_mcp_stdio_run(ng_agent_cfg *agent) {
  ng_log("mcp stdio server start version=%s", NG_VERSION);
  ng_improve_seed();
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
        "\"capabilities\":{\"tools\":{},\"prompts\":{}},"
        "\"serverInfo\":{\"name\":\"nanobot\",\"version\":\"%s\"}}",
        NG_VERSION);
      mcp_reply_result(id, result);
      free(result);
    } else if (strcmp(method, "notifications/initialized") == 0 ||
               strcmp(method, "initialized") == 0) {
      if (id && strcmp(id, "null") != 0) mcp_reply_result(id, "{}");
    } else if (strcmp(method, "ping") == 0) {
      mcp_reply_result(id, "{}");
    } else if (strcmp(method, "tools/list") == 0) {
      mcp_reply_result(id, TOOLS_JSON);
    } else if (strcmp(method, "tools/call") == 0) {
      char *result = NULL;
      handle_tools_call(agent, msg, &result);
      mcp_reply_result(id, result ? result : "{\"content\":[]}");
      free(result);
    } else if (strcmp(method, "prompts/list") == 0) {
      mcp_reply_result(id,
        "{\"prompts\":["
        "{\"name\":\"self_improve\",\"description\":\"Run a self-improvement cycle on this host\","
        "\"arguments\":[{\"name\":\"focus\",\"description\":\"optional focus\",\"required\":false}]}"
        "]}");
    } else if (strcmp(method, "prompts/get") == 0) {
      char *pname = tool_arg(msg, "name");
      if (pname && strcmp(pname, "self_improve") == 0) {
        char *focus = tool_arg(msg, "focus");
        char *text = NULL;
        asprintf(&text,
          "Run self-improvement. Call tool self_improve_cycle with focus=%s",
          focus && focus[0] ? focus : "general");
        char *esc = ng_json_escape(text);
        char *res = NULL;
        asprintf(&res,
          "{\"description\":\"self improve\",\"messages\":[{\"role\":\"user\","
          "\"content\":{\"type\":\"text\",\"text\":\"%s\"}}]}",
          esc ? esc : "");
        mcp_reply_result(id, res ? res : "{}");
        free(text); free(esc); free(res); free(focus);
      } else {
        mcp_reply_error(id, -32602, "unknown prompt");
      }
      free(pname);
    } else if (strcmp(method, "shutdown") == 0) {
      if (id && strcmp(id, "null") != 0) mcp_reply_result(id, "{}");
      free(method); free(id); free(msg);
      break;
    } else if (strncmp(method, "notifications/", 14) == 0) {
      /* no-op */
    } else {
      mcp_reply_error(id, -32601, "method not found");
    }
    free(method); free(id); free(msg);
  }
  ng_log("mcp stdio server exit");
  return 0;
}
