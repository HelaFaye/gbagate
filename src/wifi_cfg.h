/*
 * wifi_cfg.h -- the network the board joins, and the Celio server it plays on.
 *
 * Two ways in, one result:
 *
 *   wifi.txt   a text file on the USB drive, read at boot. Easiest when a PC
 *              is at hand: plug in, edit, eject.
 *
 *                  # gbagate WiFi
 *                  ssid     = Home Network
 *                  password = hunter22
 *                  celio    = wss://celio-server.up.railway.app
 *                  session  = V1StGXR8_Z5jdHi6B-myT
 *
 *   setup page a hotspot the board raises when it has no network (or on
 *              request), with a form at http://192.168.4.1/. For a phone.
 *
 * Both parse here, into a wifi_cfg_t, which is stored as its own blob (not in
 * settings_t: it is bigger, and a password has no business in the settings
 * the UI draws). Storage is injected, as for settings.
 */

#ifndef WIFI_CFG_H
#define WIFI_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX     32   /* 802.11: up to 32 octets                  */
#define WIFI_PASS_MAX     64   /* WPA2: 8-63 characters, or 64 hex digits  */
#define WIFI_URL_MAX      96
#define WIFI_SESSION_MAX  32   /* Celio's ids are nanoid()s: 21 characters */

/* Celio-Client's own production server (its environment.ts). */
#define WIFI_CELIO_DEFAULT "wss://celio-server.up.railway.app:443"

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];      /* empty: no network, hotspot only   */
    char pass[WIFI_PASS_MAX + 1];      /* empty: an open network            */
    char celio[WIFI_URL_MAX + 1];      /* ws:// or wss://, never empty      */
    char session[WIFI_SESSION_MAX + 1];/* Celio session to join; empty: create one */
    char ap_pass[WIFI_PASS_MAX + 1];   /* the setup hotspot's; empty: open  */
} wifi_cfg_t;

void wifi_cfg_defaults(wifi_cfg_t *c);

/* What a parse found, for the screen and the log. */
typedef struct {
    uint8_t  applied;      /* keys taken                                     */
    uint8_t  unknown;      /* keys it does not know (a typo, usually)        */
    uint8_t  rejected;     /* known keys whose value was too long or invalid */
    uint16_t first_bad;    /* 1-based line of the first unknown or rejected, 0 if none */
} wifi_parse_t;

/*
 * wifi.txt: "key = value" lines, '#' or ';' comments, blank lines, CRLF or
 * LF, an optional UTF-8 BOM, optional double quotes around a value. Keys are
 * ssid, password, celio, session and hotspot_password, in any case. A key
 * absent from the file keeps its value in *c, so a file with only a session
 * line changes only the session. An invalid value leaves the field as it was.
 */
void wifi_cfg_parse_text(wifi_cfg_t *c, const char *text, size_t len, wifi_parse_t *res);

/*
 * The setup page's POST body (application/x-www-form-urlencoded), the same
 * keys. An empty password or hotspot_password keeps the stored one -- the
 * page never echoes a password back, so an empty box means "unchanged"; the
 * clear_password field, if present, empties it.
 */
void wifi_cfg_parse_form(wifi_cfg_t *c, const char *body, size_t len, wifi_parse_t *res);

/* Is this a URL the Celio client can open: ws:// or wss://, a host, and no
 * spaces? */
bool wifi_cfg_url_ok(const char *url);

/*
 * Split a Celio URL into what a socket needs: TLS or not, host, port
 * (default 80 / 443), and the path for Socket.IO's WebSocket endpoint --
 * the URL's own path with "/socket.io/?EIO=4&transport=websocket" after it,
 * as socket.io-client builds it. False if the URL is not wifi_cfg_url_ok()
 * or a part does not fit.
 */
bool wifi_url_split(const char *url, bool *tls, char *host, size_t host_cap,
                    uint16_t *port, char *path, size_t path_cap);

/* A WPA2 passphrase: empty (open), 8-63 printable characters, or 64 hex. */
bool wifi_cfg_pass_ok(const char *pass);

/* Blob: magic, version, the five strings, CRC. Returns bytes, 0 if cap is short. */
#define WIFI_CFG_BLOB_MAX 320
size_t wifi_cfg_pack(const wifi_cfg_t *c, uint8_t *buf, size_t cap);
/* False, and *c at defaults, on a short, corrupt or foreign blob. */
bool   wifi_cfg_unpack(wifi_cfg_t *c, const uint8_t *buf, size_t len);

/*
 * Write s into out as HTML text or attribute content (& < > " ' escaped),
 * for the setup page. Returns the length written; out is always terminated
 * and truncated whole-entity, never mid-escape.
 */
size_t wifi_html_escape(char *out, size_t cap, const char *s);

/* ---- the setup page ------------------------------------------------------- */

typedef struct {
    const char *status;    /* one line: "on Home Network at 192.168.1.7"     */
    const char *notice;    /* after a save: what happened, or NULL           */
    const char *session;   /* the Celio session the board is in, or NULL     */
} wifi_page_t;

/*
 * The whole page, GET / and the reply to POST /save: what the board is doing,
 * and a form for the network, the server and a session to join. Passwords
 * are never written into it (the boxes say "unchanged if empty"). Every
 * string is HTML-escaped. Returns the length, or 0 if cap is too small.
 */
#define WIFI_PAGE_MAX 4096
size_t wifi_setup_html(char *out, size_t cap, const wifi_cfg_t *c, const wifi_page_t *pg);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CFG_H */
