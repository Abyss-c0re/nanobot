#include "augogen.h"
#include "util.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#if NANOBOT_HAS_BRAINCUBE
#include <braincube/braincube.h>
#endif

#define SUG_CAP 240
#define ID_CAP 40
#define ROLE_CAP 16

typedef struct {
  char id[ID_CAP];
  char suggestion[SUG_CAP];
  char status[24]; /* empty|pending|approved|rejected|applied */
  unsigned long generation;
  int guide_vote;   /* -1 unknown, 0 no, 1 yes */
  int oversee_vote;
  int mesh_vote;
  int confirmed;
  int pair_ok;
} ng_augogen;

static void augogen_dir(char *buf, size_t n) {
  snprintf(buf, n, "%s/augogen", ng_workdir());
}

static void plate_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/augogen/pending.json", ng_workdir());
}

#if NANOBOT_HAS_BRAINCUBE
static void cube_path(char *buf, size_t n, const char *who) {
  snprintf(buf, n, "%s/augogen/%s.bin", ng_workdir(), who);
}

static void load_cube(lhlam_cube *c, const char *who) {
  char path[640];
  size_t len = 0;
  char *raw;
  uint8_t seed[32];
  cube_path(path, sizeof path, who);
  raw = ng_read_file(path, &len);
  if (raw && len > 8 && lhlam_cube_import(c, (const uint8_t *)raw, len) == 0) {
    free(raw);
    return;
  }
  free(raw);
  memset(seed, who[0] == 'g' ? 0x17 : 0x0E, sizeof seed);
  seed[0] = (uint8_t)(who[0] == 'g' ? 3 : 9);
  lhlam_cube_init(c, seed);
}

static void save_cube(const lhlam_cube *c, const char *who) {
  uint8_t bin[8192];
  size_t need = lhlam_cube_export(c, bin, sizeof bin);
  char path[640];
  if (need == 0 || need > sizeof bin) return;
  cube_path(path, sizeof path, who);
  ng_write_file(path, (const char *)bin, need);
}
#endif

static void ensure_dir(void) {
  char d[640];
  augogen_dir(d, sizeof d);
  mkdir(d, 0700);
}

static void plate_clear(ng_augogen *p) {
  memset(p, 0, sizeof *p);
  snprintf(p->status, sizeof p->status, "empty");
  p->guide_vote = -1;
  p->oversee_vote = -1;
  p->mesh_vote = -1;
}

static char *escape_or_empty(const char *s) {
  char *e = ng_json_escape(s ? s : "");
  return e ? e : strdup("");
}

static char *plate_json(const ng_augogen *p) {
  char *sug = escape_or_empty(p->suggestion);
  char *id = escape_or_empty(p->id);
  char *st = escape_or_empty(p->status);
  char *out = NULL;
  asprintf(&out,
           "{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":true,"
           "\"action\":\"%s\",\"id\":\"%s\",\"generation\":%lu,"
           "\"suggestion\":\"%s\",\"status\":\"%s\","
           "\"guide_vote\":%d,\"oversee_vote\":%d,\"mesh_vote\":%d,"
           "\"pair_ok\":%s,\"confirmed\":%s,\"auto_execute\":false,"
           "\"next_prompt\":%s,"
           "\"api\":\"x.ai/suggestPrompt\","
           "\"cli_compat\":\"%s\","
           "\"hold_flash\":1,\"share\":\"state_matrix_only\","
           "\"python\":0}",
           p->status,
           id ? id : "",
           p->generation,
           sug ? sug : "",
           st ? st : "empty",
           p->guide_vote, p->oversee_vote, p->mesh_vote,
           p->pair_ok ? "true" : "false",
           p->confirmed ? "true" : "false",
           (p->confirmed && p->suggestion[0])
             ? (sug ? sug : "\"\"")
             : "null",
           ng_cli_version());
  /* next_prompt must be a JSON string when confirmed — fix when confirmed */
  if (p->confirmed && p->suggestion[0] && out) {
    free(out);
    out = NULL;
    asprintf(&out,
             "{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":true,"
             "\"action\":\"%s\",\"id\":\"%s\",\"generation\":%lu,"
             "\"suggestion\":\"%s\",\"status\":\"%s\","
             "\"guide_vote\":%d,\"oversee_vote\":%d,\"mesh_vote\":%d,"
             "\"pair_ok\":%s,\"confirmed\":true,\"auto_execute\":false,"
             "\"next_prompt\":\"%s\","
             "\"api\":\"x.ai/suggestPrompt\","
             "\"cli_compat\":\"%s\","
             "\"hold_flash\":1,\"share\":\"state_matrix_only\","
             "\"python\":0}",
             p->status, id ? id : "", p->generation, sug ? sug : "",
             st ? st : "approved",
             p->guide_vote, p->oversee_vote, p->mesh_vote,
             p->pair_ok ? "true" : "false",
             sug ? sug : "",
             ng_cli_version());
  }
  free(sug);
  free(id);
  free(st);
  return out ? out
             : strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                      "\"error\":\"oom\",\"python\":0}");
}

