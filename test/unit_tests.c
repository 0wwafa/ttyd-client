/*
 * Unit tests for the self contained parts of ttyd-client: base64, SHA-1, the
 * JSON parser/writer, the URL parser and the ttyd wire protocol codec.
 *
 * Everything that needs a socket, a terminal or a websocket connection is
 * covered by test/integration.py instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <string.h>
#else
#include <strings.h>
#endif

#include "b64.h"
#include "buffer.h"
#include "compat.h"
#include "json.h"
#include "protocol.h"
#include "sha1.h"
#include "url.h"
#include "utils.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...)                     \
  do {                                       \
    checks++;                                \
    if (!(cond)) {                           \
      failures++;                            \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                   \
      printf("\n");                          \
    }                                        \
  } while (0)

#define CHECK_STR(got, want)                                                      \
  do {                                                                            \
    checks++;                                                                     \
    const char *g_ = (got);                                                       \
    const char *w_ = (want);                                                      \
    if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0) {                        \
      failures++;                                                                 \
      printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__,          \
             g_ ? g_ : "(null)", w_ ? w_ : "(null)");                             \
    }                                                                             \
  } while (0)

static void test_base64(void) {
  char out[128];
  unsigned char decoded[128];

  size_t len = base64_encode("hello", 5, out, sizeof(out));
  CHECK(len == 8 && strcmp(out, "aGVsbG8=") == 0, "encode hello: %s (%zu)", out, len);

  len = base64_encode("a", 1, out, sizeof(out));
  CHECK_STR(out, "YQ==");
  len = base64_encode("ab", 2, out, sizeof(out));
  CHECK_STR(out, "YWI=");
  len = base64_encode("user:pass", 9, out, sizeof(out));
  CHECK_STR(out, "dXNlcjpwYXNz");

  size_t n = base64_decode("aGVsbG8=", 8, decoded, sizeof(decoded));
  CHECK(n == 5 && memcmp(decoded, "hello", 5) == 0, "decode hello: %zu", n);

  n = base64_decode("dXNlcjpwYXNz", 12, decoded, sizeof(decoded));
  CHECK(n == 9 && memcmp(decoded, "user:pass", 9) == 0, "decode user:pass: %zu", n);

  /* Whitespace and padding tolerance */
  n = base64_decode("YQ==", 4, decoded, sizeof(decoded));
  CHECK(n == 1 && decoded[0] == 'a', "decode single char: %zu", n);

  char *allocated = base64_encode_alloc("ttyd", 4);
  CHECK_STR(allocated, "dHR5ZA==");
  free(allocated);

  /* Round trip of every byte value */
  unsigned char all[256];
  for (int i = 0; i < 256; i++) all[i] = (unsigned char)i;
  char encoded[512];
  len = base64_encode(all, sizeof(all), encoded, sizeof(encoded));
  n = base64_decode(encoded, len, decoded, sizeof(decoded));
  CHECK(n == sizeof(all) && memcmp(all, decoded, sizeof(all)) == 0, "round trip: %zu", n);
}

static void digest_to_hex(const unsigned char *digest, char *hex) {
  static const char *digits = "0123456789abcdef";
  for (int i = 0; i < SHA1_DIGEST_SIZE; i++) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  hex[SHA1_DIGEST_SIZE * 2] = '\0';
}

