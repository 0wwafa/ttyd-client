#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

#define JSON_MAX_DEPTH 64

typedef struct {
  const char *p;
  const char *end;
  int depth;
  bool error;
} json_parser;

static json_value *parse_value(json_parser *parser);

static json_value *value_new(json_type type) {
  json_value *v = xcalloc(1, sizeof(json_value));
  v->type = type;
  return v;
}

static void skip_ws(json_parser *parser) {
  while (parser->p < parser->end) {
    char c = *parser->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      parser->p++;
    } else {
      break;
    }
  }
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void utf8_encode(buffer_t *buf, unsigned int cp) {
  char tmp[4];
  if (cp < 0x80) {
    tmp[0] = (char)cp;
    buffer_append(buf, tmp, 1);
  } else if (cp < 0x800) {
    tmp[0] = (char)(0xc0 | (cp >> 6));
    tmp[1] = (char)(0x80 | (cp & 0x3f));
    buffer_append(buf, tmp, 2);
  } else if (cp < 0x10000) {
    tmp[0] = (char)(0xe0 | (cp >> 12));
    tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    tmp[2] = (char)(0x80 | (cp & 0x3f));
    buffer_append(buf, tmp, 3);
  } else {
    tmp[0] = (char)(0xf0 | (cp >> 18));
    tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    tmp[3] = (char)(0x80 | (cp & 0x3f));
    buffer_append(buf, tmp, 4);
  }
}

static bool parse_hex4(json_parser *parser, unsigned int *out) {
  if (parser->end - parser->p < 4) return false;
  unsigned int value = 0;
  for (int i = 0; i < 4; i++) {
    int h = hex_value(parser->p[i]);
    if (h < 0) return false;
    value = (value << 4) | (unsigned int)h;
  }
  parser->p += 4;
  *out = value;
  return true;
}

static char *parse_string_raw(json_parser *parser) {
  buffer_t buf;
  buffer_init(&buf);

  if (parser->p >= parser->end || *parser->p != '"') {
    parser->error = true;
    buffer_free(&buf);
    return NULL;
  }
  parser->p++;

  while (parser->p < parser->end) {
    unsigned char c = (unsigned char)*parser->p++;
    if (c == '"') {
      char *result = buffer_detach(&buf);
      return result;
    }
    if (c != '\\') {
      if (c < 0x20) { /* control characters are not allowed unescaped */
        parser->error = true;
        break;
      }
      buffer_append_char(&buf, (char)c);
      continue;
    }
    if (parser->p >= parser->end) break;
    char esc = *parser->p++;
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        buffer_append_char(&buf, esc);
        break;
      case 'b':
        buffer_append_char(&buf, '\b');
        break;
      case 'f':
        buffer_append_char(&buf, '\f');
        break;
      case 'n':
        buffer_append_char(&buf, '\n');
        break;
      case 'r':
        buffer_append_char(&buf, '\r');
        break;
      case 't':
        buffer_append_char(&buf, '\t');
        break;
      case 'u': {
        unsigned int cp = 0;
        if (!parse_hex4(parser, &cp)) {
          parser->error = true;
          break;
        }
        if (cp >= 0xd800 && cp <= 0xdbff && parser->end - parser->p >= 6 && parser->p[0] == '\\' &&
            parser->p[1] == 'u') {
          const char *save = parser->p;
          parser->p += 2;
          unsigned int low = 0;
          if (parse_hex4(parser, &low) && low >= 0xdc00 && low <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          } else {
            parser->p = save;
          }
        }
        utf8_encode(&buf, cp);
        break;
      }
      default:
        parser->error = true;
        break;
    }
    if (parser->error) break;
  }

  parser->error = true;
  buffer_free(&buf);
  return NULL;
}

static json_value *parse_number(json_parser *parser) {
  const char *start = parser->p;
  if (parser->p < parser->end && (*parser->p == '-' || *parser->p == '+')) parser->p++;
  while (parser->p < parser->end && *parser->p >= '0' && *parser->p <= '9') parser->p++;
  if (parser->p < parser->end && *parser->p == '.') {
    parser->p++;
    while (parser->p < parser->end && *parser->p >= '0' && *parser->p <= '9') parser->p++;
  }
  if (parser->p < parser->end && (*parser->p == 'e' || *parser->p == 'E')) {
    parser->p++;
    if (parser->p < parser->end && (*parser->p == '-' || *parser->p == '+')) parser->p++;
    while (parser->p < parser->end && *parser->p >= '0' && *parser->p <= '9') parser->p++;
  }
  if (parser->p == start) {
    parser->error = true;
    return NULL;
  }
  char *tmp = xstrndup(start, (size_t)(parser->p - start));
  json_value *v = value_new(JSON_NUMBER);
  v->u.number = strtod(tmp, NULL);
  free(tmp);
  return v;
}

