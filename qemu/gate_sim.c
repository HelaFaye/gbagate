/*
 * gate_sim.c -- the firmware's gate, as a host process.
 *
 * Same chipgate.c and link_proto.c the firmware compiles, minus ESP-IDF. It
 * listens where the QEMU firmware's UART would be, so the rest of the chain --
 * ROM, mGBA, Lua, broker -- can be exercised end to end before ESP-IDF or an
 * emulator is involved.
 *
 * If the ROM turns green against this and red against QEMU, the difference is
 * the ESP32 build or the emulator, not the protocol.
 *
 *   cc -std=c11 -Isrc -o gate_sim qemu/gate_sim.c src/chipgate.c src/link_proto.c
 *   ./gate_sim 5556 bn6
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chipgate.h"
#include "link_proto.h"

static cg_game_t game_from_name(const char *s) {
    if (!strcmp(s, "bn4"))   return CG_GAME_BN4;
    if (!strcmp(s, "bn5"))   return CG_GAME_BN5;
    if (!strcmp(s, "bn6jp")) return CG_GAME_BN6_JP;
    return CG_GAME_BN6_US;
}

static void send_pkt(int fd, const lp_packet_t *p) {
    uint8_t buf[LP_MAX_FRAMED];
    size_t n = lp_encode(p, buf, sizeof buf);
    if (n) {
        ssize_t unused = write(fd, buf, n);
        (void)unused;
    }
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 5556;
    const char *game = (argc > 2) ? argv[2] : "bn6";

    chipgate_t gate;
    chipgate_init(&gate, game_from_name(game));
    fprintf(stderr, "gate_sim: %s, id %04X, listening on :%d\n",
            chipgate_game_name(game_from_name(game)), gate.gate_id, port);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = INADDR_ANY };
    if (bind(srv, (struct sockaddr *)&a, sizeof a) || listen(srv, 1)) {
        perror("bind/listen");
        return 1;
    }

    int fd = accept(srv, NULL, NULL);
    fprintf(stderr, "gate_sim: broker connected\n");

    lp_rx_t rx;
    lp_rx_init(&rx);

    uint8_t buf[512];
    ssize_t n;
    unsigned transfers = 0;

    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            lp_packet_t in;
            if (!lp_rx_feed(&rx, buf[i], &in)) {
                continue;
            }

            if (in.type == LP_HELLO) {
                lp_packet_t ack = { .type = LP_HELLO_ACK, .seq = in.seq };
                ack.u.hello.version = LP_VERSION;
                ack.u.hello.role    = GBA_ROLE_SLAVE;
                ack.u.hello.players = 2;
                send_pkt(fd, &ack);
                continue;
            }

            if (in.type == LP_PING) {
                lp_packet_t pong = { .type = LP_PONG, .seq = in.seq };
                pong.u.ping.stamp_us = in.u.ping.stamp_us;
                send_pkt(fd, &pong);
                continue;
            }

            if (in.type != LP_WORD) {
                continue;
            }

            /* This is the firmware's whole gate loop. */
            uint16_t answer = chipgate_step(&gate, in.u.word.word);
            transfers++;

            lp_packet_t out = { .type = LP_RESULT, .seq = in.seq };
            out.u.result.word[0] = in.u.word.word;  /* the game, as parent */
            out.u.result.word[1] = answer;          /* us, player 2        */
            out.u.result.word[2] = GBA_WORD_EMPTY;
            out.u.result.word[3] = GBA_WORD_EMPTY;
            out.u.result.players = 2;
            out.u.result.self_id = 1;
            send_pkt(fd, &out);
        }
    }

    fprintf(stderr, "gate_sim: disconnected after %u transfers, state %s\n",
            transfers, chipgate_state_name(gate.state));
    close(fd);
    close(srv);
    return 0;
}
