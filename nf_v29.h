#ifndef NF_V29_H
#define NF_V29_H

#include <stdint.h>
#include "nf_qam.h"

/*
 * nf_v29 - ITU-T V.29 modem (9600/7200/4800 bps, 2400 baud, 1700 Hz carrier)
 * on the nf_qam engine. Wire-compatible with spandsp's v29tx/v29rx: same
 * training sequence (ALT 128 / scrambled CDCD 384 / 48 test ones), scrambler
 * (1 + x^-18 + x^-23), differential phase encoding and constellations.
 */

typedef struct {
    nf_qam_tx_t qam;
    int bit_rate;
    int (*get_bit)(void *user);
    void *get_user;
    int (*current_get_bit)(void *user);
    void *current_user;
    uint32_t scramble_reg;
    uint8_t training_scramble_reg;
    int in_training;
    int training_step;
    int training_offset;
    int constellation_state;
} nf_v29_tx_t;

typedef struct {
    nf_qam_rx_t qam;
    int bit_rate;
    void (*put_bit)(void *user, int bit);
    void *put_user;
    uint32_t scramble_reg;
    uint8_t training_scramble_reg;
    int training_stage;
    int training_count;
    int training_cd;                /* 0/2/4 by bit rate */
    int constellation_state;
    int32_t last_angles[2];
    int32_t diff_angles[16];
    float training_error;
    int eq_skip;
} nf_v29_rx_t;

void nf_v29_tx_init(nf_v29_tx_t *s, int bit_rate, int (*get_bit)(void *), void *user);
int  nf_v29_tx_restart(nf_v29_tx_t *s, int bit_rate);
void nf_v29_tx_set_get_bit(nf_v29_tx_t *s, int (*get_bit)(void *), void *user);
int  nf_v29_tx(nf_v29_tx_t *s, int16_t *amp, int max_len);

void nf_v29_rx_init(nf_v29_rx_t *s, int bit_rate, void (*put_bit)(void *, int), void *user);
int  nf_v29_rx_restart(nf_v29_rx_t *s, int bit_rate);
void nf_v29_rx_set_put_bit(nf_v29_rx_t *s, void (*put_bit)(void *, int), void *user);
void nf_v29_rx_set_status_handler(nf_v29_rx_t *s, void (*status)(void *, int), void *user);
int  nf_v29_rx(nf_v29_rx_t *s, const int16_t *amp, int len);

#endif /* NF_V29_H */
