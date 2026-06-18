/*
 * nf_modemtest - per-module oracle harness for the nf modem layer.
 *
 * Each subcommand pits one nf module against the corresponding real spandsp
 * implementation (the oracle) over an in-process audio loop (the nf_interop.c
 * pattern: int16 mono 8 kHz pumped in 160-sample blocks), optionally through
 * inline line impairments (AWGN at a given SNR, a G.711 A-law round-trip,
 * flat gain). Everything is deterministic: fixed PRNG seeds, no wall clock.
 *
 * Usage:
 *   nf_modemtest tones                      nf CED/CNG -> spandsp tone rx
 *   ... (subcommands grow with the migration stages)
 *
 * Exit status 0 = pass.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <spandsp.h>

#include "nf_dsp.h"
#include "nf_hdlc.h"
#include "g711.h"

#define BLK 160

/* ── inline channel impairments ────────────────────────────────────── */

#define HILB_TAPS 63
#define HILB_HALF (HILB_TAPS / 2)

typedef struct {
    int use_alaw;
    double snr_db;          /* <= 0: off */
    double gain_db;
    double foff_hz;         /* carrier frequency offset (SSB shift) */
    uint32_t prng;
    /* frequency shifter state */
    double dline[HILB_TAPS];
    int dpos;
    double ph;
} channel_t;

static double chan_rand_gauss(channel_t *c)
{
    /* Box-Muller on a xorshift PRNG; deterministic across runs */
    uint32_t *s = &c->prng;
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    double u1 = (*s & 0xFFFFFF) / 16777216.0 + 1e-12;
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    double u2 = (*s & 0xFFFFFF) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* windowed FIR Hilbert transformer coefficient */
static double hilb_coeff(int k)
{
    int n = k - HILB_HALF;
    if ((n & 1) == 0)
        return 0.0;
    double w = 0.54 - 0.46 * cos(2.0 * M_PI * k / (HILB_TAPS - 1));
    return w * 2.0 / (M_PI * n);
}

/* single-sideband frequency shift: y = x*cos(wt) - H{x}*sin(wt) */
static double chan_freq_shift(channel_t *c, double x)
{
    c->dline[c->dpos] = x;
    double xh = 0.0;
    for (int k = 0; k < HILB_TAPS; k++) {
        int idx = c->dpos - k;
        if (idx < 0) idx += HILB_TAPS;
        xh += c->dline[idx] * hilb_coeff(k);
    }
    int mid = c->dpos - HILB_HALF;
    if (mid < 0) mid += HILB_TAPS;
    double xd = c->dline[mid];
    if (++c->dpos >= HILB_TAPS) c->dpos = 0;
    c->ph += 2.0 * M_PI * c->foff_hz / 8000.0;
    if (c->ph > 2.0 * M_PI) c->ph -= 2.0 * M_PI;
    return xd * cos(c->ph) - xh * sin(c->ph);
}

static void chan_apply(channel_t *c, int16_t *amp, int len)
{
    double gain = pow(10.0, c->gain_db / 20.0);
    /* noise level for the requested SNR, referenced to the -14 dBm0 fax tx
     * level all the modems here use */
    double nrms = 0.0;
    if (c->snr_db > 0.0) {
        double sig_rms = 32767.0 * pow(10.0, (-14.0 - 3.14) / 20.0) / sqrt(2.0);
        nrms = sig_rms / pow(10.0, c->snr_db / 20.0);
    }
    for (int i = 0; i < len; i++) {
        double v = amp[i] * gain;
        if (c->foff_hz != 0.0)
            v = chan_freq_shift(c, v);
        if (nrms > 0.0)
            v += chan_rand_gauss(c) * nrms;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        int16_t s = (int16_t) lrint(v);
        if (c->use_alaw)
            s = alaw_to_linear(linear_to_alaw(s));
        amp[i] = s;
    }
}

static channel_t chan_parse(int argc, char **argv, int first)
{
    channel_t c;
    memset(&c, 0, sizeof c);
    c.prng = 0x12345678u;
    for (int i = first; i < argc; i++) {
        if (!strcmp(argv[i], "--alaw")) c.use_alaw = 1;
        else if (!strcmp(argv[i], "--snr") && i + 1 < argc) c.snr_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) c.gain_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--foff") && i + 1 < argc) c.foff_hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) c.prng = (uint32_t) strtoul(argv[++i], NULL, 0);
        else { fprintf(stderr, "unknown channel arg %s\n", argv[i]); exit(2); }
    }
    return c;
}

/* ── tones: nf CED/CNG tx -> spandsp modem_connect_tones_rx ────────── */

/* nf_fax.c owns the production tone generators; to test exactly that code
 * without exporting private symbols, drive a minimal nf_fax instance. */
#include "nf_fax.h"

struct tone_result {
    int hits;
    int tone;
};

static void tone_report(void *user, int tone, int level, int delay)
{
    struct tone_result *r = user;
    (void) level; (void) delay;
    if (tone != MODEM_CONNECT_TONES_NONE) {
        r->tone = tone;
        r->hits++;
    }
}