static void load_plate(ng_augogen *p) {
  char path[640];
  size_t len = 0;
  char *raw;
  char *s;
  plate_clear(p);
  plate_path(path, sizeof path);
  raw = ng_read_file(path, &len);
  if (!raw || !raw[0]) {
    free(raw);
    return;
  }
  s = ng_json_get_string(raw, "id");
  if (s) {
    snprintf(p->id, sizeof p->id, "%s", s);
    free(s);
  }
  s = ng_json_get_string(raw, "suggestion");
  if (s) {
    snprintf(p->suggestion, sizeof p->suggestion, "%s", s);
    free(s);
  }
  s = ng_json_get_string(raw, "status");
  if (s) {
    snprintf(p->status, sizeof p->status, "%s", s);
    free(s);
  }
  s = ng_json_get_string(raw, "generation");
  if (s) {
    p->generation = strtoul(s, NULL, 10);
    free(s);
  } else if (strstr(raw, "\"generation\":")) {
    const char *g = strstr(raw, "\"generation\":");
    if (g) p->generation = strtoul(g + 13, NULL, 10);
  }
  if (strstr(raw, "\"guide_vote\":1")) p->guide_vote = 1;
  else if (strstr(raw, "\"guide_vote\":0")) p->guide_vote = 0;
  if (strstr(raw, "\"oversee_vote\":1")) p->oversee_vote = 1;
  else if (strstr(raw, "\"oversee_vote\":0")) p->oversee_vote = 0;
  if (strstr(raw, "\"mesh_vote\":1")) p->mesh_vote = 1;
  else if (strstr(raw, "\"mesh_vote\":0")) p->mesh_vote = 0;
  p->pair_ok = (p->guide_vote == 1 && p->oversee_vote == 1);
  p->confirmed = p->pair_ok && (p->mesh_vote != 0);
  if (p->confirmed && strcmp(p->status, "applied") != 0)
    snprintf(p->status, sizeof p->status, "approved");
  free(raw);
}

static int save_plate(const ng_augogen *p) {
  char path[640];
  char *js;
  int rc;
  ensure_dir();
  plate_path(path, sizeof path);
  js = plate_json(p);
  rc = ng_write_file(path, js, strlen(js));
  free(js);
  return rc;
}

static void recompute(ng_augogen *p) {
  p->pair_ok = (p->guide_vote == 1 && p->oversee_vote == 1);
  /* Mesh may abstain (-1). A 0 is a veto. Pair approve + no veto = confirmed. */
  p->confirmed = p->pair_ok && (p->mesh_vote != 0) && p->suggestion[0];
  if (p->confirmed) {
    if (strcmp(p->status, "applied") != 0)
      snprintf(p->status, sizeof p->status, "approved");
  } else if (p->guide_vote == 0 || p->oversee_vote == 0 || p->mesh_vote == 0) {
    snprintf(p->status, sizeof p->status, "rejected");
    p->confirmed = 0;
  } else if (p->suggestion[0]) {
    snprintf(p->status, sizeof p->status, "pending");
  } else {
    snprintf(p->status, sizeof p->status, "empty");
  }
}

