#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

static char g_workdir[512] = "/mnt/data/nanobot";
static char g_logpath[576] = "/mnt/data/nanobot/nanobot.log";
static FILE *g_logf;

void ng_set_workdir(const char *dir) {
  if (!dir || !*dir) return;
  snprintf(g_workdir, sizeof g_workdir, "%s", dir);
  snprintf(g_logpath, sizeof g_logpath, "%s/nanobot.log", g_workdir);
}

const char *ng_workdir(void) { return g_workdir; }
const char *ng_log_path(void) { return g_logpath; }

/* ---- Runtime CLI version (auto-bump on proxy "outdated") ---- */
static char g_cli_ver[32] = NG_CLI_VERSION_DEFAULT;
static char g_cli_ua[48] = "grok-cli/" NG_CLI_VERSION_DEFAULT;
static int g_cli_inited = 0;

static void cli_refresh_ua(void) {
  snprintf(g_cli_ua, sizeof g_cli_ua, "grok-cli/%s", g_cli_ver);
}

static void cli_save(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/cli_version", g_workdir);
  char line[64];
  int n = snprintf(line, sizeof line, "%s\n", g_cli_ver);
  if (n > 0) ng_write_file(path, line, (size_t)n);
}

static int parse_semver(const char *s, int *a, int *b, int *c) {
  *a = *b = *c = 0;
  if (!s || !*s) return 0;
  return sscanf(s, "%d.%d.%d", a, b, c) >= 1;
}

static int ver_cmp(const char *x, const char *y) {
  int xa, xb, xc, ya, yb, yc;
  parse_semver(x, &xa, &xb, &xc);
  parse_semver(y, &ya, &yb, &yc);
  if (xa != ya) return xa < ya ? -1 : 1;
  if (xb != yb) return xb < yb ? -1 : 1;
  if (xc != yc) return xc < yc ? -1 : 1;
  return 0;
}

void ng_cli_version_init(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/cli_version", g_workdir);
  size_t len = 0;
  char *raw = ng_read_file(path, &len);
  if (raw && raw[0]) {
    /* first token on first line */
    char v[32];
    int i = 0;
    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != '\n' && *p != '\r' && *p != ' ' && i + 1 < (int)sizeof v)
      v[i++] = *p++;
    v[i] = 0;
    if (v[0] && ver_cmp(v, NG_CLI_VERSION_DEFAULT) >= 0)
      snprintf(g_cli_ver, sizeof g_cli_ver, "%s", v);
    else
      snprintf(g_cli_ver, sizeof g_cli_ver, "%s", NG_CLI_VERSION_DEFAULT);
  } else {
    snprintf(g_cli_ver, sizeof g_cli_ver, "%s", NG_CLI_VERSION_DEFAULT);
  }
  free(raw);
  cli_refresh_ua();
  g_cli_inited = 1;
}

const char *ng_cli_version(void) {
  if (!g_cli_inited) ng_cli_version_init();
  return g_cli_ver;
}

const char *ng_cli_user_agent(void) {
  if (!g_cli_inited) ng_cli_version_init();
  return g_cli_ua;
}

static int extract_required_version(const char *resp, char *out, size_t outsz) {
  if (!resp || !out || outsz < 8) return 0;
  /* "update to version 0.1.202 or later" / "Please update to version X" */
  const char *p = strstr(resp, "update to version");
  if (!p) p = strstr(resp, "Update to version");
  if (!p) p = strstr(resp, "to version");
  if (!p) return 0;
  p = strstr(p, "version");
  if (!p) return 0;
  p += 7;
  while (*p == ' ' || *p == '\t' || *p == '"' || *p == ':') p++;
  int a = 0, b = 0, c = 0;
  if (sscanf(p, "%d.%d.%d", &a, &b, &c) < 2) return 0;
  snprintf(out, outsz, "%d.%d.%d", a, b, c);
  return 1;
}

static void bump_patch(void) {
  int a = 0, b = 0, c = 0;
  parse_semver(g_cli_ver, &a, &b, &c);
  c++;
  if (c > 9999) { b++; c = 0; }
  snprintf(g_cli_ver, sizeof g_cli_ver, "%d.%d.%d", a, b, c);
}

int ng_cli_version_handle_error(const char *resp) {
  if (!resp) return 0;
  if (!g_cli_inited) ng_cli_version_init();
  int outdated = 0;
  if (strstr(resp, "outdated") || strstr(resp, "CLI version") ||
      strstr(resp, "cli version") || strstr(resp, "Grok CLI version"))
    outdated = 1;
  if (!outdated) return 0;

  char need[32];
  char old[32];
  snprintf(old, sizeof old, "%s", g_cli_ver);
  if (extract_required_version(resp, need, sizeof need)) {
    /* set to required, or required+1 patch if equal (still rejected) */
    if (ver_cmp(need, g_cli_ver) > 0)
      snprintf(g_cli_ver, sizeof g_cli_ver, "%s", need);
    else
      bump_patch();
  } else {
    bump_patch();
  }
  cli_refresh_ua();
  cli_save();
  ng_log("cli_version: auto-bump %s -> %s (proxy outdated)", old, g_cli_ver);
  return 1;
}

