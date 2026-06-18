#include "nf_qam.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int qamdbg(void) { static int d = -1; if (d < 0) d = getenv("NFQAMDBG") ? 1 : 0; return d; }
static long qam_clock;          /* diagnostic sample counter, shared */

/* ── tx ────────────────────────────────────────────────────────────── */

void nf_qam_tx_init(nf_qam_tx_t *s, const float *shaper, int sets, int taps,
                    int baud_inc, double carrier_hz,
                    nf_cpx_t (*getbaud)(void *), void *user)
{
    memset(s, 0, sizeof(*s));
    s->shaper = shaper;
    s->sets = sets;
    s->taps = taps;
    s->baud_inc = baud_inc;
    s->carrier_rate = nf_dds_phase_rate(carrier_hz);
    s->getbaud = getbaud;
    s->user = user;
}

void nf_qam_tx_restart(nf_qam_tx_t *s, float gain)
{
    memset(s->rrc_buf, 0, sizeof(s->rrc_buf));
    s->rrc_ptr = 0;
    s->baud_phase = 0;
    s->carrier_phase = 0;
    s->gain = gain;
    s->done = 0;
}

int nf_qam_tx(nf_qam_tx_t *s, int16_t *amp, int max_len)
{
    int sample;

    if (s->done)
        return 0;
    for (sample = 0; sample < max_len; sample++) {
        if ((s->baud_phase += s->baud_inc) >= s->sets) {
            s->baud_phase -= s->sets;
            s->rrc_buf[s->rrc_ptr] = s->getbaud(s->user);
            if (++s->rrc_ptr >= s->taps)
                s->rrc_ptr = 0;
            if (s->done)
                break;
        }
        /* root raised cosine pulse shaping at baseband (polyphase) */
        const float *c = &s->shaper[(s->sets - 1 - s->baud_phase) * s->taps];
        nf_cpx_t x = nf_cpx(0.0f, 0.0f);
        for (int i = 0; i < s->taps; i++) {
            int k = s->rrc_ptr + i;
            if (k >= s->taps) k -= s->taps;
            x.re += s->rrc_buf[k].re * c[i];
            x.im += s->rrc_buf[k].im * c[i];
        }
        /* modulate the carrier */
        nf_cpx_t z = nf_dds_cpx_mod(&s->carrier_phase, s->carrier_rate);
        float famp = (x.re * z.re - x.im * z.im) * s->gain;
        amp[sample] = (int16_t) lrintf(famp);
    }
    return sample;
}

/* ── rx ────────────────────────────────────────────────────────────── */

void nf_qam_rx_init(nf_qam_rx_t *s,
                    const float *shaper_re, const float *shaper_im,
                    int sets, int taps, int half_baud_step,
                    int eq_len, int eq_pre, float eq_delta,
                    float agc_target, double carrier_hz,
                    void (*process_baud)(void *, const nf_cpx_t *),
                    void (*carrier_drop)(void *), void *user)
{
    memset(s, 0, sizeof(*s));
    s->shaper_re = shaper_re;
    s->shaper_im = shaper_im;
    s->sets = sets;
    s->taps = taps;
    s->half_baud_step = half_baud_step;
    s->eq_len = eq_len;
    s->eq_pre = eq_pre;
    s->eq_centre_val = 3.0f;
    s->eq_centre_idx = eq_pre;
    s->eq_put_init = half_baud_step - 1;
    s->eq_delta_base = eq_delta;
    s->agc_target = agc_target;
    s->carrier_nominal_rate = nf_dds_phase_rate(carrier_hz);
    s->process_baud = process_baud;
    s->carrier_drop = carrier_drop;
    s->user = user;
}

