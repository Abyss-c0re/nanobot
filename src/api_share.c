/*
 * API share / exit node
 * CubalC SMX2 discovers who has API (see cubalc programs/p2p/api_*).
 * This module: advertise exit + HTTP relay POST /peer/v1/prompt to exits.
 */
#include "api_share.h"
#include "agent.h"
#include "auth.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

int ng_api_share_enabled(void) {
  const char *e = getenv("NANOBOT_API_SHARE");
  if (e && e[0]) {
    if (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F')
      return 0;
    return 1;
  }
  /* default ON — mesh homes should share when possible */
  return 1;
}

int ng_api_share_local_exit_ok(ng_agent_cfg *c) {
  if (!c) return 0;
  if (!ng_api_share_enabled()) return 0;
  if (ng_agent_is_grok_backend(c)) {
    if (!c->session) return 0;
    if (ng_session_ensure(c->session) != 0) return 0;
    if (!ng_session_valid(c->session)) return 0;
    const char *b = ng_session_bearer(c->session);
    return (b && b[0]) ? 1 : 0;
  }
  /* OpenAI-compatible / local: treat as exit if base_url set */
  return (c->base_url && c->base_url[0]) ? 1 : 0;
}

static char *read_token_file(const char *path) {
  if (!path || !path[0]) return NULL;
  size_t n = 0;
  char *raw = ng_read_file(path, &n);
  if (!raw) return NULL;
  /* token=HEX or bare */
  char *p = raw;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (!strncmp(p, "token=", 6)) p += 6;
  char *end = p + strlen(p);
  while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
    *--end = 0;
  char *out = strdup(p);
  free(raw);
  return out;
}

/* Collect exit peer URLs into out[] (max maxn). Returns count. */
static int collect_exit_peers(char urls[][512], char toks[][256], int maxn) {
  int n = 0;
  /* 1) env NANOBOT_EXIT_PEERS=url1,url2 */
  const char *env = getenv("NANOBOT_EXIT_PEERS");
  char *tok_env = ng_getenv_dup("NANOBOT_EXIT_PEER_TOKEN");
  if (!tok_env) tok_env = ng_getenv_dup("NANOBOT_PEER_TOKEN");
  if (!tok_env) {
    char tp[640];
    snprintf(tp, sizeof tp, "%s/peer_token", ng_workdir());
    tok_env = read_token_file(tp);
  }
  if (env && env[0] && n < maxn) {
    char *dup = strdup(env);
    char *save = NULL;
    for (char *p = strtok_r(dup, ",; \t\n", &save); p && n < maxn;
         p = strtok_r(NULL, ",; \t\n", &save)) {
      if (!p[0]) continue;
      snprintf(urls[n], 512, "%s", p);
      /* strip trailing slash */
      size_t L = strlen(urls[n]);
      while (L > 0 && urls[n][L - 1] == '/') urls[n][--L] = 0;
      if (tok_env) snprintf(toks[n], 256, "%s", tok_env);
      else toks[n][0] = 0;
      n++;
    }
    free(dup);
  }
  free(tok_env);

  /* 2) $HOME/mesh/api_peers.json — list of {url,token?,exit_node} */
  if (n < maxn) {
    char path[640];
    snprintf(path, sizeof path, "%s/mesh/api_peers.json", ng_workdir());
    size_t len = 0;
    char *j = ng_read_file(path, &len);
    if (j && len) {
      /* crude parse: find "url":"..." with optional exit_node true nearby */
      const char *p = j;
      while (n < maxn && (p = strstr(p, "\"url\"")) != NULL) {
        p += 5;
        while (*p && *p != '"') p++;
        if (*p != '"') break;
        p++;
        const char *e = p;
        while (*e && *e != '"') e++;
        if (*e != '"') break;
        size_t ul = (size_t)(e - p);
        if (ul > 0 && ul < 500) {
          /* check exit_node false skip — default include */
          const char *window_end = e + 1;
          const char *next = strstr(window_end, "\"url\"");
          size_t wlen = next ? (size_t)(next - window_end) : 200;
          int skip = 0;
          if (wlen > 0) {
            char win[256];
            if (wlen > sizeof win - 1) wlen = sizeof win - 1;
            memcpy(win, window_end, wlen);
            win[wlen] = 0;
            if (strstr(win, "\"exit_node\":false") || strstr(win, "\"exit_node\": false"))
              skip = 1;
            if (strstr(win, "\"api_ok\":false") || strstr(win, "\"api_ok\": false"))
              skip = 1;
          }
          if (!skip) {
            memcpy(urls[n], p, ul);
            urls[n][ul] = 0;
            size_t L = strlen(urls[n]);
            while (L > 0 && urls[n][L - 1] == '/') urls[n][--L] = 0;
            /* token in window */
            toks[n][0] = 0;
            const char *tt = strstr(window_end, "\"token\"");
            if (tt && (!next || tt < next)) {
              tt = strchr(tt + 7, '"');
              if (tt) {
                tt++;
                const char *te = strchr(tt, '"');
                if (te && (size_t)(te - tt) < 255) {
                  memcpy(toks[n], tt, (size_t)(te - tt));
                  toks[n][te - tt] = 0;
                }
              }
            }
            n++;
          }
        }
        p = e + 1;
      }
    }
    free(j);
  }

  /* 3) CubalC export plate exits_example (hint only if still empty) */
  if (n == 0) {
    const char *croot = getenv("CUBALC_ROOT");
    if (!croot) croot = getenv("CUBALC_STATE");
    char path[640];
    if (croot && croot[0])
      snprintf(path, sizeof path, "%s/API_EXIT_MESH.json", croot);
    else
      snprintf(path, sizeof path, "%s/../cubalc/state/API_EXIT_MESH.json", ng_workdir());
    size_t len = 0;
    char *j = ng_read_file(path, &len);
    if (!j) {
      /* host lab default */
      j = ng_read_file("/home/voldemar/Dev/cubalc/state/API_EXIT_MESH.json", &len);
    }
    if (j && strstr(j, "exits_example")) {
      const char *p = strstr(j, "\"url\"");
      if (p) {
        p = strchr(p + 5, '"');
        if (p) {
          p++;
          const char *e = strchr(p, '"');
          if (e && n < maxn) {
            size_t ul = (size_t)(e - p);
            if (ul < 500) {
              memcpy(urls[n], p, ul);
              urls[n][ul] = 0;
              toks[n][0] = 0;
              n++;
            }
          }
        }
      }
    }
    free(j);
  }
  return n;
}

static char *curl_post_peer_prompt(const char *base, const char *token,
                                   const char *prompt) {
  if (!base || !base[0] || !prompt) return NULL;
  char url[640];
  snprintf(url, sizeof url, "%s/peer/v1/prompt", base);

  char body[8192];
  char *pe = ng_json_escape(prompt);
  if (!pe) return NULL;
  int bl = snprintf(body, sizeof body, "{\"prompt\": \"%s\"}", pe);
  free(pe);
  if (bl < 0 || (size_t)bl >= sizeof body) return NULL;

  char outtmpl[640], errtmpl[640], intmpl[640];
  int ofd = ng_mkstemp_home(outtmpl, sizeof outtmpl, "ng_exit_");
  if (ofd < 0) return NULL;
  close(ofd);
  int efd = ng_mkstemp_home(errtmpl, sizeof errtmpl, "ng_exite_");
  if (efd < 0) {
    unlink(outtmpl);
    return NULL;
  }
  close(efd);
  int ifd = ng_mkstemp_home(intmpl, sizeof intmpl, "ng_exiti_");
  if (ifd < 0) {
    unlink(outtmpl);
    unlink(errtmpl);
    return NULL;
  }
  if (write(ifd, body, (size_t)bl) != bl) {
    close(ifd);
    unlink(outtmpl);
    unlink(errtmpl);
    unlink(intmpl);
    return NULL;
  }
  close(ifd);

  char auth[400];
  if (token && token[0])
    snprintf(auth, sizeof auth, "X-Nanobot-Peer-Token: %s", token);
  else
    auth[0] = 0;
  char dataarg[700];
  snprintf(dataarg, sizeof dataarg, "@%s", intmpl);

  pid_t p = fork();
  if (p < 0) {
    unlink(outtmpl);
    unlink(errtmpl);
    unlink(intmpl);
    return NULL;
  }
  if (p == 0) {
    int er = open(errtmpl, O_WRONLY | O_TRUNC);
    if (er >= 0) {
      dup2(er, STDERR_FILENO);
      close(er);
    }
    char *argv[32];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = "120";
    argv[a++] = "--connect-timeout";
    argv[a++] = "15";
    argv[a++] = "-H";
    argv[a++] = "Content-Type: application/json";
    if (auth[0]) {
      argv[a++] = "-H";
      argv[a++] = auth;
    }
    argv[a++] = "--data-binary";
    argv[a++] = dataarg;
    argv[a++] = "-o";
    argv[a++] = outtmpl;
    argv[a++] = url;
    argv[a++] = NULL;
    execvp("curl", argv);
    _exit(127);
  }
  int st = 0;
  waitpid(p, &st, 0);
  unlink(intmpl);
  unlink(errtmpl);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    unlink(outtmpl);
    return NULL;
  }
  char *resp = ng_read_file(outtmpl, NULL);
  unlink(outtmpl);
  if (!resp || !resp[0]) {
    free(resp);
    return NULL;
  }
  /* Prefer "reply" field from peer plate */
  char *reply = ng_json_get_string(resp, "reply");
  if (reply && reply[0]) {
    free(resp);
    return reply;
  }
  /* whole body if ok */
  if (strstr(resp, "\"ok\":true") || strstr(resp, "\"ok\": true"))
    return resp;
  free(resp);
  return NULL;
}

