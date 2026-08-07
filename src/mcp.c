#include "mcp.h"
#include "improve.h"
#include "memory.h"
#include "shell.h"
#include "braincube_plugin.h"
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

/* Dual-wire local MCP plates — machine tokens only (no free-text essays).
 * Envelope stays MCP content[]; plate body is nanobot.mcp.v1 (stdio transport). */
static char *mcp_err(const char *error) {
  char *out = NULL;
  const char *e = error && error[0] ? error : "mcp_failed";
  asprintf(&out,
           "{\"schema\":\"nanobot.mcp.v1\",\"ok\":false,\"error\":\"%s\","
           "\"transport\":\"stdio\",\"python\":0}",
           e);
  return out ? out
             : strdup("{\"schema\":\"nanobot.mcp.v1\",\"ok\":false,"
                      "\"error\":\"oom\",\"transport\":\"stdio\",\"python\":0}");
}

static char *mcp_ok(const char *action) {
  char *out = NULL;
  const char *a = action && action[0] ? action : "ok";
  asprintf(&out,
           "{\"schema\":\"nanobot.mcp.v1\",\"ok\":true,\"action\":\"%s\","
           "\"transport\":\"stdio\",\"python\":0}",
           a);
  return out ? out : mcp_err("oom");
}

/* Wrap dual-wire plate into MCP tools/call content envelope; frees plate. */
static int plate_result(char **out, char *plate, int is_err) {
  int rc;
  if (!plate) {
    plate = mcp_err("oom");
    is_err = 1;
  }
  rc = text_result(out, plate, is_err);
  free(plate);
  return rc;
}

