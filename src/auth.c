#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "auth.h"
#include "util.h"
#include <nanobot/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#include <ctype.h>

void ng_session_init(ng_session *s) { memset(s, 0, sizeof *s); s->poll_interval = 5; }
void ng_session_clear(ng_session *s) {
  free(s->access_token); free(s->refresh_token); free(s->email);
  free(s->user_code); free(s->verification_uri); free(s->verification_uri_complete);
  free(s->device_code);
  memset(s, 0, sizeof *s);
  s->poll_interval = 5;
  /* Clear secret pending file when fully wiping session (e.g. force reconnect). */
  ng_session_clear_pending();
}
void ng_session_free(ng_session *s) { ng_session_clear(s); }

const char *ng_session_bearer(const ng_session *s) {
  return (s && s->access_token && s->access_token[0]) ? s->access_token : NULL;
}

int ng_session_valid(const ng_session *s) {
  if (!s || !s->access_token || !s->access_token[0]) return 0;
  if (s->expires_at && time(NULL) + 60 >= s->expires_at) return 0;
  return 1;
}

static char *session_path(void) {
  static char p[640];
  snprintf(p, sizeof p, "%s/session", ng_workdir());
  return p;
}

static char *device_login_path(void) {
  static char p[640];
  snprintf(p, sizeof p, "%s/device_login", ng_workdir());
  return p;
}

/* Atomic write, mode 0600 only (secret material). */
static int write_secret_file(const char *path, const char *body) {
  if (!path || !body) return -1;
  char tmp[700];
  snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  size_t n = strlen(body);
  ssize_t w = write(fd, body, n);
  if (w < 0 || (size_t)w != n) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  if (fsync(fd) != 0) { /* best-effort on tiny flash */ }
  close(fd);
  if (chmod(tmp, 0600) != 0) { /* enforce */ }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  chmod(path, 0600);
  return 0;
}

/* Load peer token secret (raw hex or token= line). Caller frees. */
static char *load_peer_token_secret(void) {
  char path[700];
  snprintf(path, sizeof path, "%s/peer_token", ng_workdir());
  char *t = ng_slurp_env_file(path, "token");
  if (t && t[0]) return t;
  free(t);
  size_t bl = 0;
  char *raw = ng_read_file(path, &bl);
  if (!raw || !raw[0]) {
    free(raw);
    return NULL;
  }
  /* first non-comment line */
  char *p = raw;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p == '#') {
    free(raw);
    return NULL;
  }
  size_t n = strcspn(p, "\r\n");
  while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
  char *out = NULL;
  if (n > 0) {
    out = malloc(n + 1);
    if (out) {
      memcpy(out, p, n);
      out[n] = 0;
    }
  }
  free(raw);
  return out;
}

/**
 * Provider-seal key (nanobot-local; no external product required):
 *  1) Prefer KDF(peer_token) — LAN peer secret protects provider OAuth at rest
 *  2) Fallback session.key (legacy / pre-token installs)
 */
static int provider_seal_key(unsigned char key[32], int *from_peer) {
  if (from_peer) *from_peer = 0;
  char *tok = load_peer_token_secret();
  if (tok && tok[0]) {
    nb_kdf_provider_key(tok, strlen(tok), key);
    nb_secure_wipe(tok, strlen(tok));
    free(tok);
    if (from_peer) *from_peer = 1;
    return 0;
  }
  free(tok);
  char p[700];
  snprintf(p, sizeof p, "%s/session.key", ng_workdir());
  return nb_master_key_load_or_create(p, key);
}

/** Try open with peer-derived key then legacy session.key. */
static char *open_with_provider_keys(const char *envelope) {
  unsigned char key[32];
  int from_peer = 0;
  if (provider_seal_key(key, &from_peer) != 0) return NULL;
  char *plain = nb_secret_open(key, envelope, NULL);
  nb_secure_wipe(key, 32);
  if (plain) return plain;
  /* migration: sealed under old session.key while peer token now exists */
  if (from_peer) {
    char p[700];
    snprintf(p, sizeof p, "%s/session.key", ng_workdir());
    if (nb_master_key_load_or_create(p, key) == 0) {
      plain = nb_secret_open(key, envelope, NULL);
      nb_secure_wipe(key, 32);
      if (plain) {
        ng_log("auth: opened session with legacy session.key — will re-seal under peer token");
        return plain;
      }
    }
  }
  return NULL;
}

