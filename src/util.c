#include "util.h"
#include <nanobot/os.h>
#include <nanobot/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <fcntl.h>

static char g_logpath[576] = "/tmp/nanobot/nanobot.log";
static FILE *g_logf;

void ng_set_workdir(const char *dir) {
  nb_set_workdir(dir);
  snprintf(g_logpath, sizeof g_logpath, "%s/nanobot.log", nb_workdir());
}

const char *ng_workdir(void) { return nb_workdir(); }
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
  snprintf(path, sizeof path, "%s/cli_version", nb_workdir());
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
  snprintf(path, sizeof path, "%s/cli_version", nb_workdir());
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

/* lean accessors implemented below */
extern size_t ng_log_max(void);

void ng_log_init(const char *path) {
  if (path && *path) snprintf(g_logpath, sizeof g_logpath, "%s", path);
  mkdir(nb_workdir(), 0755);
  g_logf = fopen(g_logpath, "a");
}

static void ng_log_maybe_rotate(void) {
  if (!g_logpath[0]) return;
  struct stat st;
  if (stat(g_logpath, &st) != 0) return;
  size_t maxb = ng_log_max();
  if ((size_t)st.st_size < maxb) return;
  char bak[600];
  snprintf(bak, sizeof bak, "%s.1", g_logpath);
  if (g_logf) { fclose(g_logf); g_logf = NULL; }
  rename(g_logpath, bak);
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
  if (!g_logf) g_logf = fopen(g_logpath, "a");
  if (g_logf) {
    va_start(ap, fmt);
    fprintf(g_logf, "[%s] ", ts);
    vfprintf(g_logf, fmt, ap);
    fprintf(g_logf, "\n");
    fflush(g_logf);
    va_end(ap);
    ng_log_maybe_rotate();
  }
}


/* ---- os / json wrappers (implementations in libnanobot_*) ---- */
char *ng_read_file(const char *path, size_t *out_len) {
  return nb_read_file(path, out_len);
}
int ng_write_file(const char *path, const char *data, size_t len) {
  return nb_write_file(path, data, len);
}

int ng_mkstemp_home(char *path, size_t path_sz, const char *prefix) {
  if (!path || path_sz < 64) return -1;
  const char *pre = (prefix && prefix[0]) ? prefix : "ng_";
  const char *wd = ng_workdir();
  /* TMPDIR (host-set), $NANOBOT_HOME/tmp, then /tmp — no product OS paths. */
  const char *cands[6];
  int nc = 0;
  const char *td = getenv("TMPDIR");
  if (td && td[0]) cands[nc++] = td;
  if (wd && wd[0]) {
    static char wdtmp[640];
    snprintf(wdtmp, sizeof wdtmp, "%s/tmp", wd);
    cands[nc++] = wdtmp;
  }
  cands[nc++] = "/tmp";

  mode_t old = umask(0);
  int fd = -1;
  for (int i = 0; i < nc && fd < 0; i++) {
    const char *base = cands[i];
    if (!base || !base[0]) continue;
    mkdir(base, 0777);
    /* also try base/tmp for bare TMPDIR roots */
    char dir[640];
    if (strstr(base, "/tmp") == NULL && strcmp(base, "/tmp") != 0) {
      snprintf(dir, sizeof dir, "%s/tmp", base);
      mkdir(dir, 0777);
    } else {
      snprintf(dir, sizeof dir, "%s", base);
    }
    int n = snprintf(path, path_sz, "%s/%sXXXXXX", dir, pre);
    if (n < 0 || (size_t)n >= path_sz) continue;
    fd = mkstemp(path);
    if (fd < 0) {
      /* try without nested tmp */
      n = snprintf(path, path_sz, "%s/%sXXXXXX", base, pre);
      if (n < 0 || (size_t)n >= path_sz) continue;
      fd = mkstemp(path);
    }
  }
  umask(old);
  if (fd >= 0) fchmod(fd, 0666);
  return fd;
}