static int test_one_tone(int nf_modem, int want_tone, const char *name)
{
    nf_fax_iface_t iface; memset(&iface, 0, sizeof iface);
    nf_fax_t *fx = nf_fax_init(1, &iface);
    nf_fax_set_tx_type(fx, nf_modem, 0, 0, 0);

    struct tone_result res = { 0, -1 };
    modem_connect_tones_rx_state_t *rx =
        modem_connect_tones_rx_init(NULL,
            want_tone == MODEM_CONNECT_TONES_FAX_CNG ? MODEM_CONNECT_TONES_FAX_CNG
                                                     : MODEM_CONNECT_TONES_FAX_CED,
            tone_report, &res);

    int16_t amp[BLK];
    long total = 0, energy_samples = 0;
    double energy = 0.0;
    for (int blk = 0; blk < 5 * 50; blk++) {        /* up to 5 s */
        int n = nf_fax_tx(fx, amp, BLK);
        if (n < BLK)
            memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
        /* level: average power over blocks that are fully inside the tone */
        double bpow = 0.0;
        for (int i = 0; i < BLK; i++)
            bpow += (double) amp[i] * amp[i];
        if (bpow / BLK > 32767.0 * 32767.0 * 1e-4) {   /* > ~-34 dBm0 */
            energy += bpow;
            energy_samples += BLK;
        }
        modem_connect_tones_rx(rx, amp, BLK);
        total += BLK;
        if (res.hits && want_tone != MODEM_CONNECT_TONES_FAX_CNG)
            break;
    }
    int ok = res.hits > 0 && res.tone == want_tone;
    /* dBm0 of the tone: 10log10(avg_power / fullscale^2) + DBM0_MAX_POWER */
    double dbm0 = energy_samples
        ? 10.0 * log10(energy / energy_samples / (32767.0 * 32767.0)) + 6.16
        : -99.0;
    printf("tones %-4s: detected=%s level=%.1f dBm0 (want -11+-1) after %.1fs\n",
           name, ok ? "yes" : "NO", dbm0, (double) total / 8000.0);
    if (ok && (dbm0 < -12.0 || dbm0 > -10.0)) ok = 0;
    modem_connect_tones_rx_free(rx);
    nf_fax_free(fx);
    return ok ? 0 : 1;
}

static int cmd_tones(void)
{
    int rc = 0;
    rc |= test_one_tone(NF_MODEM_CED, MODEM_CONNECT_TONES_FAX_CED, "CED");
    rc |= test_one_tone(NF_MODEM_CNG, MODEM_CONNECT_TONES_FAX_CNG, "CNG");
    return rc;
}

/* ── hdlc: nf_hdlc <-> spandsp hdlc, pure bit level ────────────────── */

#define HDLC_NFRAMES 8

typedef struct {
    /* expected frames (sent), received bookkeeping */
    uint8_t frames[HDLC_NFRAMES][300];
    int     flen[HDLC_NFRAMES];
    int     nframes;
    int     next_tx;            /* next frame the underflow handler queues */
    int     rx_ok, rx_bad, rx_match;
    int     stream;             /* queue next frame from underflow */
    void   *tx;                 /* nf_hdlc_tx_t* or spandsp hdlc_tx_state_t* */
    int     tx_is_nf;
} hdlc_ctx_t;

static uint32_t hdlc_prng = 0x2468ACE1u;
static uint32_t hdlc_rand(void)
{
    hdlc_prng ^= hdlc_prng << 13; hdlc_prng ^= hdlc_prng >> 17; hdlc_prng ^= hdlc_prng << 5;
    return hdlc_prng;
}

static void hdlc_make_frames(hdlc_ctx_t *c, int n)
{
    c->nframes = n;
    for (int i = 0; i < n; i++) {
        c->flen[i] = 3 + (int) (hdlc_rand() % 258);    /* 3..260 bytes */
        for (int j = 0; j < c->flen[i]; j++)
            c->frames[i][j] = (uint8_t) hdlc_rand();
    }
}

static void hdlc_queue_next(hdlc_ctx_t *c)
{
    if (c->next_tx < c->nframes) {
        if (c->tx_is_nf)
            nf_hdlc_tx_frame(c->tx, c->frames[c->next_tx], c->flen[c->next_tx]);
        else
            hdlc_tx_frame(c->tx, c->frames[c->next_tx], (size_t) c->flen[c->next_tx]);
        c->next_tx++;
    } else {
        if (c->tx_is_nf)
            nf_hdlc_tx_frame(c->tx, NULL, 0);
        else
            hdlc_tx_frame(c->tx, (const uint8_t *) "", 0);
    }
}

static void hdlc_on_underflow(void *user)
{
    hdlc_ctx_t *c = user;
    if (c->stream)
        hdlc_queue_next(c);
}

static void hdlc_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    hdlc_ctx_t *c = user;
    if (len < 0)
        return;                  /* status */
    if (!ok) {
        c->rx_bad++;
        return;
    }
    c->rx_ok++;
    /* match against the expected sequence */
    int idx = c->rx_match;
    if (idx < c->nframes && len == c->flen[idx]
        && memcmp(msg, c->frames[idx], (size_t) len) == 0)
        c->rx_match++;
}

