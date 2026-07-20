#ifndef NANOBOT_JSON_H
#define NANOBOT_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

/** Escape for JSON string content (malloc'd). */
char *nb_json_escape(const char *s);

/** Naive "key":"value" extractor (malloc'd value). */
char *nb_json_get_string(const char *json, const char *key);

/** choices[0].message.content or top-level content. */
char *nb_json_message_content(const char *json);

/** First tool_calls function name/args/id. Returns 1 if found. */
int nb_json_first_tool_call(const char *json, char **name, char **args, char **id);

#ifdef __cplusplus
}
#endif

#endif
