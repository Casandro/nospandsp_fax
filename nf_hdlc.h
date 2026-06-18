#ifndef NF_HDLC_H
#define NF_HDLC_H

#include <stdint.h>

/*
 * nf_hdlc - HDLC framing for fax: CRC-16/X.25 (poly 0x8408 reflected, init
 * 0xFFFF, good residue 0xF0B8), zero-bit stuffing after five ones, 0x7E flags,
 * abort on seven-plus ones. Wire-compatible with spandsp's hdlc.c in the modes
 * the fax driver uses (16-bit CRC, non-progressive tx).
 *
 * The tx side is a bit pump: nf_hdlc_tx_get_bit() yields the stream (flags /
 * stuffed frame bits) and returns NF_SIG_END_OF_DATA (from nf_dsp.h) once a
 * queued end-marker (zero-length frame) has drained, which is how the modem
 * learns to drop the carrier. The underflow callback fires when a timed flag
 * preamble runs out with nothing queued, and after every completed frame -
 * the ECM path queues the next frame from inside it for back-to-back frames.
 */

typedef void (*nf_hdlc_frame_fn)(void *user, const uint8_t *msg, int len, int ok);
typedef void (*nf_hdlc_underflow_fn)(void *user);

#define NF_HDLC_MAXFRAME 400            /* payload bytes (ECM FCD is 260) */

typedef struct {
    nf_hdlc_underflow_fn underflow;
    void *user;
    int inter_frame_flags;
    uint8_t buf[NF_HDLC_MAXFRAME + 2];  /* payload + appended CRC */
    int nbytes;                         /* payload + 2, or 0 = nothing queued */
    int pos, bytebit;                   /* read position in buf */
    int ones, stuff;                    /* stuffing state */
    int sending_data;                   /* else: flags / idle */
    int flag_bitpos;
    int flag_octets;                    /* timed flags left; 0 = untimed idle */
    int report_flag_underflow;
    int tx_end;
} nf_hdlc_tx_t;

typedef struct {
    nf_hdlc_frame_fn handler;
    void *user;
    int framing_ok_threshold;
    uint16_t raw;                       /* 16-bit raw bit history */
    int flags_seen;
    int framing_ok_announced;
    uint32_t byte_in_progress;
    int num_bits;
    uint8_t buffer[NF_HDLC_MAXFRAME + 2];
    int len;
} nf_hdlc_rx_t;

void nf_hdlc_tx_init(nf_hdlc_tx_t *s, int inter_frame_flags,
                     nf_hdlc_underflow_fn underflow, void *user);
/* Queue a frame (CRC appended internally). len == 0 marks end-of-transmission.
 * Returns -1 if a frame is already queued (lockout), else 0. */
int  nf_hdlc_tx_frame(nf_hdlc_tx_t *s, const uint8_t *buf, int len);
void nf_hdlc_tx_flags(nf_hdlc_tx_t *s, int count);
void nf_hdlc_tx_restart(nf_hdlc_tx_t *s);
int  nf_hdlc_tx_get_bit(nf_hdlc_tx_t *s);

void nf_hdlc_rx_init(nf_hdlc_rx_t *s, int framing_ok_threshold,
                     nf_hdlc_frame_fn handler, void *user);
void nf_hdlc_rx_put_bit(nf_hdlc_rx_t *s, int bit);

uint16_t nf_crc16(const uint8_t *buf, int len, uint16_t crc);

#endif /* NF_HDLC_H */
