#include "wire_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    fails++;
  }
}

static int ends_with_mark(const char *s) {
  size_t n;
  const char mark[] = "\n\xE2\x80\xA6";
  if (!s) return 0;
  n = strlen(s);
  return n >= 4 && memcmp(s + n - 4, mark, 4) == 0;
}

static void test_effort(void) {
  char out[40];
  expect(ng_effort_canon("xHigh", out, sizeof out) == 0 && strcmp(out, "xhigh") == 0,
         "xHigh folds");
  expect(ng_effort_canon("x-high", out, sizeof out) == 0 && strcmp(out, "xhigh") == 0,
         "x-high alias");
  expect(ng_effort_canon("x_high", out, sizeof out) == 0 && strcmp(out, "xhigh") == 0,
         "x_high alias");
  expect(ng_effort_canon("OFF", out, sizeof out) == 0 && out[0] == 0, "OFF clears");
  expect(ng_effort_canon("none", out, sizeof out) == 0 && out[0] == 0, "none clears");
  expect(ng_effort_canon("default", out, sizeof out) == 0 && out[0] == 0, "default clears");
  expect(ng_effort_canon("Catalog_1", out, sizeof out) == 0 && strcmp(out, "catalog_1") == 0,
         "catalog token");
  expect(ng_effort_canon("bad token", out, sizeof out) == -1, "space rejected");
  expect(ng_effort_canon("\"quoted\"", out, sizeof out) == -1, "quote rejected");
  expect(ng_effort_canon("1abc", out, sizeof out) == -1, "leading digit rejected");
  expect(ng_effort_canon("-nope", out, sizeof out) == -1, "leading hyphen rejected");
}

static void test_client_id(void) {
  unsetenv("NANOBOT_CLIENT_NAME");
  unsetenv("GROK_CLIENT_NAME");
  expect(strcmp(ng_client_identifier(), "nanobot") == 0, "default nanobot");
  setenv("NANOBOT_CLIENT_NAME", "bad\nname", 1);
  expect(strcmp(ng_client_identifier(), "nanobot") == 0, "newline id rejected");
  setenv("NANOBOT_CLIENT_NAME", "peer-1", 1);
  expect(strcmp(ng_client_identifier(), "peer-1") == 0, "nanobot name wins");
  setenv("NANOBOT_CLIENT_NAME", "bad name", 1);
  setenv("GROK_CLIENT_NAME", "alt_peer", 1);
  expect(strcmp(ng_client_identifier(), "alt_peer") == 0, "fallback client name");
  unsetenv("NANOBOT_CLIENT_NAME");
  unsetenv("GROK_CLIENT_NAME");
}

static void test_truncate(void) {
  char *s = ng_suggest_truncate("  hello  ", 20);
  char *cut;
  char *utf;
  expect(s && strcmp(s, "hello") == 0, "short trim");
  free(s);
  cut = ng_suggest_truncate("alpha beta gamma", 12);
  expect(cut && strncmp(cut, "alpha", 5) == 0, "cut keeps word");
  expect(cut && !strstr(cut, "beta"), "cut stops before next word");
  expect(ends_with_mark(cut), "cut marker");
  free(cut);
  utf = ng_suggest_truncate("abcd\xC3\xA9wxyz", 9);
  expect(utf && strcmp(utf, "abcd\n\xE2\x80\xA6") == 0, "utf-8 boundary");
  free(utf);
  utf = ng_suggest_truncate("\xC2\xA0read the law and prove\xC2\xA0", 80);
  expect(utf && strcmp(utf, "read the law and prove") == 0,
         "a unicode space at the turn edge drops");
  free(utf);
  utf = ng_suggest_truncate("read\xC2\xA0the law and prove", 80);
  expect(utf && strcmp(utf, "read\xC2\xA0the law and prove") == 0,
         "an interior unicode space stays in the turn");
  free(utf);
  utf = ng_suggest_truncate("read the law and prove\xE2\x80\x94", 80);
  expect(utf && strcmp(utf, "read the law and prove\xE2\x80\x94") == 0,
         "an em dash stays on the turn");
  free(utf);
}

