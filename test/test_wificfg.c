/*
 * test_wificfg.c -- wifi.txt, the setup page's form, and the stored blob.
 *
 * The file is typed by a person in whatever editor was to hand, so the cases
 * that matter are the untidy ones: CRLF, a BOM, spaces around '=', a typo'd
 * key, a password too short for WPA2. Each must either work or say which line
 * it did not like; none may leave a half-written field.
 */

#include <stdio.h>
#include <string.h>

#include "wifi_cfg.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-52s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("  FAIL %-52s got \"%s\", want \"%s\"\n", what, got, want);
        g_fail++;
    }
}

static void parse(wifi_cfg_t *c, const char *text, wifi_parse_t *r) {
    wifi_cfg_parse_text(c, text, strlen(text), r);
}

static void form(wifi_cfg_t *c, const char *body, wifi_parse_t *r) {
    wifi_cfg_parse_form(c, body, strlen(body), r);
}

static void test_defaults(void) {
    wifi_cfg_t c;
    wifi_cfg_defaults(&c);
    check_str("no network by default", c.ssid, "");
    check_str("Celio-Client's own server", c.celio, WIFI_CELIO_DEFAULT);
    check("the default server is a valid URL", wifi_cfg_url_ok(c.celio), 1);
}

static void test_text_file(void) {
    wifi_cfg_t c;
    wifi_parse_t r;
    wifi_cfg_defaults(&c);
    parse(&c, "\xEF\xBB\xBF# gbagate WiFi\r\n"
              "SSID = Home Network\r\n"
              "\r\n"
              "  password=hunter22  \r\n"
              "; a comment\r\n"
              "celio = ws://192.168.1.20:443\r\n"
              "session = V1StGXR8_Z5jdHi6B-myT", &r);
    check_str("BOM, CRLF, upper-case key: ssid", c.ssid, "Home Network");
    check_str("spaces trimmed around the value", c.pass, "hunter22");
    check_str("a local server", c.celio, "ws://192.168.1.20:443");
    check_str("last line without a newline", c.session, "V1StGXR8_Z5jdHi6B-myT");
    check("four keys taken", r.applied, 4);
    check("nothing wrong", r.unknown + r.rejected, 0);
    check("no bad line", r.first_bad, 0);

    /* only what the file names changes */
    parse(&c, "session=abc\n", &r);
    check_str("a partial file keeps the rest", c.ssid, "Home Network");
    check_str("... and sets what it names", c.session, "abc");

    /* quotes keep spaces a passphrase may start or end with */
    parse(&c, "password = \"  spaced pass \"\n", &r);
    check_str("quoted value kept whole", c.pass, "  spaced pass ");
}

static void test_text_mistakes(void) {
    wifi_cfg_t c;
    wifi_parse_t r;
    wifi_cfg_defaults(&c);
    parse(&c, "ssid=Net\npasword=hunter22\npassword=short\nceilo=x\n"
              "celio=http://example.com\nsession=has space\njust words\n", &r);
    check_str("the good line is taken", c.ssid, "Net");
    check_str("a short WPA2 password is refused", c.pass, "");
    check_str("an http:// server is refused", c.celio, WIFI_CELIO_DEFAULT);
    check_str("a session with a space is refused", c.session, "");
    check("typo'd keys and a line without '=' are unknown", r.unknown, 3);
    check("bad values are rejected", r.rejected, 3);
    check("the first bad line is 2", r.first_bad, 2);

    char ssid[64];
    memset(ssid, 'a', 33);
    ssid[33] = '\0';
    char line[80];
    snprintf(line, sizeof line, "ssid=%s\n", ssid);
    parse(&c, line, &r);
    check_str("a 33-byte SSID is refused, not cut", c.ssid, "Net");
    check("... and counted", r.rejected, 1);
    ssid[32] = '\0';
    snprintf(line, sizeof line, "ssid=%s\n", ssid);
    parse(&c, line, &r);
    check("32 bytes is the most an SSID holds", (long)strlen(c.ssid), 32);

    parse(&c, "celio=\n", &r);
    check_str("an empty server means the default", c.celio, WIFI_CELIO_DEFAULT);
    parse(&c, "", &r);
    check("an empty file changes nothing", r.applied + r.unknown + r.rejected, 0);
}

static void test_passwords(void) {
    check("open", wifi_cfg_pass_ok(""), 1);
    check("7 is too short", wifi_cfg_pass_ok("1234567"), 0);
    check("8 is enough", wifi_cfg_pass_ok("12345678"), 1);
    char p[70];
    memset(p, 'x', 63); p[63] = '\0';
    check("63 printable", wifi_cfg_pass_ok(p), 1);
    memset(p, 'x', 64); p[64] = '\0';
    check("64 must be hex", wifi_cfg_pass_ok(p), 0);
    memset(p, 'a', 64);
    check("64 hex digits is a raw key", wifi_cfg_pass_ok(p), 1);
    check("a control character", wifi_cfg_pass_ok("abc\tdefgh"), 0);
}

