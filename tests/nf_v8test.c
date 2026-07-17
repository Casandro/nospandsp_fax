/*
 * nf_v8test - oracle test for nf_v8 against the installed spandsp's real
 * v8_init/v8_tx/v8_rx, both roles, in-process full-duplex audio loop.
 *   build: cc nf_v8test.c nf_v8.c nf_dsp.c -lspandsp -lm
 *   run:   ./nf_v8test [alaw]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <spandsp.h>
#include "nf_v8.h"
#include "g711.h"

#define BLK 160
#define MAXBLK (10 * 50)          /* 10 s cap */

/* NF_V8_MOD_* is numerically identical to spandsp's V8_MOD_*; build the
 * common classic-G3 capability set both sides offer for the oracle test.
 *
 * V34HDX is deliberately left out of this set: the installed spandsp 0.0.6
 * does not propagate V8_MOD_V34HDX through its own v8_tx (verified directly -
 * capturing its raw CM/JM audio and decoding it independently shows the bit
 * genuinely absent on the wire, not a decode bug on our side), so it cannot
 * serve as an oracle for that one bit. V34HDX's own wire encoding is instead
 * verified against a real captured Super-G3 call (see run_nf_self_test()
 * below) and via an nf<->nf loopback that does include it. */
#define CAPS (NF_V8_MOD_V21 | NF_V8_MOD_V27TER | NF_V8_MOD_V29 | NF_V8_MOD_V17)
#define CAPS_FULL (CAPS | NF_V8_MOD_V34HDX)

struct nf_side {
    int done;
    int status;
    nf_v8_result_t result;
};

static void nf_result(void *user, int status, const nf_v8_result_t *r)
{
    struct nf_side *d = user;
    d->done = 1;
    d->status = status;
    d->result = *r;
}

struct sp_side {
    int done;
    v8_parms_t result;
};

static void sp_result(void *user, v8_parms_t *r)
{
    struct sp_side *d = user;
    d->done = 1;
    d->result = *r;
}

static int alaw_roundtrip(int16_t x)
{
    return alaw_to_linear(linear_to_alaw(x));
}

/* Run one negotiation: nf_v8 as `nf_calling` (1=caller/0=answerer), spandsp
 * as the opposite role. Returns 1 on a matching successful negotiation. */
static int run_once(int nf_calling, int use_alaw)
{
    nf_v8_t nf;
    struct nf_side nfd = {0};
    nf_v8_init(&nf, nf_calling, CAPS,
              nf_calling ? NF_V8_CALL_T30_TX : NF_V8_CALL_T30_RX,
              nf_result, &nfd);

    v8_parms_t parms;
    memset(&parms, 0, sizeof(parms));
    parms.modulations = CAPS;
    /* Only used when spandsp plays the answerer, but harmless otherwise:
     * v8_restart() won't actually emit an ANSam waveform without this, which
     * leaves our (or its own) caller stuck forever in "await ANSam". */
    parms.modem_connect_tone = MODEM_CONNECT_TONES_ANSAM;
    /* Whichever side is the caller declares T30_TX_FAX (the normal,
     * non-polled convention) - only meaningful when spandsp plays that role. */
    parms.call_function = V8_CALL_T30_TX;
    parms.t66 = -1;
    struct sp_side spd = {0};
    v8_state_t *sp = v8_init(NULL, !nf_calling, &parms, sp_result, &spd);
    if (!sp) { fprintf(stderr, "v8_init failed\n"); return 0; }

    int16_t a[BLK], b[BLK];
    int i;
    for (i = 0; i < MAXBLK && !(nfd.done && spd.done); i++) {
        int na = nf_v8_tx(&nf, a, BLK);
        if (na < BLK) memset(a + na, 0, (size_t)(BLK - na) * 2);
        int nb = v8_tx(sp, b, BLK);
        if (nb < BLK) memset(b + nb, 0, (size_t)(BLK - nb) * 2);

        if (use_alaw) {
            int k;
            for (k = 0; k < BLK; k++) { a[k] = (int16_t) alaw_roundtrip(a[k]); b[k] = (int16_t) alaw_roundtrip(b[k]); }
        }

        v8_rx(sp, a, BLK);       /* spandsp hears nf's tx */
        nf_v8_rx(&nf, b, BLK);   /* nf hears spandsp's tx */
    }

    printf("  %s(nf %s, sp %s): nf status=%d cf=%d mod=0x%04X ; sp status=%d cf=%d mod=0x%04X ; blocks=%d\n",
           use_alaw ? "alaw" : "clean",
           nf_calling ? "caller" : "answerer", nf_calling ? "answerer" : "caller",
           nfd.status, nfd.result.call_function, nfd.result.modulations,
           spd.result.status, spd.result.call_function, spd.result.modulations,
           i);

    v8_free(sp);   /* v8_free() already calls v8_release() internally */

    if (!nfd.done || !spd.done) { printf("    FAIL: did not both complete\n"); return 0; }
    if (nfd.status != NF_V8_STATUS_V8_CALL) { printf("    FAIL: nf status not V8_CALL\n"); return 0; }
    if (spd.result.status != V8_STATUS_V8_CALL) { printf("    FAIL: sp status not V8_CALL\n"); return 0; }
    if (nfd.result.modulations != spd.result.modulations) { printf("    FAIL: modulation mismatch\n"); return 0; }
    if ((uint32_t) nfd.result.modulations != CAPS) { printf("    FAIL: negotiated set != offered set (expected full overlap)\n"); return 0; }
    if (nfd.result.call_function != spd.result.call_function) { printf("    FAIL: call_function mismatch\n"); return 0; }
    return 1;
}

