#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "auth.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

void ng_session_init(ng_session *s) { memset(s, 0, sizeof *s); s->poll_interval = 5; }
void ng_session_clear(ng_session *s) {
  free(s->access_token); free(s->refresh_token); free(s->email);
  free(s->user_code); free(s->verification_uri); free(s->verification_uri_complete);
  free(s->device_code);
  memset(s, 0, sizeof *s);
  s->poll_interval = 5;
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

int ng_session_save(const ng_session *s) {
  char path[640];
  snprintf(path, sizeof path, "%s/session", ng_workdir());
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  if (s->access_token) fprintf(f, "access_token=%s\n", s->access_token);
  if (s->refresh_token) fprintf(f, "refresh_token=%s\n", s->refresh_token);
  if (s->expires_at) fprintf(f, "expires_at=%ld\n", (long)s->expires_at);
  if (s->email) fprintf(f, "email=%s\n", s->email);
  fprintf(f, "issuer=%s\n", NG_AUTH_ISSUER);
  fprintf(f, "client_id=%s\n", NG_AUTH_CLIENT_ID);
  fclose(f);
  chmod(path, 0600);
  return 0;
}

int ng_session_load(ng_session *s) {
  char *at = ng_slurp_env_file(session_path(), "access_token");
  if (!at) return -1;
  ng_session_clear(s);
  s->access_token = at;
  s->refresh_token = ng_slurp_env_file(session_path(), "refresh_token");
  char *ex = ng_slurp_env_file(session_path(), "expires_at");
  if (ex) { s->expires_at = (time_t)strtol(ex, NULL, 10); free(ex); }
  s->email = ng_slurp_env_file(session_path(), "email");
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
  ng_session_save(s);
  ng_log("auth: session stored (expires_at=%ld)", (long)s->expires_at);
  return 0;
}

int ng_session_poll_login(ng_session *s) {
  if (!s->login_pending || !s->device_code) return -1;
  if (time(NULL) > s->device_deadline) {
    ng_log("auth: device code expired");
    s->login_pending = 0;
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
  fprintf(stderr, "  ACTIVATE GROK SESSION (browser on BlackCube)\n");
  fprintf(stderr, "  ═══════════════════════════════════════════════\n");
  fprintf(stderr, "  Open this link while logged into your Grok account:\n\n");
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
      fprintf(stderr, "  ✓ Grok session active\n\n");
      return 0;
    }
    if (r < 0) return -1;
  }
}
