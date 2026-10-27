/*
 * json_lite.c -- see json_lite.h.
 */

#include "json_lite.h"

#include <string.h>

static size_t ws(const char *s, size_t i, size_t n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        i++;
    }
    return i;
}

static bool hex4(const char *s, size_t i, size_t n, uint32_t *out) {
    if (i + 4 > n) {
        return false;
    }
    uint32_t v = 0;
    for (size_t k = 0; k < 4; k++) {
        char ch = s[i + k];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (uint32_t)(ch - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

/* Skip the string starting at s[i] == '"'. Returns the index after it, 0 on error. */
static size_t skip_string(const char *s, size_t i, size_t n) {
    i++;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"') {
            return i + 1;
        }
        if (ch < 0x20) {
            return 0;
        }
        if (ch == '\\') {
            if (i + 1 >= n) {
                return 0;
            }
            char e = s[i + 1];
            if (e == 'u') {
                uint32_t v;
                if (!hex4(s, i + 2, n, &v)) {
                    return 0;
                }
                i += 6;
                continue;
            }
            if (!strchr("\"\\/bfnrt", e) || e == '\0') {
                return 0;
            }
            i += 2;
            continue;
        }
        i++;
    }
    return 0;
}

static size_t skip_number(const char *s, size_t i, size_t n) {
    size_t st = i;
    if (i < n && s[i] == '-') i++;
    if (i >= n) return 0;
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    } else {
        return 0;
    }
    if (i < n && s[i] == '.') {
        i++;
        size_t d = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        size_t d = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return 0;
    }
    return i > st ? i : 0;
}

/* Skip one value at s[i] (no leading whitespace). Returns the index after
 * it, or 0 on error. */
static size_t skip_value(const char *s, size_t i, size_t n, int depth) {
    if (i >= n || depth > JSON_MAX_DEPTH) {
        return 0;
    }
    switch (s[i]) {
    case '"':
        return skip_string(s, i, n);
    case '{':
    case '[': {
        char close = s[i] == '{' ? '}' : ']';
        bool obj = s[i] == '{';
        i = ws(s, i + 1, n);
        if (i < n && s[i] == close) {
            return i + 1;
        }
        for (;;) {
            if (obj) {
                if (i >= n || s[i] != '"') return 0;
                i = skip_string(s, i, n);
                if (!i) return 0;
                i = ws(s, i, n);
                if (i >= n || s[i] != ':') return 0;
                i = ws(s, i + 1, n);
            }
            i = skip_value(s, i, n, depth + 1);
            if (!i) return 0;
            i = ws(s, i, n);
            if (i >= n) return 0;
            if (s[i] == close) return i + 1;
            if (s[i] != ',') return 0;
            i = ws(s, i + 1, n);
        }
    }
    case 't':
        return (n - i >= 4 && memcmp(s + i, "true", 4) == 0) ? i + 4 : 0;
    case 'f':
        return (n - i >= 5 && memcmp(s + i, "false", 5) == 0) ? i + 5 : 0;
    case 'n':
        return (n - i >= 4 && memcmp(s + i, "null", 4) == 0) ? i + 4 : 0;
    default:
        return skip_number(s, i, n);
    }
}

bool json_first(const char *s, size_t n, json_t *out, size_t *used) {
    size_t i = ws(s, 0, n);
    size_t e = skip_value(s, i, n, 0);
    if (!e) {
        return false;
    }
    out->p = s + i;
    out->n = e - i;
    if (used) {
        *used = e;
    }
    return true;
}

bool json_parse(const char *s, size_t n, json_t *out) {
    size_t used;
    if (!json_first(s, n, out, &used)) {
        return false;
    }
    return ws(s, used, n) == n;
}

json_type_t json_type(json_t v) {
    if (v.n == 0) return JSON_BAD;
    switch (v.p[0]) {
    case '{': return JSON_OBJECT;
    case '[': return JSON_ARRAY;
    case '"': return JSON_STRING;
    case 't': return JSON_TRUE;
    case 'f': return JSON_FALSE;
    case 'n': return JSON_NULL;
    default:  return JSON_NUMBER;
    }
}

bool json_get(json_t obj, const char *key, json_t *out) {
    if (json_type(obj) != JSON_OBJECT) {
        return false;
    }
    const char *s = obj.p;
    size_t n = obj.n, i = ws(s, 1, n);
    while (i < n && s[i] == '"') {
        size_t ke = skip_string(s, i, n);
        if (!ke) return false;
        json_t k = { s + i, ke - i };
        i = ws(s, ke, n);
        if (i >= n || s[i] != ':') return false;
        i = ws(s, i + 1, n);
        size_t ve = skip_value(s, i, n, 1);
        if (!ve) return false;
        if (json_str_is(k, key)) {
            out->p = s + i;
            out->n = ve - i;
            return true;
        }
        i = ws(s, ve, n);
        if (i < n && s[i] == ',') i = ws(s, i + 1, n);
    }
    return false;
}

