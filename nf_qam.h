#ifndef NF_QAM_H
#define NF_QAM_H

#include <stdint.h>
#include "nf_dsp.h"

/*
 * nf_qam - the shared engine under the V.27ter / V.29 / V.17 fast modems,
 * reproducing the common half of spandsp's three receivers/transmitters.
 *
 * tx: a descriptor-driven modulator. The modem supplies getbaud() (training
 * sequence generator + data encoder); the engine does polyphase RRC
 * interpolation, carrier mixing and gain. The modem ends the burst by setting
 * tx.done (the engine then returns 0 samples, the driver's step-complete).
 *
 * rx: power-based signal detection with hysteresis, AGC (adapting until the
 * modem locks it), a quadrature polyphase RRC front end at passband, complex
 * mix to baseband, T/2-spaced LMS equalizer, and a pluggable symbol timing
 * error detector (Godard band-edge, or Gardner for V.27ter). The engine calls
 * the modem's process_baud() with each equalized symbol; the modem implements
 * its training FSM and data decoding there, using the helpers below
 * (track_carrier / tune_eq / spin / park / save_state). All engine state is
 * exposed so the per-modem code can manipulate gains and counters exactly as
 * spandsp's per-modem code does.
 *
 * All the numeric conventions (constellation units, AGC targets, carrier PI
 * loop gains on the 32-bit phase word, TED triggers) follow spandsp, since
 * the project's acceptance bar is parity with spandsp under line impairments.
 */

/* A get_bit callback that always yields a 1 (idle/filler bits). The V.17,
 * V.27ter and V.29 tx state machines all point current_get_bit at this during
 * training and once a burst has ended; it was previously copy-pasted into each. */
static inline int nf_fake_get_bit(void *user) { (void) user; return 1; }

#define NF_QAM_MAX_TX_TAPS   9
#define NF_QAM_MAX_TX_SETS   20
#define NF_QAM_MAX_RX_TAPS   27
#define NF_QAM_EQ_MAX        33

enum { NF_TED_GODARD = 0, NF_TED_GARDNER };

/* ── tx ────────────────────────────────────────────────────────────── */

typedef struct nf_qam_tx nf_qam_tx_t;

struct nf_qam_tx {
    nf_cpx_t (*getbaud)(void *user);
    void *user;
    const float *shaper;            /* [sets][taps] real interpolator */
    int sets, taps;
    int baud_phase, baud_inc;       /* +=inc, wrap at sets */
    nf_cpx_t rrc_buf[NF_QAM_MAX_TX_TAPS];
    int rrc_ptr;
    uint32_t carrier_phase;
    int32_t carrier_rate;
    float gain;
    int done;                       /* set by the modem after shutdown syms */
};

void nf_qam_tx_init(nf_qam_tx_t *s, const float *shaper, int sets, int taps,
                    int baud_inc, double carrier_hz,
                    nf_cpx_t (*getbaud)(void *), void *user);
void nf_qam_tx_restart(nf_qam_tx_t *s, float gain);
int  nf_qam_tx(nf_qam_tx_t *s, int16_t *amp, int max_len);

/* ── rx ────────────────────────────────────────────────────────────── */

typedef struct nf_qam_rx nf_qam_rx_t;

struct nf_qam_rx {
    /* configuration */
    const float *shaper_re, *shaper_im;  /* [sets][taps] complex bandpass */
    int sets, taps;
    int half_baud_step;             /* sets*samples_per_baud/2, in 1/sets units */
    int eq_len, eq_pre;
    float eq_delta_base;            /* e.g. 0.21f (engine divides by len) */
    float eq_centre_val;            /* reset centre tap (3.0 / 1.414) */
    int eq_centre_idx;              /* reset centre tap index (pre, or pre+1) */
    int eq_put_init;                /* initial eq_put_step on (re)start */
    float agc_target;               /* e.g. 1.25f */
    int32_t carrier_nominal_rate;
    int ted_type;
    /* Godard band-edge TED coefficients (precomputed in init) */
    float g_lo[3], g_hi[3], g_mix;  /* [0]=2a*cos(e) [1]=-a^2 [2]=-a*sin(e) */
    float g_coarse_trig, g_fine_trig;
    int g_coarse_step, g_fine_step;

