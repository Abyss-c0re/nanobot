#define _POSIX_C_SOURCE 200809L
#include "mcp_remote.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

/* ---- tiny helpers ---- */

static char *servers_path(void) {
  static char p[640];
  snprintf(p, sizeof p, "%s/mcp_servers.json", ng_workdir());
  return p;
}

static char *read_config_raw(void) {
  size_t n = 0;
  char *r = ng_read_file(servers_path(), &n);
  if (!r || !r[0]) {
    free(r);
    return strdup("{\"servers\":[]}");
  }
  return r;
}

static int json_bool_default(const char *obj, const char *key, int def) {
  char *v = ng_json_get_string(obj, key);
  if (!v) {
    /* try unquoted true/false */
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return def;
  }
  int on = !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
  free(v);
  return on;
}

/* Extract string field; also accepts "key":null → NULL */
static char *jstr(const char *json, const char *key) {
  return ng_json_get_string(json, key);
}

/* HTTP JSON-RPC POST via curl; captures optional Mcp-Session-Id response header. */
static char *mcp_http_post(const char *url, const char *auth_hdr,
                           const char *session_id, const char *body,
                           char **out_session) {
  if (out_session) *out_session = NULL;
  if (!url || !url[0] || !body) return strdup("{\"error\":\"bad mcp request\"}");

  char reqp[640], outp[640], errp[640], hdrp[640];
  int fd = ng_mkstemp_home(reqp, sizeof reqp, "mcp_req_");
  if (fd < 0) return strdup("{\"error\":\"mkstemp\"}");
  write(fd, body, strlen(body));
  close(fd);
  int ofd = ng_mkstemp_home(outp, sizeof outp, "mcp_out_");
  if (ofd < 0) { unlink(reqp); return strdup("{\"error\":\"mkstemp out\"}"); }
  close(ofd);
  int efd = ng_mkstemp_home(errp, sizeof errp, "mcp_err_");
  if (efd < 0) { unlink(reqp); unlink(outp); return strdup("{\"error\":\"mkstemp err\"}"); }
  close(efd);
  int hfd = ng_mkstemp_home(hdrp, sizeof hdrp, "mcp_hdr_");
  if (hfd < 0) { unlink(reqp); unlink(outp); unlink(errp); return strdup("{\"error\":\"mkstemp hdr\"}"); }
  close(hfd);

  char dataarg[700];
  snprintf(dataarg, sizeof dataarg, "@%s", reqp);
  char authline[1700] = {0};
  if (auth_hdr && auth_hdr[0]) {
    if (strchr(auth_hdr, ':'))
      snprintf(authline, sizeof authline, "%s", auth_hdr);
    else
      snprintf(authline, sizeof authline, "Authorization: %s", auth_hdr);
  }
  char sessline[700] = {0};
  if (session_id && session_id[0])
    snprintf(sessline, sizeof sessline, "Mcp-Session-Id: %s", session_id);

  pid_t p = fork();
  if (p < 0) {
    unlink(reqp); unlink(outp); unlink(errp); unlink(hdrp);
    return strdup("{\"error\":\"fork failed\"}");
  }
  if (p == 0) {
    int er = open(errp, O_WRONLY | O_TRUNC);
    if (er >= 0) { dup2(er, STDERR_FILENO); close(er); }
    char *argv[48];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = "25";
    argv[a++] = "--connect-timeout";
    argv[a++] = "8";
    argv[a++] = "-D";
    argv[a++] = hdrp;
    argv[a++] = "-H";
    argv[a++] = "Content-Type: application/json";
    argv[a++] = "-H";
    argv[a++] = "Accept: application/json, text/event-stream";
    if (authline[0]) { argv[a++] = "-H"; argv[a++] = authline; }
    if (sessline[0]) { argv[a++] = "-H"; argv[a++] = sessline; }
    argv[a++] = "--data-binary";
    argv[a++] = dataarg;
    argv[a++] = "-o";
    argv[a++] = outp;
    argv[a++] = (char *)url;
    argv[a++] = NULL;
    execvp("curl", argv);
    _exit(127);
  }
  int st = 0;
  if (waitpid(p, &st, 0) < 0) {
    unlink(reqp); unlink(outp); unlink(errp); unlink(hdrp);
    return strdup("{\"error\":\"waitpid\"}");
  }
  unlink(reqp);

  /* parse session id from response headers */
  if (out_session) {
    size_t hl = 0;
    char *hdr = ng_read_file(hdrp, &hl);
    if (hdr) {
      const char *k = strstr(hdr, "Mcp-Session-Id:");
      if (!k) k = strstr(hdr, "mcp-session-id:");
      if (k) {
        k = strchr(k, ':');
        if (k) {
          k++;
          while (*k == ' ' || *k == '\t') k++;
          const char *e = k;
          while (*e && *e != '\r' && *e != '\n') e++;
          size_t n = (size_t)(e - k);
          if (n > 0 && n < 200) {
            char *s = malloc(n + 1);
            if (s) { memcpy(s, k, n); s[n] = 0; *out_session = s; }
          }
        }
      }
      free(hdr);
    }
  }
  unlink(hdrp);

  char *raw = ng_read_file(outp, NULL);
  char *cerr = ng_read_file(errp, NULL);
  unlink(outp); unlink(errp);
  if ((!raw || !raw[0]) && cerr && cerr[0]) {
    char *out = NULL;
    char *esc = ng_json_escape(cerr);
    asprintf(&out, "{\"error\":\"curl: %s\"}", esc ? esc : "?");
    free(esc); free(raw); free(cerr);
    return out ? out : strdup("{\"error\":\"curl\"}");
  }
  free(cerr);
  if (!raw) return strdup("{\"error\":\"empty response\"}");

  /* SSE: take last data: JSON line with result or error */
  if (strstr(raw, "data:") || strncmp(raw, "event:", 6) == 0) {
    char *last = NULL;
    char *p2 = raw;
    while (*p2) {
      char *nl = strchr(p2, '\n');
      if (nl) *nl = 0;
      char *line = p2;
      if (!strncmp(line, "data:", 5)) {
        line += 5;
        while (*line == ' ') line++;
        if (*line == '{' ) {
          free(last);
          last = strdup(line);
        }
      }
      if (!nl) break;
      p2 = nl + 1;
    }
    free(raw);
    return last ? last : strdup("{\"error\":\"empty SSE\"}");
  }
  return raw;
}