static void test_pack(void) {
  const char *roles_user[] = {"user", "user"};
  const char *texts_user[] = {"one", "two"};
  char user[1801];
  char agent[1801];
  const char *roles[2];
  const char *texts[2];
  char *packed;
  char *big;
  char *tail;
  enum { N = 24 };
  const char *mroles[N];
  const char *mtexts[N];
  char *owned[N];
  int i;
  expect(ng_suggest_pack_turns(roles_user, texts_user, 2) == NULL, "no agent line");
  memset(user, 'u', sizeof user - 1);
  user[sizeof user - 1] = 0;
  memset(agent, 'a', sizeof agent - 1);
  memcpy(agent + sizeof agent - 9, "AGENTEND", 8);
  agent[sizeof agent - 1] = 0;
  roles[0] = "user";
  roles[1] = "assistant";
  texts[0] = user;
  texts[1] = agent;
  packed = ng_suggest_pack_turns(roles, texts, 2);
  expect(packed && strstr(packed, "AGENTEND"), "newest agent kept whole");
  expect(packed && strstr(packed, "\n\xE2\x80\xA6"), "older turn capped");
  expect(packed && !strstr(packed, "uuuuAGENT") , "user body was cut");
  free(packed);
  for (i = 0; i < N; i++) {
    owned[i] = malloc(1500);
    expect(owned[i] != NULL, "alloc turn");
    if (!owned[i]) return;
    memset(owned[i], 'm', 1490);
    owned[i][1490] = 0;
    if (i == 0) memcpy(owned[i], "OLDEST", 6);
    else if (i == N - 1) memcpy(owned[i], "NEWEST", 6);
    mroles[i] = (i % 2) ? "assistant" : "user";
    mtexts[i] = owned[i];
  }
  packed = ng_suggest_pack_turns(mroles, mtexts, N);
  expect(packed && strstr(packed, "NEWEST"), "budget keeps newest");
  expect(packed && !strstr(packed, "OLDEST"), "budget drops oldest");
  expect(packed && strlen(packed) <= 24000, "budget ceiling");
  free(packed);
  for (i = 0; i < N; i++) free(owned[i]);
  big = malloc(25001);
  expect(big != NULL, "alloc transcript");
  if (!big) return;
  memset(big, 'q', 25000);
  memcpy(big, "HEAD", 4);
  memcpy(big + 24996, "TAIL", 4);
  big[25000] = 0;
  tail = ng_suggest_pack_text(big);
  expect(tail && strlen(tail) <= 24000, "pack text budget");
  expect(tail && strstr(tail, "TAIL") && !strstr(tail, "HEAD"), "pack text keeps tail");
  free(tail);
  free(big);
}

static char *one_msg(const char *role, const char *tag, size_t n) {
  char *body = malloc(n + 1);
  char *s = NULL;
  if (!body) return NULL;
  memset(body, 'x', n);
  body[n] = 0;
  if (tag && tag[0] && strlen(tag) < n) memcpy(body, tag, strlen(tag));
  if (asprintf(&s, "{\"role\":\"%s\",\"content\":\"%s\"}", role, body) < 0) s = NULL;
  free(body);
  return s;
}

