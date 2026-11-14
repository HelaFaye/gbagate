/*
 * wifi_cfg.c -- see wifi_cfg.h.
 */

#include "wifi_cfg.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"   /* settings_crc32 */

#define WIFI_MAGIC   0x47425749u   /* "GBWI" */
#define WIFI_VERSION 1

void wifi_cfg_defaults(wifi_cfg_t *c) {
    memset(c, 0, sizeof *c);
    strcpy(c->celio, WIFI_CELIO_DEFAULT);
}

bool wifi_cfg_url_ok(const char *url) {
    const char *host;
    if (strncmp(url, "ws://", 5) == 0) {
        host = url + 5;
    } else if (strncmp(url, "wss://", 6) == 0) {
        host = url + 6;
    } else {
        return false;
    }
    if (*host == '\0' || *host == '/' || *host == ':') {
        return false;
    }
    for (const char *p = url; *p; p++) {
        if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7F) {
            return false;
        }
    }
    return strlen(url) <= WIFI_URL_MAX;
}

bool wifi_url_split(const char *url, bool *tls, char *host, size_t host_cap,
                    uint16_t *port, char *path, size_t path_cap) {
    if (!wifi_cfg_url_ok(url)) {
        return false;
    }
    *tls = url[2] == 's';
    const char *h = url + (*tls ? 6 : 5);
    const char *end = h + strcspn(h, "/?#");
    const char *colon = memchr(h, ':', (size_t)(end - h));
    if (h[0] == '[') {
        return false;   /* an IPv6 literal: the board's stack is IPv4 here */
    }
    const char *hend = colon ? colon : end;
    size_t hl = (size_t)(hend - h);
    if (hl == 0 || hl >= host_cap) {
        return false;
    }
    memcpy(host, h, hl);
    host[hl] = '\0';
    *port = *tls ? 443 : 80;
    if (colon) {
        unsigned long v = 0;
        const char *q = colon + 1;
        if (q == end) {
            return false;
        }
        for (; q < end; q++) {
            if (*q < '0' || *q > '9') return false;
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > 65535) return false;
        }
        if (v == 0) return false;
        *port = (uint16_t)v;
    }
    /* the URL's path, without a query or a trailing slash, then Socket.IO's */
    size_t pl = strcspn(end, "?#");
    while (pl > 0 && end[pl - 1] == '/') {
        pl--;
    }
    static const char sio[] = "/socket.io/?EIO=4&transport=websocket";
    if (pl + sizeof sio > path_cap) {
        return false;
    }
    memcpy(path, end, pl);
    memcpy(path + pl, sio, sizeof sio);
    return true;
}

bool wifi_cfg_pass_ok(const char *pass) {
    size_t n = strlen(pass);
    if (n == 0) {
        return true;
    }
    if (n == 64) {
        for (size_t i = 0; i < n; i++) {
            if (!isxdigit((unsigned char)pass[i])) {
                return false;
            }
        }
        return true;
    }
    if (n < 8 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)pass[i] < ' ' || (unsigned char)pass[i] > '~') {
            return false;
        }
    }
    return true;
}

/* A Celio session id: nanoid's alphabet, A-Z a-z 0-9 _ -. */
static bool session_ok(const char *s) {
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') {
            return false;
        }
    }
    return true;
}

typedef enum { K_NONE, K_SSID, K_PASS, K_CELIO, K_SESSION, K_AP_PASS, K_CLEAR } key_t_;

static key_t_ key_of(const char *k, size_t n) {
    static const struct { const char *name; key_t_ key; } keys[] = {
        { "ssid", K_SSID }, { "password", K_PASS }, { "celio", K_CELIO },
        { "session", K_SESSION }, { "hotspot_password", K_AP_PASS },
        { "clear_password", K_CLEAR },
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        size_t m = strlen(keys[i].name);
        if (m != n) {
            continue;
        }
        size_t j = 0;
        while (j < n && tolower((unsigned char)k[j]) == keys[i].name[j]) {
            j++;
        }
        if (j == n) {
            return keys[i].key;
        }
    }
    return K_NONE;
}

static void note_bad(wifi_parse_t *r, uint16_t line, bool unknown) {
    if (unknown) {
        r->unknown++;
    } else {
        r->rejected++;
    }
    if (!r->first_bad) {
        r->first_bad = line;
    }
}