static void test_sha1(void) {
  unsigned char digest[SHA1_DIGEST_SIZE];
  char hex[SHA1_DIGEST_SIZE * 2 + 1];

  sha1("", 0, digest);
  digest_to_hex(digest, hex);
  CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

  sha1("abc", 3, digest);
  digest_to_hex(digest, hex);
  CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

  const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha1(two_blocks, strlen(two_blocks), digest);
  digest_to_hex(digest, hex);
  CHECK_STR(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

  /* Streaming must produce the same digest as the one shot call */
  sha1_ctx ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, "a", 1);
  sha1_update(&ctx, "bc", 2);
  sha1_final(&ctx, digest);
  digest_to_hex(digest, hex);
  CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

  /* The RFC 6455 handshake example */
  const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
  char src[128];
  snprintf(src, sizeof(src), "%s%s", key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  sha1(src, strlen(src), digest);
  char accept[64];
  base64_encode(digest, SHA1_DIGEST_SIZE, accept, sizeof(accept));
  CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_buffer(void) {
  buffer_t buf;
  buffer_init(&buf);
  buffer_append_str(&buf, "hello");
  buffer_append_char(&buf, ' ');
  buffer_appendf(&buf, "%s-%d", "world", 42);
  CHECK(buf.len == strlen("hello world-42"), "len: %zu", buf.len);
  CHECK_STR(buf.data, "hello world-42");

  buffer_consume(&buf, 6);
  CHECK_STR(buf.data, "world-42");

  /* Appending a large chunk must grow the buffer, including a NUL byte */
  buffer_reset(&buf);
  for (int i = 0; i < 1000; i++) buffer_append(&buf, "0123456789", 10);
  CHECK(buf.len == 10000, "large len: %zu", buf.len);
  CHECK(buf.data[9999] == '9', "large byte: %c", buf.data[9999]);

  buffer_append_char(&buf, '\0');
  CHECK(buf.len == 10001 && buf.data[10000] == '\0', "embedded NUL");

  char *detached = buffer_detach(&buf);
  CHECK(detached != NULL && detached[0] == '0' && buf.data == NULL, "detach");
  free(detached);
  buffer_free(&buf);
}

static void test_json(void) {
  const char *text =
      "{\"columns\":120,\"rows\":40,\"title\":\"a\\\"b\\\\c\",\"ok\":true,\"nil\":null,"
      "\"list\":[1,2,3],\"nested\":{\"deep\":\"value\"},\"uni\":\"\\u00e9\\u4e2d\"}";
  json_value *value = json_parse(text, strlen(text));
  CHECK(value != NULL, "parse failed");
  if (value == NULL) return;

  CHECK(json_get_int(value, "columns", 0) == 120, "columns: %ld", json_get_int(value, "columns", 0));
  CHECK(json_get_int(value, "rows", 0) == 40, "rows");
  CHECK_STR(json_get_string(value, "title", ""), "a\"b\\c");
  CHECK(json_get_bool(value, "ok", false) == true, "ok");
  CHECK(json_get_bool(value, "missing", false) == false, "missing bool");
  CHECK_STR(json_get_string(value, "uni", ""), "\xc3\xa9\xe4\xb8\xad");
  CHECK_STR(json_get_string(value, "missing", "def"), "def");

  json_value *list = json_object_get(value, "list");
  CHECK(list != NULL && list->type == JSON_ARRAY && list->u.array.count == 3, "array");
  if (list != NULL && list->u.array.count == 3) {
    CHECK(list->u.array.items[2]->u.number == 3.0, "array item");
  }
  json_value *nested = json_object_get(value, "nested");
  CHECK(nested != NULL, "nested missing");
  if (nested != NULL) CHECK_STR(json_get_string(nested, "deep", ""), "value");

  /* Serializing must round trip */
  buffer_t out;
  buffer_init(&out);
  json_write(&out, value);
  json_value *again = json_parse(out.data, out.len);
  CHECK(again != NULL, "re-parse failed");
  if (again != NULL) {
    CHECK(json_get_int(again, "columns", 0) == 120, "re-parse columns");
    CHECK_STR(json_get_string(again, "title", ""), "a\"b\\c");
    json_free(again);
  }
  buffer_free(&out);
  json_free(value);

  /* Invalid documents must be rejected, not crash */
  const char *bad[] = {"", "{", "{\"a\":}", "[1,", "{\"a\":1}}", "\"unterminated", "{\"a\" 1}"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    json_value *v = json_parse(bad[i], strlen(bad[i]));
    CHECK(v == NULL, "accepted invalid json: %s", bad[i]);
    json_free(v);
  }

  /* Escape handling on the way out */
  buffer_t esc;
  buffer_init(&esc);
  json_write_string(&esc, "line\nbreak\"quote\"\x01");
  CHECK_STR(esc.data, "\"line\\nbreak\\\"quote\\\"\\u0001\"");
  buffer_free(&esc);
}

static void test_url(void) {
  char err[256] = "";
  tc_url url;

  CHECK(url_parse("localhost:7681", false, &url, err, sizeof(err)), "parse bare host: %s", err);
  CHECK_STR(url.host, "localhost");
  CHECK(url.port == 7681, "port: %d", url.port);
  CHECK_STR(url.path, "/");
  CHECK(url.tls == false, "tls");
  tc_endpoints endpoints;
  url_endpoints(&url, &endpoints);
  CHECK_STR(endpoints.base, "");
  CHECK_STR(endpoints.ws, "/ws");
  CHECK_STR(endpoints.token, "/token");
  url_endpoints_free(&endpoints);
  url_free(&url);

  CHECK(url_parse("localhost", false, &url, err, sizeof(err)), "parse no port: %s", err);
  CHECK(url.port == 7681, "default port: %d", url.port);
  url_free(&url);

  CHECK(url_parse("https://example.com/mounted/here/", false, &url, err, sizeof(err)), "parse https: %s", err);
  CHECK(url.tls == true, "https tls");
  CHECK(url.port == 443, "https port: %d", url.port);
  CHECK_STR(url.path, "/mounted/here/");
  url_endpoints(&url, &endpoints);
  CHECK_STR(endpoints.base, "/mounted/here");
  CHECK_STR(endpoints.ws, "/mounted/here/ws");
  CHECK_STR(endpoints.token, "/mounted/here/token");
  url_endpoints_free(&endpoints);
  url_free(&url);

  CHECK(url_parse("ws://127.0.0.1:1234/ws", false, &url, err, sizeof(err)), "parse ws: %s", err);
  CHECK_STR(url.host, "127.0.0.1");
  CHECK(url.port == 1234, "ws port: %d", url.port);
  url_endpoints(&url, &endpoints);
  CHECK_STR(endpoints.base, "");
  CHECK_STR(endpoints.ws, "/ws");
  url_endpoints_free(&endpoints);
  url_free(&url);

  CHECK(url_parse("http://[::1]:8080/x", false, &url, err, sizeof(err)), "parse ipv6: %s", err);
  CHECK_STR(url.host, "::1");
  CHECK(url.port == 8080, "ipv6 port: %d", url.port);
  CHECK_STR(url.path, "/x");
  url_free(&url);

  CHECK(url_parse("ws://user:secret@host:9000/", false, &url, err, sizeof(err)), "parse userinfo: %s", err);
  CHECK_STR(url.user, "user");
  CHECK_STR(url.password, "secret");
  CHECK_STR(url.host, "host");
  url_free(&url);

  CHECK(url_parse("wss://host/base/token", false, &url, err, sizeof(err)), "parse token url: %s", err);
  url_endpoints(&url, &endpoints);
  CHECK_STR(endpoints.base, "/base");
  CHECK_STR(endpoints.token, "/base/token");
  url_endpoints_free(&endpoints);
  url_free(&url);

  CHECK(url_parse("wss://host/base", true, &url, err, sizeof(err)), "parse wss: %s", err);
  CHECK(url.tls == true, "wss tls");
  CHECK(url.port == 443, "wss port: %d", url.port);
  url_free(&url);

  CHECK(url_parse("", false, &url, err, sizeof(err)) == false, "empty url accepted");
  CHECK(url_parse(":8080", false, &url, err, sizeof(err)) == false, "empty host accepted");

  char *encoded = url_encode("a b&c/d?e=f+g%");
  CHECK_STR(encoded, "a%20b%26c%2Fd%3Fe%3Df%2Bg%25");
  free(encoded);

  char *query = NULL;
  url_append_query(&query, "arg", "hello world");
  CHECK_STR(query, "arg=hello%20world");
  url_append_query(&query, "x", "1");
  CHECK_STR(query, "arg=hello%20world&x=1");
  free(query);
}

static void test_protocol(void) {
  buffer_t buf;
  buffer_init(&buf);

  protocol_init_message(&buf, "dXNlcjpwYXNz", 80, 24);
  CHECK_STR(buf.data, "{\"AuthToken\":\"dXNlcjpwYXNz\",\"columns\":80,\"rows\":24}");

  buffer_reset(&buf);
  protocol_init_message(&buf, "", 132, 50);
  CHECK_STR(buf.data, "{\"AuthToken\":\"\",\"columns\":132,\"rows\":50}");

  buffer_reset(&buf);
  protocol_input(&buf, "ls\r", 3);
  CHECK(buf.len == 4 && buf.data[0] == TC_CMD_INPUT && memcmp(buf.data + 1, "ls\r", 3) == 0, "input frame");

  buffer_reset(&buf);
  protocol_resize(&buf, 100, 30);
  CHECK(buf.data[0] == TC_CMD_RESIZE_TERMINAL, "resize cmd");
  CHECK_STR(buf.data + 1, "{\"columns\":100,\"rows\":30}");

  buffer_reset(&buf);
  protocol_pause(&buf);
  protocol_resume(&buf);
  CHECK(buf.len == 2 && buf.data[0] == TC_CMD_PAUSE && buf.data[1] == TC_CMD_RESUME, "pause/resume");
  buffer_free(&buf);

  tc_msg msg;
  protocol_parse("0hello world", strlen("0hello world"), &msg);
  CHECK(msg.type == TC_MSG_OUTPUT && msg.len == 11, "output msg");
  CHECK(memcmp(msg.data, "hello world", 11) == 0, "output payload");

  protocol_parse("1bash (host)", strlen("1bash (host)"), &msg);
  CHECK(msg.type == TC_MSG_WINDOW_TITLE && msg.len == 11, "title msg");

  protocol_parse("2{\"fontSize\":15}", strlen("2{\"fontSize\":15}"), &msg);
  CHECK(msg.type == TC_MSG_PREFERENCES && msg.len == 14, "prefs msg");

  protocol_parse("", 0, &msg);
  CHECK(msg.type == TC_MSG_UNKNOWN && msg.len == 0, "empty msg");
}

static void test_utils(void) {
  CHECK(endswith("hello.c", ".c"), "endswith");
  CHECK(!endswith("hello.h", ".c"), "endswith negative");
  CHECK(startswith("--verbose", "--"), "startswith");

  char trimmed[] = "  hello \t\r\n";
  CHECK_STR(str_trim(trimmed), "hello");

  long value = 0;
  CHECK(parse_long("123", &value) && value == 123, "parse_long");
  CHECK(parse_long("-7", &value) && value == -7, "parse_long negative");
  CHECK(!parse_long("12x", &value), "parse_long garbage");
  CHECK(!parse_long("", &value), "parse_long empty");

  unsigned char random_a[16], random_b[16];
  CHECK(random_bytes(random_a, sizeof(random_a)), "random_bytes");
  CHECK(random_bytes(random_b, sizeof(random_b)), "random_bytes again");
  CHECK(memcmp(random_a, random_b, sizeof(random_a)) != 0, "random_bytes repeats");

  CHECK(now_ms() > 0, "now_ms");
  CHECK(strcasecmp("HEAD", "head") == 0, "strcasecmp");
}

int main(void) {
  test_utils();
  test_base64();
  test_sha1();
  test_buffer();
  test_json();
  test_url();
  test_protocol();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
