#ifndef NF_V34_H
#define NF_V34_H

#include <stdint.h>
#include "nf_dsp.h"
#include "nf_hdlc.h"

/*
 * nf_v34 - ITU-T V.34 modem (T.30 Annex F "Super G3" data pump).
 *
 * Status: COMPLETE for the loopback fax path. Phase B added the half-duplex
 * session state machine (nf_v34_sess_t at the end of this header - clause 12
 * Phases 2-3, control-channel startup, steady-state control/primary
 * alternation) which nf_fax drives via NF_MODEM_V34 and nf_t30 runs the
 * T.30 Annex F procedures over - see check-v34fax for the end-to-end
 * pixel-exact page transfer between two nf_t30 engines. BOTH directions of
 * the signal math exist in batch form (the session driver energy-gates
 * receive audio into per-burst buffers and runs these batch decoders under
 * a streaming facade): the RECEIVE side of the primary
 * channel is validated end-to-end against the real capture - see
 * nf_v34_page_train/nf_v34_page_decode_burst below, which recover the two
 * ECM image blocks as FCS-valid HDLC frames - and the TRANSMIT side (all
 * the nf_v34_*_tx / nf_v34_pc_* primitives at the end of this header) is
 * loopback-validated against those same capture-validated receivers,
 * including through an impaired channel (G.711 A-law + AWGN + sampling-
 * clock offset - see check-v34's txpage). What exists and is real, not
 * scaffolding:
 *
 *   - The six symbol-rate/carrier table (`nf_v34_rates[]`) and per-rate RRC
 *     shaper tables (`nf_v34_init()`) - built, sanity-tested, not yet used
 *     by anything (the main data pump would use these).
 *   - `nf_v34_ctrl_rx_t` / `nf_v34_ctrl_rx()`: a WORKING, VALIDATED
 *     demodulator for a real signal found in references/v.34_modem_test.wav
 *     - a 600-baud QPSK control-channel/retrain exchange at ~1200 Hz that
 *       recurs at every half-duplex turnaround in that real captured call.
 *       Carrier acquisition, Gardner symbol-timing recovery and a QPSK
 *       Costas carrier loop were built, debugged (see git history / session
 *       notes: the Gardner loop initially had an inverted correction sign,
 *       found by a synthetic-signal self-test, and the Costas loop's phase-
 *       word scaling was initially off by a factor of ~2^16, found by
 *       measuring lock quality against the real capture across a gain
 *       sweep), and tuned entirely empirically against this real audio -
 *       see `make check-v34`'s `ctrl` mode for the regression check.
 *
 * This module's main data pump would need line probing, INFO0/INFOh framing
 * (the RECEIVE side of which now exists and is capture-validated - see
 * nf_v34_info_rx_batch below), a 4D trellis Viterbi decoder, shell-mapping/
 * nonlinear-encoder inversion and a self-synchronizing descrambler - most
 * of which do not have an accessible
 * primary source on this machine (spandsp-master's v34tx.c/v34rx.c/
 * v34_tables.h are readable as algorithmic reference only; that V.34 does not
 * actually train, so it is not an oracle - see the project plan). That work
 * is reconstructed from general DSP/comms knowledge and validated the same
 * way this control-channel receiver was: empirically, against the real
 * capture, not against a spec citation.
 *
 * Carrier frequencies: `nf_v34_rates[]` keeps the rounded Table 2 values
 * for display; the EXACT rational carriers ((d/e)*S, verified against the
 * PDF's Table 2 and re-derived by nf_v34_modeparams_check) live in
 * `nf_v34_srates[]` below, which the mode-parameterized TX/RX paths use.
 */

typedef struct {
    int    baud;             /* symbol rate, baud                              */
    double carrier_low_hz;   /* low-carrier option, ITU-T V.34 Table 2 (exact) */
    double carrier_high_hz;  /* high-carrier option, ITU-T V.34 Table 2        */
} nf_v34_rate_t;

/* Index matches NF_V34_RATE_* below. */
extern const nf_v34_rate_t nf_v34_rates[6];

enum {
    NF_V34_RATE_2400 = 0,
    NF_V34_RATE_2743,
    NF_V34_RATE_2800,
    NF_V34_RATE_3000,
    NF_V34_RATE_3200,
    NF_V34_RATE_3429,
    NF_V34_NUM_RATES
};

/* ── complete mode parameter tables (Tables 1/2/7/8/10, stage: all rates) ─
 *
 * Every value below is either transcribed from the recommendation PDF in
 * references/ or derived from its EXACT rational definitions - never from
 * rounded decimals ("3429" is a display name; the symbol rate is 24000/7).
 * All transcription is verified programmatically by
 * nf_v34_modeparams_check(): N = R*0.28/J (must be integer), b = ceil(N/P),
 * r = N-(b-1)P in [1,P], SWP re-derived from the 8.2 counter algorithm,
 * K/q per eq 9-1, M_min/M_exp per 9.2's 2^(K/8) rules, L = 4M*2^q (9-2) -
 * a mismatch in any row is a transcription bug and fails check-v34's
 * `modetab` mode. */

/* Per-symbol-rate constants (Tables 1, 2, 7). Index = NF_V34_RATE_*. */
typedef struct {
    int baud_name;       /* display name (2400..3429)                       */
    int a, c;            /* S = (a/c) * 2400 exactly (Table 1)              */
    int sps_num, sps_den;/* samples/symbol at 8 kHz = num/den, lowest terms */
    int J, P;            /* framing parameters (Table 7)                    */
    /* carriers (Table 2): frequency = (d/e)*S; cnum/cden = exact
     * cycles/sample at 8 kHz (used by the exact-rational modulator) */
    struct {
        int d, e;
        int cnum, cden;
    } car[2];            /* [0] = low carrier option, [1] = high            */
    int trn_unit_sym;    /* symbols per 35 ms TRN unit = 84*a/c (integer)   */
} nf_v34_srate_t;

extern const nf_v34_srate_t nf_v34_srates[6];

/* Per-(symbol rate, data rate) mapping parameters (Tables 8 and 10).
 * Rows with aux = 1 are the R = primary + 200 bit/s auxiliary-channel
 * variants (8.3) - transcribed and cross-checked, but not usable until the
 * aux channel itself (AMP bit substitution) is implemented. */
typedef struct {
    int rate;            /* R, bit/s (sum of primary + aux, Table 8)        */
    int aux;             /* 1 = includes the 200 bit/s aux channel          */
    int b;               /* bits per high mapping frame                     */
    uint16_t swp;        /* switching pattern, P bits, bit (P-1-i)=frame i  */
    int K;               /* shell-mapper bits (Table 10 / eq 9-1)           */
    int q;               /* raw Q bits per 2D symbol (eq 9-1)               */
    int m_min, m_exp;    /* shell rings, minimum / expanded (Table 10)      */
} nf_v34_rateparam_t;

/* All Table 8 rows for symbol rate sr_idx; *count receives the row count. */
const nf_v34_rateparam_t *nf_v34_ratetab(int sr_idx, int *count);

/* Row lookup for one (S, R); NULL if that combination does not exist. */
const nf_v34_rateparam_t *nf_v34_rateparam(int sr_idx, int rate);

/* Verify EVERY table row against the derivation rules above. Returns 0 if
 * all rows check out; prints each failure to stderr and returns -1. */
int nf_v34_modeparams_check(void);

/* Bitmask of PRIMARY data rates available at sr_idx, in MPh rate-mask
 * convention (bit 0 = 2400 bit/s, bit 13 = 33600; Table 23 bits 35:48). */
int nf_v34_rate_mask(int sr_idx);

/* ── one negotiated primary-channel operating point ─────────────────────
 * Everything the TX encoder and page decoder need for one (S, R, carrier,
 * shaping, TRN constellation) choice, precomputed by nf_v34_pcparams_init.
 * The page decoder additionally takes the received symbol clock as a
 * separate argument (a real receiver measures it; see the `baud` params
 * below) - baud_nominal here is the exact TX-side clock. */
typedef struct {
    int sr_idx;          /* 0..5 (NF_V34_RATE_*)                            */
    int rate;            /* R bit/s                                         */
    int high_carrier;    /* Table 2 option                                  */
    int expanded;        /* shaping: 1 = expanded M, 0 = minimum (Table 10) */
    int trn_16pt;        /* TRN/B1-preamble constellation (INFOh bit 30)    */
    /* derived: */
    int M, q, K, b, P, J;
    uint16_t swp;
    double theta;        /* 9.7 non-linear parameter (0.3125 when enabled)  */
    double baud_nominal; /* exact (a/c)*2400                                */
    double carrier_hz;   /* exact (d/e)*S                                   */
    int sps_num, sps_den, cnum, cden;
    int b1_sym;          /* B1 length = one data frame = 8P symbols         */
    int b1_bits;         /* raw data bits in one data frame (SWP-summed)    */
    int trn_unit_sym;    /* symbols per 35 ms TRN unit                      */
} nf_v34_pcparams_t;

/* Fill *pp for (sr_idx, rate, ...). rate must be a non-aux Table 8 row at
 * this symbol rate. nonlinear selects theta 0.3125 vs 0. Returns 0, or -1
 * for an invalid combination. */
int nf_v34_pcparams_init(nf_v34_pcparams_t *pp, int sr_idx, int rate,
                         int high_carrier, int expanded, int nonlinear,
                         int trn_16pt);

#define NF_V34_RRC_SETS 8      /* polyphase tx interpolator resolution   */
#define NF_V34_RRC_TAPS 33     /* matches nf_qam's rx filter length      */

/* One rate's shaping filters: `tx` is the real (baseband) interpolator
 * (NF_V34_RRC_SETS phases x NF_V34_RRC_TAPS taps); `rx_re`/`rx_im` are the
 * complex bandpass matched filter at that rate's nominal carrier. */
