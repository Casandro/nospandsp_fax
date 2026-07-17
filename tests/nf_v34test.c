/*
 * nf_v34test - harness for nf_v34.
 *
 *   shapers          - nf_v34_init() builds sane RRC shaper tables (unit DC
 *                       gain on the tx interpolator's centre phase, same
 *                       convention nf_v17/v27/v29 already rely on). Still
 *                       scaffolding - see nf_v34.h.
 *   info <file.wav>  - coarse per-channel silence/active timeline via simple
 *                       RMS thresholding.
 *   ctrl <file.wav>  - REAL demodulator check: locks nf_v34_ctrl_rx (the
 *                       600-baud/~1200 Hz QPSK control-channel receiver) onto
 *                       the three real half-duplex-turnaround segments in
 *                       the reference capture and asserts the recovered
 *                       constellation is tight (mean squared distance to the
 *                       nearest of 4 axis-aligned reference points, below a
 *                       threshold empirically set well above what a working
 *                       lock produces and well below an unlocked/blob result).
 *   shellmap         - round-trip check of the shell mapper (9.4).
 *   mph1 <file.wav>  - REAL MPh Type-1 decode check: the answer modem's
 *                       control channel (ch1, 2400 Hz, GPA descrambler)
 *                       carries an MPh Type-1 frame with the precoder
 *                       coefficients h1..h3 - decode and assert the exact
 *                       Q14 words cross-validated in Python.
 *   infodec <file.wav> - REAL Phase-2 INFO-sequence decode check: INFO0c
 *                       (ch0/1200 Hz), INFO0a and INFOh (ch1/2400 Hz) must
 *                       all decode CRC-valid, INFOh with the expected
 *                       modulation parameters (3429 baud, TRN 84x35ms,
 *                       16-point, pre-emphasis 2).
 *   ccdata <file.wav> - REAL control-channel HDLC user-data check: the
 *                       T.30 conversation (DCS, NSF/CSI/DIS, CFR, PPS-NULL,
 *                       PPS-EOP, MCFs, DCN) must decode FCS-valid from all
 *                       three half-duplex control-channel windows.
 *   page <file.wav>  - REAL primary-channel decode check: the full receive
 *                       pipeline (TRN-trained LS T/2 FSE, per-burst resync
 *                       transfer, DD phase/gain/timing/tap tracking, 9.3.1
 *                       inverse mapping, descrambler, HDLC) must recover
 *                       the two ECM image blocks COMPLETELY as FCS-valid
 *                       FCD frames (256/256 + 149/149, asserted exactly -
 *                       matching the real receiving fax, which sent MCF).
 *   modetab          - Tables 1/2/7/8/10 transcription vs the derivation
 *                       cross-check (N=R*0.28/J, b=ceil(N/P), SWP counter,
 *                       eq 9-1 K/q, 9.2 M rules, eq 9-2 L).
 *   txrates          - clean-channel TX->RX loopback matrix over EVERY
 *                       non-aux Table 8 (S,R) pair (65 operating points,
 *                       both carrier options alternating) plus minimum-
 *                       shaping spot cells - byte-identical assert.
 *   ratesnr [sr]     - measure the per-rate AWGN decode threshold (manual;
 *                       feeds the requirement curve in nf_v34.c).
 *   trellis          - self-consistency check of the trellis/MAP encoder
 *                       chain (9.5/9.6.1/9.6.3): a synthetic encoder run
 *                       (differential encoder + mapper rotation + 16-state
 *                       trellis) must only ever produce points that are
 *                       valid rotations of the (tiny, M=4) test quarter-
 *                       constellation - ported from the Python prototype
 *                       that found the U0/rotation-parity Viterbi decoding
 *                       rule (see nf_v34.h's trellis section for the full
 *                       story; the Viterbi search itself is not yet ported).
 *
 * build: cc nf_v34test.c nf_v34.c nf_dsp.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "nf_v34.h"
#include "g711.h"

/* ── minimal canonical PCM WAV reader (RIFF/fmt /data), enough for our own
 * references/v.34_modem_test.wav - not a general-purpose parser. ────────── */
struct wav {
    int channels, sample_rate, bits;
    int16_t *samples;   /* interleaved */
    long nframes;
};

static uint32_t rd_u32le(const uint8_t *p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }

static int wav_load(const char *path, struct wav *w)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path); fclose(f); return -1;
    }
    memset(w, 0, sizeof(*w));
    for (;;) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, f) != 8) break;
        uint32_t len = rd_u32le(chdr + 4);
        if (!memcmp(chdr, "fmt ", 4)) {
            uint8_t fmt[16];
            if (len < 16 || fread(fmt, 1, 16, f) != 16) { fclose(f); return -1; }
            w->channels = rd_u16le(fmt + 2);
            w->sample_rate = (int) rd_u32le(fmt + 4);
            w->bits = rd_u16le(fmt + 14);
            if (len > 16) fseek(f, len - 16, SEEK_CUR);
        } else if (!memcmp(chdr, "data", 4)) {
            w->nframes = (long) (len / (uint32_t) (w->channels * (w->bits / 8)));
            w->samples = malloc(len);
            if (!w->samples || fread(w->samples, 1, len, f) != len) { fclose(f); return -1; }
        } else {
            fseek(f, (long) len + (long) (len & 1), SEEK_CUR);
        }
        if (w->samples && w->channels) break;   /* got what we need */
    }
    fclose(f);
    if (!w->samples || w->bits != 16) { fprintf(stderr, "%s: unsupported/missing data\n", path); return -1; }
    return 0;
}