static char *rpc_body(int id, const char *method, const char *params_json) {
  char *out = NULL;
  asprintf(&out,
    "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
    id, method, params_json && params_json[0] ? params_json : "{}");
  return out ? out : strdup("{}");
}

/* Walk servers array objects naively: find "{...}" chunks after "servers" */
typedef struct {
  char *id;
  char *name;
  char *url;
  char *auth;
  int enabled;
} mcp_srv;

static void free_srv(mcp_srv *s) {
  if (!s) return;
  free(s->id); free(s->name); free(s->url); free(s->auth);
  memset(s, 0, sizeof *s);
}

/* Parse up to max servers from config */
static int parse_servers(mcp_srv *arr, int max) {
  char *raw = read_config_raw();
  if (!raw) return 0;
  int n = 0;
  const char *p = strstr(raw, "\"servers\"");
  if (!p) { free(raw); return 0; }
  p = strchr(p, '[');
  if (!p) { free(raw); return 0; }
  p++;
  while (*p && n < max) {
    while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
    if (*p == ']') break;
    if (*p != '{') break;
    /* find matching brace */
    int depth = 0;
    const char *s = p;
    for (; *p; p++) {
      if (*p == '{') depth++;
      else if (*p == '}') {
        depth--;
        if (depth == 0) { p++; break; }
      } else if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
      }
    }
    size_t len = (size_t)(p - s);
    char *obj = malloc(len + 1);
    if (!obj) break;
    memcpy(obj, s, len);
    obj[len] = 0;
    mcp_srv *e = &arr[n];
    memset(e, 0, sizeof *e);
    e->id = jstr(obj, "id");
    e->name = jstr(obj, "name");
    e->url = jstr(obj, "url");
    e->auth = jstr(obj, "auth");
    if (!e->auth) e->auth = jstr(obj, "authorization");
    e->enabled = json_bool_default(obj, "enabled", 1);
    if (!e->id || !e->id[0]) {
      free(e->id);
      char tmp[16];
      snprintf(tmp, sizeof tmp, "s%d", n + 1);
      e->id = strdup(tmp);
    }
    if (!e->name) e->name = strdup(e->id);
    free(obj);
    if (e->url && e->url[0]) n++;
    else free_srv(e);
  }
  free(raw);
  return n;
}

char *ng_mcp_servers_list_json(void) {
  char *raw = read_config_raw();
  /* ensure ok wrapper */
  char *out = NULL;
  asprintf(&out, "{\"ok\":true,\"config\":%s}", raw ? raw : "{\"servers\":[]}");
  free(raw);
  return out ? out : strdup("{\"ok\":false}");
}