typedef struct {
    float tx[NF_V34_RRC_SETS * NF_V34_RRC_TAPS];
    float rx_re[NF_V34_RRC_SETS * NF_V34_RRC_TAPS];
    float rx_im[NF_V34_RRC_SETS * NF_V34_RRC_TAPS];
} nf_v34_shaper_t;

typedef struct nf_v34_s {
    nf_v34_shaper_t shaper[NF_V34_NUM_RATES];   /* built once, at init */
} nf_v34_t;

/* Builds the RRC shaper tables for all six rates. Nothing else - see the
 * file comment above. */
void nf_v34_init(nf_v34_t *s);

/* ── control-channel QPSK demodulator ──────────────────────────────────
 *
 * NOT scaffolding - this one is real and validated against
 * references/v.34_modem_test.wav: at every half-duplex turnaround in that
 * capture, the caller side transmits a clean, lockable QPSK signal at
 * 600 baud on a ~1200 Hz carrier (almost certainly the V.34 retrain/INFO
 * exchange - a much simpler, more robust modulation than the main 3429-baud
 * data pump). This receiver locks carrier + symbol timing on that signal and
 * outputs one QPSK dibit per baud. See tests/nf_v34test.c's `ctrl` mode for
 * the regression check against the real capture.
 *
 * Front end: downconvert -> real RRC lowpass filter (applied to both the
 * real and imaginary rails of the complex baseband) -> Gardner timing
 * recovery via direct fractional-sample interpolation of the filtered
 * stream (no resampling stage) -> QPSK-decision-directed Costas carrier
 * loop. Deliberately simple (no adaptive equalizer yet - unneeded so far,
 * this signal locks cleanly without one); nf_qam's more elaborate
 * equalizer/AGC machinery is a candidate future refinement once this
 * baseline is solid.
 */

#define NF_V34_CTRL_BAUD   600
#define NF_V34_CTRL_TAPS   33     /* real RRC lowpass, applied post-downconvert */
/* The BATCH cc demod uses a longer filter: it must bury the answer role's
 * 1800 Hz guard tone (-6 dB, only 600 Hz below the 2400 carrier). 33 taps
 * left it at -21.7 dB and the residual CW jitter made real-line cc decodes
 * marginal; 129 taps put it at -48 dB. (The streaming nf_v34_ctrl_rx keeps
 * 33 - its capture regression thresholds are calibrated to it.) */
#define NF_V34_CC_BATCH_TAPS 129
#define NF_V34_CTRL_HIST   4      /* filtered-sample history kept for interpolation */

typedef struct {
    /* downconvert LO */
    uint32_t lo_phase;
    int32_t  lo_rate;

    /* real RRC lowpass (design: t_step = baud/8000, carrier_hz = 0) */
    float    rrc[NF_V34_CTRL_TAPS];
    nf_cpx_t firbuf[NF_V34_CTRL_TAPS];
    int      firptr;

    /* running AGC on the filtered signal - without this, the Gardner loop's
     * error term (proportional to amplitude squared) is wildly out of scale
     * for real int16 audio and the loop free-runs instead of tracking baud
     * (found the hard way porting the validated Python prototype to C). */
    float    agc_power;             /* EMA of |filtered sample|^2            */
    float    agc_scale;

    /* short history of filtered samples for fractional interpolation,
     * hist[0] = most recent, indexed by (native 8kHz) sample count */
    nf_cpx_t hist[NF_V34_CTRL_HIST];
    long     hist_n;                 /* total filtered samples seen so far */

    /* Gardner timing recovery (fractional index into the native sample
     * stream, in units of samples; steps by baud_period_samples/2) */
    double   baud_period;            /* 8000.0/baud samples/symbol */
    double   idx;                    /* next mid-point sample position */
    int      have_mid;
    nf_cpx_t mid_sample;
    nf_cpx_t prev_symbol;
    double   gardner_gain;

    /* QPSK decision-directed Costas carrier loop (same PI-on-a-32-bit-phase-
     * word convention as nf_qam_track_carrier, reimplemented locally so this
     * module has no nf_qam dependency) */
    uint32_t carrier_phase;
    int32_t  carrier_rate;
    float    track_p, track_i;

    /* output of the most recently completed baud */
    int      got_symbol;
    nf_cpx_t symbol;         /* carrier-corrected, unit-ish amplitude       */
    int      dibit;          /* 0..3, nearest-QPSK-point quadrant           */
} nf_v34_ctrl_rx_t;

void nf_v34_ctrl_rx_init(nf_v34_ctrl_rx_t *s, double carrier_hz);

/* Feed `len` int16 8kHz samples. Calls nothing back - the caller polls
 * s->got_symbol after each call (set true once per completed baud; the
 * caller should check it after every sample or just after each nf_v34_ctrl_rx
 * call and consume s->symbol/s->dibit before the next one completes). */
void nf_v34_ctrl_rx(nf_v34_ctrl_rx_t *s, const int16_t *amp, int len,
                    void (*on_symbol)(void *user, const nf_cpx_t *z, int dibit),
                    void *user);

/* ── shell mapper (ITU-T V.34 (02/98) 9.4) ──────────────────────────────
 *
 * REAL, spec-derived and round-trip validated (see tests/nf_v34test.c's
 * `shellmap` mode) - not scaffolding. Maps a K-bit integer R0 (the shell
 * mapper's input bits, K < 32) to 8 ring indices m[j][k] (0 <= m[j][k] < M,
 * j=0..3, k=0..1) per equations 9-3 to 9-24, and back. This is the primary
 * channel data pump's per-mapping-frame amplitude-shaping step; still
 * missing around it: the parser (9.3), differential encoder (9.5), mapper/
 * precoder/trellis encoder (9.6), and the convolutional/Viterbi decoder
 * (9.6.3) - not yet implemented.
 *
 * IMPORTANT precoder correction (an earlier revision of this comment
 * claimed the 9.6.2 precoder "is transmit-side only and does not need
 * inverting at the receiver" - that is WRONG): although precoding runs in
 * the transmitter, the transmitted point is x(n) = y(n) - p(n) + c(n),
 * so whenever the negotiated h(1..3) coefficients are non-zero the
 * receiver must reconstruct p(n)/c(n) by decision feedback (filtering its
 * own past decisions through the MPh-reported h taps) and remove them to
 * recover y(n). It merely HAPPENS to be a near-no-op for this repo's
 * reference capture, whose negotiated taps are tiny (|h| ~ 0.016 - see
 * nf_v34_mp_rx_t's Type-1 decode) - that is a property of this one line,
 * not of V.34.
 */
void nf_v34_shell_map(int M, uint32_t R0, int rings[4][2]);
uint32_t nf_v34_shell_unmap(int M, const int rings[4][2]);

/* ── trellis encoder / Viterbi decoder building blocks (9.6.3) ──────────
 *
 * REAL and validated - not scaffolding, but validated so far only via a
 * synthetic Python prototype (see tests/nf_v34test.c's `trellis` mode for
 * the C-side consistency check ported from it), not yet wired to real
 * captured audio. Two independent things were cross-checked against
 * spandsp's v34tx.c/v34rx.c/make_v34_convolutional_coders.c (read as
 * algorithmic reference only, not copied) before trusting this:
 *   - nf_v34_subset_label()'s bit formula matches spandsp's
 *     get_binary_subset_label() bit-for-bit, and independently matches all
 *     16 points shown in the recommendation's Figure 9 by direct check.
 *   - nf_v34_trellis_step()'s recurrence matches spandsp's
 *     make_v34_16_state_convolutional_encoder() output table exactly.
 *
 * IMPORTANT correctness note (found and fixed by rigorous synthetic
 * testing this session, see git history/session notes): a first attempt at
 * a Viterbi decoder modeled the trellis branch as being selected by the
 * *value* of (Y2,Y1) derived from the observed subset-label pair - this is
 * wrong and creates a silent, total degeneracy (every state ends up with
 * identical path metrics forever, so the decoder always agrees with naive
 * per-symbol slicing - no coding gain, no error correction, despite
 * appearing to run correctly). The real constraint the trellis code
 * enforces is on U0(m) (from the encoder's state) fixing the *parity* of
 * the rotation difference between the two 2D symbols of a 4D interval,
 * per 9.6.1's u(2m+1) rotation formula - not on (Y2,Y1) directly. A
 * corrected Python Viterbi built around that constraint (searching top-K
 * hypotheses per symbol so a noisy first-symbol decode doesn't get locked
 * in before the second symbol's parity constraint can help disambiguate
 * it) demonstrated real, substantial coding gain against additive noise
 * (e.g. ~95% vs ~83% symbol-pair accuracy at one tested noise level,
 * ~82% vs ~69% at another) - genuine confirmation the mechanism is
 * understood correctly. That full Viterbi search has NOT been ported to C
 * yet; only the encoder-side building blocks below have been.
 */

/* 3-bit subset label for one 2D channel-output point (odd-integer coords),
 * per Figure 9's 8-way partition. */
static __inline__ int nf_v34_subset_label(int re, int im)
{
    int xored = re ^ im;
    int x = xored & 2;
    return ((xored & 4) ^ (x << 1)) | (re & 2) | (x >> 1);
}

/* Table 13/V.34 - [Y4,Y3,Y2,Y1] (returned packed as Y4<<3|Y3<<2|Y2<<1|Y1)
 * as a function of s(2m), s(2m+1), each a subset label 0..7. */
extern const uint8_t nf_v34_table13[8][8];

/* 16-state convolutional encoder (Figure 10/V.34), cross-validated against
 * spandsp's make_v34_16_state_convolutional_encoder(). `state` packs
 * (t1,t2,t3,t4) as t1|t2<<1|t3<<2|t4<<3. Returns the new state; *y0 (if
 * non-NULL) receives Y0(m) - the state's t1 bit BEFORE this update (i.e.
 * the trellis encoder's output for the CURRENT interval, which is why it
 * doesn't depend on this interval's y2/y1 - see 9.6.3.2). */
