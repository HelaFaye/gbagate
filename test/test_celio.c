/*
 * test_celio.c -- the on-board Celio client against a scripted server.
 *
 * The frames the server sends here are copied from what socket.io 4.8 and
 * Celio-Server (9f02d3e, and backport/Celio-Server's patched one) put on the
 * wire. test/gblink_compat/test_celio_device.mjs then runs the same code in
 * the simulator against the real server; this suite covers what that run
 * cannot provoke on demand -- reordering, duplicates, a lost socket.
 */

#include <stdio.h>
#include <string.h>

#include "celio.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-52s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

/* ---- a fake socket and adapter ------------------------------------------- */

static char     g_out[64][CELIO_FRAME_MAX];
static int      g_nout;
static uint8_t  g_cmd[32][4];
static int      g_ncmd;
static uint8_t  g_data[64][64];
static int      g_ndata;
static bool     g_socket_up = true;
static uint32_t g_rand = 1;

static bool ws_send(void *ctx, const char *t, size_t n) {
    (void)ctx;
    if (!g_socket_up || g_nout >= 64 || n >= CELIO_FRAME_MAX) return false;
    memcpy(g_out[g_nout], t, n);
    g_out[g_nout][n] = '\0';
    g_nout++;
    return true;
}

static void gbl_command(void *ctx, const uint8_t *c, size_t n) {
    (void)ctx;
    if (g_ncmd < 32) {
        memset(g_cmd[g_ncmd], 0xEE, 4);
        memcpy(g_cmd[g_ncmd], c, n > 4 ? 4 : n);
        g_ncmd++;
    }
}

static void gbl_data(void *ctx, const uint8_t d[64]) {
    (void)ctx;
    if (g_ndata < 64) memcpy(g_data[g_ndata++], d, 64);
}

static uint32_t rnd(void *ctx) {
    (void)ctx;
    g_rand = g_rand * 1103515245u + 12345u;
    return g_rand;
}

static const celio_io_t k_io = { ws_send, gbl_command, gbl_data, rnd, NULL };

static void reset(void) {
    g_nout = g_ncmd = g_ndata = 0;
    g_socket_up = true;
}

static void rx(celio_t *c, const char *t, uint32_t now) {
    celio_on_text(c, t, strlen(t), now);
}

static const char *last(void) {
    return g_nout ? g_out[g_nout - 1] : "";
}

static bool sent(const char *prefix) {
    for (int i = 0; i < g_nout; i++) {
        if (strncmp(g_out[i], prefix, strlen(prefix)) == 0) return true;
    }
    return false;
}

#define OPEN "0{\"sid\":\"Lbo5JLzTotvW3g2LAAAA\",\"upgrades\":[],\"pingInterval\":500,\"pingTimeout\":2000,\"maxPayload\":1000000}"

/* Up to a session: connect, create (or join), the server's reply. */
static void up(celio_t *c, const char *join, bool full) {
    reset();
    celio_start(c, &k_io, join, 0);
    celio_on_open(c, 1);
    rx(c, OPEN, 2);
    rx(c, "40{\"sid\":\"x\"}", 3);
    char r[160];
    snprintf(r, sizeof r, "430[{\"variant\":\"Ok\",\"value\":{\"id\":\"V1StGXR8_Z5jdHi6B-myT\",\"full\":%s}}]",
             full ? "true" : "false");
    rx(c, r, 4);
}

/* Through Cancel, SetMode and DeviceReady. */
static void link(celio_t *c, uint32_t t) {
    celio_tick(c, t + 499);
    celio_tick(c, t + 500);
    celio_on_status(c, 0xFF08, t + 600);
}

static void test_framing(void) {
    sio_packet_t p;
    check("an event", sio_parse("42[\"a\"]", 7, &p) && p.eio == '4' && p.sio == '2' &&
                      !p.has_id && p.json_len == 5, 1);
    check("an event with an ack id", sio_parse("4217[\"a\"]", 9, &p) && p.has_id && p.id == 17, 1);
    check("an ack", sio_parse("430[true]", 9, &p) && p.sio == '3' && p.id == 0, 1);
    check("the default namespace", sio_parse("42/,[1]", 7, &p) && p.json_len == 3, 1);
    check("another namespace", sio_parse("42/admin,[1]", 12, &p), 0);
    check("binary event", sio_parse("451-[\"a\"]", 9, &p), 0);
    check("ping", sio_parse("2", 1, &p) && p.eio == '2', 1);
    check("an ack id past 32 bits", sio_parse("424294967296[1]", 15, &p), 0);
    check("empty", sio_parse("", 0, &p), 0);
    check("a lone 4", sio_parse("4", 1, &p), 0);
}