/** Write plaintext KEY=val body as AEAD envelope (nbenc1:…). */
static int write_sealed_secret(const char *path, const char *plain) {
  if (!path || !plain) return -1;
  unsigned char key[32];
  int from_peer = 0;
  if (provider_seal_key(key, &from_peer) != 0) return -1;
  char *env = nb_secret_seal(key, plain, strlen(plain));
  nb_secure_wipe(key, 32);
  if (!env) return -1;
  int rc = write_secret_file(path, env);
  free(env);
  return rc;
}

/** Read file; decrypt if nbenc1 envelope; else return raw (legacy plaintext). */
static char *read_maybe_sealed(const char *path) {
  size_t len = 0;
  char *raw = ng_read_file(path, &len);
  if (!raw) return NULL;
  if (!nb_secret_is_sealed(raw)) return raw; /* legacy cleartext */
  char *plain = open_with_provider_keys(raw);
  free(raw);
  return plain;
}

/* KEY=val from in-memory body (same rules as ng_slurp_env_file). */
static char *slurp_body(const char *body, const char *key) {
  if (!body || !key) return NULL;
  char linekey[128];
  snprintf(linekey, sizeof linekey, "%s=", key);
  const char *p = body;
  while (p && *p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char line[2048];
    if (len >= sizeof line) len = sizeof line - 1;
    memcpy(line, p, len);
    line[len] = 0;
    char *q = line;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '#' && strncmp(q, linekey, strlen(linekey)) == 0) {
      char *v = q + strlen(linekey);
      while (*v == ' ' || *v == '"' || *v == '\'') v++;
      size_t L = strlen(v);
      while (L && (v[L - 1] == ' ' || v[L - 1] == '"' || v[L - 1] == '\'' ||
                   v[L - 1] == '\r'))
        v[--L] = 0;
      return strdup(v);
    }
    if (!nl) break;
    p = nl + 1;
  }
  return NULL;
}

int ng_session_save(const ng_session *s) {
  char path[640];
  snprintf(path, sizeof path, "%s/session", ng_workdir());
  char buf[8192];
  size_t o = 0;
  o += (size_t)snprintf(buf + o, sizeof buf - o,
                        "# nanobot browser session (AEAD at rest)\n"
                        "issuer=%s\nclient_id=%s\n",
                        NG_AUTH_ISSUER, NG_AUTH_CLIENT_ID);
  if (s->access_token)
    o += (size_t)snprintf(buf + o, sizeof buf - o, "access_token=%s\n", s->access_token);
  if (s->refresh_token)
    o += (size_t)snprintf(buf + o, sizeof buf - o, "refresh_token=%s\n", s->refresh_token);
  if (s->expires_at)
    o += (size_t)snprintf(buf + o, sizeof buf - o, "expires_at=%ld\n", (long)s->expires_at);
  if (s->email)
    o += (size_t)snprintf(buf + o, sizeof buf - o, "email=%s\n", s->email);
  if (o >= sizeof buf) return -1;
  int rc = write_sealed_secret(path, buf);
  if (rc == 0) {
    char *pt = load_peer_token_secret();
    ng_log("auth: provider session sealed under %s",
           (pt && pt[0]) ? "peer_token-derived key" : "session.key fallback");
    free(pt);
  }
  return rc;
}

void ng_session_clear_pending(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/device_login", ng_workdir());
  unlink(path);
}

int ng_session_save_pending(const ng_session *s) {
  if (!s || !s->login_pending || !s->device_code || !s->user_code) {
    ng_session_clear_pending();
    return -1;
  }
  char buf[4096];
  int n = snprintf(buf, sizeof buf,
    "# nanobot device-login (AEAD at rest)\n"
    "login_pending=1\n"
    "device_code=%s\n"
    "user_code=%s\n"
    "verification_uri=%s\n"
    "verification_uri_complete=%s\n"
    "poll_interval=%d\n"
    "device_deadline=%ld\n"
    "issuer=%s\n"
    "client_id=%s\n",
    s->device_code,
    s->user_code,
    s->verification_uri ? s->verification_uri : "",
    s->verification_uri_complete ? s->verification_uri_complete : "",
    s->poll_interval > 0 ? s->poll_interval : 5,
    (long)s->device_deadline,
    NG_AUTH_ISSUER,
    NG_AUTH_CLIENT_ID);
  if (n < 0 || n >= (int)sizeof buf) return -1;
  int rc = write_sealed_secret(device_login_path(), buf);
  if (rc == 0)
    ng_log("auth: device login pending sealed (user_code=%s)",
           s->user_code ? s->user_code : "?");
  return rc;
}