void ng_log_init(const char *path) {
  if (path && *path) snprintf(g_logpath, sizeof g_logpath, "%s", path);
  mkdir(g_workdir, 0755);
  g_logf = fopen(g_logpath, "a");
}

void ng_log(const char *fmt, ...) {
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[32];
  strftime(ts, sizeof ts, "%H:%M:%S", &tm);
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[%s] ", ts);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  if (g_logf) {
    va_start(ap, fmt);
    fprintf(g_logf, "[%s] ", ts);
    vfprintf(g_logf, fmt, ap);
    fprintf(g_logf, "\n");
    fflush(g_logf);
    va_end(ap);
  }
}

char *ng_read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[rd] = 0;
  if (out_len) *out_len = rd;
  return buf;
}

int ng_write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t w = fwrite(data, 1, len, f);
  fclose(f);
  return w == len ? 0 : -1;
}

char *ng_getenv_dup(const char *k) {
  const char *v = getenv(k);
  return v ? strdup(v) : NULL;
}

char *ng_slurp_env_file(const char *path, const char *key) {
  char *body = ng_read_file(path, NULL);
  if (!body) return NULL;
  char linekey[128];
  snprintf(linekey, sizeof linekey, "%s=", key);
  char *p = body;
  char *found = NULL;
  while (p && *p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#' && strncmp(p, linekey, strlen(linekey)) == 0) {
      char *v = p + strlen(linekey);
      while (*v == ' ' || *v == '"' || *v == '\'') v++;
      size_t L = strlen(v);
      while (L && (v[L-1] == ' ' || v[L-1] == '"' || v[L-1] == '\'' || v[L-1] == '\r'))
        v[--L] = 0;
      found = strdup(v);
      break;
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(body);
  return found;
}

char *ng_json_escape(const char *s) {
  if (!s) s = "";
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') n += 2;
    else if (*p < 0x20) n += 6; /* \u00XX */
    else n++;
  }
  char *o = malloc(n + 1);
  if (!o) return NULL;
  char *d = o;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"') { *d++ = '\\'; *d++ = '"'; }
    else if (*p == '\\') { *d++ = '\\'; *d++ = '\\'; }
    else if (*p == '\n') { *d++ = '\\'; *d++ = 'n'; }
    else if (*p == '\r') { *d++ = '\\'; *d++ = 'r'; }
    else if (*p == '\t') { *d++ = '\\'; *d++ = 't'; }
    else if (*p < 0x20) {
      d += sprintf(d, "\\u%04x", (unsigned)*p);
    } else {
      *d++ = (char)*p;
    }
  }
  *d = 0;
  return o;
}

/* Find "key" then skip to next " and read string (handles simple escapes) */
static const char *find_key(const char *json, const char *key) {
  char pat[128];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = json;
  while ((p = strstr(p, pat)) != NULL) {
    /* ensure not mid-string: crude check of preceding char */
    if (p > json && (p[-1] == '\\')) { p++; continue; }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == ':') return p + 1;
    p++;
  }
  return NULL;
}

char *ng_json_get_string(const char *json, const char *key) {
  const char *p = find_key(json, key);
  if (!p) return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != '"') return NULL;
  p++;
  char *out = malloc(strlen(p) + 1);
  if (!out) return NULL;
  char *d = out;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'n') *d++ = '\n';
      else if (*p == 'r') *d++ = '\r';
      else if (*p == 't') *d++ = '\t';
      else *d++ = *p;
      p++;
    } else {
      *d++ = *p++;
    }
  }
  *d = 0;
  return out;
}

/* Chat completions assistant message content */
char *ng_json_message_content(const char *json) {
  /* Prefer choices[0].message.content */
  const char *msg = strstr(json, "\"message\"");
  if (msg) {
    char *c = ng_json_get_string(msg, "content");
    if (c && c[0]) return c;
    free(c);
  }
  return ng_json_get_string(json, "content");
}

int ng_json_first_tool_call(const char *json, char **name, char **args, char **id) {
  *name = *args = *id = NULL;
  const char *tc = strstr(json, "\"tool_calls\"");
  if (!tc) return 0;
  const char *fn = strstr(tc, "\"function\"");
  if (!fn) return 0;
  *name = ng_json_get_string(fn, "name");
  *args = ng_json_get_string(fn, "arguments");
  *id = ng_json_get_string(tc, "id");
  if (!*name) return 0;
  if (!*args) *args = strdup("{}");
  if (!*id) *id = strdup("call_0");
  return 1;
}

char *ng_read_log_tail(size_t max_bytes) {
  FILE *f = fopen(g_logpath, "rb");
  if (!f) return strdup("");
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return strdup(""); }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return strdup(""); }
  long start = sz > (long)max_bytes ? sz - (long)max_bytes : 0;
  fseek(f, start, SEEK_SET);
  size_t n = (size_t)(sz - start);
  char *buf = malloc(n + 1);
  if (!buf) { fclose(f); return strdup(""); }
  n = fread(buf, 1, n, f);
  fclose(f);
  buf[n] = 0;
  /* align to line */
  if (start > 0) {
    char *nl = strchr(buf, '\n');
    if (nl) {
      char *t = strdup(nl + 1);
      free(buf);
      return t ? t : strdup("");
    }
  }
  return buf;
}