char *ng_api_share_relay_prompt(const char *prompt) {
  if (!ng_api_share_enabled() || !prompt || !prompt[0]) return NULL;
  char urls[8][512];
  char toks[8][256];
  int n = collect_exit_peers(urls, toks, 8);
  if (n <= 0) {
    ng_log("api_share: no exit peers (set NANOBOT_EXIT_PEERS or mesh/api_peers.json)");
    return NULL;
  }
  for (int i = 0; i < n; i++) {
    ng_log("api_share: relay → %s", urls[i]);
    char *r = curl_post_peer_prompt(urls[i], toks[i][0] ? toks[i] : NULL, prompt);
    if (r && r[0]) {
      ng_log("api_share: exit ok via %s", urls[i]);
      return r;
    }
    free(r);
  }
  ng_log("api_share: all exits failed");
  return NULL;
}

void ng_api_share_info_json(ng_agent_cfg *c, char *buf, size_t cap) {
  int exit_ok = ng_api_share_local_exit_ok(c);
  int en = ng_api_share_enabled();
  snprintf(buf, cap,
           "\"api_share\":%s,\"api_ok\":%s,\"exit_node\":%s,"
           "\"api_share_proto\":\"cubalc_smx2+peer_http\","
           "\"api_share_bits\":\"0=ALIVE,4=API_OK,5=EXIT_NODE,6=SIGNED_IN,8=NEED_EXIT\"",
           en ? "true" : "false", exit_ok ? "true" : "false",
           exit_ok ? "true" : "false");
}