static void test_create_and_wait(void) {
    celio_t c;
    reset();
    celio_start(&c, &k_io, NULL, 0);
    check("connecting", c.state, CELIO_CONNECTING);
    check("a v4 uuid for clientId", strlen(c.client_id) == 36 && c.client_id[14] == '4', 1);
    rx(&c, OPEN, 1);
    char want[80];
    snprintf(want, sizeof want, "40{\"clientId\":\"%s\"}", c.client_id);
    check("joins the namespace with Celio's auth", strcmp(last(), want), 0);
    rx(&c, "2", 2);
    check("ping, pong", strcmp(last(), "3"), 0);
    rx(&c, "40{\"sid\":\"x\"}", 3);
    check("creates a session, with an ack", strcmp(last(), "420[\"sessionCreate\",null]"), 0);
    check("entering", c.state, CELIO_ENTERING);
    rx(&c, "430[{\"variant\":\"Ok\",\"value\":{\"id\":\"V1StGXR8_Z5jdHi6B-myT\",\"full\":false}}]", 4);
    check("waits for a partner", c.state, CELIO_WAITING);
    check("shows the id", strcmp(c.session, "V1StGXR8_Z5jdHi6B-myT"), 0);
    check("nothing to the adapter yet", g_ncmd, 0);

    /* the patched server sends session events acked, with a uuid */
    rx(&c, "425[\"partnerJoined\",{\"uuid\":\"u1\"}]", 10);
    check("acks it", strcmp(last(), "435[true]"), 0);
    check("starting", c.state, CELIO_STARTING);
    check("Cancel first", g_ncmd == 1 && g_cmd[0][0] == 0x01, 1);
    celio_tick(&c, 509);
    check("no SetMode before 500 ms", g_ncmd, 1);
    celio_tick(&c, 510);
    check("then SetMode onlineLink", g_ncmd == 2 && g_cmd[1][0] == 0x00 && g_cmd[1][1] == 0x01, 1);
    int before = g_nout;
    celio_on_status(&c, 0xFF07, 520);   /* Cancel's own LinkClosed, say */
    check("statuses before DeviceReady stay local", g_nout, before);
    celio_on_status(&c, 0xFF08, 530);
    check("linked on DeviceReady", c.state, CELIO_LINKED);
    check("DeviceReady itself is not sent", g_nout, before);
    celio_on_status(&c, 0xFF02, 540);
    check("AwaitMode goes up", strncmp(last(), "42[\"deviceStatus\",{\"uuid\":\"", 26), 0);
    check("... as a number", strstr(last(), "\"linkStatus\":65282}]") != NULL, 1);
}

static void test_join(void) {
    celio_t c;
    reset();
    celio_start(&c, &k_io, "V1StGXR8_Z5jdHi6B-myT", 0);
    rx(&c, OPEN, 1);
    rx(&c, "40{\"sid\":\"x\"}", 2);
    check("joins by id", strcmp(last(), "420[\"sessionJoin\",\"V1StGXR8_Z5jdHi6B-myT\"]"), 0);
    rx(&c, "430[{\"variant\":\"Ok\",\"value\":{\"id\":\"V1StGXR8_Z5jdHi6B-myT\",\"full\":true}}]", 3);
    check("a joiner starts at once", c.state, CELIO_STARTING);

    up(&c, "nope", false);
    reset();
    celio_start(&c, &k_io, "nope", 0);
    rx(&c, OPEN, 1);
    rx(&c, "40{\"sid\":\"x\"}", 2);
    rx(&c, "430[{\"variant\":\"Err\",\"error\":\"Session not found\"}]", 3);
    check("a refusal fails", c.state, CELIO_FAILED);
    check("... with the server's words", strcmp(c.error, "Session not found"), 0);
    check("an ack for another id is ignored", (rx(&c, "431[{}]", 4), c.state), CELIO_FAILED);
}