static void test_compact(void) {
  char *parts[5];
  char *joined = NULL;
  char *out;
  size_t total = 2;
  int i;
  parts[0] = one_msg("user", "HEADMARK", 400);
  parts[1] = one_msg("user", "AAAA", 400);
  parts[2] = one_msg("assistant", "MIDMARK", 400);
  parts[3] = one_msg("user", "BBBB", 400);
  parts[4] = one_msg("assistant", "LASTMARK", 400);
  for (i = 0; i < 5; i++) total += parts[i] ? strlen(parts[i]) + 1 : 0;
  joined = malloc(total + 4);
  expect(joined != NULL, "alloc messages");
  if (joined && parts[0] && parts[1] && parts[2] && parts[3] && parts[4]) {
    snprintf(joined, total + 4, "[%s,%s,%s,%s,%s]",
             parts[0], parts[1], parts[2], parts[3], parts[4]);
    out = ng_messages_compact(joined, 1);
    expect(out && strlen(out) < strlen(joined), "refill 1 shrinks");
    expect(out && strstr(out, "HEADMARK") && strstr(out, "LASTMARK"), "refill 1 keeps ends");
    expect(out && strstr(out, "BBBB") && !strstr(out, "MIDMARK"), "refill 1 drops middle");
    expect(out && strstr(out, "nanobot.compact.v1") && strstr(out, "dropped"), "refill 1 plate");
    free(out);
    out = ng_messages_compact(joined, 2);
    expect(out && strstr(out, "HEADMARK") && strstr(out, "LASTMARK"), "refill 2 keeps head and last");
    expect(out && !strstr(out, "BBBB") && !strstr(out, "MIDMARK"), "refill 2 drops the rest");
    free(out);
    out = ng_messages_compact(joined, 3);
    expect(out && strstr(out, "LASTMARK") && !strstr(out, "HEADMARK"), "refill 3 keeps last");
    expect(out && strlen(out) < strlen(joined), "refill 3 shrinks");
    free(out);
  }
  free(joined);
  for (i = 0; i < 5; i++) free(parts[i]);

  out = ng_messages_compact(
      "[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":\"yo\"}]", 1);
  expect(out == NULL, "short pair cannot shrink");
  free(out);

  {
    char *only = one_msg("user", "ONLY", 3000);
    char *arr = NULL;
    if (only && asprintf(&arr, "[%s]", only) > 0) {
      expect(ng_messages_compact(arr, 1) == NULL, "lone prompt refill 1");
      expect(ng_messages_compact(arr, 3) == NULL, "lone prompt refill 3");
    }
    free(arr);
    free(only);
  }

  {
    char *fat = one_msg("user", "FAT", 2000);
    char *arr = NULL;
    if (fat && asprintf(&arr, "[%s,{\"role\":\"assistant\",\"content\":\"ok\"}]", fat) > 0) {
      out = ng_messages_compact(arr, 1);
      expect(out && strlen(out) < strlen(arr), "oversized object shrinks");
      expect(out && strstr(out, "truncated"), "shrink plate");
      free(out);
    }
    free(arr);
    free(fat);
  }
}

static void test_compact_policy(void) {
  char *parts[6];
  char *joined = NULL;
  char *out;
  size_t total = 2;
  int i;
  ng_compact_policy pol;
  char tag[8];
  for (i = 0; i < 6; i++) {
    snprintf(tag, sizeof tag, "M%dMARK", i);
    parts[i] = one_msg(i % 2 ? "assistant" : "user", tag, 220);
    total += parts[i] ? strlen(parts[i]) + 1 : 0;
  }
  joined = malloc(total + 8);
  expect(joined && parts[0] && parts[5], "policy alloc");
  if (joined && parts[0] && parts[1] && parts[2] && parts[3] && parts[4] && parts[5]) {
    snprintf(joined, total + 8, "[%s,%s,%s,%s,%s,%s]",
             parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]);
    memset(&pol, 0, sizeof pol);
    pol.refill = 1;
    pol.max_refills = 3;
    pol.keep_tail = 4;
    pol.keep_head = -1;
    pol.shrink_at = 1800;
    out = ng_messages_compact_ex(joined, &pol);
    expect(out && strstr(out, "M0MARK") && strstr(out, "M5MARK"), "wide tail keeps ends");
    expect(out && strstr(out, "M2MARK") && !strstr(out, "M1MARK"), "wide tail drops the oldest middle");
    expect(out && strlen(out) < strlen(joined), "wide tail shrinks");
    free(out);
    pol.refill = 3;
    pol.max_refills = 5;
    pol.keep_tail = 2;
    out = ng_messages_compact_ex(joined, &pol);
    expect(out && strstr(out, "M0MARK") && strstr(out, "M5MARK"), "later refill still keeps head");
    expect(out && !strstr(out, "M4MARK"), "later refill keeps one tail");
    free(out);
    pol.refill = 5;
    out = ng_messages_compact_ex(joined, &pol);
    expect(out && strstr(out, "M5MARK") && !strstr(out, "M0MARK"), "last refill drops head");
    free(out);
  }
  free(joined);
  for (i = 0; i < 6; i++) free(parts[i]);
}