/* Set one field from a decoded value. form: an empty password keeps it. */
static void apply(wifi_cfg_t *c, key_t_ k, const char *v, size_t n, bool form,
                  wifi_parse_t *r, uint16_t line) {
    char tmp[WIFI_URL_MAX + 1 > WIFI_PASS_MAX + 1 ? WIFI_URL_MAX + 1 : WIFI_PASS_MAX + 1];
    size_t cap;
    char *dst;
    switch (k) {
    case K_SSID:    dst = c->ssid;    cap = WIFI_SSID_MAX;    break;
    case K_PASS:    dst = c->pass;    cap = WIFI_PASS_MAX;    break;
    case K_CELIO:   dst = c->celio;   cap = WIFI_URL_MAX;     break;
    case K_SESSION: dst = c->session; cap = WIFI_SESSION_MAX; break;
    case K_AP_PASS: dst = c->ap_pass; cap = WIFI_PASS_MAX;    break;
    case K_CLEAR:
        if (n == 0 || (n == 1 && v[0] == '0')) {
            return;   /* an unticked box */
        }
        c->pass[0] = '\0';
        r->applied++;
        return;
    default:
        note_bad(r, line, true);
        return;
    }
    if (n > cap) {
        note_bad(r, line, false);
        return;
    }
    memcpy(tmp, v, n);
    tmp[n] = '\0';
    if (memchr(tmp, '\0', n)) {
        note_bad(r, line, false);   /* a %00 in a form */
        return;
    }
    bool ok = true;
    switch (k) {
    case K_PASS:
    case K_AP_PASS:
        if (form && n == 0) {
            return;   /* unchanged: the page does not echo passwords */
        }
        ok = wifi_cfg_pass_ok(tmp);
        break;
    case K_CELIO:
        if (n == 0) {
            strcpy(tmp, WIFI_CELIO_DEFAULT);   /* empty: back to the default */
        }
        ok = wifi_cfg_url_ok(tmp);
        break;
    case K_SESSION:
        ok = session_ok(tmp);
        break;
    default:
        break;
    }
    if (!ok) {
        note_bad(r, line, false);
        return;
    }
    strcpy(dst, tmp);
    r->applied++;
}

static bool blank(char ch) {
    return ch == ' ' || ch == '\t';
}

void wifi_cfg_parse_text(wifi_cfg_t *c, const char *text, size_t len, wifi_parse_t *res) {
    wifi_parse_t r = {0};
    size_t i = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        i = 3;
    }
    uint16_t line = 0;
    while (i < len) {
        line++;
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;   /* the '\n' */
        }
        if (end > start && text[end - 1] == '\r') {
            end--;
        }
        while (start < end && blank(text[start])) {
            start++;
        }
        if (start == end || text[start] == '#' || text[start] == ';') {
            continue;
        }
        const char *eq = memchr(text + start, '=', end - start);
        if (!eq) {
            note_bad(&r, line, true);
            continue;
        }
        size_t ke = (size_t)(eq - text);
        while (ke > start && blank(text[ke - 1])) {
            ke--;
        }
        size_t vs = (size_t)(eq - text) + 1, ve = end;
        while (vs < ve && blank(text[vs])) {
            vs++;
        }
        while (ve > vs && blank(text[ve - 1])) {
            ve--;
        }
        if (ve - vs >= 2 && text[vs] == '"' && text[ve - 1] == '"') {
            vs++;
            ve--;   /* quotes keep spaces at either end of a passphrase */
        }
        apply(c, key_of(text + start, ke - start), text + vs, ve - vs, false, &r, line);
    }
    if (res) {
        *res = r;
    }
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Decode one urlencoded component into out; false if malformed or too long. */
static bool url_decode(const char *s, size_t n, char *out, size_t cap, size_t *outn) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            if (i + 2 >= n) {
                return false;
            }
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            ch = (char)(hi << 4 | lo);
            i += 2;
        }
        if (o >= cap) {
            return false;
        }
        out[o++] = ch;
    }
    *outn = o;
    return true;
}