static void test_commands_and_data(void) {
    celio_t c;
    up(&c, NULL, true);
    link(&c, 4);
    check("linked", c.state, CELIO_LINKED);
    int n = g_ncmd;
    rx(&c, "42[\"deviceCommand\",{\"uuid\":\"c1\",\"command\":16}]", 700);
    check("SetModeMaster to the adapter", g_ncmd == n + 1 && g_cmd[n][0] == 0x10, 1);
    rx(&c, "42[\"deviceCommand\",{\"uuid\":\"c1\",\"command\":16}]", 701);
    check("a duplicate uuid is dropped", g_ncmd, n + 1);
    check("... and counted", c.duplicates, 1);
    rx(&c, "42[\"deviceCommand\",{\"uuid\":\"c2\",\"command\":300}]", 702);
    check("a command past a byte is ignored", g_ncmd, n + 1);

    /* up: 64 bytes as 32 little-endian words */
    uint8_t d[64];
    for (int i = 0; i < 64; i++) d[i] = (uint8_t)i;
    celio_on_data(&c, d, 64, 710);
    check("data goes up with sequence 0",
          strncmp(last(), "42[\"deviceData\",{\"sequence\":0,\"data\":[256,770,", 46), 0);
    check("... 32 words, the last 0x3F3E", strstr(last(), ",16190]}]") != NULL, 1);
    celio_on_data(&c, d, 64, 711);
    check("then sequence 1", strstr(last(), "\"sequence\":1,") != NULL, 1);
    int before = g_nout;
    celio_on_data(&c, d, 63, 712);
    check("a short packet is not sent", g_nout, before);

    /* down: the public server sends one packet, acked */
    char f[400];
    int l = snprintf(f, sizeof f, "4212[\"deviceData\",{\"sequence\":0,\"data\":[");
    for (int i = 0; i < 32; i++) l += snprintf(f + l, sizeof f - (size_t)l, i ? ",%d" : "%d", 0xBEEF);
    snprintf(f + l, sizeof f - (size_t)l, "]}]");
    rx(&c, f, 720);
    check("acked", strcmp(last(), "4312[true]"), 0);
    check("one packet to the adapter", g_ndata, 1);
    check("... little-endian", g_data[0][0] == 0xEF && g_data[0][1] == 0xBE, 1);
}

/* A data frame for seq with every word = seq, as an object or a batch. */
static void data_frame(char *f, size_t cap, const uint32_t *seqs, int k) {
    int l = snprintf(f, cap, "42[\"deviceData\",%s", k > 1 ? "[" : "");
    for (int j = 0; j < k; j++) {
        l += snprintf(f + l, cap - (size_t)l, "%s{\"sequence\":%u,\"data\":[", j ? "," : "",
                      (unsigned)seqs[j]);
        for (int i = 0; i < 32; i++) {
            l += snprintf(f + l, cap - (size_t)l, i ? ",%u" : "%u", (unsigned)(seqs[j] & 0xFFFF));
        }
        l += snprintf(f + l, cap - (size_t)l, "]}");
    }
    snprintf(f + l, cap - (size_t)l, "%s]", k > 1 ? "]" : "");
}

static void rx_seq(celio_t *c, uint32_t seq) {
    static char f[8192];
    data_frame(f, sizeof f, &seq, 1);
    rx(c, f, 800);
}

static int got_seq(int i) {
    return g_data[i][0] | g_data[i][1] << 8;
}

static void test_ordering(void) {
    celio_t c;
    up(&c, NULL, true);
    link(&c, 4);
    rx_seq(&c, 1);
    rx_seq(&c, 2);
    check("held until 0 comes", g_ndata, 0);
    rx_seq(&c, 0);
    check("then all three", g_ndata, 3);
    check("in order", got_seq(0) == 0 && got_seq(1) == 1 && got_seq(2) == 2, 1);
    rx_seq(&c, 1);
    check("a late duplicate is dropped", g_ndata, 3);
    rx_seq(&c, 5);
    rx_seq(&c, 5);
    check("a held duplicate is dropped", c.duplicates, 2);

    /* 3 is lost for good: once 3 + CELIO_REORDER arrives it is skipped */
    for (uint32_t s = 4; s < 3 + CELIO_REORDER; s++) {
        if (s != 5) rx_seq(&c, s);
    }
    check("still waiting for 3", g_ndata, 3);
    rx_seq(&c, 3 + CELIO_REORDER);
    check("skipped: everything held, then the new one", g_ndata, 3 + CELIO_REORDER);
    check("... in order", got_seq(3) == 4 && got_seq(4) == 5 && got_seq(g_ndata - 1) == 3 + CELIO_REORDER, 1);
    check("one dropped", c.dropped, 1);

    /* the patched server batches: an array of packets */
    up(&c, NULL, true);
    link(&c, 4);
    static char f[8192];
    uint32_t seqs[3] = { 0, 2, 1 };
    data_frame(f, sizeof f, seqs, 3);
    rx(&c, f, 900);
    check("a batch, reordered", g_ndata == 3 && got_seq(2) == 2, 1);

    /* far ahead resets rather than looping */
    rx_seq(&c, 0x7FFFFFF0u);
    check("a far jump delivers at once", g_ndata, 4);
    check("... counted as lost", c.dropped >= 0x7FFFFFF0u - 3, 1);
    rx_seq(&c, 3);
    check("then the old range is old", g_ndata, 4);
}