/* grok-build prompt_suggest::sanitize_suggestion */
static int sanitize_suggestion(const char *raw, char *out, size_t cap) {
  char line[SUG_CAP];
  size_t i = 0;
  const char *p;
  char lowered[SUG_CAP];
  static const char *meta[] = {
      "none", "n/a", "no suggestion", "nothing", "(silence)", "silence", "null",
      NULL};
  if (!raw || !out || cap < 2) return 0;
  while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') raw++;
  while (*raw && *raw != '\n' && *raw != '\r' && i + 1 < sizeof line)
    line[i++] = *raw++;
  line[i] = 0;
  /* trim quotes */
  p = line;
  while (*p == '"' || *p == '\'' || *p == '`' || (unsigned char)*p == 0xE2)
    p++;
  i = strlen(p);
  while (i > 0 && (p[i - 1] == '"' || p[i - 1] == '\'' || p[i - 1] == '`' ||
                   p[i - 1] == ' ' || p[i - 1] == '.'))
    i--;
  if (i >= cap) i = cap - 1;
  memcpy(out, p, i);
  out[i] = 0;
  if (!out[0]) return 0;
  for (i = 0; out[i] && i + 1 < sizeof lowered; i++) {
    char c = out[i];
    lowered[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lowered[i] = 0;
  for (i = 0; meta[i]; i++) {
    if (strcmp(lowered, meta[i]) == 0) return 0;
    if (strncmp(lowered, meta[i], strlen(meta[i])) == 0 &&
        lowered[strlen(meta[i])] == '.')
      return 0;
  }
  /* 2–12 words, matching grok-build eval bound */
  {
    int words = 0, inw = 0;
    for (i = 0; out[i]; i++) {
      if (out[i] != ' ' && out[i] != '\t') {
        if (!inw) {
          words++;
          inw = 1;
        }
      } else {
        inw = 0;
      }
    }
    if (words < 2 || words > 12) return 0;
  }
  return 1;
}

static void new_id(char *out, size_t n) {
  snprintf(out, n, "ag-%ld-%d", (long)time(NULL), (int)(getpid() & 0xFFFF));
}

char *ng_augogen_pending_json(void) {
  ng_augogen p;
  load_plate(&p);
  return plate_json(&p);
}

char *ng_augogen_handle(ng_agent_cfg *agent, const char *json_body) {
  ng_augogen p;
  char *action;
  if (!json_body) json_body = "{}";
  load_plate(&p);
  action = ng_json_get_string(json_body, "action");
  if (!action) action = strdup("pending");

  if (!strcmp(action, "pending") || !strcmp(action, "status") ||
      !strcmp(action, "get")) {
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "reset") || !strcmp(action, "clear")) {
    plate_clear(&p);
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "reject")) {
    p.confirmed = 0;
    p.pair_ok = 0;
    snprintf(p.status, sizeof p.status, "rejected");
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "vote")) {
    char *role = ng_json_get_string(json_body, "role");
    char *vs = ng_json_get_string(json_body, "vote");
    int v = 0;
    if (!p.suggestion[0]) {
      free(role);
      free(vs);
      free(action);
      return strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                    "\"action\":\"vote\",\"error\":\"no_pending\","
                    "\"auto_execute\":false,\"python\":0}");
    }
    if (vs && vs[0]) v = (vs[0] == '1' || vs[0] == 't' || vs[0] == 'T' ||
                          vs[0] == 'y' || vs[0] == 'Y')
                             ? 1
                             : 0;
    if (!role) role = strdup("mesh");
    if (!strcmp(role, "guide") || !strcmp(role, "construct") ||
        !strcmp(role, "center"))
      p.guide_vote = v;
    else if (!strcmp(role, "oversee") || !strcmp(role, "deconstruct") ||
             !strcmp(role, "observer"))
      p.oversee_vote = v;
    else
      p.mesh_vote = v;
#if NANOBOT_HAS_BRAINCUBE
    {
      lhlam_cube guide, oversee;
      int pair;
      load_cube(&guide, "guide");
      load_cube(&oversee, "oversee");
      pair = lhlam_cube_pair_confirm_cstr(&guide, &oversee, p.suggestion);
      p.pair_ok = pair || (p.guide_vote == 1 && p.oversee_vote == 1);
      save_cube(&guide, "guide");
      save_cube(&oversee, "oversee");
    }
#endif
    recompute(&p);
    save_plate(&p);
    free(role);
    free(vs);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "apply")) {
    /* Never run the suggestion. Confirmed plate exposes next_prompt only. */
    if (!p.confirmed) {
      free(action);
      return strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                    "\"action\":\"apply\",\"error\":\"not_confirmed\","
                    "\"auto_execute\":false,\"python\":0}");
    }
    snprintf(p.status, sizeof p.status, "applied");
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "generate") || !strcmp(action, "suggest") ||
      !strcmp(action, "suggestPrompt")) {
    char *given = ng_json_get_string(json_body, "suggestion");
    char *transcript = ng_json_get_string(json_body, "transcript");
    char cleaned[SUG_CAP];
    unsigned long gen = p.generation + 1;
    cleaned[0] = 0;
    if (given && sanitize_suggestion(given, cleaned, sizeof cleaned)) {
      /* host-supplied line (CubalC / mesh) still needs pair vote */
    } else if (agent) {
      char *raw = ng_agent_suggest_prompt(agent, transcript, ng_workdir());
      if (raw) {
        sanitize_suggestion(raw, cleaned, sizeof cleaned);
        free(raw);
      }
    }
    free(given);
    free(transcript);
    plate_clear(&p);
    p.generation = gen;
    new_id(p.id, sizeof p.id);
    if (cleaned[0]) {
      snprintf(p.suggestion, sizeof p.suggestion, "%s", cleaned);
      snprintf(p.status, sizeof p.status, "pending");
    } else {
      snprintf(p.status, sizeof p.status, "empty");
    }
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  {
    char *esc = ng_json_escape(action);
    char *err = NULL;
    asprintf(&err,
             "{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
             "\"action\":\"error\",\"error\":\"unknown_action\","
             "\"got\":\"%s\",\"auto_execute\":false,\"python\":0}",
             esc ? esc : "");
    free(esc);
    free(action);
    return err ? err
               : strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                        "\"error\":\"unknown_action\",\"python\":0}");
  }
}