int ng_session_load_pending(ng_session *s) {
  if (!s) return -1;
  const char *path = device_login_path();
  char *body = read_maybe_sealed(path);
  if (!body) return -1;
  char *pending = slurp_body(body, "login_pending");
  if (!pending || pending[0] != '1') {
    free(pending);
    free(body);
    return -1;
  }
  free(pending);
  char *dc = slurp_body(body, "device_code");
  char *uc = slurp_body(body, "user_code");
  if (!dc || !dc[0] || !uc || !uc[0]) {
    free(dc); free(uc); free(body);
    ng_session_clear_pending();
    return -1;
  }
  free(s->device_code); free(s->user_code);
  free(s->verification_uri); free(s->verification_uri_complete);
  s->device_code = dc;
  s->user_code = uc;
  s->verification_uri = slurp_body(body, "verification_uri");
  s->verification_uri_complete = slurp_body(body, "verification_uri_complete");
  char *pi = slurp_body(body, "poll_interval");
  s->poll_interval = pi ? atoi(pi) : 5;
  if (s->poll_interval < 1) s->poll_interval = 5;
  free(pi);
  char *dl = slurp_body(body, "device_deadline");
  s->device_deadline = dl ? (time_t)strtol(dl, NULL, 10) : 0;
  free(dl);
  free(body);
  if (s->device_deadline && time(NULL) > s->device_deadline) {
    ng_log("auth: stored device login expired — clearing");
    free(s->device_code); s->device_code = NULL;
    free(s->user_code); s->user_code = NULL;
    free(s->verification_uri); s->verification_uri = NULL;
    free(s->verification_uri_complete); s->verification_uri_complete = NULL;
    s->login_pending = 0;
    ng_session_clear_pending();
    return -1;
  }
  s->login_pending = 1;
  return 0;
}

int ng_session_load(ng_session *s) {
  /* Keep tokens and/or pending device login across fork workers. */
  char *body = read_maybe_sealed(session_path());
  char *at = body ? slurp_body(body, "access_token") : NULL;
  char *rt = body ? slurp_body(body, "refresh_token") : NULL;
  char *ex = body ? slurp_body(body, "expires_at") : NULL;
  char *em = body ? slurp_body(body, "email") : NULL;
  int have_tok = (at && at[0]);
  int have_rt = (rt && rt[0]);

  /* Migrate legacy plaintext session → sealed on next successful load. */
  if (body && !nb_secret_is_sealed(body) && (have_tok || have_rt)) {
    /* body was cleartext from disk; re-save encrypted if we have tokens */
  }

  free(s->access_token); free(s->refresh_token); free(s->email);
  s->access_token = at;
  s->refresh_token = rt;
  s->email = em;
  s->expires_at = 0;
  if (ex) { s->expires_at = (time_t)strtol(ex, NULL, 10); free(ex); }

  int was_clear = 0;
  if (body) {
    size_t fl = 0;
    char *raw = ng_read_file(session_path(), &fl);
    was_clear = raw && !nb_secret_is_sealed(raw);
    free(raw);
  }
  free(body);

  /* Re-seal under peer_token KDF when: cleartext, or peer token now present */
  if ((have_tok || have_rt)) {
    char *pt = load_peer_token_secret();
    if (was_clear || (pt && pt[0])) {
      ng_log("auth: re-sealing provider session under %s",
             (pt && pt[0]) ? "peer_token KDF" : "session.key");
      ng_session_save(s);
    }
    free(pt);
  }

  /* Always try pending file (device login survives worker exit). */
  int pend = ng_session_load_pending(s);
  if (!have_tok && !have_rt && pend != 0) return -1;
  return 0;
}