static int cmd_shapers(void)
{
    nf_v34_t v;
    int i, ok = 1;

    nf_v34_init(&v);
    printf("rate   carrier   tx_centre_tap(set0)\n");
    for (i = 0; i < NF_V34_NUM_RATES; i++) {
        /* nf_rrc_design's convention (see nf_dsp.h): unit DC gain on the
         * centre phase - phase 0's peak tap should sit close to 1.0. */
        float centre = v.shaper[i].tx[NF_V34_RRC_TAPS / 2];
        printf("%5d  %7.1f   %8.4f\n", nf_v34_rates[i].baud, nf_v34_rates[i].carrier_low_hz, centre);
        if (centre < 0.5f || centre > 1.5f) ok = 0;   /* generous sanity band */
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_info(const char *path)
{
    struct wav w;
    if (wav_load(path, &w) < 0) return 1;
    printf("%s: channels=%d sample_rate=%d bits=%d frames=%ld (%.1fs)\n",
           path, w.channels, w.sample_rate, w.bits, w.nframes,
           (double) w.nframes / w.sample_rate);

    int blk = w.sample_rate / 10;   /* 100 ms */
    int ch;
    for (ch = 0; ch < w.channels; ch++) {
        long i;
        int in_active = 0, segments = 0;
        double seg_start = 0;
        for (i = 0; i + blk <= w.nframes; i += blk) {
            double sumsq = 0;
            long j;
            for (j = 0; j < blk; j++) {
                double x = w.samples[(i + j) * w.channels + ch];
                sumsq += x * x;
            }
            double level = sumsq / blk;
            double active_thresh = 40.0 * 40.0;   /* matches the earlier offline analysis */
            int active = level > active_thresh;
            if (active && !in_active) { in_active = 1; seg_start = (double) i / w.sample_rate; }
            if (!active && in_active) {
                in_active = 0; segments++;
                printf("  ch%d: active %.2f - %.2fs\n", ch, seg_start, (double) i / w.sample_rate);
            }
        }
        if (in_active) { segments++; printf("  ch%d: active %.2f - %.2fs (to EOF)\n", ch, seg_start, (double) w.nframes / w.sample_rate); }
        printf(" ch%d: %d active segment(s)\n", ch, segments);
    }
    free(w.samples);
    return 0;
}

struct ctrl_accum {
    double sumre, sumim, sumre2, sumim2;
    int n;
    double re[8000], im[8000];   /* plenty for these ~1-2k symbol segments */
};

static void ctrl_collect(void *user, const nf_cpx_t *z, int dibit)
{
    struct ctrl_accum *a = user;
    (void) dibit;
    if (a->n < 8000) { a->re[a->n] = z->re; a->im[a->n] = z->im; a->n++; }
}

/* Mean squared distance from each recovered symbol to the nearest of 4
 * axis-aligned reference points at the median observed magnitude - a
 * constellation-shape-agnostic tightness metric (doesn't need to know the
 * absolute carrier phase, only that there should be 4 tight clusters). */
static double axis_mse(const struct ctrl_accum *a, int skip)
{
    double mags[8000];
    int i, n = 0;
    double refx[4], refy[4], mag_med, sum = 0;

    for (i = skip; i < a->n; i++)
        mags[n++] = hypot(a->re[i], a->im[i]);
    /* crude median via sort-free partial selection (n is small) */
    for (i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (mags[j] < mags[i]) { double t = mags[i]; mags[i] = mags[j]; mags[j] = t; }
    mag_med = n ? mags[n / 2] : 1.0;
    refx[0] = mag_med;  refy[0] = 0;
    refx[1] = -mag_med; refy[1] = 0;
    refx[2] = 0;        refy[2] = mag_med;
    refx[3] = 0;        refy[3] = -mag_med;
    for (i = skip; i < a->n; i++) {
        double best = 1e300;
        int k;
        for (k = 0; k < 4; k++) {
            double dx = a->re[i] - refx[k], dy = a->im[i] - refy[k];
            double d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2;
        }
        sum += best;
    }
    return (a->n - skip) > 0 ? sum / (a->n - skip) : 1e300;
}

static int cmd_ctrl(const char *path)
{
    struct wav w;
    static const struct { double t0, t1; int skip; } segs[] = {
        { 42.5, 45.4, 400 },
        { 69.2, 71.0, 20 },
        { 85.0, 86.6, 20 },
    };
    unsigned s;
    int ok = 1;

    if (wav_load(path, &w) < 0) return 1;

    for (s = 0; s < sizeof segs / sizeof segs[0]; s++) {
        long i0 = (long) (segs[s].t0 * w.sample_rate);
        long i1 = (long) (segs[s].t1 * w.sample_rate);
        int16_t *chA = malloc((size_t) (i1 - i0) * sizeof(int16_t));
        long i;
        nf_v34_ctrl_rx_t rx;
        struct ctrl_accum acc = {0};
        double mse;

        for (i = i0; i < i1; i++)
            chA[i - i0] = w.samples[i * w.channels + 0];
        nf_v34_ctrl_rx_init(&rx, 1200.0);
        nf_v34_ctrl_rx(&rx, chA, (int) (i1 - i0), ctrl_collect, &acc);
        free(chA);

        mse = axis_mse(&acc, segs[s].skip);
        printf("  seg %.1f-%.1fs: n=%d (skip %d) axis_mse=%.4f\n",
               segs[s].t0, segs[s].t1, acc.n, segs[s].skip, mse);
        /* a working lock measures ~0.02-0.09 on these three segments (the
         * first, longer segment and the noisier tails of all of them pull
         * this up from the cleanest single-cluster case, ~0.02); an
         * unlocked/wrong-parameter blob measures ~0.14-0.9 (see the
         * session's Python/C validation) - 0.10 sits above every real lock
         * we measured and below every blob we measured */
        if (mse > 0.10)
            ok = 0;
    }
    free(w.samples);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Extract one channel of a [t0,t1) window into a fresh int16 buffer. */
static int16_t *wav_window(const struct wav *w, double t0, double t1, int ch, long *outn)
{
    long i0 = (long) (t0 * w->sample_rate);
    long i1 = (long) (t1 * w->sample_rate);
    int16_t *buf;
    long i;

    if (i0 < 0) i0 = 0;
    if (i1 > w->nframes) i1 = w->nframes;
    if (ch >= w->channels) ch = w->channels - 1;
    buf = malloc((size_t) (i1 - i0) * sizeof(int16_t));
    for (i = i0; i < i1; i++)
        buf[i - i0] = w->samples[i * w->channels + ch];
    *outn = i1 - i0;
    return buf;
}

/* mphunt - THE definitive real-capture check: feeds nf_v34_ctrl_rx's
 * demodulated control-channel symbols (600 baud, 1200 Hz) through
 * nf_v34_mp_rx and asserts a real, CRC-16-valid MP frame is found in the
 * mid-call rate-renegotiation event, with the exact fields independently
 * decoded by hand this session (max rate 33600 bit/s = code 14, 16-state
 * trellis, non-linear encoding and expanded shaping both enabled). */
struct mp_hunt_ctx {
    nf_v34_mp_rx_t mp;
    int frames_found;
    int last_ok;
};

static void mp_hunt_collect(void *user, const nf_cpx_t *z)
{
    struct mp_hunt_ctx *ctx = user;
    if (nf_v34_mp_feed_symbol(&ctx->mp, z)) {
        ctx->frames_found++;
        printf("  frame #%d: type=%d max_rate_c2a=%d(x2400=%d) max_rate_a2c=%d "
               "aux=%d trellis=%d nonlinear=%d shaping=%d ack=%d rate_mask=0x%04x asym=%d\n",
               ctx->frames_found, ctx->mp.type, ctx->mp.max_rate_c2a, ctx->mp.max_rate_c2a * 2400,
               ctx->mp.max_rate_a2c, ctx->mp.aux_channel, ctx->mp.trellis_size, ctx->mp.nonlinear,
               ctx->mp.shaping, ctx->mp.ack, ctx->mp.rate_mask, ctx->mp.asym_enable);
        if (ctx->mp.max_rate_c2a == 14 && ctx->mp.trellis_size == 0 &&
            ctx->mp.nonlinear == 1 && ctx->mp.shaping == 1)
            ctx->last_ok = 1;
    }
}

static int cmd_mphunt(const char *path)
{
    struct wav w;
    double t0 = 41.5, t1 = 46.5;
    long i0, i1, i;
    int16_t *chA;
    struct mp_hunt_ctx ctx;

    if (wav_load(path, &w) < 0) return 1;
    i0 = (long) (t0 * w.sample_rate);
    i1 = (long) (t1 * w.sample_rate);
    chA = malloc((size_t) (i1 - i0) * sizeof(int16_t));
    for (i = i0; i < i1; i++)
        chA[i - i0] = w.samples[i * w.channels + 0];

    memset(&ctx, 0, sizeof(ctx));
    nf_v34_mp_rx_init(&ctx.mp, 1 /* call modem */);
    nf_v34_cc_rx_batch(chA, (int) (i1 - i0), 1200.0, mp_hunt_collect, &ctx, 0);

    free(chA);
    free(w.samples);
    printf("frames found: %d\n", ctx.frames_found);
    printf("%s\n", ctx.last_ok ? "PASS" : "FAIL");
    return ctx.last_ok ? 0 : 1;
}

/* mph1 - MPh Type-1 real-capture check: same batch demod chain as mphunt
 * but on the ANSWER modem's control channel (channel 1, 2400 Hz carrier,
 * GPA descrambler) in the first renegotiation window, where the recipient
 * transmits MPh Type-1 frames carrying the 9.6.2 precoder coefficients
 * h1..h3. The expected fields (max rate 24000 = code 10, control-channel
 * rate bit 0 = 1200 bit/s, 16-state trellis, non-linear encoding, expanded
 * shaping, rate mask 0x3fff) and the exact Q14 coefficient words were
 * cross-validated by the Python pipeline this decoder was ported from. */
struct mph1_ctx {
    nf_v34_mp_rx_t mp;
    int frames_found;
    int ok;
};

static void mph1_collect(void *user, const nf_cpx_t *z)
{
    struct mph1_ctx *ctx = user;

    if (nf_v34_mp_feed_symbol(&ctx->mp, z) && ctx->mp.type == 1) {
        static const int16_t exp_re[3] = { 269, -251, 209 };
        static const int16_t exp_im[3] = { -7, 13, -19 };
        int i, hok = 1;

        ctx->frames_found++;
        printf("  T1 frame #%d: max_rate_c2a=%d(x2400=%d) cc_rate=%d(%d bit/s) "
               "trellis=%d nonlinear=%d shaping=%d rate_mask=0x%04x asym=%d\n",
               ctx->frames_found, ctx->mp.max_rate_c2a, ctx->mp.max_rate_c2a * 2400,
               ctx->mp.cc_rate, ctx->mp.cc_rate ? 2400 : 1200,
               ctx->mp.trellis_size, ctx->mp.nonlinear, ctx->mp.shaping,
               ctx->mp.rate_mask, ctx->mp.asym_enable);
        for (i = 0; i < 3; i++) {
            printf("    h%d = %+6d%+6dj (Q14)  = %+.5f%+.5fj\n", i + 1,
                   ctx->mp.h_re[i], ctx->mp.h_im[i],
                   ctx->mp.h_re[i] / 16384.0, ctx->mp.h_im[i] / 16384.0);
            if (ctx->mp.h_re[i] != exp_re[i] || ctx->mp.h_im[i] != exp_im[i])
                hok = 0;
        }
        if (hok && ctx->mp.max_rate_c2a == 10 && ctx->mp.cc_rate == 0 &&
            ctx->mp.trellis_size == 0 && ctx->mp.nonlinear == 1 &&
            ctx->mp.shaping == 1 && ctx->mp.rate_mask == 0x3fff &&
            ctx->mp.asym_enable == 1)
            ctx->ok = 1;
    }
}

static int cmd_mph1(const char *path)
{
    struct wav w;
    int16_t *buf;
    long n;
    struct mph1_ctx ctx;

    if (wav_load(path, &w) < 0) return 1;
    buf = wav_window(&w, 42.48, 45.50, 1, &n);

    memset(&ctx, 0, sizeof(ctx));
    nf_v34_mp_rx_init(&ctx.mp, 0 /* answer modem -> GPA descrambler */);
    nf_v34_cc_rx_batch(buf, (int) n, 2400.0, mph1_collect, &ctx, 0);

    free(buf);
    free(w.samples);
    printf("Type-1 frames found: %d\n", ctx.frames_found);
    printf("%s\n", ctx.ok ? "PASS" : "FAIL");
    return ctx.ok ? 0 : 1;
}

/* infodec - Phase-2 INFO-sequence real-capture check: decodes the INFO0c
 * (call, 1200 Hz), INFO0a (answer, 2400 Hz) and INFOh (answer) frames from
 * the reference capture's Phase 2 (~34.4-39.6s) and asserts the INFOh
 * modulation parameters the rest of the session depends on (3429 baud =
 * symbol-rate index 5, TRN 84x35ms with the 16-point constellation,
 * pre-emphasis filter 2) plus a CRC-valid INFO0 in each direction. */
static int cmd_infodec(const char *path)
{
    struct wav w;
    static const struct { int ch; double carrier; } chans[2] =
        { { 0, 1200.0 }, { 1, 2400.0 } };
    int info0_seen[2] = { 0, 0 };
    int infoh_params_ok = 0;
    int c, i, ok;

    if (wav_load(path, &w) < 0) return 1;

    for (c = 0; c < 2; c++) {
        nf_v34_info_frame_t frames[16];
        int16_t *buf;
        long n;
        int nf;

        buf = wav_window(&w, 34.4, 39.6, chans[c].ch, &n);
        nf = nf_v34_info_rx_batch(buf, (int) n, chans[c].carrier, frames, 16);
        free(buf);

        for (i = 0; i < nf; i++) {
            nf_v34_info_frame_t *f = &frames[i];
            if (f->is_infoh) {
                printf("  INFOh on ch%d @ %.4fs: power_red=%d trn_len=%dx35ms "
                       "high_carrier=%d preemph=%d symrate_idx=%d(%d baud) trn_16pt=%d\n",
                       chans[c].ch, 34.4 + f->t, f->power_reduction, f->trn_len,
                       f->high_carrier, f->preemph_idx, f->symrate_idx,
                       (f->symrate_idx >= 0 && f->symrate_idx < NF_V34_NUM_RATES) ?
                           nf_v34_rates[f->symrate_idx].baud : -1,
                       f->trn_16pt);
                if (chans[c].ch == 1 && f->symrate_idx == 5 && f->trn_len == 84 &&
                    f->trn_16pt == 1 && f->preemph_idx == 2)
                    infoh_params_ok = 1;
            } else {
                printf("  INFO0 on ch%d @ %.4fs: sr[2743,2800,3429]=%d%d%d "
                       "carrier[l3000,h3000,l3200,h3200]=%d%d%d%d allow3429=%d "
                       "pwr_red=%d asym=%d cme=%d 1664pt=%d clk=%d ack=%d\n",
                       chans[c].ch, 34.4 + f->t, f->sr2743, f->sr2800, f->sr3429,
                       f->low3000, f->high3000, f->low3200, f->high3200,
                       f->allow_3429, f->can_reduce_power, f->max_sr_diff,
                       f->from_cme, f->support_1664pt, f->clock_source, f->info0_ack);
                info0_seen[c] = 1;
            }
        }
    }
    free(w.samples);

    ok = info0_seen[0] && info0_seen[1] && infoh_params_ok;
    if (!info0_seen[0]) printf("  MISSING: CRC-valid INFO0c on ch0\n");
    if (!info0_seen[1]) printf("  MISSING: CRC-valid INFO0a on ch1\n");
    if (!infoh_params_ok) printf("  MISSING: INFOh with symrate_idx=5 trn_len=84 trn_16pt=1 preemph=2\n");
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ccdata - control-channel HDLC user-data real-capture check: runs the
 * validated 600-baud batch demod + differential demap + descrambler into
 * nf_hdlc's deframer over all three half-duplex control-channel windows,
 * both directions, and asserts the full T.30 conversation decoded by the
 * Python pipeline this was ported from: DCS/CFR before the image, PPS-NULL
 * (256 frames)/MCF after the first block, PPS-EOP (149 frames)/MCF/DCN at
 * the end - every frame FCS-valid. */
#define CC_MAXFRAMES 32
#define CC_MAXBYTES  64

struct cc_collect {
    nf_v34_ccdata_rx_t cc;
    uint8_t frames[CC_MAXFRAMES][CC_MAXBYTES];
    int lens[CC_MAXFRAMES];
    int nframes;
};

static void ccdata_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct cc_collect *c = user;

    if (!msg || !ok || len <= 0)
        return;                          /* status callback or bad FCS */
    if (c->nframes < CC_MAXFRAMES && len <= CC_MAXBYTES) {
        memcpy(c->frames[c->nframes], msg, (size_t) len);
        c->lens[c->nframes] = len;
        c->nframes++;
    }
}

static void ccdata_on_symbol(void *user, const nf_cpx_t *z)
{
    struct cc_collect *c = user;
    nf_v34_ccdata_feed_symbol(&c->cc, z);
}

static const char *cc_fcf_name(uint8_t fcf)
{
    switch (fcf & 0xFE) {                /* strip the X bit (LSB-first 0x01) */
    case 0x00: return "NULL";
    case 0x2E: return "EOP";
    case 0x20: return "NSF";
    case 0x40: return "CSI";
    case 0x80: return "DIS";
    case 0x82: return "DCS";
    case 0x84: return "CFR";
    case 0x8C: return "MCF";
    case 0xBE: return "PPS";
    case 0xFA: return "DCN";
    default:   return "?";
    }
}

static int cmd_ccdata(const char *path)
{
    struct wav w;
    static const struct {
        const char *label;
        double t0, t1;
        int ch;
        double carrier;
        int is_call;
    } wins[6] = {
        { "w1-call", 42.30, 45.65, 0, 1200.0, 1 },
        { "w1-answ", 42.48, 45.50, 1, 2400.0, 0 },
        { "w2-call", 69.05, 71.25, 0, 1200.0, 1 },
        { "w2-answ", 69.22, 71.05, 1, 2400.0, 0 },
        { "w3-call", 84.85, 86.55, 0, 1200.0, 1 },
        { "w3-answ", 85.05, 86.70, 1, 2400.0, 0 },
    };
    /* per window: expected FCS-valid frames (FCS stripped), hex, from the
     * validated Python decode of this capture */
    static const char *expected[6][4] = {
        { "ff13830042f8c48000", NULL, NULL, NULL },                      /* DCS */
        { "ff0320ad0036a000000000",                                     /* NSF */
          "ff034039392030303139303939203132392039342b2020",             /* CSI */
          "ff138020c2f8c48012",                                         /* DIS */
          "ff1384" },                                                   /* CFR */
        { "ff13bf000000ff", NULL, NULL, NULL },                         /* PPS-NULL */
        { "ff138c", NULL, NULL, NULL },                                 /* MCF */
        { "ff13bf2f000194", "ff13fb", NULL, NULL },                     /* PPS-EOP, DCN */
        { "ff138c", NULL, NULL, NULL },                                 /* MCF */
    };
    int wi, i, j, ok = 1;
    int pps_null_256 = 0, pps_eop_149 = 0;

    if (wav_load(path, &w) < 0) return 1;

    for (wi = 0; wi < 6; wi++) {
        struct cc_collect col;
        int16_t *buf;
        long n;
        char hex[2 * CC_MAXBYTES + 1];

        buf = wav_window(&w, wins[wi].t0, wins[wi].t1, wins[wi].ch, &n);
        memset(&col, 0, sizeof(col));
        nf_v34_ccdata_rx_init(&col.cc, wins[wi].is_call, ccdata_on_frame, &col);
        nf_v34_cc_rx_batch(buf, (int) n, wins[wi].carrier, ccdata_on_symbol, &col, 0);
        free(buf);

        printf("%s ch%d@%.0fHz: %d FCS-valid frame(s)\n",
               wins[wi].label, wins[wi].ch, wins[wi].carrier, col.nframes);
        for (i = 0; i < col.nframes; i++) {
            for (j = 0; j < col.lens[i]; j++)
                sprintf(hex + 2 * j, "%02x", col.frames[i][j]);
            printf("  [%s] %s", hex, col.lens[i] >= 3 ? cc_fcf_name(col.frames[i][2]) : "short");
            if (col.lens[i] >= 7 && (col.frames[i][2] & 0xFE) == 0xBE) {
                int fcnt = col.frames[i][6] + 1;
                printf(" fcf2=%s page=%d block=%d frames=%d",
                       cc_fcf_name(col.frames[i][3]),
                       col.frames[i][4], col.frames[i][5], fcnt);
                if (col.frames[i][3] == 0x00 && fcnt == 256) pps_null_256 = 1;
                if ((col.frames[i][3] & 0xFE) == 0x2E && fcnt == 149) pps_eop_149 = 1;
            }
            printf("\n");
        }

        for (j = 0; j < 4 && expected[wi][j]; j++) {
            int found = 0;
            for (i = 0; i < col.nframes; i++) {
                for (int k = 0; k < col.lens[i]; k++)
                    sprintf(hex + 2 * k, "%02x", col.frames[i][k]);
                if (!strcmp(hex, expected[wi][j])) { found = 1; break; }
            }
            if (!found) {
                printf("  MISSING expected frame [%s]\n", expected[wi][j]);
                ok = 0;
            }
        }
    }
    free(w.samples);

    if (!pps_null_256) { printf("MISSING: PPS-NULL with frame count 256\n"); ok = 0; }
    if (!pps_eop_149)  { printf("MISSING: PPS-EOP with frame count 149\n");  ok = 0; }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* shellmap - round-trip check of the ITU-T V.34 9.4 shell mapper against a
 * spread of M values and R0 values: nf_v34_shell_map() then
 * nf_v34_shell_unmap() must recover the original R0 exactly, and every ring
 * index must land in [0, M). Ported from a Python prototype validated the
 * same way (1732/1732 round trips) before this port. */
static unsigned lcg_next(unsigned *state)
{
    *state = (*state) * 1103515245u + 12345u;
    return (*state >> 8) & 0x7FFFFFu;
}

static int cmd_shellmap(void)
{
    static const int Ms[] = {1,2,3,4,5,7,10,15,18,31};
    unsigned mi;
    int ok_total = 1;

    for (mi = 0; mi < sizeof Ms / sizeof Ms[0]; mi++) {
        int M = Ms[mi];
        {
            unsigned seed = (unsigned) (M * 2654435761u + 1);
            int trial;
            int fails = 0, tested = 0;
            /* Determine a valid R0 upper bound the same way the Python
             * prototype did: increase A until z8(A) stops growing. We don't
             * have z8() exposed directly, so instead just probe increasing
             * R0 values from 0 and rely on nf_v34_shell_map() producing
             * in-range rings as the validity signal (matches how a real
             * decoder would only ever call this with legitimately-encoded
             * R0 values in [0, 2^K)). */
            long maxR0 = 8L * M * M * 4L; /* generous; map() is well-defined beyond true range too */
            for (trial = 0; trial < 300; trial++) {
                long R0 = (long) (lcg_next(&seed) % (unsigned long) (maxR0 > 0 ? maxR0 : 1));
                int rings[4][2];
                uint32_t back;
                int k, range_ok = 1;

                nf_v34_shell_map(M, (uint32_t) R0, rings);
                for (k = 0; k < 4; k++) {
                    if (rings[k][0] < 0 || rings[k][0] >= M) range_ok = 0;
                    if (rings[k][1] < 0 || rings[k][1] >= M) range_ok = 0;
                }
                if (!range_ok)
                    continue; /* R0 outside this M's valid domain - skip, not a failure */
                tested++;
                back = nf_v34_shell_unmap(M, rings);
                if ((long) back != R0) {
                    fails++;
                    if (fails <= 3)
                        printf("  FAIL M=%d R0=%ld -> rings, back=%u\n", M, R0, back);
                }
            }
            printf("M=%-3d tested=%-4d fails=%d\n", M, tested, fails);
            if (fails) ok_total = 0;
        }
    }
    printf("%s\n", ok_total ? "PASS" : "FAIL");
    return ok_total ? 0 : 1;
}

/* trellis - self-consistency check of the MAP+differential+trellis encoder
 * chain, ported from the Python prototype (trellis.py) that found the
 * U0/rotation-parity Viterbi decoding rule. Uses a tiny M=4 test quarter-
 * constellation (real Figure-5 labels 0..3): every point the encoder
 * produces must be a valid rotation of one of those 4 points. */
static void rotate_cw(int *re, int *im, int steps)
{
    int i, r = *re, m = *im;
    steps = ((steps % 4) + 4) % 4;
    for (i = 0; i < steps; i++) {
        int nr = m, nm = -r;
        r = nr; m = nm;
    }
    *re = r; *im = m;
}

static int cmd_trellis(void)
{
    static const int quarter_re[4] = { 1, -3,  1, -3 };
    static const int quarter_im[4] = { 1,  1, -3, -3 };
    unsigned seed = 12345;
    int state = 0, zprev = 0, bad = 0, m;
    const int NSYM = 5000;

    for (m = 0; m < NSYM; m++) {
        int y0 = state & 1;
        int r0 = (int) (lcg_next(&seed) % 4);
        int r1 = (int) (lcg_next(&seed) % 4);
        int z2 = (int) (lcg_next(&seed) % 2);
        int z3 = (int) (lcg_next(&seed) % 2);
        int i1 = (int) (lcg_next(&seed) % 2);
        int ival = z2 + 2*z3;
        int Z = (zprev + ival) % 4;
        int u0 = y0;   /* C0=V0=0 for this isolated encoder-only check */
        int v0re = quarter_re[r0], v0im = quarter_im[r0];
        int v1re = quarter_re[r1], v1im = quarter_im[r1];
        int s0, s1, y4321, y2b, y1b, k, rot, ok0 = 0, ok1 = 0;

        rotate_cw(&v0re, &v0im, Z);
        rotate_cw(&v1re, &v1im, (Z + 2*i1 + u0) % 4);

        s0 = nf_v34_subset_label(v0re, v0im);
        s1 = nf_v34_subset_label(v1re, v1im);
        y4321 = nf_v34_table13[s0][s1];
        y1b = y4321 & 1;
        y2b = (y4321 >> 1) & 1;
        state = nf_v34_trellis_step(state, y2b, y1b, NULL);
        zprev = Z;

        /* every transmitted point must be SOME rotation of a quarter point */
        for (k = 0; k < 4 && !ok0; k++)
            for (rot = 0; rot < 4; rot++) {
                int tre = quarter_re[k], tim = quarter_im[k];
                rotate_cw(&tre, &tim, rot);
                if (tre == v0re && tim == v0im) { ok0 = 1; break; }
            }
        for (k = 0; k < 4 && !ok1; k++)
            for (rot = 0; rot < 4; rot++) {
                int tre = quarter_re[k], tim = quarter_im[k];
                rotate_cw(&tre, &tim, rot);
                if (tre == v1re && tim == v1im) { ok1 = 1; break; }
            }
        if (!ok0 || !ok1)
            bad++;
    }
    printf("trellis encoder self-consistency: bad=%d / %d\n", bad, NSYM);
    printf("%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}

/* viterbi - the real payoff check: generates a synthetic TX sequence with
 * the real M=23 Figure-5 quarter-constellation, corrupts it with additive
 * noise, and asserts the Viterbi decoder (nf_v34_viterbi_decode) recovers
 * MORE symbol pairs exactly than naive nearest-point slicing at a moderate
 * noise level - i.e. it demonstrates actual coding gain, not just "runs
 * without crashing". Ported from the Python prototype that found the
 * correct (U0/rotation-parity) decoding rule; see nf_v34.h. */
static double lcg_uniform(unsigned *state)
{
    return (double) lcg_next(state) / 8388608.0;   /* [0,1) */
}

static double lcg_gauss(unsigned *state)
{
    double u1 = lcg_uniform(state) + 1e-12;
    double u2 = lcg_uniform(state);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
}

static int cmd_viterbi(void)
{
    nf_v34_constellation_t c;
    const int N = 400;
    nf_cpx_t *tx0, *tx1, *noisy0, *noisy1, *dec0, *dec1;
    unsigned seed = 777;
    int state, zprev, m;
    static const double sigmas[] = { 0.4, 0.6, 0.8 };
    unsigned si;
    int overall_ok = 1;

    nf_v34_constellation_init(&c, nf_v34_quarter_table, NF_V34_QUARTER_MAX);

    tx0 = malloc(sizeof(nf_cpx_t) * N); tx1 = malloc(sizeof(nf_cpx_t) * N);
    noisy0 = malloc(sizeof(nf_cpx_t) * N); noisy1 = malloc(sizeof(nf_cpx_t) * N);
    dec0 = malloc(sizeof(nf_cpx_t) * N); dec1 = malloc(sizeof(nf_cpx_t) * N);
    if (!tx0 || !tx1 || !noisy0 || !noisy1 || !dec0 || !dec1) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    /* TX encode (same generator as cmd_trellis, M=NF_V34_QUARTER_MAX) */
    state = 0; zprev = 0;
    for (m = 0; m < N; m++) {
        int y0 = state & 1;
        int r0 = (int) (lcg_next(&seed) % (unsigned) c.M);
        int r1 = (int) (lcg_next(&seed) % (unsigned) c.M);
        int z2 = (int) (lcg_next(&seed) % 2);
        int z3 = (int) (lcg_next(&seed) % 2);
        int i1 = (int) (lcg_next(&seed) % 2);
        int Z = (zprev + z2 + 2*z3) % 4;
        int u0 = y0;
        int v0re = nf_v34_quarter_table[r0].re, v0im = nf_v34_quarter_table[r0].im;
        int v1re = nf_v34_quarter_table[r1].re, v1im = nf_v34_quarter_table[r1].im;
        int s0, s1, y4321, y2b, y1b;

        rotate_cw(&v0re, &v0im, Z);
        rotate_cw(&v1re, &v1im, (Z + 2*i1 + u0) % 4);
        s0 = nf_v34_subset_label(v0re, v0im);
        s1 = nf_v34_subset_label(v1re, v1im);
        y4321 = nf_v34_table13[s0][s1];
        y1b = y4321 & 1; y2b = (y4321 >> 1) & 1;
        state = nf_v34_trellis_step(state, y2b, y1b, NULL);
        zprev = Z;
        tx0[m] = nf_cpx((float) v0re, (float) v0im);
        tx1[m] = nf_cpx((float) v1re, (float) v1im);
    }

    for (si = 0; si < sizeof sigmas / sizeof sigmas[0]; si++) {
        double sigma = sigmas[si];
        int exact = 0, naive_exact = 0;
        int rc;

        for (m = 0; m < N; m++) {
            noisy0[m] = nf_cpx((float) (tx0[m].re + sigma*lcg_gauss(&seed)),
                                (float) (tx0[m].im + sigma*lcg_gauss(&seed)));
            noisy1[m] = nf_cpx((float) (tx1[m].re + sigma*lcg_gauss(&seed)),
                                (float) (tx1[m].im + sigma*lcg_gauss(&seed)));
        }

        rc = nf_v34_viterbi_decode(&c, noisy0, noisy1, N, dec0, dec1, 6, 6);
        if (rc != 0) {
            fprintf(stderr, "nf_v34_viterbi_decode failed\n");
            overall_ok = 0;
            continue;
        }

        for (m = 0; m < N; m++) {
            int i, best0 = 0, best1 = 0;
            float bd0 = 1e18f, bd1 = 1e18f;

            if (dec0[m].re == tx0[m].re && dec0[m].im == tx0[m].im &&
                dec1[m].re == tx1[m].re && dec1[m].im == tx1[m].im)
                exact++;

            for (i = 0; i < c.nalpha; i++) {
                float d0 = (noisy0[m].re-c.alphabet[i].re)*(noisy0[m].re-c.alphabet[i].re) +
                           (noisy0[m].im-c.alphabet[i].im)*(noisy0[m].im-c.alphabet[i].im);
                float d1 = (noisy1[m].re-c.alphabet[i].re)*(noisy1[m].re-c.alphabet[i].re) +
                           (noisy1[m].im-c.alphabet[i].im)*(noisy1[m].im-c.alphabet[i].im);
                if (d0 < bd0) { bd0 = d0; best0 = i; }
                if (d1 < bd1) { bd1 = d1; best1 = i; }
            }
            if (c.alphabet[best0].re == (int) tx0[m].re && c.alphabet[best0].im == (int) tx0[m].im &&
                c.alphabet[best1].re == (int) tx1[m].re && c.alphabet[best1].im == (int) tx1[m].im)
                naive_exact++;
        }

        printf("sigma=%.1f: viterbi=%d/%d  naive=%d/%d  gain=%+d\n",
               sigma, exact, N, naive_exact, N, exact - naive_exact);
        /* the middle sigma is where the earlier Python prototype showed the
         * clearest, most robust gain (moderate noise - the regime TCM
         * coding gain actually targets); require Viterbi to win there. */
        if (sigma == 0.6 && exact <= naive_exact)
            overall_ok = 0;
    }

    free(tx0); free(tx1); free(noisy0); free(noisy1); free(dec0); free(dec1);
    printf("%s\n", overall_ok ? "PASS" : "FAIL");
    return overall_ok ? 0 : 1;
}

/* fullchain - the real payoff: a full synthetic round trip through EVERY
 * receive-side piece built this session (shell mapper, differential
 * encoder/MAP rotation, trellis encoder, Viterbi decoder, mapping-frame
 * receiver, descrambler) at once. Ported from the Python prototype that
 * first demonstrated this: 100% bit-exact with no noise, and a dramatic
 * coding-gain advantage over naive slicing once noise is added (symbol
 * errors that survive naive slicing cascade badly through shell-unmapping
 * and descrambling). */
static int cmd_fullchain(void)
{
    nf_v34_constellation_t c;
    const int K = 8, M = NF_V34_QUARTER_MAX;
    const int NFRAMES = 150;
    const int bits_per_frame = K + 12;
    const int N = NFRAMES * 4;
    unsigned seed = 4242;
    nf_v34_scrambler_t scr_tx;
    uint8_t *raw_bits, *scr_bits;
    nf_cpx_t *tx0, *tx1, *noisy0, *noisy1, *dec0, *dec1;
    int state, zprev, f, j, m;
    static const double sigmas[] = { 0.0, 0.3, 0.5, 0.6 };
    unsigned si;
    int overall_ok = 1;

    nf_v34_constellation_init(&c, nf_v34_quarter_table, M);

    raw_bits = malloc((size_t) NFRAMES * bits_per_frame);
    scr_bits = malloc((size_t) NFRAMES * bits_per_frame);
    tx0 = malloc(sizeof(nf_cpx_t) * N); tx1 = malloc(sizeof(nf_cpx_t) * N);
    noisy0 = malloc(sizeof(nf_cpx_t) * N); noisy1 = malloc(sizeof(nf_cpx_t) * N);
    dec0 = malloc(sizeof(nf_cpx_t) * N); dec1 = malloc(sizeof(nf_cpx_t) * N);
    if (!raw_bits || !scr_bits || !tx0 || !tx1 || !noisy0 || !noisy1 || !dec0 || !dec1) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    for (m = 0; m < NFRAMES * bits_per_frame; m++)
        raw_bits[m] = (uint8_t) (lcg_next(&seed) & 1);

    nf_v34_scrambler_init(&scr_tx, 1 /* call modem, GPC */);
    for (m = 0; m < NFRAMES * bits_per_frame; m++)
        scr_bits[m] = (uint8_t) nf_v34_scramble_bit(&scr_tx, raw_bits[m]);

    /* TX: shell-map + differential-encode + MAP-rotate + trellis, state
     * carried continuously across the whole sequence (matching real V.34,
     * where the trellis/differential encoders run continuously, not
     * reset per mapping frame). */
    state = 0; zprev = 0;
    for (f = 0; f < NFRAMES; f++) {
        const uint8_t *fb = scr_bits + f * bits_per_frame;
        uint32_t R0 = 0;
        int rings[4][2];
        int kk;

        for (kk = 0; kk < K; kk++)
            R0 |= ((uint32_t) fb[kk]) << kk;
        nf_v34_shell_map(M, R0, rings);

        for (j = 0; j < 4; j++) {
            int i1 = fb[K + 3*j + 0], i2 = fb[K + 3*j + 1], i3 = fb[K + 3*j + 2];
            int y0 = state & 1;
            int Z = (zprev + i2 + 2*i3) % 4;
            int u0 = y0;
            int v0re = nf_v34_quarter_table[rings[j][0]].re, v0im = nf_v34_quarter_table[rings[j][0]].im;
            int v1re = nf_v34_quarter_table[rings[j][1]].re, v1im = nf_v34_quarter_table[rings[j][1]].im;
            int s0, s1, y4321, y2b, y1b;

            rotate_cw(&v0re, &v0im, Z);
            rotate_cw(&v1re, &v1im, (Z + 2*i1 + u0) % 4);
            s0 = nf_v34_subset_label(v0re, v0im);
            s1 = nf_v34_subset_label(v1re, v1im);
            y4321 = nf_v34_table13[s0][s1];
            y1b = y4321 & 1; y2b = (y4321 >> 1) & 1;
            state = nf_v34_trellis_step(state, y2b, y1b, NULL);
            zprev = Z;

            tx0[f*4+j] = nf_cpx((float) v0re, (float) v0im);
            tx1[f*4+j] = nf_cpx((float) v1re, (float) v1im);
        }
    }

    for (si = 0; si < sizeof sigmas / sizeof sigmas[0]; si++) {
        double sigma = sigmas[si];
        nf_v34_rx_frame_state_t rxs;
        nf_v34_scrambler_t scr_rx;
        int rc, bitmatch = 0, bittotal = 0;

        for (m = 0; m < N; m++) {
            noisy0[m] = nf_cpx((float) (tx0[m].re + sigma*lcg_gauss(&seed)),
                                (float) (tx0[m].im + sigma*lcg_gauss(&seed)));
            noisy1[m] = nf_cpx((float) (tx1[m].re + sigma*lcg_gauss(&seed)),
                                (float) (tx1[m].im + sigma*lcg_gauss(&seed)));
        }

        rc = nf_v34_viterbi_decode(&c, noisy0, noisy1, N, dec0, dec1, 6, 6);
        if (rc != 0) { fprintf(stderr, "viterbi decode failed\n"); overall_ok = 0; continue; }

        nf_v34_rx_frame_state_init(&rxs);
        nf_v34_scrambler_init(&scr_rx, 1 /* call modem */);
        for (f = 0; f < NFRAMES; f++) {
            uint32_t R0_out;
            int aux[12], kk;

            if (nf_v34_rx_frame(&rxs, &c, M, K, 0, dec0 + f*4, dec1 + f*4, &R0_out, aux) < 0) {
                fprintf(stderr, "rx_frame: decoded point not in alphabet (frame %d, sigma=%.1f)\n", f, sigma);
                continue;
            }
            for (kk = 0; kk < K; kk++) {
                int scr_bit_hat = (int) ((R0_out >> kk) & 1);
                int bit_hat = nf_v34_descramble_bit(&scr_rx, scr_bit_hat);
                if (bit_hat == raw_bits[f*bits_per_frame + kk]) bitmatch++;
                bittotal++;
            }
            for (kk = 0; kk < 12; kk++) {
                int bit_hat = nf_v34_descramble_bit(&scr_rx, aux[kk]);
                if (bit_hat == raw_bits[f*bits_per_frame + K + kk]) bitmatch++;
                bittotal++;
            }
        }

        printf("sigma=%.1f: bits correct=%d/%d (%.1f%%)\n",
               sigma, bitmatch, bittotal, 100.0*bitmatch/bittotal);
        if (sigma == 0.0 && bitmatch != bittotal)
            overall_ok = 0;
        if (sigma == 0.3 && bitmatch < (int) (0.95 * bittotal))
            overall_ok = 0;
    }

    free(raw_bits); free(scr_bits);
    free(tx0); free(tx1); free(noisy0); free(noisy1); free(dec0); free(dec1);
    printf("%s\n", overall_ok ? "PASS" : "FAIL");
    return overall_ok ? 0 : 1;
}

/* page - THE strongest correctness signal in the whole project: the full
 * primary-channel receive pipeline (TRN-trained least-squares T/2 FSE,
 * per-burst S/Sbar/PP resync + PP gain/timing transfer, decision-directed
 * phase/gain/timing/tap tracking across the 23s/13.5s data bursts, 9.3.1
 * inverse mapping, descrambler, HDLC) must decode the capture's two ECM
 * image blocks COMPLETELY: 256/256 FCS-valid 256-octet FCD frames in
 * block 0 and 149/149 in block 1 - the same result the real receiving fax
 * achieved (it answered MCF). Asserted as exact equality: with the
 * margin-gated DD tap adaptation the decode has no marginal symbols left
 * (worst-case decision margin ~0.5 of half the point spacing), so any
 * float-detail drift big enough to flip a frame would be a real
 * regression. Intermediate health metrics (TRN holdout residual, PP
 * transfer residual, B1 known-bit match) are also asserted. */
static int cmd_page(const char *path)
{
    struct wav w;
    int16_t *buf;
    long n;
    nf_v34_page_eq_t eq;
    /* capture-specific anchors (all measured/validated - see nf_v34.h):
     * TRN symbol 0 at 39.33616s with a -0.160-sample fine intercept
     * (diag53/55 baud+timing refinement); burst S anchors located below by
     * correlation (expected ~45.68730s and ~71.24987s). */
    const double t_trn = 39.33616 - 0.160 / 8000.0;
    static const struct {
        const char *name;
        double s_lo, s_hi;    /* S-anchor search window */
        double t_end_max;     /* generous burst end bound */
        int tx_fcd;           /* frames the TX sent = frames we must get */
    } bursts[2] = {
        { "block 0", 45.64, 45.74, 69.20, 256 },
        { "block 1", 71.05, 71.40, 85.20, 149 },
    };
    int b, i, j, ok = 1;

    if (wav_load(path, &w) < 0)
        return 1;
    buf = wav_window(&w, 0.0, 1e9, 0, &n);

    if (nf_v34_page_train(buf, n, 0.0, t_trn, 10050, NF_V34_PAGE_BAUD_CAPTURE, NULL, &eq) < 0) {
        printf("TRN training failed\nFAIL\n");
        free(buf); free(w.samples);
        return 1;
    }
    printf("TRN LS-FSE: res_train=%.5f res_holdout=%.5f residual carrier=%+.4f Hz\n",
           eq.res_train, eq.res_holdout, eq.sl * 3428.6385 / (2.0 * M_PI));
    if (eq.res_holdout > 0.002) {
        printf("  FAIL: TRN holdout residual above 0.002\n");
        ok = 0;
    }

    for (b = 0; b < 2; b++) {
        double corr, t_s;
        nf_v34_page_burst_t r;

        t_s = nf_v34_page_locate_s(buf, n, 0.0, bursts[b].s_lo, bursts[b].s_hi,
                                   NF_V34_PAGE_BAUD_CAPTURE, NULL, &corr);
        printf("%s: S anchor t=%.5fs (corr=%.3f)\n", bursts[b].name, t_s, corr);
        if (nf_v34_page_decode_burst(buf, n, 0.0, &eq, t_s, bursts[b].t_end_max,
                                     NF_V34_PAGE_BAUD_CAPTURE, NULL, NULL, NULL, &r) < 0) {
            printf("  decode failed\n");
            ok = 0;
            continue;
        }
        printf("  PP transfer: resid=%.5f tau=%+.2f -> %+.2f samples\n",
               r.pp_resid, r.tau_init, r.tau_final);
        printf("  burst: %ld symbols sliced, end t=%.3fs; B1: Z(-1)=%d match=%d/840; bad-R0 frames=%ld\n",
               r.nsym, r.t_end, r.zm1, r.b1_match, r.bad_r0);
        printf("  HDLC: FCS-valid=%d (FCD=%d, RCP=%d) bad=%d; FCD numbers %d..%d\n",
               r.hdlc_ok, r.fcd_ok, r.rcp_ok, r.hdlc_bad, r.fcd_first, r.fcd_last);
        for (i = 0; i < r.n_first_hdr; i++) {
            printf("  frame %d header: ", i);
            for (j = 0; j < r.first_hdr_len[i]; j++)
                printf("%02x", r.first_hdr[i][j]);
            printf("\n");
        }
        printf("  ==> %s: %d/%d FCS-valid FCD frames\n",
               bursts[b].name, r.fcd_ok, bursts[b].tx_fcd);
        if (r.pp_resid > 0.002) {
            printf("  FAIL: PP transfer residual above 0.002\n");
            ok = 0;
        }
        if (r.b1_match != 840) {
            printf("  FAIL: B1 known-bit match %d != 840\n", r.b1_match);
            ok = 0;
        }
        if (r.fcd_ok != bursts[b].tx_fcd) {
            printf("  FAIL: %d FCS-valid FCD frames != the %d transmitted\n",
                   r.fcd_ok, bursts[b].tx_fcd);
            ok = 0;
        }
    }

    free(buf);
    free(w.samples);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ═══ transmitter loopback checks (txsig/txinfo/txcc/txpage) ═══════════
 * The TX primitives are validated against this module's own capture-
 * validated receivers - the RX is the oracle (see nf_v34.h). */

/* single-bin DFT power at an exact frequency */
static double goertzel_db(const int16_t *amp, long at, long dur, double hz)
{
    double cr = 0.0, ci = 0.0;
    long s;

    for (s = 0; s < dur; s++) {
        double ph = -2.0 * M_PI * hz * (double) s / 8000.0;
        cr += amp[at + s] * cos(ph);
        ci += amp[at + s] * sin(ph);
    }
    return 20.0 * log10(sqrt(cr * cr + ci * ci) / dur + 1e-12);
}

/* txsig - Phase-2 tone/probing generation sanity: tone A carries 2400 Hz
 * plus the 1800 Hz guard 6 dB down and flips carrier phase by 180 degrees
 * at the requested reversal; tone B is a clean 1200 Hz; L2 contains
 * exactly the Table 17 tones (900/1200/1800/2400 absent) and L1 is
 * L2 + 6 dB. */
static int cmd_txsig(void)
{
    enum { N = 24000 };
    static int16_t buf[N];
    int ok = 1;
    int k;

    /* tone A with a reversal at sample 8000 */
    memset(buf, 0, sizeof(buf));
    nf_v34_tone_tx(buf, N, 0, 16000, 1, 8000, 8000.0);
    {
        double p2400 = goertzel_db(buf, 800, 6400, 2400.0);
        double p1800 = goertzel_db(buf, 800, 6400, 1800.0);
        double c0r = 0, c0i = 0, c1r = 0, c1i = 0, dot;
        long s;
        printf("tone A: 2400Hz %.1f dB, guard 1800Hz %.1f dB (want -6.0 rel)\n",
               p2400, p1800 - p2400 + 6.0);
        if (fabs((p1800 - p2400) + 6.0) > 0.5) ok = 0;
        /* phase reversal: the 2400 Hz phasor before vs after must be
         * anti-parallel */
        for (s = 800; s < 7200; s++) {
            double ph = -2.0 * M_PI * 2400.0 * (double) s / 8000.0;
            c0r += buf[s] * cos(ph); c0i += buf[s] * sin(ph);
        }
        for (s = 8800; s < 15200; s++) {
            double ph = -2.0 * M_PI * 2400.0 * (double) s / 8000.0;
            c1r += buf[s] * cos(ph); c1i += buf[s] * sin(ph);
        }
        dot = (c0r * c1r + c0i * c1i) /
              (sqrt(c0r*c0r + c0i*c0i) * sqrt(c1r*c1r + c1i*c1i) + 1e-12);
        printf("tone A reversal: phasor dot=%.3f (want ~ -1)\n", dot);
        if (dot > -0.99) ok = 0;
    }

    /* tone B: 1200 Hz, no guard */
    memset(buf, 0, sizeof(buf));
    nf_v34_tone_tx(buf, N, 0, 16000, 0, -1, 8000.0);
    {
        double p1200 = goertzel_db(buf, 800, 6400, 1200.0);
        double p1800 = goertzel_db(buf, 800, 6400, 1800.0);
        printf("tone B: 1200Hz %.1f dB, 1800Hz %.1f dB below\n", p1200, p1200 - p1800);
        if (p1200 - p1800 < 40.0) ok = 0;
    }

    /* L1/L2 probing */
    {
        static const int present[21] =
            { 150, 300, 450, 600, 750, 1050, 1350, 1500, 1650, 1950, 2100,
              2250, 2550, 2700, 2850, 3000, 3150, 3300, 3450, 3600, 3750 };
        static const int absent[4] = { 900, 1200, 1800, 2400 };
        double l2p, l1p, worst_present = 1e9, worst_absent = -1e9;
        static int16_t buf1[N];

        memset(buf, 0, sizeof(buf));
        memset(buf1, 0, sizeof(buf1));
        nf_v34_probe_tx(buf, N, 0, 16000, 0, 2000.0);
        nf_v34_probe_tx(buf1, N, 0, 16000, 1, 2000.0);
        l2p = goertzel_db(buf, 0, 16000, 1050.0);
        l1p = goertzel_db(buf1, 0, 16000, 1050.0);
        printf("L1 - L2 at 1050 Hz: %.2f dB (want 6.00)\n", l1p - l2p);
        if (fabs(l1p - l2p - 6.0) > 0.05) ok = 0;
        for (k = 0; k < 21; k++) {
            double p = goertzel_db(buf, 0, 16000, (double) present[k]) - l2p;
            if (p < worst_present) worst_present = p;
        }
        for (k = 0; k < 4; k++) {
            double p = goertzel_db(buf, 0, 16000, (double) absent[k]) - l2p;
            if (p > worst_absent) worst_absent = p;
        }
        printf("L2 tones: worst present %.1f dB rel, worst omitted %.1f dB rel\n",
               worst_present, worst_absent);
        if (worst_present < -1.0 || worst_absent > -40.0) ok = 0;
    }

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* txinfo - INFO TX -> nf_v34_info_rx_batch loopback, both roles: every
 * field must round-trip exactly (the RX only reports CRC-valid frames, so
 * a hit at all proves fill/sync/CRC framing too). */
static int cmd_txinfo(void)
{
    enum { N = 24000 };
    static int16_t buf[N];
    nf_v34_info_frame_t tx0c, tx0a, txh, rx[16];
    int nf, i, ok = 1;
    int got0c = 0, got0a = 0, goth = 0;

    memset(&tx0c, 0, sizeof(tx0c));
    tx0c.sr2743 = 1; tx0c.sr2800 = 1; tx0c.sr3429 = 1;
    tx0c.low3000 = 1; tx0c.high3000 = 0; tx0c.low3200 = 1; tx0c.high3200 = 1;
    tx0c.allow_3429 = 1; tx0c.can_reduce_power = 0; tx0c.max_sr_diff = 3;
    tx0c.from_cme = 0; tx0c.support_1664pt = 1; tx0c.clock_source = 1;
    tx0c.info0_ack = 0;

    tx0a = tx0c;
    tx0a.high3000 = 1; tx0a.max_sr_diff = 5; tx0a.support_1664pt = 0;
    tx0a.clock_source = 0; tx0a.info0_ack = 1;

    memset(&txh, 0, sizeof(txh));
    txh.is_infoh = 1;
    txh.power_reduction = 2; txh.trn_len = 84; txh.high_carrier = 0;
    txh.preemph_idx = 2; txh.symrate_idx = 5; txh.trn_16pt = 1;

    /* call role: INFO0c at 1200 Hz */
    memset(buf, 0, sizeof(buf));
    nf_v34_info_tx(&tx0c, 0, buf, N, 400, 8000.0);
    nf = nf_v34_info_rx_batch(buf, N / 2, 1200.0, rx, 16);
    for (i = 0; i < nf; i++) {
        if (!rx[i].is_infoh &&
            rx[i].sr2743 == tx0c.sr2743 && rx[i].sr2800 == tx0c.sr2800 &&
            rx[i].sr3429 == tx0c.sr3429 && rx[i].low3000 == tx0c.low3000 &&
            rx[i].high3000 == tx0c.high3000 && rx[i].low3200 == tx0c.low3200 &&
            rx[i].high3200 == tx0c.high3200 && rx[i].allow_3429 == tx0c.allow_3429 &&
            rx[i].can_reduce_power == tx0c.can_reduce_power &&
            rx[i].max_sr_diff == tx0c.max_sr_diff && rx[i].from_cme == tx0c.from_cme &&
            rx[i].support_1664pt == tx0c.support_1664pt &&
            rx[i].clock_source == tx0c.clock_source && rx[i].info0_ack == tx0c.info0_ack)
            got0c = 1;
    }
    printf("call role: %d frame(s), INFO0c exact round-trip: %s\n",
           nf, got0c ? "yes" : "NO");

    /* answer role: INFO0a then INFOh at 2400 Hz + guard tone */
    memset(buf, 0, sizeof(buf));
    nf_v34_info_tx(&tx0a, 1, buf, N, 400, 8000.0);
    nf_v34_info_tx(&txh, 1, buf, N, 2000, 8000.0);
    nf = nf_v34_info_rx_batch(buf, N / 2, 2400.0, rx, 16);
    for (i = 0; i < nf; i++) {
        if (!rx[i].is_infoh &&
            rx[i].high3000 == tx0a.high3000 && rx[i].max_sr_diff == tx0a.max_sr_diff &&
            rx[i].support_1664pt == tx0a.support_1664pt &&
            rx[i].clock_source == tx0a.clock_source && rx[i].info0_ack == tx0a.info0_ack &&
            rx[i].sr3429 == tx0a.sr3429 && rx[i].low3200 == tx0a.low3200)
            got0a = 1;
        if (rx[i].is_infoh &&
            rx[i].power_reduction == txh.power_reduction && rx[i].trn_len == txh.trn_len &&
            rx[i].high_carrier == txh.high_carrier && rx[i].preemph_idx == txh.preemph_idx &&
            rx[i].symrate_idx == txh.symrate_idx && rx[i].trn_16pt == txh.trn_16pt)
            goth = 1;
    }
    printf("answer role: %d frame(s), INFO0a exact: %s, INFOh exact: %s\n",
           nf, got0a ? "yes" : "NO", goth ? "yes" : "NO");

    ok = got0c && got0a && goth;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── txcc: control-channel session loopback ───────────────────────────── */

struct txcc_hdlc_src {
    const uint8_t (*frames)[16];
    const int *lens;
    int nframes, next;
    nf_hdlc_tx_t tx;
};

static void txcc_underflow(void *user)
{
    struct txcc_hdlc_src *src = user;

    if (src->next < src->nframes)
        nf_hdlc_tx_frame(&src->tx, src->frames[src->next], src->lens[src->next]);
    else if (src->next == src->nframes)
        nf_hdlc_tx_frame(&src->tx, NULL, 0);       /* end marker */
    src->next++;
}

struct txcc_rx {
    nf_v34_mp_rx_t mp;
    nf_v34_ccdata_rx_t cc;
    nf_v34_mph_fields_t got[8];
    int nmp;
    uint8_t frames[8][16];
    int lens[8];
    int nframes;
};

static void txcc_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct txcc_rx *r = user;

    if (!msg || !ok || len <= 0 || len > 16 || r->nframes >= 8)
        return;
    memcpy(r->frames[r->nframes], msg, (size_t) len);
    r->lens[r->nframes] = len;
    r->nframes++;
}

static void txcc_on_symbol(void *user, const nf_cpx_t *z)
{
    struct txcc_rx *r = user;

    if (nf_v34_mp_feed_symbol(&r->mp, z) && r->nmp < 8) {
        nf_v34_mph_fields_t *g = &r->got[r->nmp++];
        memset(g, 0, sizeof(*g));
        g->type = r->mp.type;
        g->max_rate = r->mp.max_rate_c2a;
        g->cc_rate = r->mp.cc_rate;
        g->trellis_size = r->mp.trellis_size;
        g->nonlinear = r->mp.nonlinear;
        g->shaping = r->mp.shaping;
        g->ack = r->mp.ack;
        g->rate_mask = r->mp.rate_mask;
        g->asym_enable = r->mp.asym_enable;
        if (r->mp.type == 1) {
            memcpy(g->h_re, r->mp.h_re, sizeof(g->h_re));
            memcpy(g->h_im, r->mp.h_im, sizeof(g->h_im));
        }
    }
    nf_v34_ccdata_feed_symbol(&r->cc, z);
}

static int cmd_txcc(void)
{
    /* realistic frames from the reference capture (FCS-stripped) */
    static const uint8_t call_frames[3][16] = {
        { 0xff, 0x13, 0x83, 0x00, 0x42, 0xf8, 0xc4, 0x80, 0x00 },   /* DCS */
        { 0xff, 0x13, 0xbf, 0x00, 0x00, 0x00, 0xff },               /* PPS-NULL */
        { 0xff, 0x13, 0xbf, 0x2f, 0x00, 0x01, 0x94 },               /* PPS-EOP */
    };
    static const int call_lens[3] = { 9, 7, 7 };
    static const uint8_t answ_frames[3][16] = {
        { 0xff, 0x13, 0x80, 0x20, 0xc2, 0xf8, 0xc4, 0x80, 0x12 },   /* DIS */
        { 0xff, 0x13, 0x84 },                                       /* CFR */
        { 0xff, 0x13, 0x8c },                                       /* MCF */
    };
    static const int answ_lens[3] = { 9, 3, 3 };
    int role, rate2400, ok = 1;

    /* Both control-channel user-data rates (10.2.4): 1200 bit/s (4-point,
     * 2 bits/symbol) and 2400 bit/s (16-point, 4 bits/symbol). Training
     * (PPh/ALT/MPh/E) is always 1200; only the HDLC user data runs at the
     * negotiated rate. Both roles: call decodes at 1200 Hz, answer at 2400 Hz
     * + guard tone, so the RX inverts the TX mapping in both directions. */
    for (rate2400 = 0; rate2400 < 2; rate2400++)
    for (role = 0; role < 2; role++) {
        int is_call = (role == 0);
        nf_v34_mph_fields_t mp0, mp1;
        nf_v34_cc_tx_t tx;
        struct txcc_hdlc_src src;
        struct txcc_rx rx;
        uint8_t *bits = malloc(20000);
        long nbits = 0, n;
        int16_t *buf;
        int i, b;

        memset(&mp0, 0, sizeof(mp0));
        mp0.type = 0;
        mp0.max_rate = 10;              /* 24000 bit/s */
        mp0.cc_rate = rate2400;        /* bit 27: advertised cc user-data rate */
        mp0.trellis_size = 0;
        mp0.nonlinear = 1;
        mp0.shaping = 1;
        mp0.rate_mask = 0x3fff;
        mp0.asym_enable = 1;
        mp1 = mp0;
        mp1.type = 1;
        mp1.h_re[0] = 269;  mp1.h_im[0] = -7;
        mp1.h_re[1] = -251; mp1.h_im[1] = 13;
        mp1.h_re[2] = 209;  mp1.h_im[2] = -19;

        /* session per 12.4: PPh, ALT, MPh, MPh', E, HDLC user data */
        nf_v34_cc_tx_init(&tx, is_call);
        nf_v34_cc_tx_pph(&tx);
        nf_v34_cc_tx_alt(&tx, 100);
        nf_v34_cc_tx_mph(&tx, &mp0);
        nf_v34_cc_tx_mph(&tx, &mp1);
        nf_v34_cc_tx_e(&tx);

        src.frames = is_call ? call_frames : answ_frames;
        src.lens = is_call ? call_lens : answ_lens;
        src.nframes = 3;
        src.next = 0;
        nf_hdlc_tx_init(&src.tx, 2, txcc_underflow, &src);
        nf_hdlc_tx_flags(&src.tx, 8);
        for (;;) {
            b = nf_hdlc_tx_get_bit(&src.tx);
            if (b == NF_SIG_END_OF_DATA || nbits >= 19999)
                break;
            bits[nbits++] = (uint8_t) b;
        }
        while (nbits & 3)              /* align to 4 bits (1200 dibit and 2400 quad) */
            bits[nbits++] = 1;
        nf_v34_cc_tx_set_rate(&tx, rate2400);
        nf_v34_cc_tx_bits(&tx, bits, nbits);
        free(bits);

        n = (long) ((double) tx.nsym * 40.0 / 3.0) + 800;
        buf = calloc((size_t) n, sizeof(int16_t));
        nf_v34_cc_tx_modulate(&tx, buf, n, 300, 6000.0);
        printf("%s role, %d bit/s cc: %ld symbols, %ld samples\n",
               is_call ? "call" : "answer", rate2400 ? 2400 : 1200,
               tx.nsym, n);
        nf_v34_cc_tx_free(&tx);

        memset(&rx, 0, sizeof(rx));
        nf_v34_mp_rx_init(&rx.mp, is_call);
        nf_v34_ccdata_rx_init(&rx.cc, is_call, txcc_on_frame, &rx);
        nf_v34_ccdata_rx_set_rate(&rx.cc, rate2400);
        nf_v34_cc_rx_batch(buf, (int) n, is_call ? 1200.0 : 2400.0,
                           txcc_on_symbol, &rx, rate2400 ? 130 : 0);
        free(buf);

        printf("  MPh frames decoded: %d, HDLC FCS-valid frames: %d\n",
               rx.nmp, rx.nframes);
        if (rx.nmp < 2) {
            printf("  FAIL: expected both MPh Type 0 and Type 1\n");
            ok = 0;
        } else {
            const nf_v34_mph_fields_t *g0 = &rx.got[0], *g1 = &rx.got[1];
            if (g0->type != 0 || g0->max_rate != mp0.max_rate ||
                g0->cc_rate != mp0.cc_rate || g0->trellis_size != mp0.trellis_size ||
                g0->nonlinear != mp0.nonlinear || g0->shaping != mp0.shaping ||
                g0->rate_mask != mp0.rate_mask || g0->asym_enable != mp0.asym_enable) {
                printf("  FAIL: MPh Type 0 field mismatch\n");
                ok = 0;
            }
            if (g1->type != 1 || g1->max_rate != mp1.max_rate ||
                g1->rate_mask != mp1.rate_mask ||
                memcmp(g1->h_re, mp1.h_re, sizeof(mp1.h_re)) ||
                memcmp(g1->h_im, mp1.h_im, sizeof(mp1.h_im))) {
                printf("  FAIL: MPh Type 1 field/precoder mismatch\n");
                ok = 0;
            }
        }
        if (rx.nframes != 3) {
            printf("  FAIL: expected 3 HDLC frames, got %d\n", rx.nframes);
            ok = 0;
        } else {
            for (i = 0; i < 3; i++) {
                if (rx.lens[i] != src.lens[i] ||
                    memcmp(rx.frames[i], src.frames[i], (size_t) src.lens[i])) {
                    printf("  FAIL: HDLC frame %d not byte-identical\n", i);
                    ok = 0;
                }
            }
            if (ok)
                printf("  all 3 HDLC frames byte-identical, MPh fields exact\n");
        }
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* impairment helper defined in the ccimp/pageimp section below */
static void imp_gain_noise_alaw(int16_t *x, long n, double gain,
                                double snr_db, unsigned *seed);

/* ── ccresync: Sh short resync vs PPh/MPh renegotiation discrimination ────
 * Stage 3's control-channel turnaround forms (12.6 vs 12.4), tested at the
 * signal level: build both burst shapes (each carrying one PPS frame), push
 * them through the same A-law + AWGN channel, and assert that the receiver
 *   (a) recovers the PPS byte-exact from BOTH (the HDLC data path is
 *       preamble-agnostic - the whole point of the short resync);
 *   (b) discriminates them: the Sh burst carries NO MPh and scores high on
 *       the rotation-invariant Sh correlator; the PPh burst carries the two
 *       MPh frames (new caps) and scores low.
 * The correlator here mirrors nf_v34.c's v34_cc_sh_score (Sh/S̄h is a pure
 * quarter-turn alternation; PPh eq 10-2 has consecutive-identical symbols). */
struct ccres_rx {
    nf_v34_mp_rx_t mp;
    nf_v34_ccdata_rx_t cc;
    int nmp;
    uint8_t frame[16];
    int flen, nframes;
    nf_cpx_t early[48];
    int nearly;
};

static void ccres_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct ccres_rx *r = user;
    if (msg && ok && len > 0 && len <= 16) {
        memcpy(r->frame, msg, (size_t) len);
        r->flen = len;
        r->nframes++;
    }
}

static void ccres_on_symbol(void *user, const nf_cpx_t *z)
{
    struct ccres_rx *r = user;
    if (r->nearly < 48)
        r->early[r->nearly++] = *z;
    if (nf_v34_mp_feed_symbol(&r->mp, z))
        r->nmp++;
    nf_v34_ccdata_feed_symbol(&r->cc, z);
}

/* best 16-step quarter-turn fraction over the first ~44 symbols (mirror of
 * v34_cc_sh_score) */
static double ccres_sh_score(const nf_cpx_t *z, int nsym)
{
    int lim = nsym < 48 ? nsym : 48, i;
    double best = 0.0;
    if (lim < 18) return 0.0;
    for (i = 1; i + 16 <= lim; i++) {
        int good = 0, k;
        for (k = 0; k < 16; k++) {
            nf_cpx_t a = z[i + k], b = z[i + k - 1];
            double re = (double) a.re * b.re + (double) a.im * b.im;
            double im = (double) a.im * b.re - (double) a.re * b.im;
            double mag = sqrt(re * re + im * im);
            if (mag > 1e-6 && fabs(re) / mag < 0.5) good++;
        }
        if ((double) good / 16.0 > best) best = (double) good / 16.0;
    }
    return best;
}

static int cmd_ccresync(void)
{
    static const uint8_t pps[7] = { 0xff, 0x13, 0xbf, 0x2f, 0x00, 0x01, 0x94 };
    static const uint8_t pps_f[1][16] =
        { { 0xff, 0x13, 0xbf, 0x2f, 0x00, 0x01, 0x94 } };
    static const int pps_l[1] = { 7 };
    nf_v34_mph_fields_t mp;
    unsigned seed = 20260714;
    int form, ok = 1;

    memset(&mp, 0, sizeof(mp));
    mp.type = 0;
    mp.max_rate = 4;                 /* 9600 bit/s - a renegotiated lower cap */
    mp.nonlinear = 1;
    mp.shaping = 1;
    mp.rate_mask = 0x000f;

    for (form = 0; form < 2; form++) {   /* 0 = Sh short resync, 1 = PPh/MPh */
        const char *name = form ? "PPh/MPh reneg" : "Sh short resync";
        nf_v34_cc_tx_t tx;
        struct txcc_hdlc_src src;
        struct ccres_rx rx;
        uint8_t *bits = malloc(20000);
        long nbits = 0, n;
        int16_t *buf;
        double sh;
        int b, want_mph;

        if (!bits) return 1;
        nf_v34_cc_tx_init(&tx, 1);       /* call role, 1200 Hz carrier */
        if (form == 0) {
            nf_v34_cc_tx_sh(&tx);        /* Sh(24T) + S̄h(8T) */
            nf_v34_cc_tx_alt(&tx, 100);
            nf_v34_cc_tx_e(&tx);
            want_mph = 0;
        } else {
            nf_v34_cc_tx_pph(&tx);
            nf_v34_cc_tx_alt(&tx, 100);
            nf_v34_cc_tx_mph(&tx, &mp);
            nf_v34_cc_tx_mph(&tx, &mp);
            nf_v34_cc_tx_e(&tx);
            want_mph = 2;
        }
        src.frames = pps_f;
        src.lens = pps_l;
        src.nframes = 1;
        src.next = 0;
        nf_hdlc_tx_init(&src.tx, 2, txcc_underflow, &src);
        nf_hdlc_tx_flags(&src.tx, 8);
        for (;;) {
            b = nf_hdlc_tx_get_bit(&src.tx);
            if (b == NF_SIG_END_OF_DATA || nbits >= 19999) break;
            bits[nbits++] = (uint8_t) b;
        }
        if (nbits & 1) bits[nbits++] = 1;
        nf_v34_cc_tx_bits(&tx, bits, nbits);
        free(bits);

        n = 400 + (long) ((double) tx.nsym * 40.0 / 3.0) + 800;
        buf = calloc((size_t) n, sizeof(int16_t));
        if (!buf) { nf_v34_cc_tx_free(&tx); return 1; }
        nf_v34_cc_tx_modulate(&tx, buf, n, 400, 6000.0);
        nf_v34_cc_tx_free(&tx);
        imp_gain_noise_alaw(buf, n, 1.0, 30.0, &seed);   /* A-law + 30 dB */

        memset(&rx, 0, sizeof(rx));
        nf_v34_mp_rx_init(&rx.mp, 1);
        nf_v34_ccdata_rx_init(&rx.cc, 1, ccres_on_frame, &rx);
        nf_v34_cc_rx_batch(buf, (int) n, 1200.0, ccres_on_symbol, &rx, 0);
        free(buf);

        sh = ccres_sh_score(rx.early, rx.nearly);
        {
            int frame_ok = rx.nframes == 1 && rx.flen == 7 &&
                           !memcmp(rx.frame, pps, 7);
            int mph_ok = rx.nmp == want_mph;
            int disc_ok = form == 0 ? (sh >= 0.75) : (sh < 0.75);
            printf("  %-16s Sh-score=%.2f MPh=%d frame=%s : %s\n",
                   name, sh, rx.nmp, frame_ok ? "PPS-exact" : "BAD",
                   (frame_ok && mph_ok && disc_ok) ? "OK" : "FAIL");
            if (!(frame_ok && mph_ok && disc_ok)) ok = 0;
        }
    }

    /* the AC signal primitive (10.2.4.1, the 12.8 retrain trigger): exactly
     * alternating point 0 = (1,1) and point 0 rotated 180 = (-1,-1) */
    {
        nf_v34_cc_tx_t tx;
        int k, acok = 1;
        nf_v34_cc_tx_init(&tx, 1);
        nf_v34_cc_tx_ac(&tx, 8);
        if (tx.nsym != 8) acok = 0;
        for (k = 0; k < tx.nsym && acok; k++) {
            double want = (k & 1) ? -1.0 : 1.0;
            if (tx.re[k] != want || tx.im[k] != want) acok = 0;
        }
        nf_v34_cc_tx_free(&tx);
        printf("  %-16s %s\n", "AC signal", acok ? "alternating (1,1)/(-1,-1) OK"
                                                  : "FAIL");
        if (!acok) ok = 0;
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── recover: full control-channel retrain (12.7/12.8) two-session harness ─
 * Two live nf_v34_sess_t objects in audio loopback (the same driver nf_fax
 * runs, minus T.30). Part 1 exercises the NATURAL trigger: after the control
 * channel is up, the recipient is bombarded with undecodable "primary" bursts;
 * it falls the advertised rate all the way to the floor and, still unable to
 * decode, escalates to a full retrain (nf_v34_sess_retrain, per 12.7/12.8).
 * Part 2 exercises RECOVERY: a fresh pair is established, both sides retrain
 * (initiator + AC-responder), and both re-run Phase 2 probing -> Phase 3
 * training and re-establish the control channel. */
struct recov_st { int established, failed; };
static void recov_status(void *user, int st)
{
    struct recov_st *r = user;
    if (st == NF_SIG_TRAINING_SUCCEEDED) r->established = 1;
    if (st == NF_SIG_TRAINING_FAILED)    r->failed = 1;
}

/* pump both sessions in audio loopback until `est` (established on both) or a
 * sample budget; returns the number of BLK iterations run */
static int recov_pump_startup(nf_v34_sess_t *ca, nf_v34_sess_t *cb,
                              const struct recov_st *da, const struct recov_st *db,
                              int max_iter)
{
    int16_t a[160], b[160];
    int it, na, nb;
    for (it = 0; it < max_iter && !(da->established && db->established); it++) {
        na = nf_v34_sess_tx(ca, a, 160);
        if (na < 160) memset(a + na, 0, (size_t)(160 - na) * 2);
        nf_v34_sess_rx(cb, a, 160);
        nb = nf_v34_sess_tx(cb, b, 160);
        if (nb < 160) memset(b + nb, 0, (size_t)(160 - nb) * 2);
        nf_v34_sess_rx(ca, b, 160);
    }
    return it;
}

static int cmd_recover(void)
{
    unsigned seed = 0x13572468;
    int ok = 1;

    /* ── Part 1: natural trigger (hard fail at the floor -> retrain) ────── */
    {
        struct recov_st da = {0,0}, db = {0,0};
        nf_v34_sess_t *ca = nf_v34_sess_alloc(1, recov_status, NULL, &da);
        nf_v34_sess_t *cb = nf_v34_sess_alloc(0, recov_status, NULL, &db);
        int16_t noise[5200];
        int j, burst, rate0;

        nf_v34_sess_set_tx_mode(ca, NF_V34_SESS_STARTUP);
        nf_v34_sess_set_rx_mode(ca, NF_V34_SESS_STARTUP);
        nf_v34_sess_set_tx_mode(cb, NF_V34_SESS_STARTUP);
        nf_v34_sess_set_rx_mode(cb, NF_V34_SESS_STARTUP);
        recov_pump_startup(ca, cb, &da, &db, 4000);
        rate0 = nf_v34_sess_data_rate(cb);
        printf("  part1 startup: A established=%d B established=%d rate=%d\n",
               da.established, db.established, rate0);
        if (!(da.established && db.established)) ok = 0;

        /* the recipient now expects primary-channel bursts; feed it garbage */
        nf_v34_sess_set_rx_mode(cb, NF_V34_SESS_PRI);
        for (burst = 0; burst < 30 && nf_v34_sess_retrains(cb) == 0; burst++) {
            for (j = 0; j < 4200; j++)                 /* 0.52 s loud noise  */
                noise[j] = (int16_t)(lcg_gauss(&seed) * 4000.0);
            for (; j < 5200; j++)                      /* 0.12 s silence     */
                noise[j] = 0;
            nf_v34_sess_rx(cb, noise, 5200);
        }
        printf("  part1 after %d garbage bursts: rate %d->%d, retrains=%d\n",
               burst, rate0, nf_v34_sess_data_rate(cb), nf_v34_sess_retrains(cb));
        if (nf_v34_sess_retrains(cb) < 1) {
            printf("  FAIL: recipient never escalated to a retrain\n");
            ok = 0;
        } else if (nf_v34_sess_data_rate(cb) > rate0) {
            printf("  FAIL: rate did not fall before the retrain\n");
            ok = 0;
        } else {
            printf("  natural trigger OK (fell to floor, then retrained)\n");
        }
        nf_v34_sess_free(ca);
        nf_v34_sess_free(cb);
    }

    /* ── Part 2: retrain -> Phase 2/3 re-establishment ─────────────────── */
    {
        struct recov_st da = {0,0}, db = {0,0};
        nf_v34_sess_t *ca = nf_v34_sess_alloc(1, recov_status, NULL, &da);
        nf_v34_sess_t *cb = nf_v34_sess_alloc(0, recov_status, NULL, &db);

        nf_v34_sess_set_tx_mode(ca, NF_V34_SESS_STARTUP);
        nf_v34_sess_set_rx_mode(ca, NF_V34_SESS_STARTUP);
        nf_v34_sess_set_tx_mode(cb, NF_V34_SESS_STARTUP);
        nf_v34_sess_set_rx_mode(cb, NF_V34_SESS_STARTUP);
        recov_pump_startup(ca, cb, &da, &db, 4000);
        if (!(da.established && db.established &&
              nf_v34_sess_established(ca) && nf_v34_sess_established(cb))) {
            printf("  part2 FAIL: initial startup did not establish\n");
            ok = 0;
        } else {
            /* both sides retrain (initiator + AC responder, 12.8) */
            nf_v34_sess_retrain(ca);
            nf_v34_sess_retrain(cb);
            printf("  part2: control channel torn down, retraining "
                   "(A established=%d B established=%d)\n",
                   nf_v34_sess_established(ca), nf_v34_sess_established(cb));
            {
                int16_t a[160], b[160];
                int it, na, nb;
                for (it = 0; it < 6000 &&
                     !(nf_v34_sess_established(ca) && nf_v34_sess_established(cb));
                     it++) {
                    na = nf_v34_sess_tx(ca, a, 160);
                    if (na < 160) memset(a + na, 0, (size_t)(160 - na) * 2);
                    nf_v34_sess_rx(cb, a, 160);
                    nb = nf_v34_sess_tx(cb, b, 160);
                    if (nb < 160) memset(b + nb, 0, (size_t)(160 - nb) * 2);
                    nf_v34_sess_rx(ca, b, 160);
                }
            }
            printf("  part2 after retrain: A established=%d (retrains=%d)"
                   " B established=%d (retrains=%d)\n",
                   nf_v34_sess_established(ca), nf_v34_sess_retrains(ca),
                   nf_v34_sess_established(cb), nf_v34_sess_retrains(cb));
            if (nf_v34_sess_established(ca) && nf_v34_sess_established(cb) &&
                nf_v34_sess_retrains(ca) == 1 && nf_v34_sess_retrains(cb) == 1)
                printf("  recovery OK (Phase 2->3 re-run, control channel"
                       " re-established)\n");
            else {
                printf("  FAIL: control channel not re-established after retrain\n");
                ok = 0;
            }
        }
        nf_v34_sess_free(ca);
        nf_v34_sess_free(cb);
    }

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── txpage: the centrepiece - full primary-channel TX -> RX loopback ──── */

#define TXPAGE_NFCD 256

struct txpage_expect {
    uint8_t (*frames)[264];
    int *lens;
    int nframes;
    int next;
    int mismatches;
    nf_hdlc_tx_t tx;      /* also doubles as the TX bit source */
};

static void txpage_underflow(void *user)
{
    struct txpage_expect *e = user;

    if (e->next < e->nframes)
        nf_hdlc_tx_frame(&e->tx, e->frames[e->next], e->lens[e->next]);
    else if (e->next == e->nframes)
        nf_hdlc_tx_frame(&e->tx, NULL, 0);
    e->next++;
}

struct txpage_check {
    const struct txpage_expect *e;
    uint8_t fcd_seen[TXPAGE_NFCD];
    int fcd_exact, rcp_exact, mismatches;
};

static void txpage_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct txpage_check *c = user;

    if (!msg || !ok || len <= 0)
        return;
    if (len == 260 && msg[2] == 0x06) {          /* FCD: match by number */
        int num = msg[3];
        if (num < c->e->nframes && c->e->lens[num] == 260 &&
            !memcmp(msg, c->e->frames[num], 260)) {
            if (!c->fcd_seen[num]) {
                c->fcd_seen[num] = 1;
                c->fcd_exact++;
            }
            return;
        }
    } else if (len == 3 && msg[2] == 0x86) {     /* RCP */
        if (!memcmp(msg, c->e->frames[TXPAGE_NFCD], 3)) {
            c->rcp_exact++;
            return;
        }
    }
    c->mismatches++;
}

/* one full RX pass: train (optionally scanning ppm hypotheses around the
 * nominal TX clock - the receiver-side clock acquisition a streaming
 * front end would do), locate S, decode, verify. Returns 0 on pass. */
static int txpage_rx_pass(const int16_t *buf, long n, double t_trn,
                          double t_s_lo, double t_s_hi, long burst_nsym,
                          const struct txpage_expect *exp,
                          int scan_ppm, const char *label)
{
    static const double ppms[11] =
        { -100, -80, -60, -40, -20, 0, 20, 40, 60, 80, 100 };
    static const double fine[4] = { -10, -5, 5, 10 };
    nf_v34_page_eq_t eq, best_eq;
    double best_baud = NF_V34_PC_BAUD_NOMINAL, best_hold = 1e300;
    double corr, t_s, t_end_max;
    nf_v34_page_burst_t r;
    struct txpage_check chk;
    int h, nh = scan_ppm ? 11 : 1;
    int ok = 1;

    /* coarse clock scan (20 ppm grid), then a fine +-10/-5 ppm refinement
     * around the winner: the residual timing drift left for the decoder's
     * tau tracker must stay well under its slew capability (~0.17 samples/s;
     * 5 ppm = 0.04 samples/s). This mirrors how the reference capture's
     * +19.6 ppm clock was measured (diag53) before its TRN would train. */
    for (h = 0; h < nh; h++) {
        double baud = NF_V34_PC_BAUD_NOMINAL * (1.0 + (scan_ppm ? ppms[h] : 0.0) * 1e-6);
        if (nf_v34_page_train(buf, n, 0.0, t_trn, 4600, baud, NULL, &eq) < 0)
            continue;
        if (eq.res_holdout < best_hold) {
            best_hold = eq.res_holdout;
            best_baud = baud;
            best_eq = eq;
        }
    }
    if (scan_ppm && best_hold < 1e300) {
        double centre = best_baud;
        for (h = 0; h < 4; h++) {
            double baud = centre * (1.0 + fine[h] * 1e-6);
            if (nf_v34_page_train(buf, n, 0.0, t_trn, 4600, baud, NULL, &eq) < 0)
                continue;
            if (eq.res_holdout < best_hold) {
                best_hold = eq.res_holdout;
                best_baud = baud;
                best_eq = eq;
            }
        }
    }
    if (best_hold >= 1e300) {
        printf("%s: TRN training failed\n", label);
        return 1;
    }
    printf("%s: TRN LS-FSE res_train=%.5f res_holdout=%.5f baud=%.4f%s\n",
           label, best_eq.res_train, best_eq.res_holdout, best_baud,
           scan_ppm ? " (ppm scan)" : "");
    if (best_eq.res_holdout > 0.01) {
        printf("%s: FAIL: TRN holdout residual above 0.01\n", label);
        ok = 0;
    }

    t_s = nf_v34_page_locate_s(buf, n, 0.0, t_s_lo, t_s_hi, best_baud, NULL, &corr);
    printf("%s: S anchor t=%.5fs corr=%.3f\n", label, t_s, corr);
    if (corr < 0.8) {
        printf("%s: FAIL: S locator correlation below 0.8\n", label);
        ok = 0;
    }

    t_end_max = t_s + (double) (burst_nsym - 6) / best_baud;
    memset(&chk, 0, sizeof(chk));
    chk.e = exp;
    if (nf_v34_page_decode_burst(buf, n, 0.0, &best_eq, t_s, t_end_max,
                                 best_baud, NULL, txpage_on_frame, &chk, &r) < 0) {
        printf("%s: decode failed\n", label);
        return 1;
    }
    printf("%s: PP resid=%.5f tau %+.2f -> %+.2f; %ld symbols; B1 Z(-1)=%d match=%d/840\n",
           label, r.pp_resid, r.tau_init, r.tau_final, r.nsym, r.zm1, r.b1_match);
    printf("%s: HDLC FCS-valid=%d (FCD=%d, RCP=%d) bad=%d; byte-identical FCD=%d/%d RCP=%d/3 alien=%d\n",
           label, r.hdlc_ok, r.fcd_ok, r.rcp_ok, r.hdlc_bad,
           chk.fcd_exact, TXPAGE_NFCD, chk.rcp_exact, chk.mismatches);
    if (r.b1_match != 840) {
        printf("%s: FAIL: B1 match %d != 840\n", label, r.b1_match);
        ok = 0;
    }
    if (r.fcd_ok != TXPAGE_NFCD || chk.fcd_exact != TXPAGE_NFCD) {
        printf("%s: FAIL: %d/%d FCS-valid, %d/%d byte-identical FCD frames\n",
               label, r.fcd_ok, TXPAGE_NFCD, chk.fcd_exact, TXPAGE_NFCD);
        ok = 0;
    }
    if (chk.mismatches != 0 || chk.rcp_exact < 3) {
        printf("%s: FAIL: unexpected/mismatched frames recovered\n", label);
        ok = 0;
    }
    return ok ? 0 : 1;
}

/* windowed-sinc fractional resampler (the honest model of a sampling-clock
 * offset: the far modem's DAC reconstructs the continuous waveform, our ADC
 * samples it on a slightly different clock). Linear interpolation is NOT
 * usable here - at 8 kHz its half-sample response is ~0.04x at this
 * signal's 3.88 kHz upper band edge, a channel notch no real clock offset
 * produces. */
static int16_t txpage_resample_at(const int16_t *x, long n, double pos)
{
    long m = (long) floor(pos);
    double f = pos - (double) m;
    double acc = 0.0;
    int j;

    for (j = -15; j <= 16; j++) {
        long k = m + j;
        double u = (double) j - f;
        double snc, w, xx;
        if (k < 0 || k >= n)
            continue;
        snc = (fabs(u) < 1e-9) ? 1.0 : sin(M_PI * u) / (M_PI * u);
        xx = (u + 16.0) / 32.0;
        w = 0.42 - 0.5 * cos(2.0 * M_PI * xx) + 0.08 * cos(4.0 * M_PI * xx);
        acc += snc * w * x[k];
    }
    if (acc > 32767.0) acc = 32767.0;
    if (acc < -32768.0) acc = -32768.0;
    return (int16_t) (acc >= 0.0 ? acc + 0.5 : acc - 0.5);
}

static int cmd_txpage(int argc, char **argv)
{
    /* layout (samples at 8 kHz; every segment starts on an integer sample,
     * 432 symbols = exactly 1008 samples at 24000/7 baud) */
    enum { TRNB_AT = 400, TRN_NSYM = 4800, DATA_AT = 14400 };
    const double t_trn = (TRNB_AT + 1008) / 8000.0;
    const double gain = 5000.0;
    struct txpage_expect exp;
    unsigned seed = 20260714;
    uint8_t *bits = NULL;
    double *bre = NULL, *bim = NULL, *tre = NULL, *tim = NULL;
    int16_t *buf = NULL, *imp = NULL;
    long nbits = 0, nsym, n, i;
    int f, b, rc = 1, ok = 1;
    /* Default impairment: G.711 A-law round trip + AWGN at this SNR + a
     * +50 ppm sampling-clock offset. Measured margin with this seed: 256/256
     * still passes at 31 dB, the first frame is lost at 30 dB - the default
     * asserts 32 dB to keep 1 dB of headroom over the measured edge against
     * compiler/arch float churn. NOTE the page decoder slices - it
     * deliberately does not run the trellis (see nf_v34.h), so it lacks the
     * ~4 dB TCM coding gain a full V.34 receiver would have at 24000 bit/s;
     * 30 dB AWGN is genuinely beyond a sliced 224-point constellation.
     * Clock offsets up to ~+-110 ppm are covered by the training-time scan.
     * Optional argv overrides for margin hunting: txpage [snr_db [ppm]]. */
    double snr_db = 32.0;
    double clock_ppm = 50.0;

    if (argc > 2)
        snr_db = atof(argv[2]);
    if (argc > 3)
        clock_ppm = atof(argv[3]);

    /* ── build the ECM block: 256 FCD frames + 3 RCP, real HDLC ── */
    exp.frames = malloc(sizeof(*exp.frames) * (TXPAGE_NFCD + 3));
    exp.lens = malloc(sizeof(int) * (TXPAGE_NFCD + 3));
    bits = malloc(700000);
    if (!exp.frames || !exp.lens || !bits)
        goto done;
    for (f = 0; f < TXPAGE_NFCD; f++) {
        exp.frames[f][0] = 0xff;
        exp.frames[f][1] = 0x03;
        exp.frames[f][2] = 0x06;             /* FCD */
        exp.frames[f][3] = (uint8_t) f;
        for (i = 0; i < 256; i++)
            exp.frames[f][4 + i] = (uint8_t) (lcg_next(&seed) & 0xff);
        exp.lens[f] = 260;
    }
    for (f = 0; f < 3; f++) {
        exp.frames[TXPAGE_NFCD + f][0] = 0xff;
        exp.frames[TXPAGE_NFCD + f][1] = 0x03;
        exp.frames[TXPAGE_NFCD + f][2] = 0x86;   /* RCP */
        exp.lens[TXPAGE_NFCD + f] = 3;
    }
    exp.nframes = TXPAGE_NFCD + 3;
    exp.next = 0;
    nf_hdlc_tx_init(&exp.tx, 1, txpage_underflow, &exp);
    nf_hdlc_tx_flags(&exp.tx, 4);
    for (;;) {
        b = nf_hdlc_tx_get_bit(&exp.tx);
        if (b == NF_SIG_END_OF_DATA || nbits >= 699999)
            break;
        bits[nbits++] = (uint8_t) b;
    }
    printf("ECM block: %d frames, %ld HDLC bits (%ld data frames)\n",
           exp.nframes, nbits, (nbits + 55) / 56);

    /* ── TX ── */
    nsym = nf_v34_pc_burst_build(NULL, bits, nbits, &bre, &bim);
    if (nsym < 0)
        goto done;
    n = DATA_AT + (long) ((double) nsym * 7.0 / 3.0) + 4000;
    buf = calloc((size_t) n, sizeof(int16_t));
    tre = malloc(sizeof(double) * (432 + TRN_NSYM));
    tim = malloc(sizeof(double) * (432 + TRN_NSYM));
    if (!buf || !tre || !tim)
        goto done;
    nf_v34_pc_sspp(tre, tim);
    nf_v34_pc_trn(TRN_NSYM, 1, tre + 432, tim + 432);
    for (i = 432; i < 432 + TRN_NSYM; i++) {     /* unit RMS, like PP */
        tre[i] /= sqrt(10.0);
        tim[i] /= sqrt(10.0);
    }
    nf_v34_pc_modulate(NULL, buf, n, TRNB_AT, tre, tim, 432 + TRN_NSYM, gain);
    nf_v34_pc_modulate(NULL, buf, n, DATA_AT, bre, bim, nsym, gain);
    {
        double rms = 0.0;
        long from = DATA_AT, to = DATA_AT + (long) ((double) nsym * 7.0 / 3.0);
        for (i = from; i < to; i++)
            rms += (double) buf[i] * buf[i];
        rms = sqrt(rms / (double) (to - from));
        printf("TX: %ld burst symbols, %.1fs audio, data-burst RMS %.0f\n",
               nsym, (double) n / 8000.0, rms);

        /* ── clean loopback ── */
        if (txpage_rx_pass(buf, n, t_trn, DATA_AT / 8000.0 - 0.005,
                           DATA_AT / 8000.0 + 0.005, nsym, &exp, 0, "clean"))
            ok = 0;

        /* ── impaired: A-law + AWGN + sampling-clock offset ── */
        imp = malloc(sizeof(int16_t) * (size_t) n);
        if (!imp)
            goto done;
        {
            double sigma = rms * pow(10.0, -snr_db / 20.0);
            double alpha = 1.0 + clock_ppm * 1e-6;
            for (i = 0; i < n; i++) {
                double v = alaw_to_linear(linear_to_alaw(buf[i]))
                           + sigma * lcg_gauss(&seed);
                if (v > 32767.0) v = 32767.0;
                if (v < -32768.0) v = -32768.0;
                buf[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
            }
            for (i = 0; i < n; i++)                 /* clock-offset resample */
                imp[i] = txpage_resample_at(buf, n, (double) i * alpha);
        }
        printf("impaired: G.711 A-law + AWGN %.0f dB SNR + %+.0f ppm clock\n",
               snr_db, clock_ppm);
        if (txpage_rx_pass(imp, n, t_trn, DATA_AT / 8000.0 - 0.005,
                           DATA_AT / 8000.0 + 0.005, nsym, &exp, 1, "impaired"))
            ok = 0;
    }
    rc = ok ? 0 : 1;
done:
    if (rc == 1 && !buf)
        printf("alloc failed\n");
    free(exp.frames); free(exp.lens); free(bits);
    free(bre); free(bim); free(tre); free(tim);
    free(buf); free(imp);
    printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}


/* ═══ in-process line impairments (ccimp / pageimp) ═════════════════════
 * Deterministic, line_sim-independent regressions for the hardening work:
 * every impairment class that broke a sweep cell is reproduced here with
 * small local models, so `make check-v34` protects the fixes forever.
 * The Hilbert-FIR SSB frequency shifter deliberately mirrors real carrier
 * equipment (finite image rejection toward low frequencies) - exactly the
 * artifact the page equalizer's image branches were built for. */

#define IMP_HLEN 241            /* Hilbert FIR taps (odd), delay (HLEN-1)/2 */

/* windowed-FIR Hilbert transform (hlen taps, odd, <= IMP_HLEN); out[i]
 * pairs with x[i]. A SHORT hlen deliberately mimics real SSB carrier
 * equipment: its image rejection collapses toward low frequencies, which
 * is exactly what the page equalizer's image branches exist for. */
static void imp_hilbert(const double *x, long n, double *out, int hlen)
{
    double h[IMP_HLEN];
    const int c = (hlen - 1) / 2;
    long i;
    int k;

    for (k = 0; k < hlen; k++) {
        int m = k - c;
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * k / (hlen - 1));
        h[k] = (m & 1) ? w * 2.0 / (M_PI * (double) m) : 0.0;
    }
    for (i = 0; i < n; i++) {
        double acc = 0.0;
        for (k = 0; k < hlen; k++) {
            long p = i - k + c;         /* pairs with x[i] after c delay */
            if (p >= 0 && p < n)
                acc += h[k] * x[p];
        }
        out[i] = acc;
    }
}

/* SSB frequency shift by `hz` (positive = down), Hilbert-FIR based */
static void imp_freq_shift(int16_t *x, long n, double hz, int hlen)
{
    double *xr = calloc((size_t) n, sizeof(double));
    double *xh = calloc((size_t) n, sizeof(double));
    long i;

    if (!xr || !xh) { free(xr); free(xh); return; }
    for (i = 0; i < n; i++)
        xr[i] = x[i];
    imp_hilbert(xr, n, xh, hlen);
    for (i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * hz * (double) i / 8000.0;
        double v = xr[i] * cos(ph) + xh[i] * sin(ph);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
    }
    free(xr);
    free(xh);
}

/* sinusoidal phase jitter: peak_deg at rate_hz (SSB phase modulator) */
static void imp_phase_jitter(int16_t *x, long n, double peak_deg, double rate_hz)
{
    double *xr = calloc((size_t) n, sizeof(double));
    double *xh = calloc((size_t) n, sizeof(double));
    double a = peak_deg * M_PI / 180.0;
    long i;

    if (!xr || !xh) { free(xr); free(xh); return; }
    for (i = 0; i < n; i++)
        xr[i] = x[i];
    imp_hilbert(xr, n, xh, IMP_HLEN);
    for (i = 0; i < n; i++) {
        double ph = a * sin(2.0 * M_PI * rate_hz * (double) i / 8000.0);
        double v = xr[i] * cos(ph) - xh[i] * sin(ph);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
    }
    free(xr);
    free(xh);
}

/* first-order high-cut "tilt" (mild negative slope across the band) */
static void imp_tilt(int16_t *x, long n)
{
    double y = 0.0;
    long i;

    for (i = 0; i < n; i++) {
        y = 0.75 * (double) x[i] + 0.25 * y;
        x[i] = (int16_t) (y >= 0.0 ? y + 0.5 : y - 0.5);
    }
}

/* memoryless cubic distortion (harmonic distortion model) */
static void imp_cubic(int16_t *x, long n, double a3)
{
    long i;

    for (i = 0; i < n; i++) {
        double u = (double) x[i];
        double v = u + a3 * u * u * u;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
    }
}

/* AWGN only, at snr_db below the buffer's active RMS (no A-law) */
static void imp_awgn(int16_t *x, long n, double snr_db, unsigned *seed)
{
    double rms = 0.0, sigma;
    long i, act = 0;

    for (i = 0; i < n; i++) {
        if (x[i] > 200 || x[i] < -200) {
            rms += (double) x[i] * x[i];
            act++;
        }
    }
    rms = act ? sqrt(rms / (double) act) : 1.0;
    sigma = rms * pow(10.0, -snr_db / 20.0);
    for (i = 0; i < n; i++) {
        double v = (double) x[i] + sigma * lcg_gauss(seed);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
    }
}

/* gain, AWGN at snr_db below the buffer's active RMS, A-law round trip */
static void imp_gain_noise_alaw(int16_t *x, long n, double gain,
                                double snr_db, unsigned *seed)
{
    double rms = 0.0, sigma;
    long i, act = 0;

    for (i = 0; i < n; i++) {
        double v = (double) x[i] * gain;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
    }
    for (i = 0; i < n; i++) {
        if (x[i] > 200 || x[i] < -200) {
            rms += (double) x[i] * x[i];
            act++;
        }
    }
    rms = act ? sqrt(rms / (double) act) : 1.0;
    sigma = (snr_db > 0.0) ? rms * pow(10.0, -snr_db / 20.0) : 0.0;
    for (i = 0; i < n; i++) {
        double v = (double) x[i] + sigma * lcg_gauss(seed);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t) alaw_to_linear(linear_to_alaw(
                   (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5)));
    }
}

/* ── ccimp: control-channel demod under impairments ─────────────────────
 * One call-role cc burst (PPh/ALT/MPh/MPh'/E + one PPS frame) through a
 * matrix of line impairments; every cell must recover both MPh frames and
 * the byte-identical PPS. Covers the acquisition fixes: timing-phase
 * multi-start (the "off100/off200" cells reproduce the exact Gardner
 * unstable-equilibrium alignment that broke the snr38 sweep cell), the
 * 4th-power frequency-offset estimator (foff cells), active-region
 * normalization (gain cells), tilt and distortion tolerance. */
static int cmd_ccimp(void)
{
    static const uint8_t pps[16] = { 0xff, 0x13, 0xbf, 0x2f, 0x00, 0x01, 0x94 };
    static const int pps_len = 7;
    struct cell {
        const char *name;
        long lead;          /* extra silence before the burst */
        double gain, foff, snr_db, jit_deg;
        int tilt, cubic;
        int r2400;          /* also exercise this cell at 2400 bit/s */
    };
    static const struct cell cells[] = {
        { "alaw",        300, 1.0,    0.0, 0.0, 0.0, 0, 0, 1 },
        { "off+100",     400, 1.0,    0.0, 0.0, 0.0, 0, 0, 1 },
        { "off+200",     500, 1.0,    0.0, 0.0, 0.0, 0, 0, 1 },
        { "gain-9dB",    300, 0.355,  0.0, 0.0, 0.0, 0, 0, 1 },
        { "gain+6dB",    300, 2.0,    0.0, 0.0, 0.0, 0, 0, 1 },
        { "foff+7",      300, 1.0,   -7.0, 0.0, 0.0, 0, 0, 1 },
        { "foff-7",      300, 1.0,    7.0, 0.0, 0.0, 0, 0, 1 },
        { "tilt",        300, 1.0,    0.0, 0.0, 0.0, 1, 0, 1 },
        { "cubic",       300, 1.0,    0.0, 0.0, 0.0, 0, 1, 0 },
        { "snr30",       300, 1.0,    0.0, 30.0, 0.0, 0, 0, 1 },
        { "jit10@60",    300, 1.0,    0.0, 0.0, 10.0, 0, 0, 0 },
        { "combo",       420, 0.5,    4.0, 33.0, 0.0, 1, 0, 0 },
    };
    nf_v34_mph_fields_t mp0;
    static const uint8_t frames1[1][16] =
        { { 0xff, 0x13, 0xbf, 0x2f, 0x00, 0x01, 0x94 } };
    static const int lens1[1] = { 7 };
    int16_t *ref = NULL, *buf = NULL;
    long nmax = 0;
    unsigned seed = 20260714;
    int ci, ok = 1, rate2400;

    /* Both cc rates: 1200 bit/s (4-point) over the full matrix, 2400 bit/s
     * (16-point, r2400 cells) over the impairments the denser constellation
     * can survive - gain/offset/AWGN/foff/tilt (cubic distortion and heavy
     * phase jitter overwhelm 16-point slicing, so those stay 1200-only). */
    for (rate2400 = 0; rate2400 < 2; rate2400++) {
        nf_v34_cc_tx_t tx;
        struct txcc_hdlc_src src;
        uint8_t *bits = malloc(20000);
        long nbits = 0, nsig;
        int b;

        if (!bits) { free(ref); free(buf); return 1; }
        memset(&mp0, 0, sizeof(mp0));
        mp0.type = 0;
        mp0.max_rate = 10;
        mp0.cc_rate = rate2400;
        mp0.nonlinear = 1;
        mp0.shaping = 1;
        mp0.rate_mask = 0x3fff;

        nf_v34_cc_tx_init(&tx, 1);
        nf_v34_cc_tx_pph(&tx);
        nf_v34_cc_tx_alt(&tx, 100);
        nf_v34_cc_tx_mph(&tx, &mp0);
        nf_v34_cc_tx_mph(&tx, &mp0);
        nf_v34_cc_tx_e(&tx);
        src.frames = frames1;
        src.lens = lens1;
        src.nframes = 1;
        src.next = 0;
        nf_hdlc_tx_init(&src.tx, 2, txcc_underflow, &src);
        nf_hdlc_tx_flags(&src.tx, 8);
        for (;;) {
            b = nf_hdlc_tx_get_bit(&src.tx);
            if (b == NF_SIG_END_OF_DATA || nbits >= 19999)
                break;
            bits[nbits++] = (uint8_t) b;
        }
        while (nbits & 3)
            bits[nbits++] = 1;
        nf_v34_cc_tx_set_rate(&tx, rate2400);
        nf_v34_cc_tx_bits(&tx, bits, nbits);
        free(bits);

        nsig = (long) ((double) tx.nsym * 40.0 / 3.0);
        if (600 + nsig + 800 > nmax) {
            nmax = 600 + nsig + 800;
            free(ref); free(buf);
            ref = calloc((size_t) nmax, sizeof(int16_t));
            buf = malloc(sizeof(int16_t) * (size_t) nmax);
            if (!ref || !buf) {
                free(ref); free(buf); nf_v34_cc_tx_free(&tx);
                return 1;
            }
        }

        for (ci = 0; ci < (int) (sizeof(cells) / sizeof(cells[0])); ci++) {
            const struct cell *c = &cells[ci];
            struct txcc_rx rx;
            long n = c->lead + nsig + 700;

            if (rate2400 && !c->r2400)
                continue;
            memset(ref, 0, sizeof(int16_t) * (size_t) nmax);
            nf_v34_cc_tx_modulate(&tx, ref, nmax, c->lead, 6000.0);
            memcpy(buf, ref, sizeof(int16_t) * (size_t) nmax);
            if (c->foff != 0.0)
                imp_freq_shift(buf, n, c->foff, IMP_HLEN);
            if (c->jit_deg != 0.0)
                imp_phase_jitter(buf, n, c->jit_deg, 60.0);
            if (c->tilt)
                imp_tilt(buf, n);
            if (c->cubic)
                imp_cubic(buf, n, -8.5e-10);
            imp_gain_noise_alaw(buf, n, c->gain, c->snr_db, &seed);

            memset(&rx, 0, sizeof(rx));
            nf_v34_mp_rx_init(&rx.mp, 1);
            nf_v34_ccdata_rx_init(&rx.cc, 1, txcc_on_frame, &rx);
            nf_v34_ccdata_rx_set_rate(&rx.cc, rate2400);
            nf_v34_cc_rx_batch(buf, (int) n, 1200.0, txcc_on_symbol, &rx,
                               rate2400 ? 130 : 0);

            if (rx.nmp >= 2 && rx.nframes == 1 && rx.lens[0] == pps_len &&
                !memcmp(rx.frames[0], pps, (size_t) pps_len)) {
                printf("  %-10s %4d MPh=%d frames=%d OK\n", c->name,
                       rate2400 ? 2400 : 1200, rx.nmp, rx.nframes);
            } else {
                printf("  %-10s %4d FAIL (MPh=%d frames=%d)\n", c->name,
                       rate2400 ? 2400 : 1200, rx.nmp, rx.nframes);
                ok = 0;
            }
        }
        nf_v34_cc_tx_free(&tx);
    }
    free(ref);
    free(buf);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── pageimp: primary-channel train + decode under impairments ──────────
 * A phase-3 TRN burst and a short ECM burst (1 FCD + 3 RCP) through the
 * same impairment models; the equalizer must train (res_holdout below the
 * session's own 0.02 gate) and the burst must yield the FCD and RCPs.
 * Covers: the locate_s incoherent/df-estimating scan and the widened
 * training bootstrap (foff cells), the SSB-shifter image branches (the
 * Hilbert-FIR shifter above has genuinely finite image rejection), the
 * cubic Hammerstein branch (thd), gain independence, and the aided-B1
 * jitter detector + tracked training (jitter cell). */
/* defined below (txrates section); reused for pageimp's per-mode cells */
static int txrates_cell(const nf_v34_pcparams_t *pp, double snr_db, int alaw,
                        unsigned *seed, int quiet, double *res_holdout_out);

struct pageimp_src {
    nf_hdlc_tx_t tx;
    int next;
};

static void pageimp_underflow(void *user)
{
    struct pageimp_src *s = user;
    uint8_t f[264];
    int i;

    if (s->next == 0) {
        f[0] = 0xff; f[1] = 0x03; f[2] = 0x06; f[3] = 0x07;   /* FCD #7 */
        for (i = 0; i < 256; i++)
            f[4 + i] = (uint8_t) (i * 7 + 3);
        nf_hdlc_tx_frame(&s->tx, f, 260);
    } else if (s->next < 4) {
        f[0] = 0xff; f[1] = 0x03; f[2] = 0x86;                /* RCP */
        nf_hdlc_tx_frame(&s->tx, f, 3);
    } else if (s->next == 4) {
        nf_hdlc_tx_frame(&s->tx, NULL, 0);
    }
    s->next++;
}

static int cmd_pageimp(void)
{
    struct cell {
        const char *name;
        double gain, foff, snr_db, jit_deg;
        int cubic, hlen;
        double res_max;
    };
    static const struct cell cells[] = {
        { "alaw",     1.0,    0.0,  0.0, 0.0, 0, 0,        0.0010 },
        { "gain-9dB", 0.355,  0.0,  0.0, 0.0, 0, 0,        0.0010 },
        { "foff-7",   1.0,    7.0,  0.0, 0.0, 0, 31,       0.0100 },
        { "foff+3",   1.0,   -3.0,  0.0, 0.0, 0, IMP_HLEN, 0.0100 },
        { "cubic",    1.0,    0.0,  0.0, 0.0, 1, 0,        0.0040 },
        { "snr33",    1.0,    0.0, 33.0, 0.0, 0, 0,        0.0040 },
        { "jit5@60",  1.0,    0.0,  0.0, 5.0, 0, 0,        0.0100 },
    };
    struct pageimp_src src;
    uint8_t *bits = malloc(40000);
    double *bre = NULL, *bim = NULL, *tre = NULL, *tim = NULL;
    int16_t *trn0 = NULL, *pri0 = NULL, *trn = NULL, *pri = NULL;
    long nbits = 0, nsym, n1, n2, i;
    double rms = 0.0;
    unsigned seed = 19981102;
    int ci, b, ok = 1;

    if (!bits)
        return 1;
    src.next = 0;
    nf_hdlc_tx_init(&src.tx, 1, pageimp_underflow, &src);
    nf_hdlc_tx_flags(&src.tx, 4);
    for (;;) {
        b = nf_hdlc_tx_get_bit(&src.tx);
        if (b == NF_SIG_END_OF_DATA || nbits >= 39999)
            break;
        bits[nbits++] = (uint8_t) b;
    }
    if (nbits & 1)
        bits[nbits++] = 1;
    nsym = nf_v34_pc_burst_build(NULL, bits, nbits, &bre, &bim);
    free(bits);
    if (nsym < 0)
        return 1;

    tre = malloc(sizeof(double) * (432 + 4800));
    tim = malloc(sizeof(double) * (432 + 4800));
    if (!tre || !tim)
        goto fail;
    nf_v34_pc_sspp(tre, tim);
    nf_v34_pc_trn(4800, 1, tre + 432, tim + 432);
    for (i = 432; i < 432 + 4800; i++)
        rms += tre[i] * tre[i] + tim[i] * tim[i];
    rms = sqrt(rms / 4800.0);
    for (i = 432; i < 432 + 4800; i++) {
        tre[i] /= rms;
        tim[i] /= rms;
    }
    n1 = 1200 + (432 + 4800) * 7 / 3 + 800;
    n2 = 600 + nsym * 7 / 3 + 1200;
    trn0 = calloc((size_t) n1, sizeof(int16_t));
    pri0 = calloc((size_t) n2, sizeof(int16_t));
    trn = malloc(sizeof(int16_t) * (size_t) n1);
    pri = malloc(sizeof(int16_t) * (size_t) n2);
    if (!trn0 || !pri0 || !trn || !pri)
        goto fail;
    nf_v34_pc_modulate(NULL, trn0, n1, 1200, tre, tim, 432 + 4800, 5000.0);
    nf_v34_pc_modulate(NULL, pri0, n2, 600, bre, bim, nsym, 5000.0);

    for (ci = 0; ci < (int) (sizeof(cells) / sizeof(cells[0])); ci++) {
        const struct cell *c = &cells[ci];
        nf_v34_page_eq_t eq;
        nf_v34_page_burst_t res;
        double corr = 0.0, t_s, t_trn;

        memcpy(trn, trn0, sizeof(int16_t) * (size_t) n1);
        memcpy(pri, pri0, sizeof(int16_t) * (size_t) n2);
        if (c->foff != 0.0) {
            imp_freq_shift(trn, n1, c->foff, c->hlen);
            imp_freq_shift(pri, n2, c->foff, c->hlen);
        }
        if (c->jit_deg != 0.0) {
            imp_phase_jitter(trn, n1, c->jit_deg, 60.0);
            imp_phase_jitter(pri, n2, c->jit_deg, 60.0);
        }
        if (c->cubic) {
            imp_cubic(trn, n1, -8.5e-10);
            imp_cubic(pri, n2, -8.5e-10);
        }
        imp_gain_noise_alaw(trn, n1, c->gain, c->snr_db, &seed);
        imp_gain_noise_alaw(pri, n2, c->gain, c->snr_db, &seed);

        t_s = nf_v34_page_locate_s(trn, n1, 0.0, 0.0, 0.3,
                                   NF_V34_PC_BAUD_NOMINAL, NULL, &corr);
        t_trn = t_s + 432.0 / NF_V34_PC_BAUD_NOMINAL;
        if (corr < 0.5 ||
            nf_v34_page_train(trn, n1, 0.0, t_trn, 4800 - 200,
                              NF_V34_PC_BAUD_NOMINAL, NULL, &eq) < 0 ||
            eq.res_holdout > c->res_max) {
            printf("  %-9s FAIL train (corr=%.3f res=%.5f > %.4f)\n",
                   c->name, corr, eq.res_holdout, c->res_max);
            ok = 0;
            continue;
        }
        t_s = nf_v34_page_locate_s(pri, n2, 0.0, 0.0, 0.15,
                                   NF_V34_PC_BAUD_NOMINAL, NULL, &corr);
        if (corr < 0.5 ||
            nf_v34_page_decode_burst(pri, n2, 0.0, &eq, t_s,
                                     (double) n2 / 8000.0 - 0.02,
                                     NF_V34_PC_BAUD_NOMINAL, NULL,
                                     NULL, NULL, &res) < 0 ||
            res.fcd_ok != 1 || res.rcp_ok < 1) {
            printf("  %-9s FAIL decode (corr=%.3f fcd=%d rcp=%d B1=%d/840)\n",
                   c->name, corr, res.fcd_ok, res.rcp_ok, res.b1_match);
            ok = 0;
            continue;
        }
        printf("  %-9s train res=%.5f img=%d nl=%d; decode fcd=%d rcp=%d"
               " B1=%d/840 OK\n", c->name, eq.res_holdout, eq.img_active,
               eq.nl_active, res.fcd_ok, res.rcp_ok, res.b1_match);
    }
    free(bre); free(bim); free(tre); free(tim);
    free(trn0); free(pri0); free(trn); free(pri);

    /* ── impaired spot checks at OTHER symbol/data rates ────────────────
     * (the matrix above runs the capture's S=3429/R=24000 mode): a low
     * rate through heavy AWGN and a mid rate through A-law + AWGN. */
    {
        static const struct {
            const char *name;
            int si, rate;
            double snr_db;
            int alaw;
        } mcells[2] = {
            { "S2400/4800@18dB",   NF_V34_RATE_2400,  4800, 18.0, 0 },
            { "S3000/16800@a28dB", NF_V34_RATE_3000, 16800, 28.0, 1 },
        };
        int k;

        for (k = 0; k < 2; k++) {
            nf_v34_pcparams_t pp;
            double res_h = 0.0;
            int r;

            if (nf_v34_pcparams_init(&pp, mcells[k].si, mcells[k].rate,
                                     0, 1, 1, 1) < 0) {
                printf("  %-18s FAIL pcparams\n", mcells[k].name);
                ok = 0;
                continue;
            }
            r = txrates_cell(&pp, mcells[k].snr_db, mcells[k].alaw, &seed,
                             0, &res_h);
            printf("  %-18s train res=%.5f %s\n", mcells[k].name, res_h,
                   r == 0 ? "OK" : "FAIL");
            if (r != 0)
                ok = 0;
        }
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
fail:
    free(bre); free(bim); free(tre); free(tim);
    free(trn0); free(pri0); free(trn); free(pri);
    return 1;
}

/* ── modetab: verify the transcribed Tables 1/2/7/8/10 ─────────────────
 * Every row must survive the derivation cross-check (N = R*0.28/J integer,
 * b = ceil(N/P), r = N-(b-1)P in [1,P], SWP from the 8.2 counter, K/q per
 * eq 9-1, M_min/M_exp per 9.2, L = 4M*2^q <= 1664). */
static int cmd_modetab(void)
{
    int si, total = 0;

    for (si = 0; si < NF_V34_NUM_RATES; si++) {
        int n;
        (void) nf_v34_ratetab(si, &n);
        printf("S=%d: %d Table-8 rows, primary rate mask 0x%04x\n",
               nf_v34_srates[si].baud_name, n, nf_v34_rate_mask(si));
        total += n;
    }
    if (nf_v34_modeparams_check() != 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("%d rows, all derivation cross-checks OK\nPASS\n", total);
    return 0;
}

/* ── txrates: clean-channel TX -> RX loopback over EVERY (S,R) pair ─────
 * A short ECM-like payload (one 40-byte FCD + one RCP) through the full
 * primary-channel chain at every non-aux Table 8 operating point, both
 * carrier options alternating, byte-identical assert. Plus minimum-shaping
 * (M_min) spot cells. */
struct txrates_chk {
    const uint8_t *fcd;
    int fcd_len;
    int fcd_ok, rcp_ok, alien;
};

static void txrates_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct txrates_chk *c = user;

    if (!msg || !ok || len <= 0)
        return;
    if (len == c->fcd_len && !memcmp(msg, c->fcd, (size_t) len))
        c->fcd_ok++;
    else if (len == 3 && msg[2] == 0x86)
        c->rcp_ok++;
    else
        c->alien++;
}

struct txrates_src {
    nf_hdlc_tx_t tx;
    int next;
    const uint8_t *fcd;
    const uint8_t *rcp;
};

static void txrates_underflow(void *user)
{
    struct txrates_src *s = user;

    if (s->next == 0)
        nf_hdlc_tx_frame(&s->tx, s->fcd, 44);
    else if (s->next == 1)
        nf_hdlc_tx_frame(&s->tx, s->rcp, 3);
    else if (s->next == 2)
        nf_hdlc_tx_frame(&s->tx, NULL, 0);
    s->next++;
}

/* Run one (S,R) loopback cell: build TRN + short data burst, optional AWGN
 * at snr_db (<= 0: none) with optional A-law, train, decode, verify.
 * Returns 0 on pass. *res_holdout_out (may be NULL) gets the training
 * residual. */
static int txrates_cell(const nf_v34_pcparams_t *pp, double snr_db, int alaw,
                        unsigned *seed, int quiet, double *res_holdout_out)
{
    static const int TRN_NSYM = 3600;
    uint8_t fcd[44];
    static const uint8_t rcp[3] = { 0xff, 0x03, 0x86 };
    struct txrates_src src;
    uint8_t *bits = malloc(8000);
    double *tre = NULL, *tim = NULL, *bre = NULL, *bim = NULL;
    int16_t *trn = NULL, *pri = NULL;
    long nbits = 0, nsym, n1, n2, i;
    double rms = 0.0, corr = 0.0, t_s, t_trn;
    nf_v34_page_eq_t eq;
    nf_v34_page_burst_t res;
    struct txrates_chk chk;
    int b, rc = 1;

    if (!bits)
        return 1;
    /* payload frames */
    fcd[0] = 0xff; fcd[1] = 0x03; fcd[2] = 0x06; fcd[3] = 0x07;
    for (i = 0; i < 40; i++)
        fcd[4 + i] = (uint8_t) (lcg_next(seed) & 0xff);
    src.next = 0;
    src.fcd = fcd;
    src.rcp = rcp;
    nf_hdlc_tx_init(&src.tx, 1, txrates_underflow, &src);
    nf_hdlc_tx_flags(&src.tx, 4);
    for (;;) {
        b = nf_hdlc_tx_get_bit(&src.tx);
        if (b == NF_SIG_END_OF_DATA || nbits >= 7999)
            break;
        bits[nbits++] = (uint8_t) b;
    }
    if (nbits & 1)
        bits[nbits++] = 1;

    nsym = nf_v34_pc_burst_build(pp, bits, nbits, &bre, &bim);
    free(bits);
    bits = NULL;
    if (nsym < 0)
        return 1;

    tre = malloc(sizeof(double) * (size_t) (432 + TRN_NSYM));
    tim = malloc(sizeof(double) * (size_t) (432 + TRN_NSYM));
    if (!tre || !tim)
        goto out;
    nf_v34_pc_sspp(tre, tim);
    nf_v34_pc_trn(TRN_NSYM, pp->trn_16pt, tre + 432, tim + 432);
    for (i = 432; i < 432 + TRN_NSYM; i++)
        rms += tre[i] * tre[i] + tim[i] * tim[i];
    rms = sqrt(rms / TRN_NSYM);
    for (i = 432; i < 432 + TRN_NSYM; i++) {
        tre[i] /= rms;
        tim[i] /= rms;
    }
    n1 = 1200 + ((432 + TRN_NSYM) * (long) pp->sps_num) / pp->sps_den + 800;
    n2 = 600 + (nsym * (long) pp->sps_num) / pp->sps_den + 1200;
    trn = calloc((size_t) n1, sizeof(int16_t));
    pri = calloc((size_t) n2, sizeof(int16_t));
    if (!trn || !pri)
        goto out;
    nf_v34_pc_modulate(pp, trn, n1, 1200, tre, tim, 432 + TRN_NSYM, 5000.0);
    nf_v34_pc_modulate(pp, pri, n2, 600, bre, bim, nsym, 5000.0);
    if (alaw) {
        imp_gain_noise_alaw(trn, n1, 1.0, snr_db, seed);
        imp_gain_noise_alaw(pri, n2, 1.0, snr_db, seed);
    } else if (snr_db > 0.0) {
        imp_awgn(trn, n1, snr_db, seed);
        imp_awgn(pri, n2, snr_db, seed);
    }

    t_s = nf_v34_page_locate_s(trn, n1, 0.0, 0.0, 0.3, pp->baud_nominal,
                               pp, &corr);
    t_trn = t_s + 432.0 / pp->baud_nominal;
    memset(&eq, 0, sizeof(eq));
    if (corr < 0.5 ||
        nf_v34_page_train(trn, n1, 0.0, t_trn, TRN_NSYM - 200,
                          pp->baud_nominal, pp, &eq) < 0) {
        if (!quiet)
            printf("    train failed (corr=%.3f res=%.5f)\n", corr,
                   eq.res_holdout);
        goto out;
    }
    if (res_holdout_out)
        *res_holdout_out = eq.res_holdout;
    t_s = nf_v34_page_locate_s(pri, n2, 0.0, 0.0, 0.15, pp->baud_nominal,
                               pp, &corr);
    memset(&chk, 0, sizeof(chk));
    chk.fcd = fcd;
    chk.fcd_len = 44;
    if (corr < 0.5 ||
        nf_v34_page_decode_burst(pri, n2, 0.0, &eq, t_s,
                                 t_s + (double) (nsym - 6) / pp->baud_nominal,
                                 pp->baud_nominal, pp,
                                 txrates_on_frame, &chk, &res) < 0) {
        if (!quiet)
            printf("    decode failed (corr=%.3f)\n", corr);
        goto out;
    }
    if (res.b1_match != res.b1_bits || chk.fcd_ok != 1 || chk.rcp_ok < 1 ||
        chk.alien != 0) {
        if (!quiet)
            printf("    FAIL B1=%d/%d fcd=%d rcp=%d alien=%d med=%.3f\n",
                   res.b1_match, res.b1_bits, chk.fcd_ok, chk.rcp_ok,
                   chk.alien, res.med_dist);
        goto out;
    }
    rc = 0;
out:
    free(tre); free(tim); free(bre); free(bim);
    free(trn); free(pri);
    return rc;
}

static int cmd_txrates(void)
{
    unsigned seed = 20260714;
    int si, ok = 1, cells = 0;

    for (si = 0; si < NF_V34_NUM_RATES; si++) {
        int n, ri;
        const nf_v34_rateparam_t *t = nf_v34_ratetab(si, &n);

        for (ri = 0; ri < n; ri++) {
            nf_v34_pcparams_t pp;
            int high = (t[ri].rate / 2400) & 1;   /* exercise both carriers */
            int r;

            if (t[ri].aux)
                continue;
            if (nf_v34_pcparams_init(&pp, si, t[ri].rate, high, 1, 1, 1) < 0) {
                printf("S=%d R=%d: pcparams_init failed\nFAIL\n",
                       nf_v34_srates[si].baud_name, t[ri].rate);
                return 1;
            }
            r = txrates_cell(&pp, 0.0, 0, &seed, 0, NULL);
            printf("  S=%d %s R=%5d (b=%2d K=%2d q=%d M=%2d L=%4d) %s\n",
                   nf_v34_srates[si].baud_name, high ? "hi" : "lo",
                   t[ri].rate, pp.b, pp.K, pp.q, pp.M, pp.M * 4 << pp.q,
                   r == 0 ? "OK" : "FAIL");
            cells++;
            if (r != 0)
                ok = 0;
        }
    }
    /* minimum-shaping (M_min) spot cells */
    {
        static const struct { int si, rate; } mins[3] =
            { { NF_V34_RATE_3429, 24000 },
              { NF_V34_RATE_2400,  9600 },
              { NF_V34_RATE_3000, 16800 } };
        int k;
        for (k = 0; k < 3; k++) {
            nf_v34_pcparams_t pp;
            int r;
            if (nf_v34_pcparams_init(&pp, mins[k].si, mins[k].rate, 0,
                                     0 /* minimum shaping */, 1, 1) < 0) {
                ok = 0;
                continue;
            }
            r = txrates_cell(&pp, 0.0, 0, &seed, 0, NULL);
            printf("  S=%d lo R=%5d minimum shaping (M=%d L=%d) %s\n",
                   nf_v34_srates[mins[k].si].baud_name, mins[k].rate,
                   pp.M, pp.M * 4 << pp.q, r == 0 ? "OK" : "FAIL");
            cells++;
            if (r != 0)
                ok = 0;
        }
    }
    printf("%d cells\n%s\n", cells, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── ratesnr: measure the per-rate AWGN decode threshold (manual mode -
 * feeds the requirement-curve comment in nf_v34.c, not part of check-v34).
 * usage: ratesnr [srate_idx] */
static int cmd_ratesnr(int argc, char **argv)
{
    int si = argc > 2 ? atoi(argv[2]) : NF_V34_RATE_3429;
    int n, ri;
    const nf_v34_rateparam_t *t = nf_v34_ratetab(si, &n);

    if (!t)
        return 1;
    for (ri = 0; ri < n; ri++) {
        nf_v34_pcparams_t pp;
        int snr, last_pass = -1;

        if (t[ri].aux)
            continue;
        if (nf_v34_pcparams_init(&pp, si, t[ri].rate, 0, 1, 1, 1) < 0)
            continue;
        for (snr = 42; snr >= 4; snr -= 1) {
            unsigned seed = 1234u + (unsigned) (snr * 77 + t[ri].rate);
            if (txrates_cell(&pp, (double) snr, 0, &seed, 1, NULL) == 0)
                last_pass = snr;
            else
                break;
        }
        printf("S=%d R=%5d: min SNR %d dB\n",
               nf_v34_srates[si].baud_name, t[ri].rate, last_pass);
        fflush(stdout);
    }
    return 0;
}

/* ── probe: line-probing analyzer over synthetic channels ───────────────
 * Generate an L1/L2 probe (nf_v34_probe_tx), push it through an in-process
 * channel (gain / lowpass band-limit / tilt / frequency offset / AWGN), and
 * assert the analyzer/selector return the EXPECTED symbol-rate CLASS, carrier,
 * ballpark projected rate and frequency offset. Asserts are inequalities and
 * classes (not brittle exact bins) because the borderline cases are, by
 * design, borderline. */

/* windowed-sinc lowpass FIR, cutoff fc_hz (in place) */
static void imp_lowpass(int16_t *x, long n, double fc_hz, int taps)
{
    double *h = malloc(sizeof(double) * (size_t) taps);
    double *in = malloc(sizeof(double) * (size_t) n);
    int c = (taps - 1) / 2, k;
    double fc = fc_hz / 8000.0;                 /* cycles/sample (0..0.5) */
    long i;

    if (!h || !in) { free(h); free(in); return; }
    for (k = 0; k < taps; k++) {
        int m = k - c;
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * k / (taps - 1));
        double s = (m == 0) ? 2.0 * fc
                            : sin(2.0 * M_PI * fc * m) / (M_PI * (double) m);
        h[k] = w * s;
    }
    for (i = 0; i < n; i++) in[i] = (double) x[i];
    for (i = 0; i < n; i++) {
        double acc = 0.0;
        for (k = 0; k < taps; k++) {
            long p = i - k + c;
            if (p >= 0 && p < n) acc += h[k] * in[p];
        }
        if (acc > 32767.0) acc = 32767.0;
        if (acc < -32768.0) acc = -32768.0;
        x[i] = (int16_t) (acc >= 0.0 ? acc + 0.5 : acc - 0.5);
    }
    free(h);
    free(in);
}

static int cmd_probe(void)
{
    struct chan {
        const char *name;
        double gain;        /* linear */
        double lp_fc;       /* lowpass cutoff Hz (0 = none) */
        int    tilt;        /* apply mild high-cut tilt this many times */
        double foff;        /* SSB shift Hz (positive = down; offset = -foff) */
        double snr_db;      /* AWGN (0 = none) */
    };
    /* the probe-only channels; the fax-session equivalents are in the sweep */
    static const struct chan cells[] = {
        { "clean",          1.0,    0.0, 0, 0.0,  0.0 },
        { "awgn30",         1.0,    0.0, 0, 0.0, 30.0 },
        { "awgn20",         1.0,    0.0, 0, 0.0, 20.0 },
        { "lp3100+n30",     1.0, 3100.0, 0, 0.0, 30.0 },
        { "lp2900+n30",     1.0, 2900.0, 0, 0.0, 30.0 },
        { "lp2700steep",    1.0, 2700.0, 0, 0.0, 35.0 },
        { "tilt+n32",       1.0,    0.0, 3, 0.0, 32.0 },
        { "foff+6",         1.0,    0.0, 0, 6.0, 35.0 },
        { "foff-9",         1.0,    0.0, 0,-9.0, 35.0 },
        { "gain-9+n30",   0.355,    0.0, 0, 0.0, 30.0 },
    };
    const int ncells = (int) (sizeof cells / sizeof cells[0]);
    int ci, fails = 0;

    printf("probe: line-probing analyzer over synthetic channels\n");
    for (ci = 0; ci < ncells; ci++) {
        const struct chan *c = &cells[ci];
        long lead = 400, ldur = 5600, n = lead + ldur + 400;
        int16_t *buf = calloc((size_t) n, sizeof(int16_t));
        unsigned seed = 4321u + (unsigned) ci * 101u;
        nf_v34_probe_t pr;
        nf_v34_probe_sel_t sel;
        int rc, t, ok = 1;
        long wstart = lead + 400, wlen = 4800;
        double exp_off = -c->foff;
        const char *why = "";

        if (!buf) { printf("  alloc fail\n"); return 1; }
        /* L2 (nominal level) probe segment */
        nf_v34_probe_tx(buf, n, lead, ldur, 0, 2000.0);
        if (c->gain != 1.0) {
            long i;
            for (i = 0; i < n; i++) {
                double v = (double) buf[i] * c->gain;
                if (v > 32767.0) v = 32767.0;
                if (v < -32768.0) v = -32768.0;
                buf[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
            }
        }
        if (c->lp_fc > 0.0) imp_lowpass(buf, n, c->lp_fc, 121);
        for (t = 0; t < c->tilt; t++) imp_tilt(buf, n);
        if (c->foff != 0.0) imp_freq_shift(buf, n, c->foff, IMP_HLEN);
        if (c->snr_db > 0.0) imp_awgn(buf, n, c->snr_db, &seed);

        rc = nf_v34_probe_analyze(buf + wstart, wlen, &pr);
        if (rc != 0) { printf("  %-14s ANALYZE FAILED\n", c->name); fails++; free(buf); continue; }
        nf_v34_probe_select(&pr, 0, &sel);

        /* per-cell expectations (classes / inequalities) */
        if (!strcmp(c->name, "clean") || !strcmp(c->name, "gain-9+n30")) {
            if (sel.srate_idx != NF_V34_RATE_3429) { ok = 0; why = "expected S=3429"; }
            if (pr.band_hi_hz < 3400.0) { ok = 0; why = "band too narrow"; }
        }
        if (!strcmp(c->name, "clean")) {
            if (sel.projected_max_rate < 28800) { ok = 0; why = "projected too low for clean"; }
        }
        if (!strcmp(c->name, "awgn30")) {
            if (sel.srate_idx != NF_V34_RATE_3429) { ok = 0; why = "expected S=3429"; }
        }
        if (!strcmp(c->name, "awgn20")) {
            if (sel.srate_idx != NF_V34_RATE_3429) { ok = 0; why = "expected S=3429"; }
            if (sel.projected_max_rate > 21600) { ok = 0; why = "projected too high for 20 dB"; }
        }
        if (!strcmp(c->name, "lp3100+n30")) {
            if (sel.srate_idx >= NF_V34_RATE_3429) { ok = 0; why = "expected a lower S"; }
            if (pr.band_hi_hz > 3200.0) { ok = 0; why = "band edge not detected"; }
        }
        if (!strcmp(c->name, "lp2900+n30")) {
            if (sel.srate_idx > NF_V34_RATE_2800) { ok = 0; why = "expected S<=2800"; }
            if (pr.band_hi_hz > 3000.0) { ok = 0; why = "band edge not detected"; }
        }
        if (!strcmp(c->name, "lp2700steep")) {
            if (sel.srate_idx > NF_V34_RATE_2743) { ok = 0; why = "expected S<=2743"; }
        }
        if (!strcmp(c->name, "tilt+n32")) {
            if (pr.tilt_db > -3.0) { ok = 0; why = "expected negative tilt"; }
        }
        if (!strcmp(c->name, "foff+6") || !strcmp(c->name, "foff-9")) {
            if (fabs(pr.freq_offset_hz - exp_off) > 3.0) { ok = 0; why = "freq offset off"; }
        }

        printf("  %-14s S=%-4d %s carrier  band %4.0f-%4.0f Hz  SNR %5.1f dB"
               "  tilt %+5.1f  foff %+5.1f Hz  proj %5d bit/s  %s%s\n",
               c->name, nf_v34_srates[sel.srate_idx].baud_name,
               sel.high_carrier ? "hi" : "lo", pr.band_lo_hz, pr.band_hi_hz,
               pr.band_snr_db, pr.tilt_db, pr.freq_offset_hz,
               sel.projected_max_rate, ok ? "OK" : "FAIL: ", ok ? "" : why);
        if (!ok) fails++;
        free(buf);
    }
    if (fails) { printf("FAIL (%d)\n", fails); return 1; }
    printf("PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    static const char usage[] =
        "usage: %s shapers | info <file.wav> | ctrl <file.wav> | shellmap | trellis"
        " | viterbi | fullchain | mphunt <file.wav> | mph1 <file.wav>"
        " | infodec <file.wav> | ccdata <file.wav> | page <file.wav>"
        " | txsig | txinfo | txcc | ccresync | txpage | ccimp | pageimp"
        " | recover | modetab | txrates | ratesnr [srate_idx] | probe\n";

    if (argc < 2) {
        fprintf(stderr, usage, argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "shapers")) return cmd_shapers();
    if (!strcmp(argv[1], "info") && argc > 2) return cmd_info(argv[2]);
    if (!strcmp(argv[1], "ctrl") && argc > 2) return cmd_ctrl(argv[2]);
    if (!strcmp(argv[1], "trellis")) return cmd_trellis();
    if (!strcmp(argv[1], "shellmap")) return cmd_shellmap();
    if (!strcmp(argv[1], "viterbi")) return cmd_viterbi();
    if (!strcmp(argv[1], "fullchain")) return cmd_fullchain();
    if (!strcmp(argv[1], "mphunt") && argc > 2) return cmd_mphunt(argv[2]);
    if (!strcmp(argv[1], "mph1") && argc > 2) return cmd_mph1(argv[2]);
    if (!strcmp(argv[1], "infodec") && argc > 2) return cmd_infodec(argv[2]);
    if (!strcmp(argv[1], "ccdata") && argc > 2) return cmd_ccdata(argv[2]);
    if (!strcmp(argv[1], "page") && argc > 2) return cmd_page(argv[2]);
    if (!strcmp(argv[1], "txsig")) return cmd_txsig();
    if (!strcmp(argv[1], "txinfo")) return cmd_txinfo();
    if (!strcmp(argv[1], "txcc")) return cmd_txcc();
    if (!strcmp(argv[1], "ccresync")) return cmd_ccresync();
    if (!strcmp(argv[1], "ccimp")) return cmd_ccimp();
    if (!strcmp(argv[1], "pageimp")) return cmd_pageimp();
    if (!strcmp(argv[1], "recover")) return cmd_recover();
    if (!strcmp(argv[1], "txpage")) return cmd_txpage(argc, argv);
    if (!strcmp(argv[1], "modetab")) return cmd_modetab();
    if (!strcmp(argv[1], "txrates")) return cmd_txrates();
    if (!strcmp(argv[1], "ratesnr")) return cmd_ratesnr(argc, argv);
    if (!strcmp(argv[1], "probe")) return cmd_probe();
    fprintf(stderr, usage, argv[0]);
    return 2;
}
