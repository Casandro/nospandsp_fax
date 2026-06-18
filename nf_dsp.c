#include "nf_dsp.h"
#include <math.h>

/*
 * Shared DSP support. See nf_dsp.h for the level conventions (they mirror
 * spandsp: full-scale sine = +3.14 dBm0, power meter in (amp^2) units).
 */

#define NF_SINE_BITS  12
#define NF_SINE_LEN   (1 << NF_SINE_BITS)

static float sine_table[NF_SINE_LEN];
static int   sine_ready;

static void sine_init(void)
{
    for (int i = 0; i < NF_SINE_LEN; i++)
        sine_table[i] = sinf(2.0f * (float) M_PI * (float) i / NF_SINE_LEN);
    sine_ready = 1;
}

float nf_dds_sin(uint32_t phase)
{
    if (!sine_ready) sine_init();
    return sine_table[phase >> (32 - NF_SINE_BITS)];
}

nf_cpx_t nf_dds_cpx(uint32_t phase)
{
    if (!sine_ready) sine_init();
    uint32_t i = phase >> (32 - NF_SINE_BITS);
    /* cos(x) = sin(x + pi/2) */
    return nf_cpx(sine_table[(i + NF_SINE_LEN/4) & (NF_SINE_LEN - 1)], sine_table[i]);
}

int32_t nf_dds_phase_rate(double freq_hz)
{
    return (int32_t) (freq_hz * 65536.0 * 65536.0 / NF_SAMPLE_RATE);
}

/* A full-scale (+-32767) sine is +3.14 dBm0 (DBM0_MAX_SINE_POWER). */
#define NF_DBM0_MAX_SINE_POWER 3.14f
/* Max combined power for dBm0; sine power + 3.02 dB. */
#define NF_DBM0_MAX_POWER      (3.14f + 3.02f)

float nf_dbm0_scaling(double level_dbm0)
{
    return powf(10.0f, ((float) level_dbm0 - NF_DBM0_MAX_SINE_POWER) / 20.0f) * 32767.0f;
}

float nf_power_dbm0(const nf_power_t *p)
{
    if (p->reading <= 0)
        return -96.329f + NF_DBM0_MAX_POWER;
    return 10.0f * log10f((float) p->reading / (32767.0f * 32767.0f) + 1.0e-10f)
           + NF_DBM0_MAX_POWER;
}

int32_t nf_power_level_dbm0(float level)
{
    level -= NF_DBM0_MAX_POWER;
    if (level > 0.0f)
        level = 0.0f;
    return (int32_t) (powf(10.0f, level / 10.0f) * (32767.0f * 32767.0f));
}

int32_t nf_angle32(float im, float re)
{
    return (int32_t) (int64_t) llrint(atan2(im, re) * (2147483648.0 / M_PI));
}

/* closed-form root-raised-cosine impulse response, t in symbol periods */
static double rrc_h(double t, double b)
{
    double x = 4.0 * b * t;
    if (fabs(t) < 1e-9)
        return 1.0 - b + 4.0 * b / M_PI;
    if (fabs(fabs(x) - 1.0) < 1e-7)
        return (b / sqrt(2.0))
             * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * b))
              + (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * b)));
    return (sin(M_PI * t * (1.0 - b)) + 4.0 * b * t * cos(M_PI * t * (1.0 + b)))
         / (M_PI * t * (1.0 - x * x));
}

void nf_rrc_design(double t_step, double beta, int nsets, int ntaps,
                   double carrier_hz, float *re, float *im)
{
    int total = nsets * ntaps + 1;
    int centre = (total - 1) / 2;
    double h[8192];
    double gain = 0.0;
    double w = 2.0 * M_PI * carrier_hz / NF_SAMPLE_RATE;

    for (int k = 0; k < total; k++)
        h[k] = rrc_h((k - centre) * t_step, beta);
    /* normalise: unit DC gain on the centre phase */
    for (int k = nsets / 2; k < total; k += nsets)
        gain += h[k];
    for (int k = 0; k < total; k++)
        h[k] /= gain;

    for (int j = 0; j < nsets; j++) {
        for (int i = 0; i < ntaps; i++) {
            double c = h[i * nsets + j];
            int m = i - (ntaps >> 1);
            if (carrier_hz != 0.0) {
                re[j * ntaps + i] = (float) (c * cos(w * m));
                if (im) im[j * ntaps + i] = (float) (c * sin(w * m));
            } else {
                re[j * ntaps + i] = (float) c;
                if (im) im[j * ntaps + i] = 0.0f;
            }
        }
    }
}