/* curl -sS -X POST url -d form -H headers -o outfile */
static char *curl_form_post(const char *url, const char *form, const char **extra_headers) {
  char outtmpl[] = "/tmp/ng_auth_XXXXXX";
  int ofd = mkstemp(outtmpl);
  if (ofd < 0) return NULL;
  close(ofd);

  char verhdr[80], uahdr[96];
  snprintf(verhdr, sizeof verhdr, "x-grok-client-version: %s", ng_cli_version());
  snprintf(uahdr, sizeof uahdr, "User-Agent: %s", ng_cli_user_agent());

  pid_t pid = fork();
  if (pid == 0) {
    char *argv[32];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = "30";
    argv[a++] = "-X";
    argv[a++] = "POST";
    argv[a++] = "-H";
    argv[a++] = "Content-Type: application/x-www-form-urlencoded";
    argv[a++] = "-H";
    argv[a++] = "x-grok-client-surface: cli";
    argv[a++] = "-H";
    argv[a++] = verhdr;
    argv[a++] = "-H";
    argv[a++] = uahdr;
    if (extra_headers) {
      for (int i = 0; extra_headers[i]; i++) {
        argv[a++] = "-H";
        argv[a++] = (char *)extra_headers[i];
      }
    }
    argv[a++] = "--data";
    argv[a++] = (char *)form;
    argv[a++] = "-o";
    argv[a++] = outtmpl;
    argv[a++] = (char *)url;
    argv[a++] = NULL;
    execvp("curl", argv);
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  char *body = ng_read_file(outtmpl, NULL);
  unlink(outtmpl);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    free(body);
    return NULL;
  }
  return body ? body : strdup("");
}

static char *json_num_as_str(const char *json, const char *key) {
  const char *p = strstr(json, key);
  if (!p) return NULL;
  p = strchr(p + strlen(key), ':');
  if (!p) return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (!isdigit((unsigned char)*p) && *p != '-') return NULL;
  const char *s = p;
  if (*p == '-') p++;
  while (isdigit((unsigned char)*p)) p++;
  size_t n = (size_t)(p - s);
  char *o = malloc(n + 1);
  memcpy(o, s, n); o[n] = 0;
  return o;
}

int ng_session_start_device_login(ng_session *s) {
  /* clear pending device state but keep nothing of old login codes */
  free(s->user_code); free(s->verification_uri); free(s->verification_uri_complete);
  free(s->device_code);
  s->user_code = s->verification_uri = s->verification_uri_complete = s->device_code = NULL;
  s->login_pending = 0;

  char form[1024];
  snprintf(form, sizeof form,
    "client_id=%s&scope=%s&referrer=nanobot",
    NG_AUTH_CLIENT_ID,
    "openid%20profile%20email%20offline_access%20grok-cli%3Aaccess%20api%3Aaccess%20"
    "conversations%3Aread%20conversations%3Awrite%20workspaces%3Aread%20workspaces%3Awrite");

  char url[256];
  snprintf(url, sizeof url, "%s/oauth2/device/code", NG_AUTH_ISSUER);

  ng_log("auth: requesting device code");
  char *body = curl_form_post(url, form, NULL);
  if (!body || !body[0]) {
    free(body);
    ng_log("auth: device code request failed");
    return -1;
  }

  if (strstr(body, "\"error\"")) {
    ng_log("auth: device code error: %.300s", body);
    free(body);
    return -1;
  }

  s->device_code = ng_json_get_string(body, "device_code");
  s->user_code = ng_json_get_string(body, "user_code");
  s->verification_uri = ng_json_get_string(body, "verification_uri");
  s->verification_uri_complete = ng_json_get_string(body, "verification_uri_complete");
  char *iv = json_num_as_str(body, "\"interval\"");
  char *ex = json_num_as_str(body, "\"expires_in\"");
  s->poll_interval = iv ? atoi(iv) : 5;
  if (s->poll_interval < 1) s->poll_interval = 5;
  long exp = ex ? strtol(ex, NULL, 10) : 1800;
  if (exp < 600) exp = 600;
  s->device_deadline = time(NULL) + exp;
  free(iv); free(ex); free(body);

  if (!s->device_code || !s->user_code || !s->verification_uri) {
    ng_log("auth: incomplete device code response");
    return -1;
  }
  s->login_pending = 1;
  /* Persist so other forked HTTP workers can poll the same device_code. */
  if (ng_session_save_pending(s) != 0)
    ng_log("auth: warning — could not persist device_login (mode 0600)");
  ng_log("auth: user_code=%s uri=%s", s->user_code,
         s->verification_uri_complete ? s->verification_uri_complete : s->verification_uri);
  return 0;
}