void nf_qam_rx_set_godard(nf_qam_rx_t *s, double carrier_hz, double baud_rate,
                          double alpha, float coarse_trig, float fine_trig,
                          int coarse_step, int fine_step)
{
    double lo = 2.0 * M_PI * (carrier_hz - baud_rate / 2.0) / NF_SAMPLE_RATE;
    double hi = 2.0 * M_PI * (carrier_hz + baud_rate / 2.0) / NF_SAMPLE_RATE;

    s->ted_type = NF_TED_GODARD;
    s->g_lo[0] = (float) (2.0 * alpha * cos(lo));
    s->g_lo[1] = (float) (-alpha * alpha);
    s->g_lo[2] = (float) (-alpha * sin(lo));
    s->g_hi[0] = (float) (2.0 * alpha * cos(hi));
    s->g_hi[1] = (float) (-alpha * alpha);
    s->g_hi[2] = (float) (-alpha * sin(hi));
    s->g_mix = (float) (-alpha * alpha * (sin(hi) * cos(lo) - sin(lo) * cos(hi)));
    s->g_coarse_trig = coarse_trig;
    s->g_fine_trig = fine_trig;
    s->g_coarse_step = coarse_step;
    s->g_fine_step = fine_step;
}

void nf_qam_rx_set_gardner(nf_qam_rx_t *s, int step)
{
    s->ted_type = NF_TED_GARDNER;
    s->gardner_step = step;
}

void nf_qam_rx_set_cutoff(nf_qam_rx_t *s, float cutoff_dbm0, float factor)
{
    s->on_power  = (int32_t) (nf_power_level_dbm0(cutoff_dbm0 + 2.5f) * factor);
    s->off_power = (int32_t) (nf_power_level_dbm0(cutoff_dbm0 - 2.5f) * factor);
}

void nf_qam_report(nf_qam_rx_t *s, int status)
{
    if (s->status)
        s->status(s->status_user, status);
}

static void equalizer_reset(nf_qam_rx_t *s)
{
    memset(s->eq_coeff, 0, sizeof(s->eq_coeff));
    s->eq_coeff[s->eq_centre_idx] = nf_cpx(s->eq_centre_val, 0.0f);
    memset(s->eq_buf, 0, sizeof(s->eq_buf));
    s->eq_delta = s->eq_delta_base / s->eq_len;
    s->eq_put_step = s->eq_put_init;
    s->eq_step = 0;
}

static void equalizer_restore(nf_qam_rx_t *s)
{
    memcpy(s->eq_coeff, s->eq_coeff_save, sizeof(s->eq_coeff));
    memset(s->eq_buf, 0, sizeof(s->eq_buf));
    s->eq_delta = s->eq_delta_base / s->eq_len;
    s->eq_put_step = s->eq_put_init;
    s->eq_step = 0;
}

void nf_qam_rx_restart(nf_qam_rx_t *s, int old_train, float agc_init)
{
    memset(s->rrc_filter, 0, sizeof(s->rrc_filter));
    s->rrc_ptr = 0;
    s->signal_present = 0;
    s->parked = 0;
    s->carrier_phase = 0;
    nf_power_init(&s->power, 4);
    s->last_sample = 0;
    if (old_train && s->agc_scaling_save != 0.0f) {
        s->carrier_phase_rate = s->carrier_phase_rate_save;
        s->agc_scaling = s->agc_scaling_save;
        equalizer_restore(s);
    } else {
        s->carrier_phase_rate = s->carrier_nominal_rate;
        s->agc_scaling_save = 0.0f;
        s->agc_scaling = agc_init;
        equalizer_reset(s);
    }
    s->ted_lo[0] = s->ted_lo[1] = 0.0f;
    s->ted_hi[0] = s->ted_hi[1] = 0.0f;
    s->ted_dc[0] = s->ted_dc[1] = 0.0f;
    s->ted_phase = 0.0f;
    s->total_timing_correction = 0;
    s->gardner_integrate = 0;
    s->baud_half = 0;
}

void nf_qam_track_carrier(nf_qam_rx_t *s, const nf_cpx_t *z, const nf_cpx_t *target)
{
    float error = z->im * target->re - z->re * target->im;
    s->carrier_phase_rate += (int32_t) (s->carrier_track_i * error);
    s->carrier_phase += (int32_t) (s->carrier_track_p * error);
}

void nf_qam_tune_eq(nf_qam_rx_t *s, const nf_cpx_t *z, const nf_cpx_t *target)
{
    /* LMS, with a little leak to tame uncontrolled wandering */
    float er = (target->re - z->re) * s->eq_delta;
    float ei = (target->im - z->im) * s->eq_delta;
    int pos = s->eq_step;
    for (int i = 0; i < s->eq_len; i++) {
        nf_cpx_t x = s->eq_buf[pos];
        if (++pos >= s->eq_len) pos = 0;
        s->eq_coeff[i].re = s->eq_coeff[i].re * 0.9999f + (x.im * ei + x.re * er);
        s->eq_coeff[i].im = s->eq_coeff[i].im * 0.9999f + (x.re * ei - x.im * er);
    }
}

