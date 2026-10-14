#include "stream_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    fails++;
  }
}

static ng_stream_filter *load(void) {
  return ng_stream_filter_parse(
      "{\"rules\":["
      "{\"op\":\"redact\",\"match\":\"sekret\",\"with\":\"***\"},"
      "{\"op\":\"drop\",\"match\":\"DROPME\"},"
      "{\"op\":\"drop_kind\",\"kind\":\"thinking\"}"
      "]}");
}

static void test_filter(void) {
  ng_stream_filter *f = load();
  char carry[160];
  size_t cn = 0;
  char *out = NULL;
  int rc;
  expect(f != NULL, "parse");
  expect(ng_stream_filter_hold(f) == 5, "hold is longest needle minus one");
  rc = ng_stream_filter_push(f, "token", "alpha sekret beta", carry, &cn, sizeof carry, &out);
  expect(rc == 1 && out && strcmp(out, "alpha *** beta") == 0, "redact one chunk");
  expect(cn == 0, "redact leaves no hold");
  free(out);
  out = NULL;
  rc = ng_stream_filter_push(f, "token", "xxsek", carry, &cn, sizeof carry, &out);
  expect(rc == 1 && out && strcmp(out, "xx") == 0, "holds a split needle");
  expect(cn == 3 && memcmp(carry, "sek", 3) == 0, "carry is the needle prefix");
  free(out);
  out = NULL;
  rc = ng_stream_filter_push(f, "token", "retYY", carry, &cn, sizeof carry, &out);
  expect(rc == 1 && out && strcmp(out, "***YY") == 0, "completes a split redact");
  expect(cn == 0, "split redact clears carry");
  free(out);
  out = NULL;
  cn = 0;
  rc = ng_stream_filter_push(f, "token", "see DROPME now", carry, &cn, sizeof carry, &out);
  expect(rc == 0 && out == NULL, "drop consumes the chunk");
  expect(cn == 0, "drop clears carry");
  rc = ng_stream_filter_push(f, "thinking", "secret thought", carry, &cn, sizeof carry, &out);
  expect(rc == 0, "drop_kind suppresses thinking");
  rc = ng_stream_filter_push(f, "token", "plain", carry, &cn, sizeof carry, &out);
  expect(rc == 1 && out && strcmp(out, "plain") == 0, "other kinds still pass");
  free(out);
  out = NULL;
  rc = ng_stream_filter_push(f, "token", "zzsek", carry, &cn, sizeof carry, &out);
  free(out);
  out = NULL;
  rc = ng_stream_filter_flush(f, "token", carry, &cn, sizeof carry, &out);
  expect(rc == 1 && out && strcmp(out, "sek") == 0, "flush emits an unfinished needle");
  expect(cn == 0, "flush clears carry");
  free(out);
  expect(ng_stream_filter_parse("nope") == NULL, "broken filter rejected");
  ng_stream_filter_free(f);
  f = ng_stream_filter_parse(NULL);
  cn = 0;
  rc = ng_stream_filter_push(f, NULL, "same", carry, &cn, sizeof carry, &out);
  expect(rc == 1 && out && strcmp(out, "same") == 0, "empty filter passes text");
  free(out);
  ng_stream_filter_free(f);
}

int main(void) {
  test_filter();
  if (fails) {
    fprintf(stderr, "%d filter checks failed\n", fails);
    return 1;
  }
  return 0;
}