int ng_mcp_servers_save_raw(const char *json_body) {
  if (!json_body || !json_body[0]) return -1;
  /* Accept either full {"servers":[...]} or bare array */
  const char *body = json_body;
  char *wrap = NULL;
  if (json_body[0] == '[') {
    asprintf(&wrap, "{\"servers\":%s}", json_body);
    body = wrap;
  }
  /* basic validate */
  if (!strstr(body, "servers")) {
    free(wrap);
    return -1;
  }
  int rc = ng_write_file(servers_path(), body, strlen(body));
  /* lab perms */
  chmod(servers_path(), 0666);
  free(wrap);
  ng_log("mcp: saved servers config (%d)", rc);
  return rc;
}

static char *initialize_and_list(const char *url, const char *auth, char **sess_out) {
  if (sess_out) *sess_out = NULL;
  char *init_params =
    "{\"protocolVersion\":\"2024-11-05\","
    "\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"nanobot\",\"version\":\"" NG_VERSION "\"}}";
  char *init_req = rpc_body(1, "initialize", init_params);
  char *sess = NULL;
  char *init_resp = mcp_http_post(url, auth, NULL, init_req, &sess);
  free(init_req);
  /* keep session for list */
  char *note = strdup(
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}");
  char *note_resp = mcp_http_post(url, auth, sess, note, NULL);
  free(note); free(note_resp);

  char *list_req = rpc_body(2, "tools/list", "{}");
  char *list_resp = mcp_http_post(url, auth, sess, list_req, NULL);
  free(list_req);
  if (sess_out) *sess_out = sess;
  else free(sess);

  /* if list failed and init had error, surface init */
  if (list_resp && strstr(list_resp, "\"error\"") && init_resp && strstr(init_resp, "\"error\"")) {
    free(list_resp);
    return init_resp;
  }
  free(init_resp);
  return list_resp;
}

char *ng_mcp_server_probe(const char *id, const char *url_override, const char *auth_header) {
  char *url = NULL;
  char *auth = NULL;
  char *name = NULL;
  if (url_override && url_override[0]) {
    url = strdup(url_override);
    auth = auth_header && auth_header[0] ? strdup(auth_header) : NULL;
    name = strdup(id && id[0] ? id : "adhoc");
  } else if (id && id[0]) {
    mcp_srv arr[16];
    int n = parse_servers(arr, 16);
    for (int i = 0; i < n; i++) {
      if (arr[i].id && !strcmp(arr[i].id, id)) {
        url = arr[i].url ? strdup(arr[i].url) : NULL;
        auth = arr[i].auth ? strdup(arr[i].auth) : NULL;
        name = arr[i].name ? strdup(arr[i].name) : strdup(id);
      }
      free_srv(&arr[i]);
    }
  }
  if (!url) {
    free(auth); free(name);
    return strdup("{\"ok\":false,\"error\":\"server not found (need id or url)\"}");
  }
  char *sess = NULL;
  char *list = initialize_and_list(url, auth, &sess);
  /* count tools roughly */
  int tools = 0;
  if (list) {
    const char *p = list;
    while ((p = strstr(p, "\"name\"")) != NULL) { tools++; p += 6; }
    /* overcounts a bit; fine for probe UI */
  }
  char *esc_url = ng_json_escape(url);
  char *esc_name = ng_json_escape(name ? name : "");
  char *esc_body = ng_json_escape(list ? list : "");
  char *out = NULL;
  int ok = list && !strstr(list, "\"error\"") && strstr(list, "tools");
  if (!ok && list && strstr(list, "\"result\"")) ok = 1;
  asprintf(&out,
    "{\"ok\":%s,\"id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\","
    "\"session\":%s,\"tools_hint\":%d,\"raw\":\"%.1200s\"}",
    ok ? "true" : "false",
    id && id[0] ? id : "adhoc",
    esc_name ? esc_name : "",
    esc_url ? esc_url : "",
    sess ? "true" : "false",
    tools,
    esc_body ? esc_body : "");
  free(list); free(sess); free(url); free(auth); free(name);
  free(esc_url); free(esc_name); free(esc_body);
  return out ? out : strdup("{\"ok\":false}");
}