int nf_v34_trellis_step(int state, int y2, int y1, int *y0);

/* ── Viterbi decoder over the trellis (9.6.3, corrected model) ─────────
 *
 * REAL and validated - ported from the Python prototype that demonstrated
 * genuine coding gain (see the file comment above). Still batch/offline
 * (not wired into a real-time streaming receiver). The quarter-
 * constellation table below covers the FULL superconstellation, labels
 * 0..415 (see nf_v34_superconstellation.h - mechanically verified against
 * the recommendation's own magnitude-ordering rule, zero violations), so
 * any negotiated M (shell-mapper ring count) shifted by any q (raw Q bits
 * per 9.6.1, i.e. any alphabet size up to M<<q <= 416) is representable.
 */
#define NF_V34_QUARTER_MAX 416
#define NF_V34_NSTATES     16

typedef struct {
    int re, im;
} nf_v34_ipoint_t;

/* Full ITU-T V.34 Figure 5 quarter-superconstellation, labels 0..415,
 * populated once (from nf_v34_superconstellation.h) on first use - see
 * nf_v34_constellation_init(). Not `const`: it's filled at runtime, but
 * effectively write-once and never mutated afterward. */
extern nf_v34_ipoint_t nf_v34_quarter_table[NF_V34_QUARTER_MAX];

typedef struct {
    int M;              /* number of quarter-table points included, NOT
                          * necessarily the shell mapper's own M - when the
                          * mapper's q > 0 (9.6.1's Q(n) formula), this is
                          * shell_M << q (all ring x raw-Q-bit combinations);
                          * see nf_v34_rx_frame's q parameter. */
    int nalpha;                                  /* = M*4 */
    nf_v34_ipoint_t alphabet[NF_V34_QUARTER_MAX * 4];
    int alpha_rot[NF_V34_QUARTER_MAX * 4];        /* rotation (0..3) each alphabet point came from */
} nf_v34_constellation_t;

/* quarter must have at least M entries; M <= NF_V34_QUARTER_MAX. Pass
 * shell_M << q here (not the bare shell-mapper M) whenever q > 0 - see
 * nf_v34_rx_frame's doc comment. */
void nf_v34_constellation_init(nf_v34_constellation_t *c, const nf_v34_ipoint_t *quarter, int M);

/* Batch Viterbi decode of n 4D-interval symbol pairs. noisy0[i]/noisy1[i]
 * are the two received (real-valued, already carrier/timing-recovered but
 * not yet sliced) 2D points for interval i. decoded0[i]/decoded1[i] receive
 * the recovered constellation points. `keep` = branch alternatives per
 * rotation-parity class considered for the second symbol; `keep0` = how
 * many first-symbol hypotheses are carried forward jointly (must be > 1 -
 * see the file comment: locking in a greedy first-symbol decode before the
 * second symbol's parity constraint can help disambiguate it was the
 * mistake that produced worse-than-naive results in an intermediate
 * version of this decoder). Returns 0 on success, -1 if n exceeds internal
 * limits or allocation fails. */
int nf_v34_viterbi_decode(const nf_v34_constellation_t *c,
                           const nf_cpx_t *noisy0, const nf_cpx_t *noisy1, int n,
                           nf_cpx_t *decoded0, nf_cpx_t *decoded1,
                           int keep, int keep0);

/* ── scrambler / descrambler (clause 7) ─────────────────────────────────
 *
 * Self-synchronizing scrambler/descrambler for the primary channel data.
 * Call modem: GPC = 1 + x^-18 + x^-23. Answer modem: GPA = 1 + x^-5 + x^-23.
 * Validated via the same round-trip test as the rest of this module (see
 * `check-v34`'s `fullchain` mode): scramble(bits) then descramble() must
 * recover the original bits exactly.
 */
typedef struct {
    uint8_t hist[32];   /* circular buffer of past (de)scrambled bits */
    int pos;
    int tap_a, tap_b;   /* 18/23 for GPC (call), 5/23 for GPA (answer) */
} nf_v34_scrambler_t;

void nf_v34_scrambler_init(nf_v34_scrambler_t *s, int is_call_modem);
int nf_v34_scramble_bit(nf_v34_scrambler_t *s, int bit);
int nf_v34_descramble_bit(nf_v34_scrambler_t *s, int scrambled_bit);

/* ── mapping-frame receiver (9.5 differential decode + 9.4 shell-unmap) ─
 *
 * Ties the Viterbi decoder's output back to real bits for one mapping
 * frame (4 4D-symbol intervals). Validated end-to-end (shell mapper +
 * differential encoder + MAP + trellis + descrambler, all together) via a
 * full synthetic round trip - see `check-v34`'s `fullchain` mode: 100%
 * bit-exact with no noise, and a clear, dramatic coding-gain benefit over
 * naive slicing once noise is added (errors that survive per-symbol
 * slicing cascade badly through shell-unmapping/descrambling, so the
 * trellis's benefit is even more pronounced at the bit level than at the
 * symbol level - e.g. 100% vs 91.9% correct bits at one tested noise
 * level, 94.9% vs 61.6% at another, in the Python prototype this was
 * ported from).
 *
 * `q` (0 when b <= 12, otherwise (b-K)/4 - 3 per 9.3.1) is the number of
 * raw "Q" bits carried alongside each 2D symbol's shell-mapper ring index,
 * per the mapper's Q(n) = Q_1 + 2*Q_2 + ... + 2^(q-1)*Q_q + 2^q*m formula
 * (9-26): the constellation label recovered per 2D point packs BOTH the
 * ring index (label >> q) and these q raw bits (label & ((1<<q)-1), Q_1
 * first/LSB) together. For this to work, `c` must have been built with
 * nf_v34_constellation_init(c, quarter, M << q) - i.e. covering all
 * M * 2^q quarter-table points, not just the M ring labels - so the
 * alphabet lookup's label already IS the packed Q(n), split here.
 *
 * NOT yet applied anywhere in this module: the non-linear encoder (9.7,
 * x'(n) = Phi(n)*x(n), a per-point radius-dependent scaling selected by the
 * MP-negotiated Theta bit). When Theta != 0, real transmitted points are
 * shifted slightly off their nominal alphabet coordinates, and this decoder
 * currently matches against un-shifted coordinates - a real gap for a
 * negotiation with non-linear encoding enabled (which is exactly what was
 * decoded from the real capture's MP frame - see nf_v34_mp_rx_t, nonlinear=1).
 * Deferred because 9.7's "average energy" term needs the shell mapper's
 * per-ring selection probability (not just a uniform average over alphabet
 * points) to compute correctly, and because front-end lock on the real
 * primary channel hasn't been achieved yet to validate against.
 */
typedef struct {
    int state;    /* trellis encoder state, carried across mapping frames */
    int zprev;    /* differential encoder Z(m-1), carried across frames  */
} nf_v34_rx_frame_state_t;

static __inline__ void nf_v34_rx_frame_state_init(nf_v34_rx_frame_state_t *s)
{
    s->state = 0;
    s->zprev = 0;
}

/* point0[j]/point1[j] (j=0..3) are the Viterbi-decoded 4D-interval symbol
 * pairs for this mapping frame (exact alphabet members - not noisy). M, K
 * are the negotiated shell-mapper parameters, q the raw-Q-bits-per-2D-symbol
 * count (see above; 0 for the simple/synthetic b<=12 case). *R0_out receives
 * the K-bit shell-mapper integer; aux receives, per group j=0..3, (i1,i2,i3)
 * followed by point0's q raw Q bits then point1's q raw Q bits (Q_1 first),
 * i.e. aux[j*(3+2*q) + ...] - caller must size aux to 4*(3+2*q) ints.
 * Returns 0 on success, -1 if a decoded point isn't an exact member of `c`'s
 * alphabet (shouldn't happen with Viterbi output, but real streaming input
 * should check). */
int nf_v34_rx_frame(nf_v34_rx_frame_state_t *s, const nf_v34_constellation_t *c,
                     int M, int K, int q,
                     const nf_cpx_t point0[4], const nf_cpx_t point1[4],
                     uint32_t *R0_out, int *aux);

/* ── control-channel batch demodulator (600 baud, static AGC + 4th-power
 * Costas) ───────────────────────────────────────────────────────────────
 *
 * A SEPARATE demodulator from nf_v34_ctrl_rx_t above, purpose-built for
 * bit-exact MP frame recovery. nf_v34_ctrl_rx_t's adaptive per-sample AGC
 * plus decision-directed Costas loop locks well enough for the axis_mse
 * lock-quality regression check, but was empirically found NOT tight
 * enough for a CRC-16 match (which needs zero bit errors across the whole
 * frame) - swapping its Costas error formula for a 4th-power one in place
 * made the existing regression *worse*, not better, most likely because
 * the adaptive AGC's continuous per-sample rescaling introduces exactly
 * the kind of scale jitter a decision-directed (scale-invariant) loop
 * shrugs off but a 4th-power loop is sensitive to. This batch variant
 * mirrors the Python prototype that DID achieve a bit-exact real decode:
 * a static (whole-buffer) RMS normalization computed once up front, then
 * Gardner timing recovery and a 4th-power Costas loop, both matching
 * nf_v34_ctrl_rx_t's algorithms otherwise. See check-v34's mphunt mode.
 */
/* cfo_max_sym: when > 0, restrict the 4th-power carrier-frequency-offset
 * estimate to the first cfo_max_sym symbols of the burst's active region.
 * This matters for the 2400 bit/s (16-point) control channel: the 16-point
 * user-data constellation is NOT a clean QPSK, so its 4th power does not
 * collapse to a single spectral line and biases (rails) the CFO estimate
 * when it dominates the burst energy. The PPh/ALT/MPh/E training ahead of
 * the user data is always 1200 bit/s 4-point QPSK, so estimating the CFO on
 * that prefix alone is clean. Pass 0 for a pure-1200 burst (whole active
 * region, bit-identical to the original) - the real-capture regressions do. */
