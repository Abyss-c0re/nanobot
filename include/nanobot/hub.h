#ifndef NANOBOT_HUB_H
#define NANOBOT_HUB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hub roles — two doors only.
 *   NB_HUB_IN  — accept work (WRITE security)
 *   NB_HUB_OUT — observe streams/events (READ security)
 */
typedef enum {
  NB_HUB_IN = 1,
  NB_HUB_OUT = 2
} nb_hub_role;

/** Append one JSON event line to $NANOBOT_HOME/hub/events.jsonl (best-effort). */
int nb_hub_event(const char *type_json_object);

/** Stream callback: partial assistant text (may be empty for status). */
typedef void (*nb_stream_fn)(void *userdata, const char *chunk, size_t n);

#ifdef __cplusplus
}
#endif

#endif