/* Build OpenAI tools fragment: mcp_list + mcp_call always when any server enabled */
char *ng_mcp_openai_tools_fragment(void) {
  mcp_srv arr[16];
  int n = parse_servers(arr, 16);
  int en = 0;
  char names[512] = {0};
  size_t nl = 0;
  for (int i = 0; i < n; i++) {
    if (arr[i].enabled && arr[i].url) {
      en++;
      if (nl + 40 < sizeof names) {
        nl += (size_t)snprintf(names + nl, sizeof names - nl, "%s%s",
                               nl ? "," : "", arr[i].id ? arr[i].id : "?");
      }
    }
    free_srv(&arr[i]);
  }
  if (!en) return strdup("");
  char *esc = ng_json_escape(names);
  char *frag = NULL;
  asprintf(&frag,
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"mcp_list\","
    "\"description\":\"List enabled MCP servers (%s) and their tools. Call before mcp_call if unsure.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"mcp_call\","
    "\"description\":\"Call a tool on a connected MCP server. Use mcp_list first.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"server\":{\"type\":\"string\",\"description\":\"server id\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"MCP tool name\"},"
    "\"arguments\":{\"type\":\"string\",\"description\":\"JSON object string of tool args\"}"
    "},\"required\":[\"server\",\"tool\"]}}}",
    esc ? esc : "mcp");
  free(esc);
  return frag ? frag : strdup("");
}

static char *extract_arg(const char *args, const char *key) {
  return jstr(args ? args : "{}", key);
}

static char *mcp_call_server(const char *server_id, const char *tool,
                             const char *arguments_json) {
  mcp_srv arr[16];
  int n = parse_servers(arr, 16);
  mcp_srv *hit = NULL;
  mcp_srv copy;
  memset(&copy, 0, sizeof copy);
  for (int i = 0; i < n; i++) {
    if (arr[i].id && server_id && !strcmp(arr[i].id, server_id) && arr[i].enabled) {
      copy.id = arr[i].id ? strdup(arr[i].id) : NULL;
      copy.name = arr[i].name ? strdup(arr[i].name) : NULL;
      copy.url = arr[i].url ? strdup(arr[i].url) : NULL;
      copy.auth = arr[i].auth ? strdup(arr[i].auth) : NULL;
      copy.enabled = 1;
      hit = &copy;
    }
    free_srv(&arr[i]);
  }
  if (!hit || !hit->url) {
    free_srv(&copy);
    char *e = NULL;
    asprintf(&e, "MCP server '%s' not found or disabled", server_id ? server_id : "?");
    return e ? e : strdup("MCP server missing");
  }
  if (!tool || !tool[0]) {
    free_srv(&copy);
    return strdup("mcp_call needs tool name");
  }
  char *sess = NULL;
  /* initialize for session */
  char *init_params =
    "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"nanobot\",\"version\":\"" NG_VERSION "\"}}";
  char *init_req = rpc_body(1, "initialize", init_params);
  char *init_resp = mcp_http_post(hit->url, hit->auth, NULL, init_req, &sess);
  free(init_req); free(init_resp);
  char *note = strdup("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}");
  char *nr = mcp_http_post(hit->url, hit->auth, sess, note, NULL);
  free(note); free(nr);

  const char *args = (arguments_json && arguments_json[0]) ? arguments_json : "{}";
  char *args_owned = NULL;
  /* Model often passes arguments as a JSON string: "{\"a\":1}" */
  if (args[0] == '"') {
    size_t n = strlen(args);
    if (n >= 2 && args[n - 1] == '"') {
      char *buf = malloc(n);
      if (buf) {
        size_t j = 0;
        for (size_t i = 1; i + 1 < n; i++) {
          if (args[i] == '\\' && i + 1 < n) {
            char c = args[++i];
            if (c == 'n') buf[j++] = '\n';
            else if (c == 't') buf[j++] = '\t';
            else if (c == 'r') buf[j++] = '\r';
            else if (c == '"') buf[j++] = '"';
            else if (c == '\\') buf[j++] = '\\';
            else buf[j++] = c;
          } else {
            buf[j++] = args[i];
          }
        }
        buf[j] = 0;
        args_owned = buf;
        args = args_owned;
      }
    }
  }
  if (args[0] != '{') {
    free(args_owned);
    args_owned = strdup("{}");
    args = args_owned;
  }

  char *esc_tool = ng_json_escape(tool);
  char *params = NULL;
  asprintf(&params, "{\"name\":\"%s\",\"arguments\":%s}",
           esc_tool ? esc_tool : tool, args);
  free(esc_tool);
  char *req = rpc_body(3, "tools/call", params);
  free(params);
  char *resp = mcp_http_post(hit->url, hit->auth, sess, req, NULL);
  free(req); free(sess); free(args_owned); free_srv(&copy);

  if (!resp) return strdup("MCP tools/call empty");
  /* prefer text content extraction */
  char *text = jstr(resp, "text");
  if (!text) {
    /* walk content array roughly */
    const char *c = strstr(resp, "\"text\"");
    if (c) text = jstr(c - 1 > resp ? c : resp, "text");
  }
  if (text && text[0]) {
    free(resp);
    return text;
  }
  free(text);
  /* return trimmed raw */
  if (strlen(resp) > 2000) {
    char *t = malloc(2004);
    if (t) {
      memcpy(t, resp, 2000);
      t[2000] = 0;
      strcat(t, "…");
      free(resp);
      return t;
    }
  }
  return resp;
}