static int apply_token_response(ng_session *s, const char *body) {
  char *at = ng_json_get_string(body, "access_token");
  if (!at) return -1;
  free(s->access_token);
  s->access_token = at;
  char *rt = ng_json_get_string(body, "refresh_token");
  if (rt) { free(s->refresh_token); s->refresh_token = rt; }
  char *ein = json_num_as_str(body, "\"expires_in\"");
  if (ein) {
    s->expires_at = time(NULL) + strtol(ein, NULL, 10);
    free(ein);
  } else {
    s->expires_at = time(NULL) + 3600;
  }
  /* try email from id_token payload — optional skip */
  s->login_pending = 0;
  free(s->device_code); s->device_code = NULL;
  free(s->user_code); s->user_code = NULL;
  free(s->verification_uri); s->verification_uri = NULL;
  free(s->verification_uri_complete); s->verification_uri_complete = NULL;
  ng_session_clear_pending(); /* wipe device_code secret */
  ng_session_save(s);         /* access/refresh AEAD-sealed under peer_token KDF */
  ng_log("auth: browser session encrypted at rest (expires_at=%ld)", (long)s->expires_at);
  return 0;
}

int ng_session_poll_login(ng_session *s) {
  if (!s) return -1;
  if (!s->login_pending || !s->device_code)
    ng_session_load_pending(s);
  if (!s->login_pending || !s->device_code) return -1;
  if (s->device_deadline && time(NULL) > s->device_deadline) {
    ng_log("auth: device code expired");
    s->login_pending = 0;
    ng_session_clear_pending();
    return -1;
  }

  char form[2048];
  snprintf(form, sizeof form,
    "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code"
    "&device_code=%s&client_id=%s",
    s->device_code, NG_AUTH_CLIENT_ID);

  char url[256];
  snprintf(url, sizeof url, "%s/oauth2/token", NG_AUTH_ISSUER);
  char *body = curl_form_post(url, form, NULL);
  if (!body) return 0; /* transient */

  if (strstr(body, "access_token")) {
    int rc = apply_token_response(s, body);
    free(body);
    return rc == 0 ? 1 : -1;
  }

  char *err = ng_json_get_string(body, "error");
  if (err) {
    if (strcmp(err, "authorization_pending") == 0) {
      free(err); free(body); return 0;
    }
    if (strcmp(err, "slow_down") == 0) {
      s->poll_interval += 5;
      free(err); free(body); return 0;
    }
    ng_log("auth: poll error: %s", err);
    free(err); free(body);
    s->login_pending = 0;
    ng_session_clear_pending();
    return -1;
  }
  free(body);
  return 0;
}

int ng_session_ensure(ng_session *s) {
  if (ng_session_valid(s)) return 0;
  if (!s->refresh_token || !s->refresh_token[0]) {
    if (s->access_token && s->access_token[0] && !s->expires_at) return 0; /* unknown expiry */
    return -1;
  }

  char form[4096];
  /* refresh_token may need url-encoding of special chars — assume token is url-safe */
  snprintf(form, sizeof form,
    "grant_type=refresh_token&refresh_token=%s&client_id=%s",
    s->refresh_token, NG_AUTH_CLIENT_ID);
  char url[256];
  snprintf(url, sizeof url, "%s/oauth2/token", NG_AUTH_ISSUER);
  ng_log("auth: refreshing access token");
  char *body = curl_form_post(url, form, NULL);
  if (!body || !strstr(body, "access_token")) {
    free(body);
    ng_log("auth: refresh failed");
    return -1;
  }
  int rc = apply_token_response(s, body);
  free(body);
  return rc;
}

int ng_session_login_blocking(ng_session *s) {
  if (ng_session_start_device_login(s) != 0) return -1;

  const char *link = s->verification_uri_complete
    ? s->verification_uri_complete
    : s->verification_uri;

  fprintf(stderr, "\n");
  fprintf(stderr, "  ═══════════════════════════════════════════════\n");
  fprintf(stderr, "  ACTIVATE GROK SESSION (browser device login)\n");
  fprintf(stderr, "  ═══════════════════════════════════════════════\n");
  fprintf(stderr, "  Open this link while logged into your cloud account:\n\n");
  fprintf(stderr, "    %s\n\n", link);
  fprintf(stderr, "  Code: %s\n", s->user_code ? s->user_code : "?");
  fprintf(stderr, "  (waiting for browser approval…)\n");
  fprintf(stderr, "  ═══════════════════════════════════════════════\n\n");
  /* also emit bare URL on stdout for easy copy when only one line is captured */
  printf("%s\n", link);
  fflush(stdout);

  while (1) {
    sleep((unsigned)s->poll_interval);
    int r = ng_session_poll_login(s);
    if (r == 1) {
      fprintf(stderr, "  ✓ cloud session active\n\n");
      return 0;
    }
    if (r < 0) return -1;
  }
}