void wifi_cfg_parse_form(wifi_cfg_t *c, const char *body, size_t len, wifi_parse_t *res) {
    wifi_parse_t r = {0};
    size_t i = 0;
    uint16_t field = 0;
    while (i < len) {
        field++;
        size_t start = i;
        while (i < len && body[i] != '&') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;
        }
        if (start == end) {
            continue;
        }
        const char *eq = memchr(body + start, '=', end - start);
        size_t ke = eq ? (size_t)(eq - body) : end;
        size_t vs = eq ? ke + 1 : end;
        char key[24], val[WIFI_URL_MAX + 2];
        size_t kn, vn;
        if (!url_decode(body + start, ke - start, key, sizeof key, &kn)) {
            note_bad(&r, field, true);
            continue;
        }
        key_t_ k = key_of(key, kn);
        if (k == K_NONE) {
            note_bad(&r, field, true);
            continue;
        }
        /* One more than any field holds, so an overlong value is rejected
         * by apply() rather than silently cut. */
        if (!url_decode(body + vs, end - vs, val, sizeof val, &vn)) {
            note_bad(&r, field, false);
            continue;
        }
        apply(c, k, val, vn, true, &r, field);
    }
    if (res) {
        *res = r;
    }
}

/* ---- the stored blob ------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t wifi_cfg_pack(const wifi_cfg_t *c, uint8_t *buf, size_t cap) {
    const char *f[] = { c->ssid, c->pass, c->celio, c->session, c->ap_pass };
    size_t n = 5;
    for (size_t i = 0; i < 5; i++) {
        n += 1 + strlen(f[i]);
    }
    n += 4;
    if (cap < n || n > WIFI_CFG_BLOB_MAX) {
        return 0;
    }
    size_t o = 0;
    put32(buf, WIFI_MAGIC);
    buf[4] = WIFI_VERSION;
    o = 5;
    for (size_t i = 0; i < 5; i++) {
        size_t l = strlen(f[i]);
        buf[o++] = (uint8_t)l;
        memcpy(buf + o, f[i], l);
        o += l;
    }
    put32(buf + o, settings_crc32(buf, o));
    return o + 4;
}

bool wifi_cfg_unpack(wifi_cfg_t *c, const uint8_t *buf, size_t len) {
    wifi_cfg_defaults(c);
    if (len < 5 + 5 + 4 || get32(buf) != WIFI_MAGIC || buf[4] != WIFI_VERSION) {
        return false;
    }
    if (settings_crc32(buf, len - 4) != get32(buf + len - 4)) {
        return false;
    }
    wifi_cfg_t t;
    wifi_cfg_defaults(&t);
    char *f[] = { t.ssid, t.pass, t.celio, t.session, t.ap_pass };
    const size_t caps[] = { WIFI_SSID_MAX, WIFI_PASS_MAX, WIFI_URL_MAX, WIFI_SESSION_MAX,
                            WIFI_PASS_MAX };
    size_t o = 5;
    for (size_t i = 0; i < 5; i++) {
        if (o >= len - 4) {
            return false;
        }
        size_t l = buf[o++];
        if (l > caps[i] || o + l > len - 4) {
            return false;
        }
        memcpy(f[i], buf + o, l);
        f[i][l] = '\0';
        o += l;
    }
    if (o != len - 4 || !wifi_cfg_url_ok(t.celio)) {
        return false;
    }
    *c = t;
    return true;
}

size_t wifi_html_escape(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (cap == 0) {
        return 0;
    }
    for (; *s; s++) {
        const char *e;
        char one[2] = { *s, '\0' };
        switch (*s) {
        case '&':  e = "&amp;";  break;
        case '<':  e = "&lt;";   break;
        case '>':  e = "&gt;";   break;
        case '"':  e = "&quot;"; break;
        case '\'': e = "&#39;";  break;
        default:   e = one;      break;
        }
        size_t l = strlen(e);
        if (o + l >= cap) {
            break;
        }
        memcpy(out + o, e, l);
        o += l;
    }
    out[o] = '\0';
    return o;
}

/* ---- the setup page ------------------------------------------------------- */

typedef struct {
    char  *out;
    size_t cap, n;
    bool   over;
} html_t;