static json_value *parse_array(json_parser *parser) {
  json_value *v = value_new(JSON_ARRAY);
  parser->p++; /* '[' */
  skip_ws(parser);
  if (parser->p < parser->end && *parser->p == ']') {
    parser->p++;
    return v;
  }
  for (;;) {
    json_value *item = parse_value(parser);
    if (item == NULL) {
      json_free(v);
      return NULL;
    }
    v->u.array.items = xrealloc(v->u.array.items, (v->u.array.count + 1) * sizeof(json_value *));
    v->u.array.items[v->u.array.count++] = item;
    skip_ws(parser);
    if (parser->p < parser->end && *parser->p == ',') {
      parser->p++;
      skip_ws(parser);
      continue;
    }
    if (parser->p < parser->end && *parser->p == ']') {
      parser->p++;
      return v;
    }
    parser->error = true;
    json_free(v);
    return NULL;
  }
}

static json_value *parse_object(json_parser *parser) {
  json_value *v = value_new(JSON_OBJECT);
  parser->p++; /* '{' */
  skip_ws(parser);
  if (parser->p < parser->end && *parser->p == '}') {
    parser->p++;
    return v;
  }
  for (;;) {
    char *key = parse_string_raw(parser);
    if (key == NULL) {
      json_free(v);
      return NULL;
    }
    skip_ws(parser);
    if (parser->p >= parser->end || *parser->p != ':') {
      free(key);
      parser->error = true;
      json_free(v);
      return NULL;
    }
    parser->p++;
    skip_ws(parser);
    json_value *item = parse_value(parser);
    if (item == NULL) {
      free(key);
      json_free(v);
      return NULL;
    }
    size_t n = v->u.object.count;
    v->u.object.keys = xrealloc(v->u.object.keys, (n + 1) * sizeof(char *));
    v->u.object.values = xrealloc(v->u.object.values, (n + 1) * sizeof(json_value *));
    v->u.object.keys[n] = key;
    v->u.object.values[n] = item;
    v->u.object.count = n + 1;

    skip_ws(parser);
    if (parser->p < parser->end && *parser->p == ',') {
      parser->p++;
      skip_ws(parser);
      continue;
    }
    if (parser->p < parser->end && *parser->p == '}') {
      parser->p++;
      return v;
    }
    parser->error = true;
    json_free(v);
    return NULL;
  }
}

static json_value *parse_value(json_parser *parser) {
  if (parser->depth >= JSON_MAX_DEPTH) {
    parser->error = true;
    return NULL;
  }
  skip_ws(parser);
  if (parser->p >= parser->end) {
    parser->error = true;
    return NULL;
  }

  char c = *parser->p;
  parser->depth++;
  json_value *result = NULL;

  if (c == '{') {
    result = parse_object(parser);
  } else if (c == '[') {
    result = parse_array(parser);
  } else if (c == '"') {
    char *str = parse_string_raw(parser);
    if (str != NULL) {
      result = value_new(JSON_STRING);
      result->u.string = str;
    }
  } else if (c == 't' && parser->end - parser->p >= 4 && !strncmp(parser->p, "true", 4)) {
    parser->p += 4;
    result = value_new(JSON_BOOL);
    result->u.boolean = true;
  } else if (c == 'f' && parser->end - parser->p >= 5 && !strncmp(parser->p, "false", 5)) {
    parser->p += 5;
    result = value_new(JSON_BOOL);
    result->u.boolean = false;
  } else if (c == 'n' && parser->end - parser->p >= 4 && !strncmp(parser->p, "null", 4)) {
    parser->p += 4;
    result = value_new(JSON_NULL);
  } else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
    result = parse_number(parser);
  } else {
    parser->error = true;
  }

  parser->depth--;
  return result;
}

json_value *json_parse(const char *text, size_t len) {
  if (text == NULL) return NULL;
  json_parser parser = {.p = text, .end = text + len, .depth = 0, .error = false};
  json_value *value = parse_value(&parser);
  if (value == NULL || parser.error) {
    json_free(value);
    return NULL;
  }
  return value;
}

