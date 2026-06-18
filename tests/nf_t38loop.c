/*
 * T.38 full-duplex loopback: a caller (sender) and answerer (receiver) nf_t30
 * engine, each on the T.38 backend, exchanging UDPTL datagrams in-process. Runs
 * a complete fax over T.38 (no audio modems). Optionally drops datagrams to
 * exercise UDPTL redundancy recovery.
 *
 *   run: ./nf_t38loop <in.tiff> <out.tiff> [noecm] [drop=N] [verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nf_t30.h"
#include "nf_udptl.h"

#define TICK_MS 30
#define QCAP    16

struct inbox {
    uint8_t buf[QCAP][NF_UDPTL_MAXDGRAM];
    int len[QCAP];
    int n;
};

static struct inbox to_a, to_b;     /* datagrams destined for A / B */
static int g_dropN;                  /* drop every Nth datagram (0 = none) */
static long g_txcount;               /* global datagram counter for the drop pattern */

static void push(struct inbox *q, const uint8_t *d, int len)
{
    g_txcount++;
    if (g_dropN && (g_txcount % g_dropN) == 0) return;   /* simulate loss */
    if (q->n >= QCAP) return;                            /* overflow: drop (test cap) */
    memcpy(q->buf[q->n], d, (size_t) len);
    q->len[q->n] = len;
    q->n++;
}

static void send_to_b(void *u, const uint8_t *d, int len) { (void) u; push(&to_b, d, len); }
static void send_to_a(void *u, const uint8_t *d, int len) { (void) u; push(&to_a, d, len); }

struct done { int done, result; };
static void phe(void *u, int r) { struct done *d = u; d->done = 1; d->result = r; }

static void drain(struct inbox *q, nf_t30_t *eng)
{
    for (int i = 0; i < q->n; i++) nf_t30_t38_rx_datagram(eng, q->buf[i], q->len[i]);
    q->n = 0;
}

int main(int argc, char **argv)
{
    const char *infile  = argc > 1 ? argv[1] : "doc.pam";
    const char *outfile = argc > 2 ? argv[2] : "t38_out.tiff";
    int noecm = 0, verbose = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "noecm") == 0) noecm = 1;
        else if (strncmp(argv[i], "drop=", 5) == 0) g_dropN = atoi(argv[i] + 5);
        else if (strcmp(argv[i], "verbose") == 0) verbose = 1;
    }

    struct done da = {0,0}, db = {0,0};
    nf_t30_t *ca = nf_t30_init(1);   /* caller / sender   */
    nf_t30_t *cb = nf_t30_init(0);   /* answerer / receiver */
    int allres = NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400;
    nf_t30_set_supported_resolutions(ca, allres);
    nf_t30_set_supported_resolutions(cb, allres);
    nf_t30_set_tx_file(ca, infile);
    nf_t30_set_rx_file(cb, outfile);
    nf_t30_set_phase_e_handler(ca, phe, &da);
    nf_t30_set_phase_e_handler(cb, phe, &db);
    if (noecm) { nf_t30_set_ecm(ca, 0); nf_t30_set_ecm(cb, 0); }
    if (verbose) { nf_t30_set_verbose(ca, 1); nf_t30_set_verbose(cb, 1); }

    /* Switch both engines to the T.38 backend, cross-wiring their transports. */
    nf_t30_t38_enable(ca, 2, NF_UDPTL_MAXDGRAM, send_to_b, NULL);
    nf_t30_t38_enable(cb, 2, NF_UDPTL_MAXDGRAM, send_to_a, NULL);

    int i, maxtick = 90 * 1000 / TICK_MS;        /* ~90 s cap */
    for (i = 0; i < maxtick && !(da.done && db.done); i++) {
        nf_t30_t38_pump(ca, TICK_MS);
        nf_t30_t38_pump(cb, TICK_MS);
        drain(&to_a, ca);
        drain(&to_b, cb);
    }

    nf_t30_stats_t sa, sb;
    nf_t30_get_stats(ca, &sa); nf_t30_get_stats(cb, &sb);
    printf("caller done=%d res=%d ; answerer done=%d res=%d ; pages_rx=%d %dx%d %ddpi ; ticks=%d (%.1fs)%s\n",
           da.done, da.result, db.done, db.result, sb.pages_rx, sb.width, sb.length, sb.y_resolution,
           i, i * (TICK_MS / 1000.0), g_dropN ? " [with loss]" : "");
    int ok = da.done && db.done && da.result == 0 && db.result == 0 && sb.pages_rx >= 1;
    nf_t30_free(ca); nf_t30_free(cb);
    printf("%s\n", ok ? "T38 CALL OK" : "T38 CALL FAIL");
    return ok ? 0 : 1;
}