static void test_urls(void) {
    check("wss", wifi_cfg_url_ok("wss://celio.example"), 1);
    check("ws with port and path", wifi_cfg_url_ok("ws://10.0.0.2:8080/x"), 1);
    check("no host", wifi_cfg_url_ok("ws://"), 0);
    check("port only", wifi_cfg_url_ok("ws://:443"), 0);
    check("http", wifi_cfg_url_ok("http://x"), 0);
    check("a space", wifi_cfg_url_ok("ws://a b"), 0);
}

static void test_form(void) {
    wifi_cfg_t c;
    wifi_parse_t r;
    wifi_cfg_defaults(&c);
    form(&c, "ssid=Caf%C3%A9+Wi-Fi&password=p%40ss+word&celio=wss%3A%2F%2Fx.org&session=", &r);
    check_str("+ is a space, %XX a byte (UTF-8 kept)", c.ssid, "Caf\xC3\xA9 Wi-Fi");
    check_str("password decoded", c.pass, "p@ss word");
    check_str("server decoded", c.celio, "wss://x.org");
    check_str("an empty session: create one", c.session, "");
    check("four fields", r.applied, 4);

    form(&c, "ssid=Caf%C3%A9+Wi-Fi&password=&hotspot_password=", &r);
    check_str("an empty password box keeps the password", c.pass, "p@ss word");
    form(&c, "password=&clear_password=1", &r);
    check_str("clear_password empties it", c.pass, "");

    form(&c, "ssid=a%2", &r);
    check("a truncated escape is rejected", r.rejected, 1);
    check_str("... and leaves the SSID", c.ssid, "Caf\xC3\xA9 Wi-Fi");
    form(&c, "ssid=a%00b", &r);
    check("an embedded NUL is rejected", r.rejected, 1);
    form(&c, "bogus=1&&ssid=Z", &r);
    check("an unknown field is counted", r.unknown, 1);
    check_str("empty pairs skipped, the rest taken", c.ssid, "Z");
    char big[300] = "celio=wss://";
    memset(big + 12, 'a', 200);
    big[212] = '\0';
    form(&c, big, &r);
    check("an overlong value is rejected, not cut", r.rejected, 1);
    check_str("... and the server is kept", c.celio, "wss://x.org");
}

static void test_blob(void) {
    wifi_cfg_t a, b;
    wifi_cfg_defaults(&a);
    strcpy(a.ssid, "Net");
    strcpy(a.pass, "hunter22");
    strcpy(a.session, "abc");
    strcpy(a.ap_pass, "12345678");
    uint8_t buf[WIFI_CFG_BLOB_MAX];
    size_t n = wifi_cfg_pack(&a, buf, sizeof buf);
    check("packs", n > 0, 1);
    check("round trip", wifi_cfg_unpack(&b, buf, n), 1);
    check("... identical", memcmp(&a, &b, sizeof a), 0);

    /* every field at its longest still fits the blob */
    memset(a.ssid, 's', WIFI_SSID_MAX); a.ssid[WIFI_SSID_MAX] = '\0';
    memset(a.pass, 'a', WIFI_PASS_MAX); a.pass[WIFI_PASS_MAX] = '\0';
    strcpy(a.celio, "wss://");
    memset(a.celio + 6, 'c', WIFI_URL_MAX - 6); a.celio[WIFI_URL_MAX] = '\0';
    memset(a.session, 'x', WIFI_SESSION_MAX); a.session[WIFI_SESSION_MAX] = '\0';
    memset(a.ap_pass, 'b', WIFI_PASS_MAX); a.ap_pass[WIFI_PASS_MAX] = '\0';
    n = wifi_cfg_pack(&a, buf, sizeof buf);
    check("the longest config fits WIFI_CFG_BLOB_MAX", n > 0, 1);
    check("... and round-trips", wifi_cfg_unpack(&b, buf, n) && memcmp(&a, &b, sizeof a) == 0, 1);
    check("a short buffer packs nothing", wifi_cfg_pack(&a, buf, n - 1), 0);

    for (size_t i = 0; i < n; i++) {
        buf[i] ^= 0x01;
        if (wifi_cfg_unpack(&b, buf, n)) {
            printf("  FAIL a flipped bit at %zu was accepted\n", i);
            g_fail++;
        }
        buf[i] ^= 0x01;
    }
    check("every truncation refused", wifi_cfg_unpack(&b, buf, n - 1), 0);
    check("... back to defaults", strcmp(b.celio, WIFI_CELIO_DEFAULT), 0);
    check("nothing refused", wifi_cfg_unpack(&b, buf, 0), 0);
}

static void test_escape(void) {
    char out[64];
    wifi_html_escape(out, sizeof out, "<a href=\"x\">'&'</a>");
    check_str("every HTML special", out, "&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;");
    wifi_html_escape(out, 8, "ab&cdef");
    check_str("cut before an entity, never inside one", out, "ab&amp;");
    wifi_html_escape(out, 6, "ab&cdef");
    check_str("... even when it does not fit", out, "ab");
}

int main(void) {
    test_defaults();
    test_text_file();
    test_text_mistakes();
    test_passwords();
    test_urls();
    test_form();
    test_blob();
    test_escape();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
