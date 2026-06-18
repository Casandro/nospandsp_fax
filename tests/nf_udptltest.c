/* nf_udptltest - roundtrip + packet-loss test for the UDPTL transport.
 *
 * Builds a stream of distinct IFP "packets", carries them through
 * nf_udptl_build -> (optional drop) -> nf_udptl_rx, and checks that the
 * receiver delivers every packet exactly once, in order, recovering gaps from
 * redundancy when the loss burst is within the redundancy depth, and that a
 * loss burst longer than the depth leaves exactly those packets missing.
 */
#include "nf_udptl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define N 200

static int g_got[N];          /* delivery count per packet index (=seq) */
static int g_bad;             /* payload mismatch / out-of-range count  */
static int g_last_seq;        /* for in-order check                     */
static int g_order_bad;

static void mkpayload(int seq, uint8_t *buf, int *len)
{
    /* variable length 3..40, content derived from seq */
    int n = 3 + (seq % 38);
    for (int i = 0; i < n; i++) buf[i] = (uint8_t) (seq * 7 + i * 3 + 1);
    *len = n;
}

static void on_ifp(void *user, const uint8_t *ifp, int len, int seq)
{
    (void) user;
    if (seq < 0 || seq >= N) { g_bad++; return; }
    uint8_t exp[64]; int explen;
    mkpayload(seq, exp, &explen);
    if (len != explen || memcmp(ifp, exp, (size_t) len) != 0) g_bad++;
    g_got[seq]++;
    if (seq <= g_last_seq && g_got[seq] == 1) g_order_bad++;   /* not ascending */
    if (g_got[seq] == 1) g_last_seq = seq;
}

/* Run N packets with a drop predicate; return 0 if results match expectations. */
static int run(const char *name, int redundancy, int (*drop)(int seq),
               int (*expect_lost)(int seq))
{
    nf_udptl_t tx, rx;
    nf_udptl_init(&tx, redundancy, NF_UDPTL_MAXDGRAM);
    nf_udptl_init(&rx, redundancy, NF_UDPTL_MAXDGRAM);
    memset(g_got, 0, sizeof(g_got));
    g_bad = g_order_bad = 0; g_last_seq = -1;

    for (int seq = 0; seq < N; seq++) {
        uint8_t ifp[64]; int ilen;
        mkpayload(seq, ifp, &ilen);
        uint8_t dg[NF_UDPTL_MAXDGRAM];
        int dlen = nf_udptl_build(&tx, ifp, ilen, dg, sizeof(dg));
        if (dlen < 0) { printf("%-22s BUILD FAIL seq=%d\n", name, seq); return 1; }
        if (drop && drop(seq)) continue;                 /* datagram lost */
        if (nf_udptl_rx(&rx, dg, dlen, on_ifp, NULL) != 0) {
            printf("%-22s RX PARSE FAIL seq=%d\n", name, seq); return 1;
        }
    }

    int missing = 0, dup = 0, unexpected_loss = 0;
    for (int seq = 0; seq < N; seq++) {
        int lost = expect_lost ? expect_lost(seq) : 0;
        if (g_got[seq] > 1) dup++;
        if (!lost && g_got[seq] == 0) { missing++; }
        if (lost && g_got[seq] != 0) unexpected_loss++;   /* should have been lost */
    }
    int ok = (g_bad == 0 && g_order_bad == 0 && missing == 0 && dup == 0 && unexpected_loss == 0);
    printf("%-22s %s  (bad=%d order=%d missing=%d dup=%d unexploss=%d)\n",
           name, ok ? "PASS" : "FAIL",
           g_bad, g_order_bad, missing, dup, unexpected_loss);
    return ok ? 0 : 1;
}

static int drop_none(int s){ (void)s; return 0; }
/* drop isolated single datagrams (recoverable with redundancy>=1) */
static int drop_every5(int s){ return (s % 5) == 2 && s > 0; }
/* drop pairs (recoverable with redundancy>=2). Don't drop near the very end:
 * a loss at the tail has no later packet to carry it as a secondary. */
static int drop_pairs(int s){ return s < N - 3 && ((s % 7) == 3 || (s % 7) == 4); }
/* drop a burst of 5 in a row at seq 50..54 (redundancy 3 -> 50,51 unrecoverable) */
static int drop_burst(int s){ return s >= 50 && s <= 54; }
static int lost_burst(int s){ return s >= 50 && s <= 51; }   /* beyond depth-3 reach */

int main(void)
{
    int rc = 0;
    rc |= run("clean",            3, drop_none,  NULL);
    rc |= run("single-loss/red3", 3, drop_every5, NULL);
    rc |= run("pair-loss/red3",   3, drop_pairs,  NULL);
    rc |= run("burst5/red3",      3, drop_burst,  lost_burst);
    rc |= run("single-loss/red1", 1, drop_every5, NULL);
    printf(rc ? "UDPTL TEST FAIL\n" : "ALL PASS\n");
    return rc;
}