char *ng_getenv_dup(const char *k) { return nb_getenv_dup(k); }
char *ng_slurp_env_file(const char *path, const char *key) {
  return nb_slurp_env_file(path, key);
}
const char *ng_settings_path(void) { return nb_settings_path(); }
char *ng_settings_get(const char *key) { return nb_settings_get(key); }
int ng_settings_set(const char *key, const char *value) {
  return nb_settings_set(key, value);
}
char *ng_json_escape(const char *s) { return nb_json_escape(s); }
char *ng_json_get_string(const char *json, const char *key) {
  return nb_json_get_string(json, key);
}
char *ng_json_message_content(const char *json) {
  return nb_json_message_content(json);
}
int ng_json_first_tool_call(const char *json, char **name, char **args, char **id) {
  return nb_json_first_tool_call(json, name, args, id);
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


/* ---- lean limits (lean hosts ~256MB RAM) ---- */
static int g_lean = -1; /* -1 unset */

void ng_limits_init(void) {
  const char *e = getenv("NANOBOT_LEAN");
  if (e && e[0]) {
    g_lean = (e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || e[0] == 't' || e[0] == 'T') ? 1 : 0;
    return;
  }
  FILE *f = fopen("/proc/meminfo", "r");
  long total_kb = 0;
  if (f) {
    char line[128];
    while (fgets(line, sizeof line, f)) {
      if (sscanf(line, "MemTotal: %ld", &total_kb) == 1) break;
    }
    fclose(f);
  }
  g_lean = (total_kb > 0 && total_kb < 400 * 1024) ? 1 : 0;
}

int ng_is_lean(void) {
  if (g_lean < 0) ng_limits_init();
  return g_lean;
}

int ng_max_turns(void) {
  return ng_is_lean() ? NG_LEAN_MAX_TURNS : NG_MAX_TURNS;
}

int ng_http_max_children(void) {
  return ng_is_lean() ? NG_LEAN_HTTP_MAX_CHILDREN : NG_HTTP_MAX_CHILDREN;
}

size_t ng_out_max(void) {
  return ng_is_lean() ? (size_t)NG_LEAN_OUT_MAX : (size_t)NG_OUT_MAX;
}

size_t ng_log_max(void) {
  return ng_is_lean() ? (size_t)NG_LEAN_LOG_MAX : (size_t)NG_HOST_LOG_MAX;
}

char *ng_resources_json(void) {
  long mem_total = 0, mem_free = 0, buffers = 0, cached = 0;
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    char line[160];
    while (fgets(line, sizeof line, f)) {
      long v;
      if (sscanf(line, "MemTotal: %ld", &v) == 1) mem_total = v;
      else if (sscanf(line, "MemFree: %ld", &v) == 1) mem_free = v;
      else if (sscanf(line, "Buffers: %ld", &v) == 1) buffers = v;
      else if (sscanf(line, "Cached: %ld", &v) == 1) cached = v;
    }
    fclose(f);
  }
  double load1 = 0, load5 = 0, load15 = 0;
  f = fopen("/proc/loadavg", "r");
  if (f) {
    if (fscanf(f, "%lf %lf %lf", &load1, &load5, &load15) < 1) {}
    fclose(f);
  }
  long data_total_kb = 0, data_free_kb = 0;
  const char *dp = nb_workdir();
  struct statvfs sv;
  memset(&sv, 0, sizeof sv);
  if (!dp || !dp[0] || statvfs(dp, &sv) != 0) {
    dp = "/";
    memset(&sv, 0, sizeof sv);
    statvfs(dp, &sv);
  }
  if (sv.f_frsize) {
    data_total_kb = (long)((sv.f_blocks * (unsigned long long)sv.f_frsize) / 1024ULL);
    data_free_kb = (long)((sv.f_bavail * (unsigned long long)sv.f_frsize) / 1024ULL);
  }
  long avail = mem_free + buffers + cached;
  char *out = NULL;
  asprintf(&out,
    "{\"ok\":true,\"lean\":%s,\"mem_total_kb\":%ld,\"mem_free_kb\":%ld,"
    "\"mem_avail_kb\":%ld,\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
    "\"disk_path\":\"%s\",\"disk_total_kb\":%ld,\"disk_free_kb\":%ld,"
    "\"limits\":{\"max_turns\":%d,\"http_children\":%d,\"out_max\":%zu,\"log_max\":%zu},"
    "\"version\":\"%s\"}",
    ng_is_lean() ? "true" : "false",
    mem_total, mem_free, avail, load1, load5, load15,
    dp, data_total_kb, data_free_kb,
    ng_max_turns(), ng_http_max_children(), ng_out_max(), ng_log_max(),
    NG_VERSION);
  return out ? out : strdup("{\"ok\":false}");
}