void nf_v34_cc_rx_batch(const int16_t *amp, int n, double carrier_hz,
                         void (*on_symbol)(void *user, const nf_cpx_t *z),
                         void *user, int cfo_max_sym);

/* ── control-channel MP frame decoder (Annex A / 10.2.4) ────────────────
 *
 * REAL and validated against the actual recording, not synthetic-only:
 * feeding nf_v34_cc_rx_batch()'s demodulated symbols (600 baud, 1200 Hz
 * call-modem carrier) from references/v.34_modem_test.wav through this
 * decoder recovers a real, CRC-16-valid MP Type-0 frame at a mid-call
 * rate-renegotiation event (repeated 4 times identically, immediately
 * followed by the 20-one E sequence marking the end of the exchange -
 * exactly the
 * MP/MP'/E structure the recommendation describes). Decoded fields: max
 * rate 33600 bit/s, 16-state trellis, non-linear encoding enabled,
 * expanded constellation shaping, all rates 2400-33600 supported - every
 * reserved bit, fill bit and the CRC itself check out, which is why this
 * is trusted as a real decode and not a coincidence.
 *
 * The exact framing/CRC conventions (bit-serial CRC-16, no complement;
 * frame sync = 17 ones + a 0 start bit; further start bits skipped from
 * the CRC every 17 bits) were cross-checked against spandsp's actual
 * working control-channel receiver (process_cc_half_baud/cc_rx in
 * v34rx.c) before trusting them - note spandsp's PRIMARY-channel J/TRN/MP
 * receive path is unfinished/dead code (`#if 0`), only the control-
 * channel path is real, working reference material.
 *
 * Type-1 frames (Table 21 duplex / Table 24 half-duplex: 170 buf bits
 * instead of Type 0's 70, adding the 9.6.2 precoder coefficients h(1..3)
 * as Q14 two's-complement re/im pairs, each 16-bit group bracketed by
 * start bits, CRC over all data groups with the same every-17-bits
 * start-bit skip) are decoded too - validated against the real capture's
 * answer-modem MPh Type 1 (bit layout cross-checked against Table 24 of
 * the spec PDF in references/, CRC matches with no complement just like
 * Type 0). See `check-v34`'s `mphunt` and `mph1` modes for the regression
 * checks against the real capture.
 *
 * Field-view caveat: bits 20:33 are parsed BOTH per the duplex Table 20/21
 * view (max_rate_c2a/max_rate_a2c/aux_channel/ack) and per the half-duplex
 * Table 23/24 view, where bits 24:26 are reserved and bit 27 is the
 * control-channel data rate selected for the remote transmitter (cc_rate:
 * 0 = 1200 bit/s, 1 = 2400 bit/s). The caller picks the view matching the
 * negotiated mode - the fax capture is half-duplex, so cc_rate is the
 * meaningful one there (and max_rate_a2c/aux_channel/ack are not).
 */
typedef struct {
    nf_v34_scrambler_t descr;
    int have_prev_quad;
    int prev_quad;
    int ones_run;
    int pos;              /* -1 = searching for sync; else 0..169, index into buf */
    uint8_t buf[170];      /* buf[i] = spec bit i+18; Type 0 uses buf[0..69],
                            * Type 1 (buf[0] = 1) the full buf[0..169] */

    /* fields of the most recently completed CRC-valid frame */
    int have_frame;
    int type;
    int max_rate_c2a;      /* x2400 bit/s */
    int max_rate_a2c;       /* x2400 bit/s (duplex Table 20/21 view of bits 24:27) */
    int cc_rate;           /* half-duplex Table 23/24 view of bit 27: remote
                            * transmitter's control-channel rate, 0=1200, 1=2400 */
    int aux_channel;
    int trellis_size;      /* 0=16-state, 1=32-state, 2=64-state */
    int nonlinear;
    int shaping;
    int ack;
    int rate_mask;         /* 15 bits, bit0 = 2400 bit/s support */
    int asym_enable;

    /* Type 1 only: precoder coefficients h(1..3) (9.6.2), Q14 two's
     * complement as transmitted - divide by 16384.0 for the real value */
    int16_t h_re[3];
    int16_t h_im[3];
} nf_v34_mp_rx_t;

void nf_v34_mp_rx_init(nf_v34_mp_rx_t *s, int is_call_modem);

/* Feed one carrier/timing-recovered control-channel symbol (from
 * nf_v34_ctrl_rx_t's on_symbol callback). Returns 1 exactly once per newly
 * completed, CRC-valid frame (fields available in s-> until the next
 * frame completes); 0 otherwise. */
int nf_v34_mp_feed_symbol(nf_v34_mp_rx_t *s, const nf_cpx_t *z);

/* ── Phase-2 INFO-sequence decoder (INFO0/INFOh, 11.2.2/Tables 14 and 22) ─
 *
 * REAL and validated against the actual recording: V.34 Phase 2 INFO
 * sequences are 600 bit/s binary differential PSK (bit = 1 <=> the carrier
 * phase flips 180 degrees between consecutive 600-baud samples, i.e.
 * Re[z(n)*conj(z(n-1))] < 0), call modem on a 1200 Hz carrier, answer
 * modem on 2400 Hz (plus an 1800 Hz guard tone, suppressed here by a
 * ~250 Hz lowpass after downconversion). Frame = 1111 fill + 01110010
 * sync + information bits + CRC-16 + 1111 fill; the CRC is the same
 * engine as the MP decoder above (poly 0x8408 LSB-first, init 0xFFFF, no
 * complement) over ONLY the information bits (INFO0: 17 bits, INFOh: 19),
 * transmitted LSB first. Acceptance = fill+sync 111101110010 AND CRC
 * valid; the TRAILING fill is deliberately not required (the transmitter
 * may drop carrier into it - seen in the reference capture). Multi-bit
 * fields are LSB-first in time throughout.
 *
 * Batch-style like nf_v34_cc_rx_batch: no timing/carrier recovery loop,
 * just a fixed 600-baud sampling grid scanned across one symbol period of
 * timing offsets (and both bit polarities), any frame passing fill+sync+
 * CRC is accepted - ported from the Python prototype that found all three
 * INFO frames (INFO0c, INFO0a, INFOh) in the reference capture this way.
 * See check-v34's `infodec` mode.
 */
typedef struct {
    int is_infoh;          /* 0 = INFO0, 1 = INFOh */
    double t;              /* frame start (first fill bit), seconds from buffer start */

    /* INFO0 fields (Table 14), valid when is_infoh == 0 */
    int sr2743, sr2800, sr3429;   /* symbol-rate support (bits 12:14) */
    int low3000, high3000;        /* carrier capability (bits 15:18) */
    int low3200, high3200;
    int allow_3429;        /* bit 19: 0 = 3429 disallowed on this connection */
    int can_reduce_power;  /* bit 20 */
    int max_sr_diff;       /* bits 21:23: max allowed asym. symbol-rate difference */
    int from_cme;          /* bit 24: sent from a CME modem */
    int support_1664pt;    /* bit 25 */
    int clock_source;      /* bits 26:27: 0=internal, 1=synchronized */
    int info0_ack;         /* bit 28: acknowledges receiving an INFO0 */

    /* INFOh fields (Table 22), valid when is_infoh == 1 */
    int power_reduction;   /* bits 12:14, dB */
    int trn_len;           /* bits 15:21, TRN length in 35 ms units */
    int high_carrier;      /* bit 22 */
    int preemph_idx;       /* bits 23:26, pre-emphasis filter index */
    int symrate_idx;       /* bits 27:29, index into nf_v34_rates[] */
    int trn_16pt;          /* bit 30: TRN uses the 16-point constellation */
} nf_v34_info_frame_t;

/* Feed `n` int16 8kHz samples of one channel; decoded frames (deduped
 * across the timing-offset scan) are written to out[0..]. Returns the
 * number of frames found (<= max_frames). */
int nf_v34_info_rx_batch(const int16_t *amp, int n, double carrier_hz,
                          nf_v34_info_frame_t *out, int max_frames);

/* ── Phase-2 line-probing analyzer (11.2.2 / Table 17) ──────────────────
 *
 * The receive-side counterpart of nf_v34_probe_tx: measures the channel from
 * a received L1/L2 probe segment (int16 8 kHz, the energy-gated probe burst)
 * and picks the symbol rate / carrier / projected data rate the recipient
 * should signal back in INFOh (Table 22).
 *
 * Method (all thresholds are tuned heuristics - the recommendation mandates
 * the measurements and the INFOh signaling, not the selection algorithm):
 *   - Per-tone level: single-bin Goertzel at each of the 21 Table-17 probe
 *     frequencies (150-3750 Hz, 150 Hz spacing, 900/1200/1800/2400 omitted).
 *     The analysis window is a multiple of 1600 samples so every 150 Hz tone
 *     lands exactly on a DFT bin (8000/1600 = 5 Hz, bin 30 per 150 Hz) and
 *     the omitted-frequency bins see negligible leakage from the tones.
 *   - Noise/echo floor: the 4 OMITTED frequencies are silent in the probe,
 *     so a Goertzel there measures the line's noise+echo floor; the per-tone
 *     SNR is the tone level over the floor interpolated to that frequency.
 *   - Usable band: the contiguous frequency span whose per-tone SNR clears a
 *     threshold; spectral tilt = high-band minus low-band average SNR.
 *   - Frequency offset: fine peak-search around the strong mid-band tones,
 *     reported in Hz (line carrier offset).
 */
#define NF_V34_PROBE_NTONES 21