bool json_at(json_t arr, size_t idx, json_t *out) {
    if (json_type(arr) != JSON_ARRAY) {
        return false;
    }
    const char *s = arr.p;
    size_t n = arr.n, i = ws(s, 1, n);
    for (size_t k = 0; i < n && s[i] != ']'; k++) {
        size_t ve = skip_value(s, i, n, 1);
        if (!ve) return false;
        if (k == idx) {
            out->p = s + i;
            out->n = ve - i;
            return true;
        }
        i = ws(s, ve, n);
        if (i < n && s[i] == ',') i = ws(s, i + 1, n);
    }
    return false;
}

int json_len(json_t arr) {
    if (json_type(arr) != JSON_ARRAY) {
        return -1;
    }
    int k = 0;
    json_t e;
    while (json_at(arr, (size_t)k, &e)) {
        k++;
    }
    return k;
}

static size_t put_utf8(char *out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool json_str(json_t v, char *out, size_t cap) {
    if (json_type(v) != JSON_STRING || v.n < 2 || cap == 0) {
        return false;
    }
    const char *s = v.p;
    size_t n = v.n - 1, o = 0;
    for (size_t i = 1; i < n;) {
        char tmp[4];
        size_t tl;
        if (s[i] == '\\') {
            char e = s[i + 1];
            if (e == 'u') {
                uint32_t cp;
                if (!hex4(s, i + 2, n, &cp)) return false;
                i += 6;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo;
                    if (i + 1 < n && s[i] == '\\' && s[i + 1] == 'u' && hex4(s, i + 2, n, &lo) &&
                        lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else {
                        cp = 0xFFFD;   /* a lone surrogate */
                    }
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                if (cp == 0) return false;   /* would end the C string early */
                tl = put_utf8(tmp, cp);
            } else {
                switch (e) {
                case 'b': tmp[0] = '\b'; break;
                case 'f': tmp[0] = '\f'; break;
                case 'n': tmp[0] = '\n'; break;
                case 'r': tmp[0] = '\r'; break;
                case 't': tmp[0] = '\t'; break;
                default:  tmp[0] = e;    break;
                }
                tl = 1;
                i += 2;
            }
        } else {
            tmp[0] = s[i++];
            tl = 1;
        }
        if (o + tl >= cap) {
            out[0] = '\0';
            return false;
        }
        memcpy(out + o, tmp, tl);
        o += tl;
    }
    out[o] = '\0';
    return true;
}

bool json_str_is(json_t v, const char *want) {
    char buf[64];
    size_t wl = strlen(want);
    if (wl >= sizeof buf || !json_str(v, buf, sizeof buf)) {
        return false;
    }
    return strcmp(buf, want) == 0;
}

bool json_int(json_t v, int64_t *out) {
    if (json_type(v) != JSON_NUMBER) {
        return false;
    }
    size_t i = 0;
    bool neg = false;
    if (v.p[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i >= v.n) return false;
    uint64_t acc = 0;
    for (; i < v.n; i++) {
        char ch = v.p[i];
        if (ch < '0' || ch > '9') return false;   /* '.', 'e' */
        if (acc > (UINT64_MAX - 9) / 10) return false;
        acc = acc * 10 + (uint64_t)(ch - '0');
    }
    if (!neg && acc > (uint64_t)INT64_MAX) return false;
    if (neg && acc > (uint64_t)INT64_MAX + 1) return false;
    *out = neg ? (int64_t)(0 - acc) : (int64_t)acc;
    return true;
}

size_t json_quote(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (cap < 3) {
        if (cap) out[0] = '\0';
        return 0;
    }
    out[o++] = '"';
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        char esc[7];
        size_t el;
        if (ch == '"' || ch == '\\') {
            esc[0] = '\\'; esc[1] = (char)ch; el = 2;
        } else if (ch < 0x20) {
            static const char hx[] = "0123456789abcdef";
            memcpy(esc, "\\u00", 4);
            esc[4] = hx[ch >> 4]; esc[5] = hx[ch & 15]; el = 6;
        } else {
            esc[0] = (char)ch; el = 1;
        }
        if (o + el + 2 > cap) {
            out[0] = '\0';
            return 0;
        }
        memcpy(out + o, esc, el);
        o += el;
    }
    out[o++] = '"';
    out[o] = '\0';
    return o;
}
