#ifndef NF_V17_H
#define NF_V17_H

#include <stdint.h>
#include "nf_qam.h"

/*
 * nf_v17 - ITU-T V.17 modem (14400/12000/9600/7200 bps, 2400 baud, 1800 Hz
 * carrier, trellis-coded) on the nf_qam engine. Wire-compatible with
 * spandsp's v17tx/v17rx: same scrambler (1 + x^-18 + x^-23, seed 0x2ECDD5),
 * differential + 8-state rate-2/3 convolutional encoding, constellations,
 * long training (256 ABAB / 2976 scrambled CDBA / 64 bridge / 48 ones) and
 * short training (38-symbol resync against the saved equalizer).
 *
 * The receiver runs an 8-state Viterbi decoder with IIR-smoothed path metrics
 * and a 16-symbol traceback; carrier tracking and (training-time) equalizer
 * adaptation use the nearest single-subset point to keep the loops lag-free.
 */

#define NF_V17_TRELLIS_DEPTH 16

typedef struct {
    nf_qam_tx_t qam;
    int bit_rate;
    int bits_per_symbol;
    const nf_cpx_t *constellation;
    int (*get_bit)(void *user);
    void *get_user;
    int (*current_get_bit)(void *user);
    void *current_user;
    uint32_t scramble_reg;
    int in_training;
    int short_train;
    int training_step;
    int constellation_state;
    int diff, convolution;
} nf_v17_tx_t;

typedef struct {
    nf_qam_rx_t qam;
    int bit_rate;
    int bits_per_symbol;
    int space_map;                  /* 0..3 for 14400..7200 */
    const nf_cpx_t *constellation;
    void (*put_bit)(void *user, int bit);
    void *put_user;
    uint32_t scramble_reg;
    int training_stage;
    int training_count;
    int short_train;
    int diff;
    int32_t last_angles[2];
    int32_t diff_angles[16];
    float training_error;
    /* trellis */
    float distances[8];
    uint8_t full_path_to_past_state_locations[NF_V17_TRELLIS_DEPTH][8];
    uint8_t past_state_locations[NF_V17_TRELLIS_DEPTH][8];
    int trellis_ptr;
} nf_v17_rx_t;

void nf_v17_tx_init(nf_v17_tx_t *s, int bit_rate, int (*get_bit)(void *), void *user);
int  nf_v17_tx_restart(nf_v17_tx_t *s, int bit_rate, int short_train);
void nf_v17_tx_set_get_bit(nf_v17_tx_t *s, int (*get_bit)(void *), void *user);
int  nf_v17_tx(nf_v17_tx_t *s, int16_t *amp, int max_len);

void nf_v17_rx_init(nf_v17_rx_t *s, int bit_rate, void (*put_bit)(void *, int), void *user);
int  nf_v17_rx_restart(nf_v17_rx_t *s, int bit_rate, int short_train);
void nf_v17_rx_set_put_bit(nf_v17_rx_t *s, void (*put_bit)(void *, int), void *user);
void nf_v17_rx_set_status_handler(nf_v17_rx_t *s, void (*status)(void *, int), void *user);
int  nf_v17_rx(nf_v17_rx_t *s, const int16_t *amp, int len);

#endif /* NF_V17_H */