typedef struct {
    int    n;                                /* NF_V34_PROBE_NTONES */
    double freq_hz[NF_V34_PROBE_NTONES];     /* nominal tone frequencies */
    double tone_amp[NF_V34_PROBE_NTONES];    /* measured amplitude (linear) */
    double snr_db[NF_V34_PROBE_NTONES];      /* per-tone SNR vs the floor */
    double noise_floor;                      /* median omitted-freq amplitude */
    double band_lo_hz, band_hi_hz;           /* usable band edges (SNR>thr)   */
    double tilt_db;                          /* high-band minus low-band SNR  */
    double freq_offset_hz;                   /* estimated line carrier offset */
    double band_snr_db;                      /* broadband SNR estimate (dB),
                                              * calibrated to the Es/N0 scale
                                              * v34_rate_req_snr_db() uses      */
} nf_v34_probe_t;

/* Analyze one L1/L2 probe segment (int16 8 kHz; the caller extracts a window
 * that sits inside L1 or L2, clear of the preceding tone). Returns 0 on
 * success, -1 if n is too short. */
int nf_v34_probe_analyze(const int16_t *amp, long n, nf_v34_probe_t *pr);

typedef struct {
    int    srate_idx;                        /* chosen NF_V34_RATE_* */
    int    high_carrier;                     /* chosen Table 2 carrier option */
    int    projected_max_rate;               /* bit/s, capped by local_max_cap */
    double freq_offset_hz;
    int    per_srate_rate[NF_V34_NUM_RATES]; /* projected rate per S (0=unfit) */
} nf_v34_probe_sel_t;

/* Select symbol rate / carrier / projected data rate from an analyzed probe:
 * the highest S whose occupied band (carrier +- (1+beta)/2 * S, beta = 0.12)
 * fits the usable band, the carrier option that centres that band best, and
 * the highest data rate whose slicer SNR requirement is met by the measured
 * band SNR. local_max_cap (bit/s; <= 0 = none) caps the projected rate.
 * Returns 0 on success, -1 if NO symbol rate fits (sel gets the lowest S so
 * the caller can still proceed). */
int nf_v34_probe_select(const nf_v34_probe_t *pr, int local_max_cap,
                        nf_v34_probe_sel_t *sel);

/* ── primary-channel page decoder (batch, capture-anchored) ─────────────
 *
 * REAL and validated against the actual recording - the strongest
 * correctness signal in this module: the full primary-channel receive
 * pipeline (front end, least-squares-trained T/2 fractionally-spaced
 * equalizer, per-burst timing/gain transfer, decision-directed tracking,
 * symbol-to-bit inverse mapping (9.3.1), descrambler, HDLC deframing)
 * decodes the two ECM image blocks of references/v.34_modem_test.wav into
 * FCS-valid 256-octet FCD frames COMPLETELY: 256/256 in block 0, 149/149
 * in block 1 - the same result the real receiving fax achieved (it
 * answered MCF, not PPR). Ported from the validated Python pipeline
 * (diag51..diag74 in the session scratchpad); see check-v34's `page` mode.
 *
 * Deliberately batch/offline and capture-anchored: the modulation
 * parameters are the ones this capture negotiated (R=24000 bit/s at
 * S=3429 baud: K=28, M=14, q=2, b=56 all-high mapping frames, P=15, J=8,
 * 16-state trellis, non-linear encoding Theta=0.3125) and the timing
 * anchors (TRN start, S start per burst) are measured properties of this
 * capture, passed in by the caller rather than acquired by a real-time
 * training FSM. What this is NOT: a streaming receiver with automatic
 * acquisition. What it IS: empirical proof that every mathematical piece
 * of the V.34 receive chain is correct against real bits on a real line.
 *
 * Front end: downconvert 1959 Hz -> RRC-as-lowpass (beta 0.3 at the
 * measured 3428.6385 baud = nominal 24000/7 +19.6 ppm) -> windowed-sinc
 * fractional interpolation (equivalent quality to the Python prototype's
 * FFT 8x oversampling + linear interpolation, judged by the achieved
 * residuals) -> T/2-spaced sampling.
 *
 * Equalizer: 63 complex T/2-spaced taps, cursor 16 T/2 samples from the
 * window head (8 symbols of future span), trained by direct least squares
 * (normal equations + Cholesky) on the full ~10050-symbol known TRN with
 * an iterated residual-carrier (slope+intercept) refit; the TRN tail is
 * held out to prove generalization (res_holdout ~ 0.0004 achieved, i.e.
 * ~34 dB). These trained taps seed every burst.
 *
 * Per burst: the S/Sbar/PP resync preamble is located by normalized
 * cross-correlation, then a timing-offset (tau) scan plus complex-gain fit
 * on the 288 known PP symbols ONLY (the real S/Sbar is transmitted 1.5x
 * hotter than the 10.1.3.7 template convention, so it is excluded), then
 * chunked decision-directed 2nd-order phase/gain tracking with slow
 * "tri-tau" timing updates across the whole multi-second burst, slicing
 * against the 224-point nonlinear-scaled (9.7 Phi(p)) alphabet.
 *
 * Tap adaptation (the fix that took block 0 from 249..250/256 to 256/256):
 * the channel drifts measurably over the 23 s burst - with taps frozen
 * from the 39.3 s TRN, the median slice distance degrades from ~0.23 to
 * ~0.33 (about 3 dB), and each lost frame traced to 1-2 near-boundary
 * symbols riding that raised noise floor (diag72: no audio clicks or
 * dropouts, no tracker glitches - pure equalizer staleness). So after each
 * 512-symbol chunk is sliced, the taps are refit by regularized LS toward
 * the sliced decisions (block RLS with an exponential prior pulling toward
 * the current taps; only decisions whose margin to the runner-up point
 * exceeds 0.25 train the taps, so rare wrong decisions cannot steer them).
 * Result: median slice distance ~0.16 held flat across the whole burst and
 * NO symbol anywhere in either burst with margin < 0.3 - every frame
 * decodes, no trellis/Viterbi or FCS-guided repair needed.
 *
 * The 9.6.2 precoder is deliberately IGNORED throughout (c(n) treated as
 * 0): this line's negotiated taps are tiny (|h| ~ 0.016 - see the MPh
 * Type-1 decode above) and the validated Python decode confirmed the
 * slicer never needs the correction on this capture. A general V.34
 * receiver would have to reconstruct and remove p(n)/c(n) - see the
 * shell-mapper section's precoder note.
 *
 * B1 quirks measured on this TX: the differential encoder's Z(-1)
 * entering B1 is 3 in burst 1 and 1 in burst 2 - brute-forced per burst
 * against B1's known scrambled-ones content (an 840/840 bit match is
 * unmistakable); U0/the trellis state is never needed for data recovery
 * (only Z rotation DIFFERENCES carry data bits), so the decoder skips the
 * trellis entirely and lets an occasional U0-driven odd rotation pass
 * straight through the delta - 2*I1 split.
 */

#define NF_V34_PAGE_TAPS 63          /* T/2 FSE taps (validated: L=63, fut=16) */

/* The reference capture's measured symbol clock (nominal 24000/7 baud,
 * +19.6 ppm) - pass this as the `baud` parameter of the page-decoder
 * functions below to reproduce the capture-validated behaviour ("page"
 * regression). A clean local transmitter (nf_v34_pc_modulate below) runs at
 * exactly 24000/7 = NF_V34_PC_BAUD_NOMINAL. */
#define NF_V34_PAGE_BAUD_CAPTURE 3428.6385
#define NF_V34_PC_BAUD_NOMINAL   (24000.0 / 7.0)

typedef struct {
    double w_re[NF_V34_PAGE_TAPS];   /* frozen T/2 FSE taps, w[0] = window head */
    double w_im[NF_V34_PAGE_TAPS];
    double sl;                       /* residual carrier, rad/symbol (~ -0.219 Hz) */
    double res_train;                /* normalized MSE on the TRN training region */
    double res_holdout;              /* ... on the held-out TRN tail (the honest one) */

    /* ── frequency-shifter image branch ──────────────────────────────────
     * A real (or line_sim's) SSB carrier-frequency shifter has finite image
     * rejection: its Hilbert splitter degrades toward low frequencies, so a
     * shifted line carries, besides the wanted signal, a LINEAR image
     * component of the same signal rotating at exactly 2x the line shift
     * (measured on the sweep's foff cells: coherence down to ~0.7 near
     * 300 Hz, an irreducible ~2.3% training residual that no static linear
     * equalizer and no phase tracking can express). When training detects a
     * genuine line shift (see nf_v34.c), it solves a joint LS over BOTH the
     * main T/2 stream and copies counter-rotated at n*img_hz,
     * n = +-1,+-2,+-3 (both error sidebands of u*cos + Hilbert(u)*sin
     * exist, and INVERTING the forward mixture needs the higher-order
     * intermod terms - each order adds ~8 dB of cancellation; +-3
     * reaches the A-law noise floor on the sweep's foff cells); the wimg
     * tap sets cancel them. img_hz = 2 x (measured carrier removal +
     * nominal excess) = +-2x the line's own shift - see nf_v34.c for the
     * calibration. wimg[k] applies to the stream counter-rotated by
     * rotation multiple {+1,-1,+2,-2,+3,-3}[k] * img_hz. */
    int    img_active;
    double img_hz;
    double wimg_re[6][NF_V34_PAGE_TAPS];
    double wimg_im[6][NF_V34_PAGE_TAPS];

    /* ── cubic (harmonic-distortion) branch ──────────────────────────────
     * A line with harmonic distortion adds a deterministic odd-order term
     * whose in-band part is ~ |s|^2 * s (third harmonic / cubic intermod).
     * Training offers an extra branch on st*|st|^2 (Hammerstein model) and
     * keeps it when the holdout residual genuinely improves; the wnl taps
     * then cancel the bulk of the distortion, which hits the high-PAR
     * 224-point data signal's peak symbols far harder than its RMS
     * suggests. */
    int    nl_active;
    double wnl_re[NF_V34_PAGE_TAPS];
    double wnl_im[NF_V34_PAGE_TAPS];
} nf_v34_page_eq_t;

