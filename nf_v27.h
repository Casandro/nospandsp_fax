#ifndef NF_V27_H
#define NF_V27_H

#include <stdint.h>
#include "nf_qam.h"

/*
 * nf_v27 - ITU-T V.27ter modem (4800 bps / 1600 baud, 2400 bps / 1200 baud,
 * 1800 Hz carrier, 8-PSK / 4-PSK differential) on the nf_qam engine.
 * Wire-compatible with spandsp's v27ter_tx/v27ter_rx: same long training
 * sequence (32 silence / 50 reversals / 1074 scrambled reversals / 8 ones),
 * 1 + x^-6 + x^-7 scrambler with the 33-bit repetition guard, and Gardner
 * symbol timing.
 */

typedef struct {
    nf_qam_tx_t qam;
    int bit_rate;
    int (*get_bit)(void *user);
    void *get_user;
    int (*current_get_bit)(void *user);
    void *current_user;
    uint32_t scramble_reg;
    int scrambler_pattern_count;
    int in_training;
    int training_step;
    int constellation_state;
} nf_v27_tx_t;

typedef struct {
    nf_qam_rx_t qam;
    int bit_rate;
    void (*put_bit)(void *user, int bit);
    void *put_user;
    uint32_t scramble_reg;
    int scrambler_pattern_count;
    int training_stage;
    int training_count;
    int training_bc;
    int constellation_state;
    int32_t last_angles[2];
    int32_t diff_angles[16];
    float training_error;
    int eq_skip;
} nf_v27_rx_t;

void nf_v27_tx_init(nf_v27_tx_t *s, int bit_rate, int (*get_bit)(void *), void *user);
int  nf_v27_tx_restart(nf_v27_tx_t *s, int bit_rate);
void nf_v27_tx_set_get_bit(nf_v27_tx_t *s, int (*get_bit)(void *), void *user);
int  nf_v27_tx(nf_v27_tx_t *s, int16_t *amp, int max_len);

void nf_v27_rx_init(nf_v27_rx_t *s, int bit_rate, void (*put_bit)(void *, int), void *user);
int  nf_v27_rx_restart(nf_v27_rx_t *s, int bit_rate);
void nf_v27_rx_set_put_bit(nf_v27_rx_t *s, void (*put_bit)(void *, int), void *user);
void nf_v27_rx_set_status_handler(nf_v27_rx_t *s, void (*status)(void *, int), void *user);
int  nf_v27_rx(nf_v27_rx_t *s, const int16_t *amp, int len);

#endif /* NF_V27_H */