static void test_session_end(void) {
    celio_t c;
    up(&c, NULL, true);
    link(&c, 4);
    int n = g_ncmd;
    rx(&c, "42[\"partnerLeft\"]", 700);
    check("partner left: back to waiting", c.state, CELIO_WAITING);
    check("... the adapter cancelled", g_ncmd == n + 1 && g_cmd[n][0] == 0x01, 1);
    rx(&c, "42[\"partnerJoined\"]", 710);
    check("a new partner starts again", c.state, CELIO_STARTING);
    link(&c, 710);
    rx(&c, "42[\"sessionClose\"]", 1500);
    check("session over", c.state, CELIO_CLOSED);
    int m = g_nout;
    celio_on_status(&c, 0xFF02, 1600);
    check("nothing goes up after", g_nout, m);
    celio_stop(&c);
    check("stop is idle", c.state, CELIO_IDLE);

    up(&c, NULL, false);
    celio_stop(&c);
    check("leaving a session says so", strcmp(last(), "42[\"sessionLeft\"]"), 0);
}

static void test_timeouts_and_reconnect(void) {
    celio_t c;
    reset();
    celio_start(&c, &k_io, NULL, 100);
    celio_tick(&c, 100 + 9999);
    check("still connecting", c.state, CELIO_CONNECTING);
    celio_tick(&c, 100 + 10000);
    check("no server", c.state, CELIO_FAILED);

    reset();
    celio_start(&c, &k_io, NULL, 0);
    rx(&c, OPEN, 1);
    rx(&c, "40{\"sid\":\"x\"}", 2);
    celio_tick(&c, 2 + 5000);
    check("no reply to create", c.state, CELIO_FAILED);

    up(&c, NULL, true);
    celio_tick(&c, 504);
    celio_tick(&c, 504 + 2500);
    check("no DeviceReady", c.state, CELIO_FAILED);
    check("... says so", strcmp(c.error, "adapter did not start"), 0);

    /* a dropped socket inside a session is ridden out */
    up(&c, NULL, true);
    link(&c, 4);
    celio_on_close(&c, 1000);
    celio_tick(&c, 1000 + CELIO_LOST_MS - 1);
    check("lost but still linked", c.state, CELIO_LINKED);
    celio_on_open(&c, 2000);
    rx(&c, OPEN, 2001);
    rx(&c, "40{\"sid\":\"y\"}", 2002);
    check("back without a new session", !sent("421"), 1);
    check("a reconnect counted", c.reconnects, 1);
    celio_tick(&c, 1000 + CELIO_LOST_MS + 10);
    check("still linked after", c.state, CELIO_LINKED);

    celio_on_close(&c, 20000);
    celio_tick(&c, 20000 + CELIO_LOST_MS);
    check("gone too long", c.state, CELIO_FAILED);
    check("... server lost", strcmp(c.error, "server lost"), 0);

    up(&c, NULL, false);
    reset();
    celio_start(&c, &k_io, NULL, 0);
    rx(&c, OPEN, 1);
    rx(&c, "40{\"sid\":\"x\"}", 2);
    celio_on_close(&c, 3);
    check("lost while entering fails", c.state, CELIO_FAILED);

    reset();
    celio_start(&c, &k_io, NULL, 0);
    rx(&c, OPEN, 1);
    rx(&c, "44{\"message\":\"no\"}", 2);
    check("connect_error fails", c.state, CELIO_FAILED);

    /* garbage never crashes, never changes state */
    up(&c, NULL, false);
    const char *junk[] = { "4", "42", "42[", "42{\"a\":1}", "42[1]", "42[\"deviceData\",{\"sequence\":-1}]",
                           "42[\"deviceData\",{\"sequence\":0,\"data\":[1,2]}]", "43", "430", "9",
                           "42[\"deviceCommand\"]", "42[\"deviceCommand\",{\"command\":\"16\"}]" };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) rx(&c, junk[i], 10);
    check("junk leaves it waiting", c.state, CELIO_WAITING);
    check("... and the adapter alone", g_ncmd + g_ndata, 0);
}

int main(void) {
    test_framing();
    test_create_and_wait();
    test_join();
    test_commands_and_data();
    test_ordering();
    test_session_end();
    test_timeouts_and_reconnect();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