static void test_overflow(void) {
  expect(!ng_resp_is_context_overflow("the context window is fine"), "prose is not overflow");
  expect(!ng_resp_is_context_overflow(
             "{\"choices\":[{\"message\":{\"content\":\"context window\"}}]}"),
         "assistant text is not overflow");
  expect(ng_resp_is_context_overflow(
             "{\"error\":{\"message\":\"maximum context length exceeded\"}}"),
         "error message is overflow");
  expect(ng_resp_is_context_overflow(
             "{\"error\":{\"code\":\"context_length_exceeded\"}}"),
         "error code is overflow");
}

static void test_mcp_parse(void) {
  char path[] = "/tmp/nanobot-wire-bearer-XXXXXX";
  int fd;
  char *tok;
  char *obj;
  FILE *f;
  char big[2048];
  fd = mkstemp(path);
  expect(fd >= 0, "bearer temp");
  if (fd >= 0) {
    f = fdopen(fd, "w");
    expect(f != NULL, "bearer fdopen");
    if (f) {
      fputs("  tok\n", f);
      fclose(f);
      tok = ng_mcp_read_bearer_file(path);
      expect(tok && strcmp(tok, "tok") == 0, "bearer trim");
      free(tok);
    } else {
      close(fd);
    }
    f = fopen(path, "w");
    if (f) {
      fputs("tok\nbad", f);
      fclose(f);
      tok = ng_mcp_read_bearer_file(path);
      expect(tok == NULL, "bearer rejects embedded newline");
      free(tok);
    }
    f = fopen(path, "w");
    if (f) {
      memset(big, 'A', sizeof big);
      fwrite(big, 1, sizeof big, f);
      fclose(f);
      tok = ng_mcp_read_bearer_file(path);
      expect(tok == NULL, "bearer rejects oversized read");
      free(tok);
    }
    unlink(path);
  }
  obj = ng_mcp_extract_tool_result(
      "{\"content\":[{\"type\":\"text\",\"text\":\"plain\"}],"
      "\"structuredContent\":{\"a\":1,\"b\":{\"c\":2}}}");
  expect(obj && strcmp(obj, "{\"a\":1,\"b\":{\"c\":2}}") == 0, "structuredContent object");
  free(obj);
  obj = ng_mcp_extract_tool_result(
      "{\"structuredContent\":{\"a\":\"}\"},\"text\":\"plain\"}");
  expect(obj && strcmp(obj, "{\"a\":\"}\"}") == 0, "structuredContent string aware");
  free(obj);
  obj = ng_mcp_extract_tool_result("{\"content\":[{\"type\":\"text\",\"text\":\"plain\"}]}");
  expect(obj == NULL, "text stays with the caller");
  free(obj);
}

int main(void) {
  test_effort();
  test_client_id();
  test_truncate();
  test_pack();
  test_compact();
  test_compact_policy();
  test_overflow();
  test_mcp_parse();
  if (fails) {
    fprintf(stderr, "%d wire checks failed\n", fails);
    return 1;
  }
  return 0;
}