void json_free(json_value *value) {
  if (value == NULL) return;
  switch (value->type) {
    case JSON_STRING:
      free(value->u.string);
      break;
    case JSON_ARRAY:
      for (size_t i = 0; i < value->u.array.count; i++) json_free(value->u.array.items[i]);
      free(value->u.array.items);
      break;
    case JSON_OBJECT:
      for (size_t i = 0; i < value->u.object.count; i++) {
        free(value->u.object.keys[i]);
        json_free(value->u.object.values[i]);
      }
      free(value->u.object.keys);
      free(value->u.object.values);
      break;
    default:
      break;
  }
  free(value);
}

json_value *json_object_get(const json_value *object, const char *key) {
  if (object == NULL || object->type != JSON_OBJECT || key == NULL) return NULL;
  for (size_t i = 0; i < object->u.object.count; i++) {
    if (!strcmp(object->u.object.keys[i], key)) return object->u.object.values[i];
  }
  return NULL;
}

const char *json_get_string(const json_value *object, const char *key, const char *def) {
  json_value *v = json_object_get(object, key);
  if (v == NULL || v->type != JSON_STRING) return def;
  return v->u.string;
}

bool json_get_bool(const json_value *object, const char *key, bool def) {
  json_value *v = json_object_get(object, key);
  if (v == NULL) return def;
  if (v->type == JSON_BOOL) return v->u.boolean;
  if (v->type == JSON_NUMBER) return v->u.number != 0;
  if (v->type == JSON_STRING) return !strcmp(v->u.string, "true") || !strcmp(v->u.string, "1");
  return def;
}

double json_get_number(const json_value *object, const char *key, double def) {
  json_value *v = json_object_get(object, key);
  if (v == NULL) return def;
  if (v->type == JSON_NUMBER) return v->u.number;
  if (v->type == JSON_STRING) return strtod(v->u.string, NULL);
  return def;
}

long json_get_int(const json_value *object, const char *key, long def) {
  double d = json_get_number(object, key, (double)def);
  if (d < -2147483648.0 || d > 2147483647.0) return def;
  /* round half away from zero, without pulling in libm */
  return (long)(d >= 0 ? (d + 0.5) : (d - 0.5));
}

void json_write_string(buffer_t *buf, const char *str) {
  buffer_append_char(buf, '"');
  if (str != NULL) {
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
      switch (*p) {
        case '"':
          buffer_append_str(buf, "\\\"");
          break;
        case '\\':
          buffer_append_str(buf, "\\\\");
          break;
        case '\b':
          buffer_append_str(buf, "\\b");
          break;
        case '\f':
          buffer_append_str(buf, "\\f");
          break;
        case '\n':
          buffer_append_str(buf, "\\n");
          break;
        case '\r':
          buffer_append_str(buf, "\\r");
          break;
        case '\t':
          buffer_append_str(buf, "\\t");
          break;
        default:
          if (*p < 0x20) {
            buffer_appendf(buf, "\\u%04x", (unsigned int)*p);
          } else {
            buffer_append_char(buf, (char)*p);
          }
          break;
      }
    }
  }
  buffer_append_char(buf, '"');
}

void json_write(buffer_t *buf, const json_value *value) {
  if (value == NULL) {
    buffer_append_str(buf, "null");
    return;
  }
  switch (value->type) {
    case JSON_NULL:
      buffer_append_str(buf, "null");
      break;
    case JSON_BOOL:
      buffer_append_str(buf, value->u.boolean ? "true" : "false");
      break;
    case JSON_NUMBER:
      if (value->u.number == (double)(long long)value->u.number) {
        buffer_appendf(buf, "%lld", (long long)value->u.number);
      } else {
        buffer_appendf(buf, "%.17g", value->u.number);
      }
      break;
    case JSON_STRING:
      json_write_string(buf, value->u.string);
      break;
    case JSON_ARRAY:
      buffer_append_char(buf, '[');
      for (size_t i = 0; i < value->u.array.count; i++) {
        if (i > 0) buffer_append_char(buf, ',');
        json_write(buf, value->u.array.items[i]);
      }
      buffer_append_char(buf, ']');
      break;
    case JSON_OBJECT:
      buffer_append_char(buf, '{');
      for (size_t i = 0; i < value->u.object.count; i++) {
        if (i > 0) buffer_append_char(buf, ',');
        json_write_string(buf, value->u.object.keys[i]);
        buffer_append_char(buf, ':');
        json_write(buf, value->u.object.values[i]);
      }
      buffer_append_char(buf, '}');
      break;
  }
}