    /* modem hooks */
    void (*process_baud)(void *user, const nf_cpx_t *z);
    void (*carrier_drop)(void *user);   /* restart the modem FSM */
    void *user;
    void (*status)(void *user, int status);
    void *status_user;

    /* signal detection */
    nf_power_t power;
    int16_t last_sample;
    int32_t on_power, off_power;
    int signal_present;             /* modem sets 60 on training success */
    int parked;                     /* modem sets on training failure */

    /* front end */
    float rrc_filter[NF_QAM_MAX_RX_TAPS];
    int rrc_ptr;
    float agc_scaling, agc_scaling_save;    /* save==0 -> AGC still adapting */
    uint32_t carrier_phase;
    int32_t carrier_phase_rate, carrier_phase_rate_save;
    float carrier_track_p, carrier_track_i;

    /* equalizer */
    nf_cpx_t eq_buf[NF_QAM_EQ_MAX];
    nf_cpx_t eq_coeff[NF_QAM_EQ_MAX];
    nf_cpx_t eq_coeff_save[NF_QAM_EQ_MAX];
    float eq_delta;
    int eq_put_step, eq_step;
    int baud_half;

    /* TED state */
    float ted_lo[2], ted_hi[2], ted_dc[2];
    float ted_phase;                /* godard integrator */
    int total_timing_correction;
    /* gardner */
    int gardner_integrate, gardner_step;

    /* diagnostics: set NFQAMDUMP to a path to log per-baud state */
    void (*diag)(void *user, const nf_cpx_t *z, const nf_cpx_t *target, int state);
    void *diag_user;
};

void nf_qam_rx_init(nf_qam_rx_t *s,
                    const float *shaper_re, const float *shaper_im,
                    int sets, int taps, int half_baud_step,
                    int eq_len, int eq_pre, float eq_delta,
                    float agc_target, double carrier_hz,
                    void (*process_baud)(void *, const nf_cpx_t *),
                    void (*carrier_drop)(void *), void *user);
void nf_qam_rx_set_godard(nf_qam_rx_t *s, double carrier_hz, double baud_rate,
                          double alpha, float coarse_trig, float fine_trig,
                          int coarse_step, int fine_step);
void nf_qam_rx_set_gardner(nf_qam_rx_t *s, int step);
void nf_qam_rx_set_cutoff(nf_qam_rx_t *s, float cutoff_dbm0, float factor);

/* Restart the engine front end. If old_train, restore the saved equalizer,
 * carrier rate and AGC (V.17 short train); else reset them (centre tap
 * (3,0)). agc_init is the unlocked starting scale (target/735-style). */
void nf_qam_rx_restart(nf_qam_rx_t *s, int old_train, float agc_init);
int  nf_qam_rx(nf_qam_rx_t *s, const int16_t *amp, int len);

/* helpers for the modem's process_baud() */
void nf_qam_track_carrier(nf_qam_rx_t *s, const nf_cpx_t *z, const nf_cpx_t *target);
void nf_qam_tune_eq(nf_qam_rx_t *s, const nf_cpx_t *z, const nf_cpx_t *target);
void nf_qam_spin(nf_qam_rx_t *s, int32_t angle);   /* step-rotate phase + eq buffer */
void nf_qam_lock_agc(nf_qam_rx_t *s);
void nf_qam_park(nf_qam_rx_t *s);                  /* + reports TRAINING_FAILED */
void nf_qam_save_state(nf_qam_rx_t *s);            /* eq + carrier rate + agc */
void nf_qam_report(nf_qam_rx_t *s, int status);

#endif /* NF_QAM_H */