static char *mcp_list_all(void) {
  mcp_srv arr[16];
  int n = parse_servers(arr, 16);
  char *acc = strdup("MCP servers:\n");
  if (!n) {
    free(acc);
    for (int i = 0; i < n; i++) free_srv(&arr[i]);
    return strdup("No MCP servers configured. Add entries to $NANOBOT_HOME/mcp_servers.json.");
  }
  for (int i = 0; i < n; i++) {
    if (!arr[i].enabled) {
      char *line = NULL;
      asprintf(&line, "%s  - %s (%s) DISABLED\n", acc, arr[i].id, arr[i].name ? arr[i].name : "");
      free(acc); acc = line ? line : acc;
      free_srv(&arr[i]);
      continue;
    }
    char *sess = NULL;
    char *list = initialize_and_list(arr[i].url, arr[i].auth, &sess);
    free(sess);
    char *line = NULL;
    asprintf(&line, "%s  - id=%s name=%s url=%s\n    tools: %.800s\n",
             acc,
             arr[i].id ? arr[i].id : "?",
             arr[i].name ? arr[i].name : "",
             arr[i].url ? arr[i].url : "",
             list ? list : "(probe failed)");
    free(acc); free(list);
    acc = line ? line : acc;
    free_srv(&arr[i]);
  }
  return acc ? acc : strdup("(empty)");
}

char *ng_mcp_try_tool(const char *name, const char *args_json) {
  if (!name || !name[0]) return NULL;
  if (!strcmp(name, "mcp_list") || !strcmp(name, "list_mcp_tools")) {
    ng_log("mcp: tool mcp_list");
    return mcp_list_all();
  }
  if (!strcmp(name, "mcp_call") || !strcmp(name, "call_mcp_tool")) {
    char *server = extract_arg(args_json, "server");
    if (!server) server = extract_arg(args_json, "server_id");
    char *tool = extract_arg(args_json, "tool");
    if (!tool) tool = extract_arg(args_json, "name");
    char *arguments = extract_arg(args_json, "arguments");
    /* arguments may be object not string — find raw */
    if (!arguments && args_json) {
      const char *p = strstr(args_json, "\"arguments\"");
      if (p) {
        p = strchr(p, ':');
        if (p) {
          p++;
          while (*p == ' ' || *p == '\t') p++;
          if (*p == '{') {
            int d = 0;
            const char *s = p;
            for (; *p; p++) {
              if (*p == '{') d++;
              else if (*p == '}') { d--; if (d == 0) { p++; break; } }
              else if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
              }
            }
            size_t n = (size_t)(p - s);
            arguments = malloc(n + 1);
            if (arguments) { memcpy(arguments, s, n); arguments[n] = 0; }
          }
        }
      }
    }
    ng_log("mcp: tool mcp_call server=%s tool=%s", server ? server : "?", tool ? tool : "?");
    char *r = mcp_call_server(server, tool, arguments);
    free(server); free(tool); free(arguments);
    return r;
  }
  /* mcp__server__tool form */
  if (!strncmp(name, "mcp__", 5)) {
    const char *rest = name + 5;
    const char *sep = strstr(rest, "__");
    if (!sep) return NULL;
    size_t sl = (size_t)(sep - rest);
    char server[64];
    if (sl >= sizeof server) sl = sizeof server - 1;
    memcpy(server, rest, sl);
    server[sl] = 0;
    const char *tool = sep + 2;
    char *arguments = extract_arg(args_json, "arguments");
    if (!arguments && args_json && args_json[0] == '{') arguments = strdup(args_json);
    char *r = mcp_call_server(server, tool, arguments);
    free(arguments);
    return r;
  }
  return NULL;
}