static int handle_tools_call(ng_agent_cfg *agent, const char *json, char **out_result) {
  char *name = tool_name(json);
  if (!name) {
    return plate_result(out_result, mcp_err("missing_tool_name"), 1);
  }

  if (strcmp(name, "run_terminal_command") == 0 || strcmp(name, "shell") == 0) {
    char *cmd = tool_arg(json, "command");
    if (!cmd) {
      free(name);
      return plate_result(out_result, mcp_err("missing_command"), 1);
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
      return plate_result(out_result, mcp_err("missing_prompt"), 1);
    }
    ng_log("mcp tools/call nanobot_ask: %.200s", prompt);
    char *reply = ng_agent_run(agent, prompt);
    if (reply)
      text_result(out_result, reply, 0);
    else
      plate_result(out_result, mcp_err("no_reply"), 1);
    free(reply); free(prompt); free(name);
    return 0;
  }

  if (strcmp(name, "memory_read") == 0) {
    char *which = tool_arg(json, "name");
    if (!which) which = strdup("core");
    /* only allow safe basenames */
    if (strchr(which, '/') || strchr(which, '\\') || strstr(which, "..")) {
      free(which); free(name);
      return plate_result(out_result, mcp_err("invalid_memory_name"), 1);
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
      return plate_result(out_result, mcp_ok("memory_missing"), 0);
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
      return plate_result(out_result, mcp_err("missing_line"), 1);
    }
    ng_memory_note_profile(line);
    plate_result(out_result, mcp_ok("memory_note"), 0);
    free(line); free(name);
    return 0;
  }

  if (strcmp(name, "home_info") == 0) {
    char *info = NULL;
    char *wd = ng_json_escape(ng_workdir());
    char *be = ng_json_escape(ng_agent_backend_kind(agent));
    char *md = ng_json_escape(agent->model ? agent->model : "");
    char *bu = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *ver = ng_json_escape(NG_VERSION);
    asprintf(&info,
             "{\"schema\":\"nanobot.mcp.v1\",\"ok\":true,\"action\":\"home_info\","
             "\"transport\":\"stdio\",\"version\":\"%s\",\"workdir\":\"%s\","
             "\"backend\":\"%s\",\"model\":\"%s\",\"base_url\":\"%s\","
             "\"lean\":%s,\"tools\":[\"shell\",\"ask\",\"memory\","
             "\"self_improve\",\"home_info\"],\"python\":0}",
             ver ? ver : "",
             wd ? wd : "",
             be ? be : "",
             md ? md : "",
             bu ? bu : "",
             ng_is_lean() ? "true" : "false");
    free(wd); free(be); free(md); free(bu); free(ver);
    plate_result(out_result, info ? info : mcp_err("oom"), info ? 0 : 1);
    free(name);
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
    if (reply)
      text_result(out_result, reply, 0);
    else
      plate_result(out_result, mcp_err("no_reply"), 1);
    free(reply); free(focus); free(name);
    return 0;
  }

  /* BrainCube / CubeChain — dual-wire plates only (no free-text train report). */
  if (strcmp(name, "braincube_train_status") == 0 ||
      strcmp(name, "train_status") == 0 ||
      strcmp(name, "cubechain_status") == 0) {
    char *resp = ng_bc_handle_post("{\"action\":\"train_status\"}");
    if (resp)
      plate_result(out_result, resp, 0);
    else
      plate_result(out_result, mcp_err("unavailable"), 1);
    free(name);
    return 0;
  }
  if (strcmp(name, "braincube_explore") == 0 || strcmp(name, "explore") == 0) {
    char *v = tool_arg(json, "value");
    char *body = NULL, *resp = NULL;
    int on = !v || v[0] == '1' || (v[0] && (v[0] == 't' || v[0] == 'T' || v[0] == 'o'));
    if (v && (v[0] == '0' || v[0] == 'f' || v[0] == 'F')) on = 0;
    asprintf(&body, "{\"action\":\"explore\",\"value\":\"%s\"}", on ? "1" : "0");
    resp = ng_bc_handle_post(body ? body : "{}");
    free(body); free(v);
    if (resp)
      plate_result(out_result, resp, strstr(resp, "\"ok\":false") ? 1 : 0);
    else
      plate_result(out_result, mcp_err("unavailable"), 1);
    free(name);
    return 0;
  }
  if (strcmp(name, "braincube_supervise") == 0) {
    char *want = tool_arg(json, "want");
    char *ttl = tool_arg(json, "ttl_sec");
    char *note = tool_arg(json, "note");
    char *want_esc = ng_json_escape(want && want[0] ? want : "free_ok");
    char *note_esc = ng_json_escape(note && note[0] ? note : "mcp_agent");
    char *body = NULL, *resp = NULL;
    int ttl_n = ttl ? atoi(ttl) : 40;
    int is_err = 0;
    if (ttl_n < 1) ttl_n = 1;
    if (ttl_n > 600) ttl_n = 600;
    /* Inject-sanitize want/note before JSON body assembly. */
    asprintf(&body,
      "{\"action\":\"supervise\",\"want\":\"%s\",\"ttl_sec\":%d,\"note\":\"%s\"}",
      want_esc ? want_esc : "free_ok",
      ttl_n,
      note_esc ? note_esc : "mcp_agent");
    resp = ng_bc_handle_post(body ? body : "{}");
    free(body); free(want); free(ttl); free(note); free(want_esc); free(note_esc);
    if (resp) {
      is_err = strstr(resp, "\"ok\":false") ? 1 : 0;
      plate_result(out_result, resp, is_err);
    } else {
      plate_result(out_result, mcp_err("unavailable"), 1);
    }
    free(name);
    return 0;
  }

  {
    char *esc = ng_json_escape(name);
    char *plate = NULL;
    asprintf(&plate,
             "{\"schema\":\"nanobot.mcp.v1\",\"ok\":false,"
             "\"error\":\"unknown_tool\",\"tool\":\"%s\","
             "\"transport\":\"stdio\",\"python\":0}",
             esc ? esc : "");
    free(esc);
    plate_result(out_result, plate ? plate : mcp_err("unknown_tool"), 1);
  }
  free(name);
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
  "}},"
  "{\"name\":\"braincube_train_status\","
  "\"description\":\"How BrainCube/CubeChain training is going on this device (continuous, explore, field trials, pick, teaches, latest trial). Use this to answer the user about training progress.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"braincube_explore\","
  "\"description\":\"Start/stop RC explore mode: robot messes around with short quiet moves to build associations (undock first). value=1/0.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"value\":{\"type\":\"string\",\"description\":\"1/on or 0/off\"}}"
  "}},"
  "{\"name\":\"braincube_supervise\","
  "\"description\":\"Supervise BrainCube: focus a sensor lane for ttl_sec (charge, free_ok, bump_L, …). Logs one-liner then purges bulky logs.\","
  "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
  "\"want\":{\"type\":\"string\"},\"ttl_sec\":{\"type\":\"string\"},\"note\":{\"type\":\"string\"}},"
  "\"required\":[\"want\"]}}"
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