/* Train the FSE on the known TRN. amp = one channel of 8 kHz samples whose
 * first sample is at absolute capture time buf_t0; t_trn = absolute time of
 * TRN symbol 0 (this capture: 39.33616s with a -0.160-sample fine
 * intercept); n_trn = TRN symbols to use (~10050 of the 10080 sent, leaving
 * headroom before J); baud = the received symbol clock
 * (NF_V34_PAGE_BAUD_CAPTURE for the capture regression, NF_V34_PC_BAUD_NOMINAL
 * for a local clean loopback - a real receiver estimates this, e.g. by
 * scanning a few ppm hypotheses for the best res_holdout, exactly how the
 * capture's value was found). `pp` selects the negotiated operating point
 * (carrier, constellation, TRN reference...); NULL = the capture's
 * S=3429/R=24000 set with the 1959.0 Hz downconvert the capture regression
 * was validated with. Returns 0 on success. */
int nf_v34_page_train(const int16_t *amp, long n, double buf_t0,
                      double t_trn, int n_trn, double baud,
                      const nf_v34_pcparams_t *pp,
                      nf_v34_page_eq_t *eq);

/* Locate an S/Sbar/PP resync preamble: scans S-start hypotheses in
 * [t_lo, t_hi] (0.1-sample resolution, small residual-frequency scan) by
 * normalized cross-correlation against the known 432-symbol template.
 * Returns the S start time (absolute); *corr_out (may be NULL) receives
 * the peak normalized correlation (~0.92 on this capture's two bursts). */
double nf_v34_page_locate_s(const int16_t *amp, long n, double buf_t0,
                            double t_lo, double t_hi, double baud,
                            const nf_v34_pcparams_t *pp,
                            double *corr_out);

typedef struct {
    double pp_resid;      /* normalized MSE on the 288 known PP symbols */
    double tau_init;      /* timing offset from the PP scan, 8 kHz samples */
    double tau_final;     /* ... after DD tracking across the burst */
    long   nsym;          /* sliced symbols (B1 + data) */
    double t_end;         /* detected burst end, absolute seconds */
    double med_dist;      /* median slice distance across the burst (median
                           * of the per-chunk medians) - the per-burst
                           * decode-quality metric the session's automatic
                           * rate fallback consumes */
    int    zm1;           /* brute-forced differential-encoder Z(-1) at B1 */
    int    b1_match;      /* B1 bits matching known scrambled ones, of
                           * b1_bits (840 for the capture's mode) */
    int    b1_bits;       /* how many known B1 bits were compared */
    long   bad_r0;        /* mapping frames whose ring 8-tuple was invalid */
    int    hdlc_ok, hdlc_bad;        /* FCS-valid / FCS-bad HDLC frames */
    int    fcd_ok, rcp_ok;           /* FCS-valid FCD (FF 03 06) / RCP frames */
    int    fcd_first, fcd_last;      /* first/last FCD frame numbers seen */
    uint8_t first_hdr[2][8];         /* first bytes of the first two ok frames */
    int    first_hdr_len[2];
    int    n_first_hdr;
} nf_v34_page_burst_t;

/* Decode one primary-channel burst: t_s from nf_v34_page_locate_s, eq from
 * nf_v34_page_train, t_end_max a generous upper bound (the real end is
 * detected by the slicer's median distance blowing up at carrier drop).
 * FCS-checked HDLC frames are counted into *res and also forwarded to
 * on_frame (may be NULL) with the same semantics as nf_hdlc. Returns 0 on
 * success (even if the burst decoded badly - judge via *res), -1 on
 * allocation/parameter failure. */
int nf_v34_page_decode_burst(const int16_t *amp, long n, double buf_t0,
                             const nf_v34_page_eq_t *eq,
                             double t_s, double t_end_max, double baud,
                             const nf_v34_pcparams_t *pp,
                             nf_hdlc_frame_fn on_frame, void *user,
                             nf_v34_page_burst_t *res);

/* ── control-channel HDLC user-data decoder ─────────────────────────────
 *
 * REAL and validated against the actual recording: after the MPh/E
 * exchange, the half-duplex control channel carries the T.30 signalling
 * (DIS/DCS/CFR/PPS/MCF/DCN...) as ordinary HDLC frames at 1200 bit/s
 * (2 bits per 600-baud symbol; MPh bit 27 = 0 in the capture, so the
 * 2400 bit/s 16-point mode is not needed). This is the same differential
 * demap and descrambler as the MP decoder above, feeding nf_hdlc's
 * validated deframer (0x7E flags, zero-bit destuffing, LSB-first bytes,
 * CRC-16/X.25 residue 0xF0B8) instead of the MP frame hunter. Frames up
 * to NF_HDLC_MAXFRAME (400) bytes are accepted - do NOT be tempted to cap
 * this near the 137 bytes control frames need, ECM image frames on this
 * channel are far larger (a short cap was found to silently discard them
 * in the Python validation). Drive it from nf_v34_cc_rx_batch's on_symbol
 * callback; FCS-checked frames arrive on the nf_hdlc handler. See
 * check-v34's `ccdata` mode for the 10-frame real-capture regression.
 */
typedef struct {
    nf_v34_scrambler_t descr;
    int have_prev_quad;
    int prev_quad;        /* 1200: previous quadrant; 2400: previous rotation  */
    int cc_rate;          /* 0 = 1200 bit/s (2 bits/sym), 1 = 2400 (4 bits/sym)*/
    /* 2400 only: the stream opens with 1200-mode training (PPh/ALT/MPh/E,
     * arbitrarily long since MPh loops until the peer's MPh arrives); it is
     * demapped at 1200 until the E sequence (>= 20 consecutive descrambled
     * ones) is seen, THEN the demap switches to 16-point with the gain
     * seeded from the median magnitude of the just-received inner-ring
     * training symbols. */
    int mode16;           /* 2400 only: E seen, 16-point demap active          */
    int ones_run;         /* 2400 only: consecutive descrambled 1 bits         */
    /* ALT-hold detector: a peer that wants a rate change answers an Sh
     * resync with [PPh +] ALT and HOLDS the ALT until it hears our PPh
     * (12.6.2.3). Legal preambles cap ALT at 120T (240 bits), so a much
     * longer descrambled 0/1 alternation is an unambiguous cc start-up
     * hold even when the PPh itself was missed. */
    int alt_last;         /* previous descrambled bit                          */
    int alt_run;          /* consecutive alternating descrambled bits          */
    int alt_hold;         /* run exceeded any legal preamble ALT               */
    double gain;          /* 2400 only: |rx|/|constellation| scale              */
    double ph;            /* 2400 only: DD residual-phase tracker (rad). The
                           * upstream batch demod freezes its carrier loop at
                           * the training prefix; over a held multi-second
                           * stream the leftover CFO error rotates the
                           * constellation - differential 1200 shrugs it off,
                           * absolute 16-point slicing does not. */
    int mag_n;            /* 2400 only: rolling recent-magnitude ring          */
    double mag_ring[32];
    nf_hdlc_rx_t hdlc;
} nf_v34_ccdata_rx_t;

void nf_v34_ccdata_rx_init(nf_v34_ccdata_rx_t *s, int is_call_modem,
                            nf_hdlc_frame_fn handler, void *user);

/* Select the user-data rate this decoder expects AFTER the PPh/ALT/MPh/E
 * training (which is always 1200 bit/s): 0 = 1200 (4-point), 1 = 2400
 * (16-point, 10.2.4). Default (from _init) is 1200 - the capture's mode, so
 * the ccdata capture regression is untouched. The session sets this from the
 * negotiated MPh bit 27. */
void nf_v34_ccdata_rx_set_rate(nf_v34_ccdata_rx_t *s, int rate_2400);

/* Feed one carrier/timing-recovered control-channel symbol (from
 * nf_v34_cc_rx_batch's on_symbol callback). */
void nf_v34_ccdata_feed_symbol(nf_v34_ccdata_rx_t *s, const nf_cpx_t *z);

/* ═══ transmitter (phase A: batch TX signal primitives) ═════════════════
 *
 * The transmit-side mirrors of everything above, loopback-validated
 * against this module's own capture-validated receivers (see check-v34's
 * txinfo/txcc/txsig/txpage modes). All output is int16 PCM at 8 kHz with
 * EXACT rational timing - no cumulative drift anywhere:
 *   - primary channel: S = 3429 baud is exactly 24000/7 (symbol period
 *     7/3 samples - 3 symbols per 7 samples), carrier 1959 3/7 Hz nominal
 *     option realised as exactly 96000/49 Hz = 12/49 cycles/sample;
 *   - control channel / INFO: 600 baud = 40/3 samples/symbol, carriers
 *     1200 Hz (call) and 2400 Hz + 1800 Hz guard tone (answer), all exact
 *     rational fractions of the sample clock.
 * Pulse shaping is transmit-side RRC realised as an exact-rational
 * polyphase interpolator (primary: beta 0.12, keeping the band edges
 * 1959.18 +- 1714.29*(1+beta) inside 0..4000 Hz; control/INFO: beta 0.3,
 * matching the receivers' front ends). Modulators ADD into the caller's
 * buffer (with saturation), so a test lays out silence and bursts freely;
 * one modulate call per contiguous carrier burst (pulse tails overlap
 * correctly only within a call). */

/* ── Phase 2 tones and line probing ─────────────────────────────────────
 * Tone A (answer): 2400 Hz at `level` peak amplitude + 1800 Hz guard tone
 * 6 dB below it (spec: carrier -1 dB, guard -7 dB relative nominal, 10.1.2.1).
 * Tone B (call): 1200 Hz at `level`. A 180-degree phase reversal of the
 * main tone (not the guard) happens at absolute sample index reversal_at
 * (< 0: none). Writes (adds) samples at..at+dur-1. */
void nf_v34_tone_tx(int16_t *amp, long n, long at, long dur,
                    int is_answer, long reversal_at, double level);