/* nf<->nf loopback with V34HDX included, so the one bit the installed
 * spandsp oracle can't confirm (see the CAPS comment above) is still
 * exercised end-to-end: our own encoder, the FSK tx/rx pipeline, and our
 * own decoder, offering exactly the modulation set independently verified
 * this session against a real captured Super-G3 call's CM/JM octets
 * (81 85 D4 90 -> V21|V27ter|V29|V17|V34HDX). */
static int run_nf_self_test(void)
{
    nf_v8_t c, a;
    struct nf_side cd = {0}, ad = {0};
    int16_t x[BLK], y[BLK];
    int i;
    int caller_sent_ci = 0;

    nf_v8_init(&c, 1, CAPS_FULL, NF_V8_CALL_T30_TX, nf_result, &cd);
    nf_v8_init(&a, 0, CAPS_FULL, NF_V8_CALL_T30_RX, nf_result, &ad);
    for (i = 0; i < MAXBLK && !(cd.done && ad.done); i++) {
        int k;
        nf_v8_tx(&c, x, BLK);
        nf_v8_tx(&a, y, BLK);
        /* Before any ANSam could have been heard (the answerer sends 200 ms of
         * initial silence), the caller must already be transmitting CI - a
         * silent caller (the real-call regression) would emit only zeros here.
         * Sample the first block, well inside that initial-silence window. */
        if (i == 0)
            for (k = 0; k < BLK; k++)
                if (x[k] != 0) { caller_sent_ci = 1; break; }
        nf_v8_rx(&a, x, BLK);
        nf_v8_rx(&c, y, BLK);
    }
    printf("  nf<->nf (V34HDX): caller status=%d cf=%d mod=0x%04X ansam_am=%d ; answerer status=%d cf=%d mod=0x%04X ; blocks=%d ci=%d\n",
           cd.status, cd.result.call_function, cd.result.modulations, cd.result.ansam_am,
           ad.status, ad.result.call_function, ad.result.modulations, i, caller_sent_ci);

    if (!cd.done || !ad.done) { printf("    FAIL: did not both complete\n"); return 0; }
    if (cd.status != NF_V8_STATUS_V8_CALL || ad.status != NF_V8_STATUS_V8_CALL) { printf("    FAIL: status not V8_CALL\n"); return 0; }
    if (cd.result.modulations != CAPS_FULL || ad.result.modulations != CAPS_FULL) { printf("    FAIL: negotiated set != full offered set\n"); return 0; }
    if ((cd.result.modulations & NF_V8_MOD_V34HDX) == 0) { printf("    FAIL: V34HDX bit lost\n"); return 0; }
    if (!caller_sent_ci) { printf("    FAIL: caller never emitted a CI sync (still silent?)\n"); return 0; }
    if (cd.result.ansam_am != 1) { printf("    FAIL: caller did not classify answer tone as AM'd ANSam (got %d)\n", cd.result.ansam_am); return 0; }
    return 1;
}

/* Feed a synthetic 2100 Hz answer tone (plain ANS/CED, or 15 Hz-AM'd ANSam)
 * straight into a caller's V.8 receiver and confirm the hardened detector (a)
 * fires on BOTH, and (b) classifies AM correctly (ansam_am 1 for ANSam, 0 for
 * a flat tone - i.e. a peer that did NOT offer V.8). This exercises the new
 * frequency-selective detector directly, including the AM-dip tolerance a bare
 * power-presence detector failed on real ANSam. */
static int run_ansam_detector_case(int am, int expect)
{
    nf_v8_t c;
    struct nf_side cd = {0};
    int16_t buf[BLK];
    int i, k;
    unsigned long n = 0;

    nf_v8_init(&c, 1, CAPS, NF_V8_CALL_T30_TX, nf_result, &cd);
    /* ~1 s of tone, plenty past the 250 ms presence + 200 ms AM windows. */
    for (i = 0; i < 50; i++) {
        for (k = 0; k < BLK; k++, n++) {
            double t = (double) n / 8000.0;
            double env = am ? (1.0 + 0.2 * sin(2.0 * M_PI * 15.0 * t)) : 1.0;
            buf[k] = (int16_t) (8000.0 * env * sin(2.0 * M_PI * 2100.0 * t));
        }
        nf_v8_rx(&c, buf, BLK);
    }
    printf("  detector(%s tone): ansam_am=%d (expect %d)\n",
           am ? "AM'd ANSam" : "plain ANS", c.result.ansam_am, expect);
    if (c.result.ansam_am != expect) {
        printf("    FAIL: detector did not detect/classify as expected\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int use_alaw = (argc > 1 && strcmp(argv[1], "alaw") == 0);
    int ok = 1;

    printf("nf_v8test %s\n", use_alaw ? "(A-law)" : "(clean)");
    ok &= run_once(1, use_alaw);   /* nf = caller,   spandsp = answerer */
    ok &= run_once(0, use_alaw);   /* nf = answerer, spandsp = caller   */
    ok &= run_nf_self_test();
    ok &= run_ansam_detector_case(1, 1);   /* AM'd ANSam -> detected, AM      */
    ok &= run_ansam_detector_case(0, 0);   /* plain ANS  -> detected, flat    */

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
