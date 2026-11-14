/*
 * json_lite.h -- just enough JSON to read what a Socket.IO server sends.
 *
 * No allocation and no tree: a value is a slice of the input, and each
 * accessor scans it again. The messages are small (a Celio data packet is
 * 32 numbers), so scanning twice costs less than building anything. Every
 * function is bounded by the slice and by JSON_MAX_DEPTH, and returns false
 * rather than reading past the end of malformed input -- the input is
 * whatever arrived from a network.
 */

#ifndef JSON_LITE_H
#define JSON_LITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_MAX_DEPTH 16

typedef struct {
    const char *p;     /* first character of the value */
    size_t      n;     /* its length                   */
} json_t;

typedef enum {
    JSON_BAD = 0, JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER,
    JSON_TRUE, JSON_FALSE, JSON_NULL
} json_type_t;

/* The one value s holds, whitespace around it allowed; false if there is
 * not exactly one well-formed value. */
bool json_parse(const char *s, size_t n, json_t *out);

/* The first value at the start of s (after whitespace); *rest is what
 * follows it. For input that carries more than JSON. */
bool json_first(const char *s, size_t n, json_t *out, size_t *used);

json_type_t json_type(json_t v);

/* obj[key], for an object; false if absent or v is not an object. */
bool json_get(json_t obj, const char *key, json_t *out);

/* Element i of an array; false past the end. */
bool json_at(json_t arr, size_t i, json_t *out);
/* How many elements, or -1 if not an array. */
int  json_len(json_t arr);

/* A string's contents, unescaped (\uXXXX to UTF-8, surrogate pairs joined),
 * terminated. False if v is not a string or it does not fit cap. */
bool json_str(json_t v, char *out, size_t cap);
/* Is v the string s? */
bool json_str_is(json_t v, const char *s);

/* An integral number in range. False for a fraction, an exponent, or out of
 * range of int64_t. */
bool json_int(json_t v, int64_t *out);

/* Write s as a JSON string literal, quotes included. Returns the length,
 * or 0 if it does not fit (out is then empty). */
size_t json_quote(char *out, size_t cap, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* JSON_LITE_H */