/* L1/L2 line probing per 10.1.2.4/Table 17: 21 cosines 150..3750 Hz spaced
 * 150 Hz (900/1200/1800/2400 omitted) with the table's initial phases.
 * level_rms = target RMS of L2; L1 = L2 + 6 dB. */
void nf_v34_probe_tx(int16_t *amp, long n, long at, long dur,
                     int is_l1, double level_rms);

/* ── INFO sequence TX (mirror of nf_v34_info_rx_batch) ──────────────────
 * Encodes *f (is_infoh selects INFO0/INFOh field set) per Tables 14/22:
 * fill 1111 + sync 01110010 + info bits + CRC-16 (LSB first) + fill 1111,
 * 600 bit/s binary DPSK preceded by one point at arbitrary phase
 * (10.1.2.3.1), on the role's carrier (+ guard tone for the answer role).
 * Symbol 0 is centred at sample `at`; gain scales the unit DPSK points to
 * line level. Returns the number of 600-baud symbols emitted. */
long nf_v34_info_tx(const nf_v34_info_frame_t *f, int is_answer,
                    int16_t *amp, long n, long at, double gain);

/* ── control-channel TX (mirror of nf_v34_cc_rx_batch + nf_v34_mp_rx_t +
 * nf_v34_ccdata_rx_t) ───────────────────────────────────────────────────
 * A symbol accumulator: append PPh/ALT/MPh/E/HDLC-data segments in session
 * order (12.4: PPh ALT [MPh MPh E] data), then modulate once. Differential
 * encoder Zn and the clause-7 scrambler (GPC call / GPA answer, zero init)
 * run continuously across segments, exactly as the receivers expect. */

typedef struct {
    int type;              /* 0 or 1 (Table 23 / Table 24) */
    int max_rate;          /* code N, data rate = N x 2400 (bits 20:23) */
    int cc_rate;           /* bit 27: 0 = 1200, 1 = 2400 bit/s for remote tx */
    int trellis_size;      /* bits 29:30: 0=16-state 1=32 2=64 */
    int nonlinear;         /* bit 31: Theta = 0.3125 when 1 */
    int shaping;           /* bit 32: expanded shaping when 1 */
    int ack;               /* bit 33 (duplex-view ack; 0 for half-duplex) */
    int rate_mask;         /* bits 35:49, bit0 = 2400 bit/s */
    int asym_enable;       /* bit 50 */
    int16_t h_re[3], h_im[3];   /* Type 1 only: precoder taps, Q14 */
} nf_v34_mph_fields_t;

typedef struct {
    nf_v34_scrambler_t scr;
    int is_call;
    int zn;                /* differential encoder state */
    int cc_rate;           /* user-data rate: 0 = 1200 (2 b/sym), 1 = 2400 (4) */
    double *re, *im;       /* accumulated 600-baud symbols */
    long nsym, cap;
} nf_v34_cc_tx_t;

void nf_v34_cc_tx_init(nf_v34_cc_tx_t *s, int is_call_modem);

/* Select the rate the user-data segment (nf_v34_cc_tx_bits) is emitted at:
 * 0 = 1200 bit/s (4-point, 2 bits/symbol), 1 = 2400 bit/s (16-point,
 * 4 bits/symbol per 10.2.4). Training (PPh/ALT/MPh/E) is ALWAYS 1200 -
 * those primitives ignore this. Call before nf_v34_cc_tx_bits. */
void nf_v34_cc_tx_set_rate(nf_v34_cc_tx_t *s, int rate_2400);
void nf_v34_cc_tx_free(nf_v34_cc_tx_t *s);
int  nf_v34_cc_tx_pph(nf_v34_cc_tx_t *s);              /* 32 symbols, eq 10-2 */
int  nf_v34_cc_tx_alt(nf_v34_cc_tx_t *s, int nsym);    /* scrambled 0/1 alt  */
int  nf_v34_cc_tx_mph(nf_v34_cc_tx_t *s, const nf_v34_mph_fields_t *f);
int  nf_v34_cc_tx_e(nf_v34_cc_tx_t *s);                /* 20 scrambled ones  */
/* Sh(24T) + S̄h(8T): the control-channel analogue of the primary channel's
 * S/S̄ (10.2.4: Sh alternates point 0 and point 0 rotated CCW 90 deg; S̄h
 * alternates point 0 rotated 180 deg and point 0 rotated CCW 270 deg). Direct
 * points (no differential encode / scramble, like PPh). Used for the short
 * control-channel resynchronization of 12.6 when no modulation-parameter
 * change is desired (32 symbols total). */
int  nf_v34_cc_tx_sh(nf_v34_cc_tx_t *s);
/* Signal AC (10.2.4.1): alternating point 0 = (1,1) and point 0 rotated
 * 180 deg = (-1,-1), direct points. Used to trigger a control-channel
 * retrain (12.8). */
int  nf_v34_cc_tx_ac(nf_v34_cc_tx_t *s, int nsym);
/* raw user-data bits (feed nf_hdlc_tx output here) at the rate set by
 * nf_v34_cc_tx_set_rate: 1200 bit/s consumes 2 bits/symbol (I1,I2; nbits
 * must be even), 2400 bit/s consumes 4 bits/symbol (I1,I2,Q1,Q2 in time
 * order; nbits must be a multiple of 4). Misaligned nbits is rejected. */
int  nf_v34_cc_tx_bits(nf_v34_cc_tx_t *s, const uint8_t *bits, long nbits);
/* Modulate the accumulated symbols (600 baud RRC beta 0.3) on the role's
 * carrier (+ guard tone for answer). Symbol 0 centred at sample `at`;
 * `gain` scales symbol units (point 0 = (1,1)) to line units. Returns the
 * symbol count. */
long nf_v34_cc_tx_modulate(const nf_v34_cc_tx_t *s, int16_t *amp, long n,
                           long at, double gain);

/* ── primary-channel TX (mirror of the page decoder above) ──────────────
 * Drives from the negotiated-parameter struct: EVERY Table 8 operating
 * point (all six symbol rates, all data rates 2400..33600, both carrier
 * options, minimum or expanded shaping, the b <= 12 no-shell-mapper path,
 * J = 7 and J = 8 superframes) is loopback-validated against this module's
 * own capture-validated receiver - see check-v34's `txrates` matrix.
 * nf_v34_pc_tx_init (no-argument) still loads exactly the capture's set
 * (S = 3429, R = 24000: K=28 M=14 q=2 b=56 SWP=0x7FFF J=8 P=15, 16-state
 * trellis, non-linear encoder Theta = 0.3125, expanded shaping,
 * precoder h = 0). */

typedef struct {
    /* negotiated parameters (nf_v34_pc_tx_init loads the R=24000 set;
     * nf_v34_pc_tx_init_mode loads any Table 8 operating point) */
    int M, q, K, b, P, J;
    uint16_t swp;          /* P-bit switching pattern, bit (P-1-i) = frame i */
    double theta;          /* 9.7 non-linear parameter (0 disables) */
    double h_re[3], h_im[3];   /* 9.6.2 precoder taps (default 0) */
    /* encoder state (all zero-initialized per 10.1.3.1 before B1) */
    nf_v34_scrambler_t scr;    /* GPC (call modem transmits the pages) */
    int state;             /* 16-state trellis encoder */
    int zprev;             /* differential encoder Z(m-1) */
    long m4d;              /* 4D-interval counter since B1 symbol 0 */
    int sf_slot;           /* Table 12 half-data-frame slot; init 2J-2: B1 is
                            * sent "as if the last data frame of a superframe"
                            * (10.1.3.1), then data continues at slot 0 */
    int mf_idx;            /* mapping-frame counter (SWP position) */
    double xh_re[3], xh_im[3];  /* precoder tap delay line x(n-1..n-3) */
    double avg_energy;     /* 9.7 mean |p|^2 over the M<<q quarter labels */
} nf_v34_pc_tx_t;

void nf_v34_pc_tx_init(nf_v34_pc_tx_t *s);

/* Load any negotiated operating point (see nf_v34_pcparams_init). */
void nf_v34_pc_tx_init_mode(nf_v34_pc_tx_t *s, const nf_v34_pcparams_t *pp);

/* Encode nframes mapping frames (8 2D symbols each) from raw (pre-scrambler)
 * bits[] - b bits per high mapping frame, K of them the shell bits LSB-first
 * (9.3.1), then per group I1,I2,I3 and q+q Q bits; low frames carry b-1 bits
 * (S_i,K forced 0). The b <= 12 special case (9.3.2: K = 0, ring indices
 * always 0, I3 bits present per the 8/9/11/12-bit patterns) is handled too.
 * All through scrambler -> shell mapper -> differential encoder ->
 * trellis/U0 (with Table 12 V0 inversions ONLY at 4D intervals
 * m == 0 mod 2P, J = 7 or 8 pattern) -> precoder (h=0: no-op) -> non-linear
 * encoder. bits == NULL feeds continuous binary ones (B1 per 10.1.3.1, and
 * the 12.5.3 turn-off frame). Output in raw alphabet units (odd-integer
 * grid, Phi-scaled). Returns the number of raw data bits consumed. */
long nf_v34_pc_encode(nf_v34_pc_tx_t *s, const uint8_t *bits, int nframes,
                      double *re, double *im);

/* Raw data bits consumed by nframes mapping frames starting at the
 * encoder's CURRENT SWP position (b per high frame, b-1 per low). */
long nf_v34_pc_frames_bits(const nf_v34_pc_tx_t *s, int nframes);

/* S(128T) + Sbar(16T) + PP(288T) per 10.1.3.7 / eq 10-1 - 432 symbols in
 * template units (S at |.| = sqrt(2), PP unit RMS). */
void nf_v34_pc_sspp(double *re, double *im);

/* TRN per 10.1.3.8 (scrambled ones, GPC zero init; 16-point when
 * sixteen_point, else 4-point), raw constellation coordinates. */
void nf_v34_pc_trn(int nsym, int sixteen_point, double *re, double *im);