char *ng_api_share_status_json(ng_agent_cfg *c) {
  char frag[512];
  ng_api_share_info_json(c, frag, sizeof frag);
  char urls[8][512];
  char toks[8][256];
  int n = collect_exit_peers(urls, toks, 8);
  size_t cap = 2048;
  char *out = malloc(cap);
  if (!out) return NULL;
  int o = snprintf(out, cap,
                   "{\"schema\":\"nanobot.api_share.v1\",\"ok\":true,"
                   "\"action\":\"api-share\","
                   "%s,"
                   "\"peers_configured\":%d,"
                   "\"law\":\"any_api_ok_is_exit_node\","
                   "\"cubalc\":[\"programs/p2p/api_exit_node.cubalc\","
                   "\"programs/p2p/api_share_mesh.cubalc\","
                   "\"programs/proof/10b_api_exit_node.cubalc\"],"
                   "\"share\":\"state_matrix_only\","
                   "\"hold_flash\":1,"
                   "\"product_wire\":\"smx2\","
                   "\"exits\":[",
                   frag, n);
  for (int i = 0; i < n && o > 0 && (size_t)o < cap - 80; i++) {
    char *ue = ng_json_escape(urls[i]);
    o += snprintf(out + o, cap - (size_t)o, "%s{\"url\":\"%s\",\"exit_node\":true}",
                  i ? "," : "", ue ? ue : "");
    free(ue);
  }
  snprintf(out + o, cap - (size_t)o, "]}");
  return out;
}