void nf_qam_spin(nf_qam_rx_t *s, int32_t angle)
{
    float p = nf_phase_to_radians(angle);
    nf_cpx_t zz = nf_cpx(cosf(p), -sinf(p));
    for (int i = 0; i < s->eq_len; i++)
        s->eq_buf[i] = nf_cpx_mul(s->eq_buf[i], zz);
    s->carrier_phase += (uint32_t) angle;
}

void nf_qam_lock_agc(nf_qam_rx_t *s)
{
    if (s->agc_scaling_save == 0.0f)
        s->agc_scaling_save = s->agc_scaling;
}

void nf_qam_park(nf_qam_rx_t *s)
{
    s->agc_scaling_save = 0.0f;
    s->parked = 1;
    nf_qam_report(s, NF_SIG_TRAINING_FAILED);
}

void nf_qam_save_state(nf_qam_rx_t *s)
{
    memcpy(s->eq_coeff_save, s->eq_coeff, sizeof(s->eq_coeff));
    s->carrier_phase_rate_save = s->carrier_phase_rate;
    s->agc_scaling_save = s->agc_scaling;
}

static nf_cpx_t equalizer_get(nf_qam_rx_t *s)
{
    nf_cpx_t z = nf_cpx(0.0f, 0.0f);
    int pos = s->eq_step;
    for (int i = 0; i < s->eq_len; i++) {
        nf_cpx_t x = s->eq_buf[pos];
        if (++pos >= s->eq_len) pos = 0;
        z.re += x.re * s->eq_coeff[i].re - x.im * s->eq_coeff[i].im;
        z.im += x.re * s->eq_coeff[i].im + x.im * s->eq_coeff[i].re;
    }
    return z;
}

/* Godard band-edge timing error, evaluated once per baud */
static int ted_per_baud(nf_qam_rx_t *s)
{
    if (s->ted_type == NF_TED_GARDNER) {
        /* Gardner early-late test on the T/2 equalizer buffer, with
         * integrate-and-dump so the put step doesn't jitter rapidly */
        int i3 = s->eq_step - 3; if (i3 < 0) i3 += s->eq_len;
        int i2 = s->eq_step - 2; if (i2 < 0) i2 += s->eq_len;
        int i1 = s->eq_step - 1; if (i1 < 0) i1 += s->eq_len;
        float v = (s->eq_buf[i3].re - s->eq_buf[i1].re) * s->eq_buf[i2].re
                + (s->eq_buf[i3].im - s->eq_buf[i1].im) * s->eq_buf[i2].im;
        s->gardner_integrate += (v > 0.0f) ? s->gardner_step : -s->gardner_step;
        if (abs(s->gardner_integrate) >= 128) {
            int hop = s->gardner_integrate / 128;
            s->gardner_integrate = 0;
            s->total_timing_correction += hop;
            return hop;
        }
        return 0;
    }
    /* cross correlate the band edges (Godard fig. 3b, rearranged) */
    float v = s->ted_lo[1] * s->ted_hi[0] * s->g_lo[2]
            - s->ted_lo[0] * s->ted_hi[1] * s->g_hi[2]
            + s->ted_lo[1] * s->ted_hi[1] * s->g_mix;
    /* filter away any DC component */
    float p = v - s->ted_dc[1];
    s->ted_dc[1] = s->ted_dc[0];
    s->ted_dc[0] = v;
    /* a little integration filters away much of the HF noise */
    s->ted_phase -= p;
    v = fabsf(s->ted_phase);
    if (v > s->g_fine_trig) {
        int i = (v > s->g_coarse_trig) ? s->g_coarse_step : s->g_fine_step;
        if (s->ted_phase < 0.0f)
            i = -i;
        s->total_timing_correction += i;
        return i;
    }
    return 0;
}