/* one direction, streaming all frames back-to-back via underflow */
static int hdlc_run_dir(int nf2sp, int corrupt_bit)
{
    hdlc_ctx_t c; memset(&c, 0, sizeof c);
    c.stream = 1;
    hdlc_make_frames(&c, HDLC_NFRAMES);

    nf_hdlc_tx_t nftx;
    hdlc_tx_state_t *sptx = NULL;
    nf_hdlc_rx_t nfrx;
    hdlc_rx_state_t *sprx = NULL;

    if (nf2sp) {
        nf_hdlc_tx_init(&nftx, 2, hdlc_on_underflow, &c);
        c.tx = &nftx; c.tx_is_nf = 1;
        nf_hdlc_tx_flags(&nftx, 32);
        sprx = hdlc_rx_init(NULL, FALSE, TRUE, 5, hdlc_on_frame, &c);
    } else {
        sptx = hdlc_tx_init(NULL, FALSE, 2, FALSE, hdlc_on_underflow, &c);
        c.tx = sptx; c.tx_is_nf = 0;
        hdlc_tx_flags(sptx, 32);
        nf_hdlc_rx_init(&nfrx, 5, hdlc_on_frame, &c);
    }
    /* preamble underflow pulls frame 0 (the ECM pattern) */
    long bits = 0;
    for (;;) {
        int b = nf2sp ? nf_hdlc_tx_get_bit(&nftx) : hdlc_tx_get_bit(sptx);
        if (b < 0)
            break;               /* END_OF_DATA */
        if (bits == corrupt_bit)
            b ^= 1;
        if (nf2sp)
            hdlc_rx_put_bit(sprx, b);
        else
            nf_hdlc_rx_put_bit(&nfrx, b);
        if (++bits > 1000000) {
            printf("hdlc %s: tx never ended\n", nf2sp ? "nf2sp" : "sp2nf");
            return 1;
        }
    }
    if (sprx) hdlc_rx_free(sprx);
    if (sptx) hdlc_tx_free(sptx);

    if (corrupt_bit >= 0) {
        /* one frame is damaged: expect at least one bad-frame report, no
         * spurious good frame, and the rest received in order */
        int ok = (c.rx_bad >= 1) && (c.rx_ok == HDLC_NFRAMES - 1);
        printf("hdlc %s corrupt@%d: ok=%d bad=%d (want %d good, >=1 bad) %s\n",
               nf2sp ? "nf2sp" : "sp2nf", corrupt_bit, c.rx_ok, c.rx_bad,
               HDLC_NFRAMES - 1, ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    int ok = (c.rx_ok == HDLC_NFRAMES) && (c.rx_match == HDLC_NFRAMES)
          && (c.rx_bad == 0);
    printf("hdlc %s: %d/%d frames ok (match=%d bad=%d) over %ld bits %s\n",
           nf2sp ? "nf2sp" : "sp2nf", c.rx_ok, HDLC_NFRAMES, c.rx_match,
           c.rx_bad, bits, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* a too-short preamble (3 flags) must not let the rx sync onto the frame */
static int hdlc_run_short_preamble(void)
{
    hdlc_ctx_t c; memset(&c, 0, sizeof c);
    hdlc_make_frames(&c, 1);

    nf_hdlc_tx_t nftx;
    nf_hdlc_tx_init(&nftx, 2, hdlc_on_underflow, &c);
    c.tx = &nftx; c.tx_is_nf = 1;
    nf_hdlc_tx_flags(&nftx, 3);
    nf_hdlc_tx_frame(&nftx, c.frames[0], c.flen[0]);
    nf_hdlc_tx_frame(&nftx, NULL, 0);

    nf_hdlc_rx_t nfrx;
    nf_hdlc_rx_init(&nfrx, 5, hdlc_on_frame, &c);
    int b;
    while ((b = nf_hdlc_tx_get_bit(&nftx)) >= 0)
        nf_hdlc_rx_put_bit(&nfrx, b);
    int ok = c.rx_ok == 0;
    printf("hdlc short-preamble: ok-frames=%d (want 0) %s\n",
           c.rx_ok, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_hdlc(void)
{
    int rc = 0;
    rc |= hdlc_run_dir(1, -1);           /* nf tx -> spandsp rx, streaming */
    rc |= hdlc_run_dir(0, -1);           /* spandsp tx -> nf rx, streaming */
    rc |= hdlc_run_dir(1, 32 * 8 + 67);  /* corrupt a bit inside frame 0   */
    rc |= hdlc_run_dir(0, 32 * 8 + 67);
    rc |= hdlc_run_short_preamble();
    return rc;
}

/* ── v21: HDLC frames over V.21 audio, nf vs spandsp ───────────────── */

#include "nf_v21.h"

/* second rx context for dualrx comparisons */
typedef struct {
    int rx_ok, rx_bad;
} count_ctx_t;

static void count_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    count_ctx_t *c = user;
    (void) msg;
    if (len < 0) return;
    if (ok) c->rx_ok++; else c->rx_bad++;
}

static void v21_status_ignore(void *user, int status)
{
    (void) user; (void) status;
}

/* mode: 0 = nf2sp, 1 = sp2nf, 2 = dualrx (sp tx -> both rx) */
static int v21_run(int mode, channel_t chan, const char *label)
{
    hdlc_ctx_t c; memset(&c, 0, sizeof c);
    c.stream = 1;
    hdlc_make_frames(&c, 5);

    /* tx side */
    nf_hdlc_tx_t nfhtx; nf_v21_tx_t nfv21tx;
    hdlc_tx_state_t *sphtx = NULL; fsk_tx_state_t *spftx = NULL;
    if (mode == 0) {
        nf_hdlc_tx_init(&nfhtx, 2, hdlc_on_underflow, &c);
        c.tx = &nfhtx; c.tx_is_nf = 1;
        nf_hdlc_tx_flags(&nfhtx, 32);
        nf_v21_tx_init(&nfv21tx, (int (*)(void *)) nf_hdlc_tx_get_bit, &nfhtx);
    } else {
        sphtx = hdlc_tx_init(NULL, FALSE, 2, FALSE, hdlc_on_underflow, &c);
        c.tx = sphtx; c.tx_is_nf = 0;
        hdlc_tx_flags(sphtx, 32);
        spftx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V21CH2],
                            (get_bit_func_t) hdlc_tx_get_bit, sphtx);
    }

    /* rx side(s): frames land in c (primary) / cmp (spandsp, for dualrx) */
    count_ctx_t cmp; memset(&cmp, 0, sizeof cmp);
    nf_v21_rx_t nfv21rx; nf_hdlc_rx_t nfhrx;
    fsk_rx_state_t *spfrx = NULL; hdlc_rx_state_t *sphrx = NULL;
    if (mode == 0) {
        sphrx = hdlc_rx_init(NULL, FALSE, TRUE, 5, hdlc_on_frame, &c);
        spfrx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC,
                            (put_bit_func_t) hdlc_rx_put_bit, sphrx);
        fsk_rx_set_modem_status_handler(spfrx, v21_status_ignore, NULL);
    } else {
        nf_hdlc_rx_init(&nfhrx, 5, hdlc_on_frame, &c);
        nf_v21_rx_init(&nfv21rx, (void (*)(void *, int)) nf_hdlc_rx_put_bit, &nfhrx);
        nf_v21_rx_set_status_handler(&nfv21rx, v21_status_ignore, NULL);
        if (mode == 2) {
            sphrx = hdlc_rx_init(NULL, FALSE, TRUE, 5, count_on_frame, &cmp);
            spfrx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC,
                                (put_bit_func_t) hdlc_rx_put_bit, sphrx);
            fsk_rx_set_modem_status_handler(spfrx, v21_status_ignore, NULL);
        }
    }

    int16_t amp[BLK];
    int idle = 0;
    for (int blk = 0; blk < 60 * 50 && idle < 10; blk++) {
        int n = (mode == 0) ? nf_v21_tx(&nfv21tx, amp, BLK)
                            : fsk_tx(spftx, amp, BLK);
        if (n < BLK) {
            memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
            idle++;
        }
        chan_apply(&chan, amp, BLK);
        if (mode == 0) {
            fsk_rx(spfrx, amp, BLK);
        } else {
            nf_v21_rx(&nfv21rx, amp, BLK);
            if (mode == 2)
                fsk_rx(spfrx, amp, BLK);
        }
    }
    if (spfrx) fsk_rx_free(spfrx);
    if (sphrx) hdlc_rx_free(sphrx);
    if (spftx) fsk_tx_free(spftx);
    if (sphtx) hdlc_tx_free(sphtx);

    int ok;
    if (mode == 2) {
        /* parity: our rx must do at least as well as spandsp's on the
         * identical impaired stream */
        ok = c.rx_ok >= cmp.rx_ok;
        printf("v21 dualrx %-18s: nf ok=%d/5 sp ok=%d/5 %s\n",
               label, c.rx_ok, cmp.rx_ok, ok ? "PASS" : "FAIL");
    } else {
        ok = (c.rx_ok == 5) && (c.rx_match == 5);
        printf("v21 %s %-18s: %d/5 frames (match=%d bad=%d) %s\n",
               mode == 0 ? "nf2sp" : "sp2nf", label, c.rx_ok, c.rx_match,
               c.rx_bad, ok ? "PASS" : "FAIL");
    }
    return ok ? 0 : 1;
}

/* all-zero TCF-style bit source for fast-modem transmitters */
static long tcf_bits_left;
static int tcf_get_bit(void *user)
{
    long *left = user;
    return (*left)-- > 0 ? 0 : SIG_STATUS_END_OF_DATA;
}

/* fast-modem (V.29 TCF) audio into our V.21 rx: no good frame may emerge */
static int v21_run_parallel(void)
{
    count_ctx_t cnt; memset(&cnt, 0, sizeof cnt);
    nf_hdlc_rx_t nfhrx;
    nf_v21_rx_t nfv21rx;
    nf_hdlc_rx_init(&nfhrx, 5, count_on_frame, &cnt);
    nf_v21_rx_init(&nfv21rx, (void (*)(void *, int)) nf_hdlc_rx_put_bit, &nfhrx);
    nf_v21_rx_set_status_handler(&nfv21rx, v21_status_ignore, NULL);

    v29_tx_state_t *v29 = v29_tx_init(NULL, 9600, FALSE, tcf_get_bit, &tcf_bits_left);
    int16_t amp[BLK];
    tcf_bits_left = 9600 * 2;       /* 2 s of zeros, like a real TCF */

    for (int blk = 0; blk < 4 * 50; blk++) {
        int n = v29_tx(v29, amp, BLK);
        if (n < BLK)
            memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
        nf_v21_rx(&nfv21rx, amp, BLK);
        if (n < BLK)
            break;
    }
    v29_tx_free(v29);
    int ok = cnt.rx_ok == 0;
    printf("v21 parallel (V.29 TCF audio): good-frames=%d (want 0), bad=%d %s\n",
           cnt.rx_ok, cnt.rx_bad, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_v21(int argc, char **argv)
{
    channel_t clean = chan_parse(argc, argv, 2);
    int rc = 0;
    channel_t alaw = clean; alaw.use_alaw = 1;
    rc |= v21_run(0, clean, "clean");
    rc |= v21_run(1, clean, "clean");
    rc |= v21_run(0, alaw, "alaw");
    rc |= v21_run(1, alaw, "alaw");
    static const double snrs[] = { 30, 20, 12, 6 };
    for (unsigned i = 0; i < sizeof snrs / sizeof snrs[0]; i++) {
        channel_t ch = alaw; ch.snr_db = snrs[i];
        char label[40];
        snprintf(label, sizeof label, "alaw snr=%g", snrs[i]);
        rc |= v21_run(0, ch, label);
        rc |= v21_run(1, ch, label);
        rc |= v21_run(2, ch, label);
    }
    rc |= v21_run_parallel();
    return rc;
}

/* ── v29: PRBS data over V.29, nf vs spandsp, all pairings ─────────── */

#include "nf_v29.h"

#define V29_DATA_BITS 19200

typedef struct {
    /* tx side */
    uint32_t tx_prng;
    long tx_left;
    /* rx side */
    uint32_t rx_prng;
    long payload;               /* compare this many leading bits */
    long rx_bits, rx_errs;
    int trained, train_failed, carrier_seen;
} fast_ctx_t;

static int prbs_next(uint32_t *st)
{
    *st ^= *st << 13; *st ^= *st >> 17; *st ^= *st << 5;
    return (int) (*st & 1);
}

static int fast_get_bit(void *user)
{
    fast_ctx_t *c = user;
    if (c->tx_left-- <= 0)
        return SIG_STATUS_END_OF_DATA;
    return prbs_next(&c->tx_prng);
}

static void fast_put_bit(void *user, int bit)
{
    fast_ctx_t *c = user;
    if (bit < 0)
        return;
    /* compare against the reference PRBS for the payload length; the tail
     * (scrambled shutdown ones) is ignored */
    if (c->rx_bits < c->payload) {
        if ((bit & 1) != prbs_next(&c->rx_prng))
            c->rx_errs++;
    }
    c->rx_bits++;
}

static void fast_status(void *user, int status)
{
    fast_ctx_t *c = user;
    switch (status) {
    case SIG_STATUS_TRAINING_SUCCEEDED: c->trained = 1; break;
    case SIG_STATUS_TRAINING_FAILED:    c->train_failed = 1; break;
    case SIG_STATUS_CARRIER_UP:         c->carrier_seen = 1; break;
    }
}

static void fast_ctx_init(fast_ctx_t *c, uint32_t seed, long bits)
{
    memset(c, 0, sizeof(*c));
    c->tx_prng = c->rx_prng = seed;
    c->tx_left = c->payload = bits;
}

/* mode: 0 nf2sp, 1 sp2nf, 2 nfnf, 3 dualrx (sp tx -> nf rx + sp rx) */
static int v29_run(int mode, int rate, channel_t chan, const char *label)
{
    fast_ctx_t c, c2;
    fast_ctx_init(&c, 0xBEEF0001u, V29_DATA_BITS);
    fast_ctx_init(&c2, 0xBEEF0001u, V29_DATA_BITS);

    nf_v29_tx_t nftx;
    v29_tx_state_t *sptx = NULL;
    nf_v29_rx_t nfrx;
    v29_rx_state_t *sprx = NULL;

    int tx_is_nf = (mode == 0 || mode == 2);
    int rx_is_nf = (mode == 1 || mode == 2 || mode == 3);
    if (tx_is_nf)
        nf_v29_tx_init(&nftx, rate, fast_get_bit, &c);
    else
        sptx = v29_tx_init(NULL, rate, FALSE, fast_get_bit, &c);
    if (rx_is_nf) {
        nf_v29_rx_init(&nfrx, rate, fast_put_bit, &c);
        nf_v29_rx_set_status_handler(&nfrx, fast_status, &c);
    }
    if (mode == 0 || mode == 3) {
        fast_ctx_t *cc = (mode == 0) ? &c : &c2;
        sprx = v29_rx_init(NULL, rate, fast_put_bit, cc);
        /* the spandsp FAX stack lowers the V.29 cutoff to V.17's level
         * (fax_modems.c); the oracle must behave like the fax stack */
        v29_rx_signal_cutoff(sprx, -45.5f);
        v29_rx_set_modem_status_handler(sprx, fast_status, cc);
    }

    int16_t amp[BLK];
    int idle = 0;
    for (int blk = 0; blk < 60 * 50 && idle < 20; blk++) {
        int n = tx_is_nf ? nf_v29_tx(&nftx, amp, BLK) : v29_tx(sptx, amp, BLK);
        if (n < BLK) {
            memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
            idle++;
        }
        chan_apply(&chan, amp, BLK);
        if (rx_is_nf)
            nf_v29_rx(&nfrx, amp, BLK);
        if (sprx)
            v29_rx(sprx, amp, BLK);
    }
    if (sptx) v29_tx_free(sptx);
    if (sprx) v29_rx_free(sprx);

    int ok;
    if (mode == 3) {
        /* parity: our rx at least as good as spandsp's on identical input */
        ok = (c.trained >= c2.trained)
          && (!c2.trained || c.rx_errs <= 2 * c2.rx_errs + 16);
        printf("v29 dualrx %5d %-16s: nf[train=%d errs=%ld/%ld] sp[train=%d errs=%ld/%ld] %s\n",
               rate, label, c.trained, c.rx_errs, c.rx_bits,
               c2.trained, c2.rx_errs, c2.rx_bits, ok ? "PASS" : "FAIL");
    } else {
        ok = c.trained && c.rx_bits >= V29_DATA_BITS && c.rx_errs == 0;
        printf("v29 %s %5d %-16s: train=%d bits=%ld errs=%ld %s\n",
               mode == 0 ? "nf2sp" : mode == 1 ? "sp2nf" : "nf2nf",
               rate, label, c.trained, c.rx_bits, c.rx_errs, ok ? "PASS" : "FAIL");
    }
    return ok ? 0 : 1;
}

static int cmd_v29(int argc, char **argv)
{
    channel_t clean = chan_parse(argc, argv, 2);
    channel_t alaw = clean; alaw.use_alaw = 1;
    int rc = 0;
    static const int rates[] = { 9600, 7200, 4800 };
    for (unsigned r = 0; r < 3; r++) {
        rc |= v29_run(0, rates[r], clean, "clean");
        rc |= v29_run(1, rates[r], clean, "clean");
        rc |= v29_run(2, rates[r], clean, "clean");
        rc |= v29_run(0, rates[r], alaw, "alaw");
        rc |= v29_run(1, rates[r], alaw, "alaw");
    }
    /* With the fax-stack cutoff (-45.5 dBm0) a continuous noise floor above
     * ~-42 dBm0 wakes the rx before the training and parks it - identical in
     * spandsp - so error-free reception is expected only down to ~28 dB,
     * with parity required below. */
    {
        channel_t ch = alaw; ch.snr_db = 28;
        rc |= v29_run(0, 9600, ch, "alaw snr=28");
        rc |= v29_run(1, 9600, ch, "alaw snr=28");
        rc |= v29_run(3, 9600, ch, "alaw snr=28");
    }
    static const double edge[] = { 22, 18, 14 };
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        channel_t ch = alaw; ch.snr_db = edge[i];
        char label[40];
        snprintf(label, sizeof label, "alaw snr=%g", edge[i]);
        rc |= v29_run(3, 9600, ch, label);
    }
    /* gain extremes (-9 keeps the level above the -26 dBm0 V.29 cutoff) */
    channel_t g1 = alaw; g1.gain_db = -9.0;
    channel_t g2 = alaw; g2.gain_db = 6.0;
    rc |= v29_run(1, 9600, g1, "alaw gain-9");
    rc |= v29_run(1, 9600, g2, "alaw gain+6");
    return rc;
}

/* ── v27: same idea for V.27ter ────────────────────────────────────── */

#include "nf_v27.h"

#define V27_DATA_BITS 9600

static int v27_run(int mode, int rate, channel_t chan, const char *label)
{
    fast_ctx_t c, c2;
    fast_ctx_init(&c, 0xACE10003u, V27_DATA_BITS);
    fast_ctx_init(&c2, 0xACE10003u, V27_DATA_BITS);

    nf_v27_tx_t nftx;
    v27ter_tx_state_t *sptx = NULL;
    nf_v27_rx_t nfrx;
    v27ter_rx_state_t *sprx = NULL;

    int tx_is_nf = (mode == 0 || mode == 2);
    int rx_is_nf = (mode == 1 || mode == 2 || mode == 3);
    if (tx_is_nf)
        nf_v27_tx_init(&nftx, rate, fast_get_bit, &c);
    else
        sptx = v27ter_tx_init(NULL, rate, FALSE, fast_get_bit, &c);
    if (rx_is_nf) {
        nf_v27_rx_init(&nfrx, rate, fast_put_bit, &c);
        nf_v27_rx_set_status_handler(&nfrx, fast_status, &c);
    }
    if (mode == 0 || mode == 3) {
        fast_ctx_t *cc = (mode == 0) ? &c : &c2;
        sprx = v27ter_rx_init(NULL, rate, fast_put_bit, cc);
        v27ter_rx_set_modem_status_handler(sprx, fast_status, cc);
    }

    int16_t amp[BLK];
    int idle = 0;
    for (int blk = 0; blk < 60 * 50 && idle < 20; blk++) {
        int n = tx_is_nf ? nf_v27_tx(&nftx, amp, BLK) : v27ter_tx(sptx, amp, BLK);
        if (n < BLK) {
            memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
            idle++;
        }
        chan_apply(&chan, amp, BLK);
        if (rx_is_nf)
            nf_v27_rx(&nfrx, amp, BLK);
        if (sprx)
            v27ter_rx(sprx, amp, BLK);
    }
    if (sptx) v27ter_tx_free(sptx);
    if (sprx) v27ter_rx_free(sprx);

    int ok;
    if (mode == 3) {
        ok = (c.trained >= c2.trained)
          && (!c2.trained || c.rx_errs <= 2 * c2.rx_errs + 16);
        printf("v27 dualrx %5d %-16s: nf[train=%d errs=%ld/%ld] sp[train=%d errs=%ld/%ld] %s\n",
               rate, label, c.trained, c.rx_errs, c.rx_bits,
               c2.trained, c2.rx_errs, c2.rx_bits, ok ? "PASS" : "FAIL");
    } else {
        ok = c.trained && c.rx_bits >= V27_DATA_BITS && c.rx_errs == 0;
        printf("v27 %s %5d %-16s: train=%d bits=%ld errs=%ld %s\n",
               mode == 0 ? "nf2sp" : mode == 1 ? "sp2nf" : "nf2nf",
               rate, label, c.trained, c.rx_bits, c.rx_errs, ok ? "PASS" : "FAIL");
    }
    return ok ? 0 : 1;
}

static int cmd_v27(int argc, char **argv)
{
    channel_t clean = chan_parse(argc, argv, 2);
    channel_t alaw = clean; alaw.use_alaw = 1;
    int rc = 0;
    static const int rates[] = { 4800, 2400 };
    for (unsigned r = 0; r < 2; r++) {
        rc |= v27_run(0, rates[r], clean, "clean");
        rc |= v27_run(1, rates[r], clean, "clean");
        rc |= v27_run(2, rates[r], clean, "clean");
        rc |= v27_run(0, rates[r], alaw, "alaw");
        rc |= v27_run(1, rates[r], alaw, "alaw");
    }
    /* Continuous noise above ~-43 dBm0 wakes the (deliberately sensitive,
     * -45.5 dBm0 cutoff) receiver before the training and parks it - spandsp
     * behaves identically, so below ~26 dB SNR only parity is required. */
    static const double edge[] = { 26, 20, 14, 10 };
    for (unsigned i = 0; i < 4; i++) {
        channel_t ch = alaw; ch.snr_db = edge[i];
        char label[40];
        snprintf(label, sizeof label, "alaw snr=%g", edge[i]);
        rc |= v27_run(3, 4800, ch, label);
        rc |= v27_run(3, 2400, ch, label);
    }
    return rc;
}

/* ── v17: PRBS over V.17, nf vs spandsp, plus short-train sequences ── */

#include "nf_v17.h"

#define V17_DATA_BITS 19200

static int v17_run(int mode, int rate, channel_t chan, const char *label)
{
    fast_ctx_t c, c2;
    fast_ctx_init(&c, 0xC0FFEE07u, V17_DATA_BITS);
    fast_ctx_init(&c2, 0xC0FFEE07u, V17_DATA_BITS);

    nf_v17_tx_t nftx;
    v17_tx_state_t *sptx = NULL;
    nf_v17_rx_t nfrx;
    v17_rx_state_t *sprx = NULL;

    int tx_is_nf = (mode == 0 || mode == 2);
    int rx_is_nf = (mode == 1 || mode == 2 || mode == 3);
    if (tx_is_nf)
        nf_v17_tx_init(&nftx, rate, fast_get_bit, &c);
    else
        sptx = v17_tx_init(NULL, rate, FALSE, fast_get_bit, &c);
    if (rx_is_nf) {
        nf_v17_rx_init(&nfrx, rate, fast_put_bit, &c);
        nf_v17_rx_set_status_handler(&nfrx, fast_status, &c);
    }
    if (mode == 0 || mode == 3) {
        fast_ctx_t *cc = (mode == 0) ? &c : &c2;
        sprx = v17_rx_init(NULL, rate, fast_put_bit, cc);
        v17_rx_set_modem_status_handler(sprx, fast_status, cc);
    }

    int16_t amp[BLK];
    int idle = 0;
    for (int blk = 0; blk < 60 * 50 && idle < 20; blk++) {
        int n = tx_is_nf ? nf_v17_tx(&nftx, amp, BLK) : v17_tx(sptx, amp, BLK);
        if (n < BLK) {
            memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
            idle++;
        }
        chan_apply(&chan, amp, BLK);
        if (rx_is_nf)
            nf_v17_rx(&nfrx, amp, BLK);
        if (sprx)
            v17_rx(sprx, amp, BLK);
    }
    if (sptx) v17_tx_free(sptx);
    if (sprx) v17_rx_free(sprx);

    int ok;
    if (mode == 3) {
        ok = (c.trained >= c2.trained)
          && (!c2.trained || c.rx_errs <= 2 * c2.rx_errs + 16);
        printf("v17 dualrx %5d %-16s: nf[train=%d errs=%ld/%ld] sp[train=%d errs=%ld/%ld] %s\n",
               rate, label, c.trained, c.rx_errs, c.rx_bits,
               c2.trained, c2.rx_errs, c2.rx_bits, ok ? "PASS" : "FAIL");
    } else {
        ok = c.trained && c.rx_bits >= V17_DATA_BITS && c.rx_errs == 0;
        printf("v17 %s %5d %-16s: train=%d bits=%ld errs=%ld %s\n",
               mode == 0 ? "nf2sp" : mode == 1 ? "sp2nf" : "nf2nf",
               rate, label, c.trained, c.rx_bits, c.rx_errs, ok ? "PASS" : "FAIL");
    }
    return ok ? 0 : 1;
}

/* One long-train burst, then short-train bursts with idle gaps, mirroring
 * TCF -> page -> page. The rx instance persists across bursts, restarted with
 * the short flag exactly as the fax driver does. */
/* mode: 0 nf2sp, 1 sp2nf, 2 nfnf */
static int v17_run_short(int mode, int rate, channel_t chan)
{
    static const char *names[] = { "nf2sp", "sp2nf", "nf2nf" };
    nf_v17_tx_t nftx;
    v17_tx_state_t *sptx = NULL;
    nf_v17_rx_t nfrx;
    v17_rx_state_t *sprx = NULL;
    fast_ctx_t c;
    long bits_per_burst = rate;             /* ~1 s per burst */
    int bursts_ok = 0;
    int tx_is_nf = (mode != 1);
    int rx_is_nf = (mode != 0);

    /* persistent instances */
    fast_ctx_init(&c, 0xD00D1234u, bits_per_burst);
    if (tx_is_nf)
        nf_v17_tx_init(&nftx, rate, fast_get_bit, &c);
    else
        sptx = v17_tx_init(NULL, rate, FALSE, fast_get_bit, &c);
    if (rx_is_nf) {
        nf_v17_rx_init(&nfrx, rate, fast_put_bit, &c);
        nf_v17_rx_set_status_handler(&nfrx, fast_status, &c);
    } else {
        sprx = v17_rx_init(NULL, rate, fast_put_bit, &c);
        v17_rx_set_modem_status_handler(sprx, fast_status, &c);
    }

    for (int burst = 0; burst < 4; burst++) {
        int short_train = burst > 0;
        fast_ctx_init(&c, 0xD00D1234u + (uint32_t) burst, bits_per_burst);
        if (tx_is_nf)
            nf_v17_tx_restart(&nftx, rate, short_train);
        else
            v17_tx_restart(sptx, rate, FALSE, short_train);
        if (rx_is_nf)
            nf_v17_rx_restart(&nfrx, rate, short_train);
        else
            v17_rx_restart(sprx, rate, short_train);
        int16_t amp[BLK];
        int idle = 0;
        for (int blk = 0; blk < 60 * 50 && idle < 20; blk++) {
            int n = tx_is_nf ? nf_v17_tx(&nftx, amp, BLK) : v17_tx(sptx, amp, BLK);
            if (n < BLK) {
                memset(amp + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
                idle++;
            }
            chan_apply(&chan, amp, BLK);
            if (rx_is_nf)
                nf_v17_rx(&nfrx, amp, BLK);
            else
                v17_rx(sprx, amp, BLK);
        }
        int ok = c.trained && c.rx_bits >= bits_per_burst && c.rx_errs == 0;
        printf("v17short %s %5d burst %d (%s): train=%d bits=%ld errs=%ld %s\n",
               names[mode], rate, burst,
               short_train ? "short" : "long ", c.trained, c.rx_bits,
               c.rx_errs, ok ? "PASS" : "FAIL");
        bursts_ok += ok;
    }
    if (sptx) v17_tx_free(sptx);
    if (sprx) v17_rx_free(sprx);
    return bursts_ok == 4 ? 0 : 1;
}

static int cmd_v17(int argc, char **argv)
{
    channel_t clean = chan_parse(argc, argv, 2);
    channel_t alaw = clean; alaw.use_alaw = 1;
    int rc = 0;
    static const int rates[] = { 14400, 12000, 9600, 7200 };
    for (unsigned r = 0; r < 4; r++) {
        rc |= v17_run(0, rates[r], clean, "clean");
        rc |= v17_run(1, rates[r], clean, "clean");
        rc |= v17_run(2, rates[r], clean, "clean");
        rc |= v17_run(0, rates[r], alaw, "alaw");
        rc |= v17_run(1, rates[r], alaw, "alaw");
    }
    /* clean reception expected at 30 dB; parity below */
    {
        channel_t ch = alaw; ch.snr_db = 30;
        rc |= v17_run(0, 14400, ch, "alaw snr=30");
        rc |= v17_run(1, 14400, ch, "alaw snr=30");
        rc |= v17_run(3, 14400, ch, "alaw snr=30");
    }
    static const double edge[] = { 26, 24, 22, 20 };
    for (unsigned i = 0; i < 4; i++) {
        channel_t ch = alaw; ch.snr_db = edge[i];
        char label[40];
        snprintf(label, sizeof label, "alaw snr=%g", edge[i]);
        rc |= v17_run(3, 14400, ch, label);
        rc |= v17_run(3, 9600, ch, label);
    }
    return rc;
}

static int cmd_v17short(int argc, char **argv)
{
    channel_t clean = chan_parse(argc, argv, 2);
    channel_t alaw = clean; alaw.use_alaw = 1;
    int rc = 0;
    rc |= v17_run_short(0, 14400, clean);
    rc |= v17_run_short(1, 14400, clean);
    rc |= v17_run_short(2, 14400, clean);
    rc |= v17_run_short(0, 14400, alaw);
    rc |= v17_run_short(1, 14400, alaw);
    rc |= v17_run_short(2, 14400, alaw);
    rc |= v17_run_short(2, 9600, alaw);
    return rc;
}

/* ── replay: raw s16le capture into nf and spandsp v17 rx side by side ── */

static void replay_status_nf(void *user, int status)
{
    long *t = user;
    printf("  t=%.2fs nf status=%d\n", *t / 8000.0, status);
}

static void replay_status_sp(void *user, int status)
{
    long *t = user;
    printf("  t=%.2fs sp status=%d\n", *t / 8000.0, status);
}

static void replay_sink(void *user, int bit)
{
    (void) user; (void) bit;
}

static int cmd_replay(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: nf_modemtest replay <file.raw> [rate]\n");
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    int rate = argc > 3 ? atoi(argv[3]) : 14400;
    static long t;
    nf_v17_rx_t nfrx;
    nf_v17_rx_init(&nfrx, rate, replay_sink, NULL);
    nf_v17_rx_set_status_handler(&nfrx, replay_status_nf, &t);
    v17_rx_state_t *sprx = v17_rx_init(NULL, rate, replay_sink, NULL);
    v17_rx_set_modem_status_handler(sprx, replay_status_sp, &t);
    int16_t amp[BLK];
    size_t n;
    while ((n = fread(amp, sizeof(int16_t), BLK, f)) > 0) {
        nf_v17_rx(&nfrx, amp, (int) n);
        v17_rx(sprx, amp, (int) n);
        t += (long) n;
    }
    fclose(f);
    v17_rx_free(sprx);
    return 0;
}

/* ── txlevel: measure rms/peak of nf and spandsp v17 tx ───────────── */

static void txlevel_report(const char *tag, int16_t *amp, int blk_fn(void *), void *st)
{
    double e = 0;
    long n = 0, peak = 0;
    for (int blk = 0; blk < 50 * 6; blk++) {
        int m = blk_fn(st);
        for (int k = 0; k < m; k++) {
            e += (double) amp[k] * amp[k];
            if (labs(amp[k]) > peak) peak = labs(amp[k]);
        }
        n += m;
        if (m < BLK) break;
    }
    double rms = sqrt(e / (double) n);
    printf("%-22s rms=%5.0f (%6.2f dBm0) peak=%ld (%.2f dB above rms)\n",
           tag, rms, 20 * log10(rms / 32767.0) + 3.14 + 3.02,
           peak, 20 * log10((double) peak / rms));
}

static int16_t txlevel_amp[BLK];
static union {
    nf_v17_tx_t v17; nf_v29_tx_t v29; nf_v27_tx_t v27;
} txlevel_nf;
static void *txlevel_sp;

static int tl_nf17(void *u) { (void) u; return nf_v17_tx(&txlevel_nf.v17, txlevel_amp, BLK); }
static int tl_sp17(void *u) { (void) u; return v17_tx(txlevel_sp, txlevel_amp, BLK); }
static int tl_nf29(void *u) { (void) u; return nf_v29_tx(&txlevel_nf.v29, txlevel_amp, BLK); }
static int tl_sp29(void *u) { (void) u; return v29_tx(txlevel_sp, txlevel_amp, BLK); }
static int tl_nf27(void *u) { (void) u; return nf_v27_tx(&txlevel_nf.v27, txlevel_amp, BLK); }
static int tl_sp27(void *u) { (void) u; return v27ter_tx(txlevel_sp, txlevel_amp, BLK); }

static int cmd_txlevel(void)
{
    fast_ctx_t c;

    fast_ctx_init(&c, 0x1234ABCDu, 28800);
    nf_v17_tx_init(&txlevel_nf.v17, 14400, fast_get_bit, &c);
    txlevel_report("nf v17 14400", txlevel_amp, tl_nf17, NULL);
    fast_ctx_init(&c, 0x1234ABCDu, 28800);
    txlevel_sp = v17_tx_init(NULL, 14400, FALSE, fast_get_bit, &c);
    txlevel_report("sp v17 14400", txlevel_amp, tl_sp17, NULL);
    v17_tx_free(txlevel_sp);

    fast_ctx_init(&c, 0x1234ABCDu, 19200);
    nf_v29_tx_init(&txlevel_nf.v29, 9600, fast_get_bit, &c);
    txlevel_report("nf v29 9600", txlevel_amp, tl_nf29, NULL);
    fast_ctx_init(&c, 0x1234ABCDu, 19200);
    txlevel_sp = v29_tx_init(NULL, 9600, FALSE, fast_get_bit, &c);
    txlevel_report("sp v29 9600", txlevel_amp, tl_sp29, NULL);
    v29_tx_free(txlevel_sp);

    fast_ctx_init(&c, 0x1234ABCDu, 9600);
    nf_v27_tx_init(&txlevel_nf.v27, 4800, fast_get_bit, &c);
    txlevel_report("nf v27 4800", txlevel_amp, tl_nf27, NULL);
    fast_ctx_init(&c, 0x1234ABCDu, 9600);
    txlevel_sp = v27ter_tx_init(NULL, 4800, FALSE, fast_get_bit, &c);
    txlevel_report("sp v27 4800", txlevel_amp, tl_sp27, NULL);
    v27ter_tx_free(txlevel_sp);
    return 0;
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void) chan_parse; (void) chan_apply;   /* used from stage 3 on */
    if (argc < 2) {
        fprintf(stderr, "usage: nf_modemtest <tones|...> [args]\n");
        return 2;
    }
    if (!strcmp(argv[1], "tones"))
        return cmd_tones();
    if (!strcmp(argv[1], "hdlc"))
        return cmd_hdlc();
    if (!strcmp(argv[1], "v21"))
        return cmd_v21(argc, argv);
    if (!strcmp(argv[1], "v29"))
        return cmd_v29(argc, argv);
    if (!strcmp(argv[1], "v27"))
        return cmd_v27(argc, argv);
    if (!strcmp(argv[1], "v17"))
        return cmd_v17(argc, argv);
    if (!strcmp(argv[1], "v17short"))
        return cmd_v17short(argc, argv);
    if (!strcmp(argv[1], "replay"))
        return cmd_replay(argc, argv);
    if (!strcmp(argv[1], "txlevel"))
        return cmd_txlevel();
    fprintf(stderr, "unknown subcommand %s\n", argv[1]);
    return 2;
}
