/*
 * test_json.c -- json_lite against what a server sends, and what it must not
 * trip over: truncation, nesting past the limit, a lone backslash.
 */

#include <stdio.h>
#include <string.h>

#include "json_lite.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-52s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static json_t P(const char *s) {
    json_t v = { NULL, 0 };
    if (!json_parse(s, strlen(s), &v)) {
        v.p = NULL;
        v.n = 0;
    }
    return v;
}

static void test_values(void) {
    check("object", json_type(P(" {\"a\":1} ")), JSON_OBJECT);
    check("array", json_type(P("[1,2,3]")), JSON_ARRAY);
    check("string", json_type(P("\"x\"")), JSON_STRING);
    check("true", json_type(P("true")), JSON_TRUE);
    check("null", json_type(P("null")), JSON_NULL);
    check("number", json_type(P("-12.5e3")), JSON_NUMBER);
    const char *bad[] = {
        "", "{", "[1,", "{\"a\"}", "{\"a\":}", "[1 2]", "tru", "01", "-", "1.", "1e",
        "\"abc", "\"a\\\"", "\"\\x\"", "\"\\u12\"", "{\"a\":1,}", "[,1]", "1 2",
        "\"tab\there\"",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        json_t v;
        if (json_parse(bad[i], strlen(bad[i]), &v)) {
            printf("  FAIL accepted malformed %s\n", bad[i]);
            g_fail++;
        }
    }
    char deep[64];
    memset(deep, '[', 40);
    memset(deep + 20, ']', 20);
    deep[40] = '\0';
    json_t v;
    check("20 deep is past JSON_MAX_DEPTH", json_parse(deep, 40, &v), 0);
    check("a truncated value at the end of a buffer",
          json_parse("{\"a\":[1,2", 9, &v), 0);
}

static void test_access(void) {
    json_t o = P("{\"uuid\":\"ab-c\",\"command\":16,\"x\":{\"y\":[true]},\"n\":null}");
    json_t v;
    char s[16];
    int64_t n;
    check("get a string", json_get(o, "uuid", &v) && json_str(v, s, sizeof s) &&
                          strcmp(s, "ab-c") == 0, 1);
    check("get a number", json_get(o, "command", &v) && json_int(v, &n) && n == 16, 1);
    check("nested", json_get(o, "x", &v) && json_get(v, "y", &v) && json_at(v, 0, &v) &&
                    json_type(v) == JSON_TRUE, 1);
    check("absent key", json_get(o, "missing", &v), 0);
    check("a key is not found inside a value", json_get(P("{\"a\":\"command\"}"), "command", &v), 0);
    json_t a = P("[ 1 , \"two\" , [3] ]");
    check("length", json_len(a), 3);
    check("at 1", json_at(a, 1, &v) && json_str_is(v, "two"), 1);
    check("past the end", json_at(a, 3, &v), 0);
    check("empty array", json_len(P("[]")), 0);
    check("not an array", json_len(o), -1);

    check("a fraction is not an int", json_int(P("1.5"), &n), 0);
    check("an exponent is not an int", json_int(P("1e3"), &n), 0);
    check("int64 max", json_int(P("9223372036854775807"), &n) && n == INT64_MAX, 1);
    check("int64 min", json_int(P("-9223372036854775808"), &n) && n == INT64_MIN, 1);
    check("past int64", json_int(P("9223372036854775808"), &n), 0);
}

static void test_strings(void) {
    char s[16];
    check("escapes", json_str(P("\"a\\n\\\"\\\\\\/\""), s, sizeof s) && strcmp(s, "a\n\"\\/") == 0, 1);
    check("\\u to UTF-8", json_str(P("\"\\u00e9\\u20ac\""), s, sizeof s) &&
                          strcmp(s, "\xC3\xA9\xE2\x82\xAC") == 0, 1);
    check("surrogate pair", json_str(P("\"\\ud83c\\udfae\""), s, sizeof s) &&
                            strcmp(s, "\xF0\x9F\x8E\xAE") == 0, 1);
    check("lone surrogate is U+FFFD", json_str(P("\"\\udc00\""), s, sizeof s) &&
                                      strcmp(s, "\xEF\xBF\xBD") == 0, 1);
    check("\\u0000 refused", json_str(P("\"a\\u0000b\""), s, sizeof s), 0);
    check("too long for the buffer", json_str(P("\"0123456789abcdef\""), s, sizeof s), 0);
    check("exactly fits", json_str(P("\"0123456789abcde\""), s, sizeof s), 1);

    char q[32];
    check("quote", (long)json_quote(q, sizeof q, "a\"b\\c\n"), 15);
    check("quoted text", strcmp(q, "\"a\\\"b\\\\c\\u000a\""), 0);
    json_t back;
    check("quote parses back", json_parse(q, strlen(q), &back) && json_str(back, s, sizeof s) &&
                               strcmp(s, "a\"b\\c\n") == 0, 1);
    check("a quote that does not fit", (long)json_quote(q, 4, "abc"), 0);
}

int main(void) {
    test_values();
    test_access();
    test_strings();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