static void ted_rx(nf_qam_rx_t *s, float sample)
{
    float v;
    if (s->ted_type != NF_TED_GODARD)
        return;
    v = s->ted_lo[0] * s->g_lo[0] + s->ted_lo[1] * s->g_lo[1] + sample;
    s->ted_lo[1] = s->ted_lo[0];
    s->ted_lo[0] = v;
    v = s->ted_hi[0] * s->g_hi[0] + s->ted_hi[1] * s->g_hi[1] + sample;
    s->ted_hi[1] = s->ted_hi[0];
    s->ted_hi[0] = v;
}

static void process_half_baud(nf_qam_rx_t *s, const nf_cpx_t *sample)
{
    /* insert at T/2; process on alternate insertions */
    s->eq_buf[s->eq_step] = *sample;
    if (++s->eq_step >= s->eq_len)
        s->eq_step = 0;
    if ((s->baud_half ^= 1))
        return;
    s->eq_put_step += ted_per_baud(s);
    nf_cpx_t z = equalizer_get(s);
    s->process_baud(s->user, &z);
}

static int signal_detect(nf_qam_rx_t *s, int16_t amp)
{
    /* power with the DC blocked by the most elementary HPF */
    int16_t x = amp >> 1;
    int16_t diff = (int16_t) (x - s->last_sample);
    s->last_sample = x;
    int32_t power = nf_power_update(&s->power, diff);
    qam_clock++;
    if (s->signal_present > 0) {
        if (power < s->off_power) {
            if (--s->signal_present <= 0) {
                if (qamdbg())
                    fprintf(stderr, "  <qam> t=%.2fs carrier DOWN (power=%d off=%d)\n",
                            qam_clock / 8000.0, power, s->off_power);
                if (s->carrier_drop)
                    s->carrier_drop(s->user);
                nf_qam_report(s, NF_SIG_CARRIER_DOWN);
                return 0;
            }
        }
    } else {
        if (power < s->on_power)
            return 0;
        s->signal_present = 1;
        if (qamdbg())
            fprintf(stderr, "  <qam> t=%.2fs carrier UP (power=%d on=%d)\n",
                    qam_clock / 8000.0, power, s->on_power);
        nf_qam_report(s, NF_SIG_CARRIER_UP);
    }
    return power;
}

int nf_qam_rx(nf_qam_rx_t *s, const int16_t *amp, int len)
{
    for (int i = 0; i < len; i++) {
        s->rrc_filter[s->rrc_ptr] = (float) amp[i];
        if (++s->rrc_ptr >= s->taps)
            s->rrc_ptr = 0;

        int32_t power = signal_detect(s, amp[i]);
        if (power == 0)
            continue;
        if (s->parked)
            continue;

        s->eq_put_step -= s->sets;
        int step = -s->eq_put_step;
        if (step < 0)
            step += s->sets;
        if (step < 0)
            step = 0;
        else if (step > s->sets - 1)
            step = s->sets - 1;

        const float *cre = &s->shaper_re[step * s->taps];
        float v = 0.0f;
        {
            int pos = s->rrc_ptr;
            for (int k = 0; k < s->taps; k++) {
                v += s->rrc_filter[pos] * cre[k];
                if (++pos >= s->taps) pos = 0;
            }
        }
        float sample_re = v * s->agc_scaling;
        ted_rx(s, sample_re);

        if (s->eq_put_step <= 0) {
            /* only AGC until the modem locks the setting */
            if (s->agc_scaling_save == 0.0f) {
                float root_power = sqrtf((float) power);
                if (root_power < 1.0f)
                    root_power = 1.0f;
                s->agc_scaling = s->agc_target / root_power;
            }
            const float *cim = &s->shaper_im[step * s->taps];
            float w = 0.0f;
            int pos = s->rrc_ptr;
            for (int k = 0; k < s->taps; k++) {
                w += s->rrc_filter[pos] * cim[k];
                if (++pos >= s->taps) pos = 0;
            }
            float sample_im = w * s->agc_scaling;
            /* bring the bandpass-filtered signal to baseband */
            nf_cpx_t z = nf_dds_cpx(s->carrier_phase);
            nf_cpx_t zz;
            zz.re = sample_re * z.re - sample_im * z.im;
            zz.im = -sample_re * z.im - sample_im * z.re;
            s->eq_put_step += s->half_baud_step;
            process_half_baud(s, &zz);
        }
        s->carrier_phase += (uint32_t) s->carrier_phase_rate;
    }
    return 0;
}
