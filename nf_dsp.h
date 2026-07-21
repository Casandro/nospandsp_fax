#ifndef NF_DSP_H
#define NF_DSP_H

#include <stdint.h>

/*
 * nf_dsp - shared DSP support for the nf modem layer: complex helpers, a DDS
 * (numerically controlled oscillator), dBm0 level conversions and a power
 * meter. The numeric conventions deliberately match spandsp's, because the
 * carrier-detect thresholds and tx levels of the whole fax ecosystem are
 * calibrated against them: a full-scale (+-32767) sine is +3.14 dBm0, and the
 * power meter is the same integer exponential average.
 */

#define NF_SAMPLE_RATE 8000

/* Signalling status events, numerically identical to spandsp's SIG_STATUS_*
 * so they can flow through nf_fax.c unchanged during the migration. */
enum {
    NF_SIG_CARRIER_DOWN         = -1,
    NF_SIG_CARRIER_UP           = -2,
    NF_SIG_TRAINING_IN_PROGRESS = -3,
    NF_SIG_TRAINING_SUCCEEDED   = -4,
    NF_SIG_TRAINING_FAILED      = -5,
    /* nf_hdlc reports rx framing-OK as -6 and rx aborts as -8, matching
     * spandsp's SIG_STATUS_FRAMING_OK / SIG_STATUS_ABORT, through the same
     * status plumbing */
    NF_SIG_FRAMING_OK           = -6,
    NF_SIG_END_OF_DATA          = -7,
    NF_SIG_ABORT                = -8,
    /* a queued tx data section finished going out while the carrier stays up
     * (the V.34 control channel holds flags between frames, so there is no
     * "burst ended" moment to infer completion from) */
    NF_SIG_SEND_COMPLETE        = -9
};

/* Lazily read a boolean flag from an environment variable and cache it.
 * *cache must start < 0; the variable counts as "on" only when set and
 * non-empty. Backs the per-module debug-trace switches (nf_fax, nf_t38). */
int nf_cached_env_flag(int *cache, const char *envvar);

/* ── complex float ─────────────────────────────────────────────────── */

typedef struct { float re, im; } nf_cpx_t;

static inline nf_cpx_t nf_cpx(float re, float im)
{
    nf_cpx_t z; z.re = re; z.im = im; return z;
}
static inline nf_cpx_t nf_cpx_add(nf_cpx_t a, nf_cpx_t b) { return nf_cpx(a.re + b.re, a.im + b.im); }
static inline nf_cpx_t nf_cpx_sub(nf_cpx_t a, nf_cpx_t b) { return nf_cpx(a.re - b.re, a.im - b.im); }
static inline nf_cpx_t nf_cpx_mul(nf_cpx_t a, nf_cpx_t b)
{
    return nf_cpx(a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re);
}
/* a * conj(b) */
static inline nf_cpx_t nf_cpx_mul_conj(nf_cpx_t a, nf_cpx_t b)
{
    return nf_cpx(a.re*b.re + a.im*b.im, a.im*b.re - a.re*b.im);
}
static inline float nf_cpx_power(nf_cpx_t z) { return z.re*z.re + z.im*z.im; }

/* ── DDS (32-bit phase accumulator, table sine) ────────────────────── */

/* Phase rate for freq in Hz at 8 kHz: freq * 2^32 / 8000. */
int32_t nf_dds_phase_rate(double freq_hz);

/* sin/e^{j.phase} for a 32-bit phase word. */
float    nf_dds_sin(uint32_t phase);
nf_cpx_t nf_dds_cpx(uint32_t phase);           /* (cos, sin) */

/* Peak amplitude (0..32767) of a sine at `level` dBm0 (full scale = +3.14). */
float nf_dbm0_scaling(double level_dbm0);

/* Advance the oscillator and return one int16 sample at peak `scaling`. */
static inline int16_t nf_dds_mod(uint32_t *phase, int32_t rate, float scaling)
{
    float v = nf_dds_sin(*phase) * scaling;
    *phase += (uint32_t) rate;
    return (int16_t) (v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* Advance and return e^{j.phase} (for mixers). */
static inline nf_cpx_t nf_dds_cpx_mod(uint32_t *phase, int32_t rate)
{
    nf_cpx_t z = nf_dds_cpx(*phase);
    *phase += (uint32_t) rate;
    return z;
}

/* ── angles as 32-bit phase words (2^32 = one turn) ────────────────── */

/* Wrap via int64 -> uint32: a plain double -> int32 cast is UB above 180
 * degrees (it saturates on x86, which is NOT the modular wrap we need). */
#define NF_PHASE(deg)  ((int32_t) (uint32_t) (int64_t) ((deg) / 360.0 * 4294967296.0))

/* atan2 as a 32-bit phase word. */
int32_t nf_angle32(float im, float re);

static inline float nf_phase_to_radians(int32_t phase)
{
    return (float) phase * (float) (3.14159265358979 / 2147483648.0);
}

/* ── polyphase (root-)raised-cosine designer ───────────────────────── */

/*
 * Build an nsets-phase pulse shaper of ntaps taps each. The RRC prototype has
 * t_step symbol periods between consecutive prototype coefficients (tx
 * interpolator: 1/nsets; rx filter at 8 kHz: baud/(nsets*8000)), rolloff
 * beta, and is normalised to unit DC gain on the centre phase (spandsp's
 * convention, so the same gain calibrations apply). If carrier_hz is nonzero
 * the taps are complex-modulated at the carrier (same phase for all sets):
 * re[j][i] = c*cos(w*(i-ntaps/2)), im[j][i] = c*sin(w*(i-ntaps/2)); im may be
 * NULL for a real (tx) shaper. Arrays are [nsets][ntaps] flattened.
 */
void nf_rrc_design(double t_step, double beta, int nsets, int ntaps,
                   double carrier_hz, float *re, float *im);

/* ── power meter (spandsp-identical integer semantics) ─────────────── */

typedef struct { int32_t reading; int shift; } nf_power_t;

static inline void nf_power_init(nf_power_t *p, int shift)
{
    p->reading = 0; p->shift = shift;
}
static inline int32_t nf_power_update(nf_power_t *p, int16_t amp)
{
    p->reading += (((int32_t) amp * amp - p->reading) >> p->shift);
    return p->reading;
}
float   nf_power_dbm0(const nf_power_t *p);
int32_t nf_power_level_dbm0(float level);      /* threshold in reading units */

#endif /* NF_DSP_H */
