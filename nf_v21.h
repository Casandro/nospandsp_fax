#ifndef NF_V21_H
#define NF_V21_H

#include <stdint.h>
#include "nf_dsp.h"

/*
 * nf_v21 - the V.21 channel-2 FSK modem used for the T.30 control channel:
 * 300 baud, bit 1 (mark) = 1650 Hz, bit 0 (space) = 1850 Hz, tx at -14 dBm0.
 * Behaviour mirrors spandsp's fsk.c in FSK_FRAME_MODE_SYNC:
 *
 * tx: DDS with phase-coherent per-bit frequency switching; pulls bits from
 *     get_bit (the HDLC stream) and shuts down on NF_SIG_END_OF_DATA, after
 *     which nf_v21_tx() returns short / 0 (the driver's step-complete signal).
 * rx: non-coherent quadrature correlation against both tones over one baud
 *     (sliding window), power-based carrier detection (-30 dBm0 cutoff with
 *     +-2.5 dB hysteresis on a one-pole HPF'd power meter), and gentle baud
 *     phase nudging (baud/8 toward mid-bit) on each transition so long HDLC
 *     frames stay bit-aligned. Statuses go to the status handler; data bits
 *     to put_bit.
 */

typedef struct {
    int (*get_bit)(void *user);
    void *get_user;
    uint32_t phase;
    int32_t rates[2];
    int32_t cur_rate;
    float scaling;
    int baud_frac;                  /* 0.01-baud units, wraps at 800000 */
    int shutdown;
} nf_v21_tx_t;

#define NF_V21_SPAN 26              /* samples per baud (8000*100/30000) */

typedef struct {
    void (*put_bit)(void *user, int bit);
    void *put_user;
    void (*status)(void *user, int status);
    void *status_user;
    uint32_t phase[2];
    int32_t rate[2];
    nf_cpx_t window[2][NF_V21_SPAN];
    nf_cpx_t dot[2];
    int buf_ptr;
    nf_power_t power;
    int16_t last_sample;
    int32_t on_power, off_power;
    int signal_present;
    int baud_phase;                 /* SAMPLE_RATE*100 units */
    int last_bit;
} nf_v21_rx_t;

void nf_v21_tx_init(nf_v21_tx_t *s, int (*get_bit)(void *), void *user);
int  nf_v21_tx(nf_v21_tx_t *s, int16_t *amp, int max_len);

void nf_v21_rx_init(nf_v21_rx_t *s, void (*put_bit)(void *, int), void *user);
void nf_v21_rx_set_status_handler(nf_v21_rx_t *s,
                                  void (*status)(void *, int), void *user);
int  nf_v21_rx(nf_v21_rx_t *s, const int16_t *amp, int len);

#endif /* NF_V21_H */