static void put(html_t *h, const char *s) {
    size_t l = strlen(s);
    if (h->over || h->n + l >= h->cap) {
        h->over = true;
        return;
    }
    memcpy(h->out + h->n, s, l);
    h->n += l;
    h->out[h->n] = '\0';
}

static void put_esc(html_t *h, const char *s) {
    char buf[WIFI_URL_MAX * 6 + 1];
    wifi_html_escape(buf, sizeof buf, s ? s : "");
    put(h, buf);
}

static void field(html_t *h, const char *label, const char *name, const char *type,
                  const char *value, const char *hint, int maxlen) {
    char num[16];
    snprintf(num, sizeof num, "%d", maxlen);
    put(h, "<label>");
    put(h, label);
    put(h, "<input name=\"");
    put(h, name);
    put(h, "\" type=\"");
    put(h, type);
    put(h, "\" maxlength=\"");
    put(h, num);
    put(h, "\" value=\"");
    put_esc(h, value);
    put(h, "\" placeholder=\"");
    put(h, hint);
    put(h, "\" autocomplete=\"off\"></label>\n");
}

size_t wifi_setup_html(char *out, size_t cap, const wifi_cfg_t *c, const wifi_page_t *pg) {
    html_t h = { out, cap, 0, false };
    if (cap) out[0] = '\0';
    put(&h,
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>gbagate setup</title><style>"
        ":root{color-scheme:light dark;--fg:#1a1a1a;--bg:#f6f6f3;--dim:#666;--acc:#c2410c}"
        "@media(prefers-color-scheme:dark){:root{--fg:#eee;--bg:#16171a;--dim:#999;--acc:#fb923c}}"
        "body{font:16px/1.4 system-ui,sans-serif;margin:0 auto;max-width:28rem;padding:1rem;"
        "color:var(--fg);background:var(--bg)}h1{font-size:1.3rem}"
        "label{display:block;margin:.8rem 0;color:var(--dim)}"
        "input{display:block;width:100%;box-sizing:border-box;font:inherit;padding:.5rem;"
        "margin-top:.2rem}button{font:inherit;padding:.6rem 1.2rem;background:var(--acc);"
        "color:#fff;border:0;border-radius:.3rem}p.s{padding:.6rem;border-left:3px solid var(--acc)}"
        "code{font-size:1.1rem;word-break:break-all}</style></head><body>"
        "<h1>gbagate setup</h1>\n");
    put(&h, "<p class=\"s\">");
    put_esc(&h, pg && pg->status ? pg->status : "");
    put(&h, "</p>\n");
    if (pg && pg->notice) {
        put(&h, "<p><b>");
        put_esc(&h, pg->notice);
        put(&h, "</b></p>\n");
    }
    if (pg && pg->session && *pg->session) {
        put(&h, "<p>Online session code, for your partner: <code>");
        put_esc(&h, pg->session);
        put(&h, "</code></p>\n");
    }
    put(&h, "<form method=\"post\" action=\"/save\">\n");
    field(&h, "WiFi network", "ssid", "text", c->ssid, "your network&#39;s name", WIFI_SSID_MAX);
    field(&h, c->pass[0] ? "Password (saved; empty keeps it)" : "Password", "password",
          "password", "", c->pass[0] ? "unchanged" : "none for an open network", WIFI_PASS_MAX);
    if (c->pass[0]) {
        put(&h, "<label><input type=\"checkbox\" name=\"clear_password\" value=\"1\" "
                "style=\"display:inline;width:auto\"> forget the saved password</label>\n");
    }
    field(&h, "Celio server", "celio", "url", c->celio, WIFI_CELIO_DEFAULT, WIFI_URL_MAX);
    field(&h, "Join a session (empty: start one)", "session", "text", c->session,
          "the code your partner sees", WIFI_SESSION_MAX);
    field(&h, c->ap_pass[0] ? "Hotspot password (saved; empty keeps it)" : "Hotspot password",
          "hotspot_password", "password", "", "8 or more characters; none: open", WIFI_PASS_MAX);
    put(&h, "<button>Save</button></form>\n"
            "<p style=\"color:var(--dim)\">The same settings can go in <code>wifi.txt</code> "
            "on the USB drive: one <code>key = value</code> per line.</p></body></html>\n");
    return h.over ? 0 : h.n;
}
