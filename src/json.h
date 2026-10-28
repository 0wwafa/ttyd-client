#ifndef TTYD_CLIENT_JSON_H
#define TTYD_CLIENT_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "buffer.h"

/* A small, dependency free JSON reader/writer.
 * The ttyd protocol only needs to (a) read the token endpoint response,
 * (b) read the `SET_PREFERENCES` payload and (c) write the initial/resize
 * messages, so this is deliberately compact while still being a complete
 * RFC 8259 parser (numbers, escapes, nesting, UTF-8 passthrough). */

typedef enum {
  JSON_NULL = 0,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
} json_type;

typedef struct json_value json_value;

struct json_value {
  json_type type;
  union {
    bool boolean;
    double number;
    char *string;
    struct {
      json_value **items;
      size_t count;
    } array;
    struct {
      char **keys;
      json_value **values;
      size_t count;
    } object;
  } u;
};

/* Parse `len` bytes of `text`, returns NULL on error.
 * A trailing NUL is not required. */
json_value *json_parse(const char *text, size_t len);

/* Free a value returned by json_parse() */
void json_free(json_value *value);

/* Look up a key in an object, returns NULL when not found */
json_value *json_object_get(const json_value *object, const char *key);

/* Typed getters with defaults, the container is checked for type */
const char *json_get_string(const json_value *object, const char *key, const char *def);
bool json_get_bool(const json_value *object, const char *key, bool def);
double json_get_number(const json_value *object, const char *key, double def);
long json_get_int(const json_value *object, const char *key, long def);

/* Append `str` to `buf` as a quoted, escaped JSON string */
void json_write_string(buffer_t *buf, const char *str);

/* Serialize a value, used by tests and debug logging */
void json_write(buffer_t *buf, const json_value *value);

#endif  // TTYD_CLIENT_JSON_H