/* Build one complete primary-channel burst symbol stream per 12.5.1:
 * S/Sbar/PP + B1 (one data frame) + enough data frames for nbits (padded
 * with binary ones = HDLC idle) + one final frame of scrambled ones (the
 * 12.5.3 turn-off). B1/data are scaled by 1/RMS(B1) relative to the
 * unit-RMS PP - the 10.1.3 power-compensation convention the page
 * decoder's G = g_pp * b1_norm transfer was validated against on the real
 * capture. pp = NULL selects the capture's S=3429/R=24000 set. *re and
 * *im are malloc'd (caller frees); returns the symbol count, -1 on
 * allocation failure. */
long nf_v34_pc_burst_build(const nf_v34_pcparams_t *pp,
                           const uint8_t *data_bits, long nbits,
                           double **re, double **im);

/* Modulate primary-channel symbols at the mode's exact rational baud and
 * carrier (pp = NULL: exactly 24000/7 baud on 96000/49 Hz), RRC beta 0.12.
 * Symbol 0 centred at sample `at`; `gain` scales symbol units to line
 * units. */
void nf_v34_pc_modulate(const nf_v34_pcparams_t *pp,
                        int16_t *amp, long n, long at,
                        const double *re, const double *im, long nsym,
                        double gain);

/* ═══ half-duplex session driver (clause 12 + T.30 Annex F, phase B) ═════
 *
 * The state machine that turns the batch primitives above into a live fax
 * modem below nf_fax/nf_t30: V.34 Phase 2 (INFO0 exchange, tones with phase
 * reversals, L1/L2 line probing), Phase 3 (S/Sbar/PP + TRN training), the
 * control-channel startup handshake (PPh/ALT/MPh/MPh/E both directions),
 * and then the steady half-duplex alternation of 1200 bit/s control-channel
 * bursts (carrying the T.30 HDLC frames) and primary-channel bursts
 * (carrying the ECM FCD frames) at the NEGOTIATED data rate: honest MPh
 * rate masks/maxima both ways, selection per 12.4.1.3 (max rate enabled in
 * both masks <= both maxima), an initial recipient-side cap from the TRN
 * training residual's SNR estimate, and AUTOMATIC FALLBACK - a failed or
 * degraded burst lowers the recipient's advertised cap, the MPh riding the
 * next control-channel round (the same round T.30's PPR path uses for the
 * retransmission) renegotiates down, and the block is retransmitted at the
 * lower rate. Streaming facade, batch under the hood: receive audio is
 * energy-gated into per-burst buffers which are handed to the
 * capture-validated batch decoders above (see nf_v34.c's session section
 * for the documented simplifications vs the letter of clause 12 - fixed
 * choreography timers instead of measured round trips). Stage-3 recovery is
 * in place: stable turnarounds take the short Sh/S̄h resync (12.6), a rate
 * change forces the full PPh/ALT/MPh/MPh/E restart (12.4), and an
 * unrecoverable burst escalates to a full control-channel retrain (12.7/12.8,
 * re-running Phase 2 probing -> Phase 3 training - see nf_v34_sess_retrain
 * and the recovery-mechanism accessors below).
 *
 * Both roles are implemented (call = source of the pages, answer =
 * recipient); the loopback regression check-v34fax runs one against the
 * other end-to-end through nf_t30. Status callback delivers NF_SIG_*
 * events (TRAINING_SUCCEEDED/FAILED once the control channel is/isn't
 * established after startup; CARRIER_DOWN after a primary-channel burst
 * has been decoded and its frames delivered). Set NFV34DBG=1 for a session
 * transcript on stderr. */

enum {
    NF_V34_SESS_OFF = 0,
    NF_V34_SESS_STARTUP,    /* clause 12 Phases 2-3 + cc startup handshake */
    NF_V34_SESS_CC,         /* control channel (T.30 frames, 1200 bit/s)   */
    NF_V34_SESS_PRI         /* primary channel (ECM image, 24000 bit/s)    */
};

typedef struct nf_v34_sess nf_v34_sess_t;
typedef void (*nf_v34_sess_status_fn)(void *user, int status);   /* NF_SIG_* */

/* is_call = 1 for the calling station (the source - it probes and sends the
 * primary-channel pages), 0 for the answerer (the recipient - it selects the
 * training parameters via INFOh and trains its equalizer on the TRN).
 * frame_fn receives every FCS-checked HDLC frame from BOTH channels (control
 * frames and primary-channel FCD/RCP), nf_hdlc semantics. */
nf_v34_sess_t *nf_v34_sess_alloc(int is_call, nf_v34_sess_status_fn status_fn,
                                 nf_hdlc_frame_fn frame_fn, void *user);
void nf_v34_sess_free(nf_v34_sess_t *s);

/* Arm the transmit/receive side (NF_V34_SESS_*). Setting the tx side to
 * STARTUP (once, right after V.8 resolves to V.34) starts the half-duplex
 * startup choreography; the rx side must be set to STARTUP too. */
void nf_v34_sess_set_tx_mode(nf_v34_sess_t *s, int mode);
void nf_v34_sess_set_rx_mode(nf_v34_sess_t *s, int mode);

/* Feed rx audio captured BEFORE the session started (the V.8 tail) into the
 * just-started Phase-2 receiver, so a peer's INFO0/tone that overlaps the
 * V.8 -> V.34 hand-over is not lost. Call once, right after the session has
 * been started via set_tx_mode/set_rx_mode(NF_V34_SESS_STARTUP); ignored at
 * any other time. */
void nf_v34_sess_rx_prime(nf_v34_sess_t *s, const int16_t *amp, int len);

/* Cap the primary-channel data rate this session will advertise/select
 * (bit/s; rounded down to a supported rate). Also settable via the
 * NFV34MAXRATE environment variable at alloc time - both exist for tests
 * and for line-quality policy above the session. */
void nf_v34_sess_set_max_rate(nf_v34_sess_t *s, int rate);

/* Currently selected primary-channel data rate, bit/s (12.4.1.3: the
 * maximum rate enabled in both modems' MPh rate masks that is <= both
 * declared maxima). 0 before the first MPh exchange completes. */
int nf_v34_sess_data_rate(const nf_v34_sess_t *s);

/* Negotiated control-channel user-data rate, bit/s (1200 or 2400 - the
 * 10.2.4 16-point mode, MPh bit 27). Both sides must advertise 2400 for it
 * to be selected; the answerer gates its advertisement on the measured line
 * SNR. Before the first MPh exchange this reports the default 1200. */
int nf_v34_sess_cc_rate(const nf_v34_sess_t *s);

/* ── Stage 3 recovery-mechanism observability (for tests / policy) ────────
 * The half-duplex session uses three distinct clause-12 recovery forms at a
 * turnaround, chosen by whether the modulation parameters change:
 *   - Sh/S̄h short control-channel resync (12.6): no parameter change - the
 *     common stable between-block/between-page case (Sh 24T + S̄h 8T + ALT + E,
 *     NO PPh, NO MPh).
 *   - full PPh/ALT/MPh/MPh/E restart (12.4): carries new MPh caps - a
 *     mid-call rate renegotiation.
 *   - control-channel retrain (12.8, AC signal) / primary retrain (12.7):
 *     full fallback through Phase 2 probing -> Phase 3 training when a burst
 *     is unrecoverable even at the floor rate. */
enum { NF_V34_CC_UNKNOWN = 0, NF_V34_CC_SH_RESYNC, NF_V34_CC_PPH_RENEG };

/* Count of Sh short resyncs / PPh-MPh renegotiations this session has
 * TRANSMITTED, and full retrains initiated/performed. */
int nf_v34_sess_sh_resyncs(const nf_v34_sess_t *s);
int nf_v34_sess_pph_renegs(const nf_v34_sess_t *s);
int nf_v34_sess_retrains(const nf_v34_sess_t *s);

/* Classification (NF_V34_CC_*) of the most recently RECEIVED control-channel
 * burst, and the Sh-correlator score [0,1] that produced it. */
int    nf_v34_sess_last_rx_cc_kind(const nf_v34_sess_t *s);
double nf_v34_sess_last_rx_cc_sh_score(const nf_v34_sess_t *s);

/* 1 once the control channel has been established (post-startup), 0 during
 * startup or a retrain. */
int nf_v34_sess_established(const nf_v34_sess_t *s);

/* Force a control-channel retrain (12.8): reset the physical layer back to
 * Phase 2 probing -> Phase 3 training and re-establish. Normally triggered
 * internally when a primary-channel burst is unrecoverable at the floor rate;
 * exposed for the recovery-path regression test. Returns 0 if armed, -1 if
 * the per-call retrain limit has been reached. */
int nf_v34_sess_retrain(nf_v34_sess_t *s);

/* Queue one T.30 control frame for the next control-channel burst (tx mode
 * NF_V34_SESS_CC). len < 0 clears the queue. */
void nf_v34_sess_queue_frame(nf_v34_sess_t *s, const uint8_t *msg, int len);

/* Begin a primary-channel burst (tx mode NF_V34_SESS_PRI): on the next tx
 * pump the session pulls HDLC frames through get_frame (until it returns 0)
 * and builds/plays the whole S/Sbar/PP + B1 + data + turn-off burst. */
void nf_v34_sess_begin_stream(nf_v34_sess_t *s,
                              int (*get_frame)(void *user, uint8_t *buf, int maxlen),
                              void *user);

/* Sample pumps (8 kHz int16). tx fills up to max_len and returns the count;
 * in STARTUP mode it always fills (idle = silence, like nf_v8_tx), in CC/PRI
 * mode it returns short once the queued burst has fully played out (which is
 * how nf_fax detects SEND_STEP_COMPLETE). rx consumes len samples. */
int  nf_v34_sess_tx(nf_v34_sess_t *s, int16_t *amp, int max_len);
int  nf_v34_sess_rx(nf_v34_sess_t *s, const int16_t *amp, int len);

#endif /* NF_V34_H */
