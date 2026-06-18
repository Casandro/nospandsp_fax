#ifndef NF_FAX_H
#define NF_FAX_H

#include <stdint.h>

/*
 * nf_fax - the fax "driver": the glue between a T.30 protocol layer and the
 * V-series modems / HDLC / fax tones. It is the ONLY module that calls into
 * libspandsp (the modems remain spandsp for now), reproducing the role of
 * spandsp's fax.c + fax_modems.c. The protocol layer (nf_t30) connects through
 * the callback struct below, so nf_fax has no dependency on nf_t30.
 *
 * All audio is int16 mono 8 kHz, processed in blocks (typically 160 = 20 ms).
 */

/* Modem selections for nf_fax_set_rx_type / nf_fax_set_tx_type. */
enum {
    NF_MODEM_NONE = 0,
    NF_MODEM_PAUSE,        /* tx: emit `short_train` ms of silence            */
    NF_MODEM_CED,          /* tx: 2100 Hz answer tone                         */
    NF_MODEM_CNG,          /* tx: 1100 Hz calling tone                        */
    NF_MODEM_V21,          /* 300 bps FSK, HDLC control channel               */
    NF_MODEM_V27TER,       /* 2400/4800                                       */
    NF_MODEM_V29,          /* 7200/9600                                       */
    NF_MODEM_V17,          /* 7200..14400                                     */
    NF_MODEM_DONE          /* call finished                                   */
};

/* Front-end status events delivered to the protocol layer. */
enum {
    NF_STATUS_CARRIER_UP = 1,
    NF_STATUS_CARRIER_DOWN,
    NF_STATUS_TRAINING_SUCCEEDED,
    NF_STATUS_TRAINING_FAILED,
    NF_STATUS_SEND_STEP_COMPLETE,   /* a tx step (tone/frame/page) finished    */
    NF_STATUS_ABORT
};

/* non_ecm_get_bit returns this when the image/TCF source is exhausted. */
#define NF_GET_BIT_END (-1)

/* Callbacks the driver invokes up into the protocol layer. Any may be NULL. */
typedef struct {
    void *user;
    void (*hdlc_accept)(void *user, const uint8_t *msg, int len, int ok);
    int  (*non_ecm_get_bit)(void *user);              /* 0/1, or NF_GET_BIT_END */
    void (*non_ecm_put_bit)(void *user, int bit);     /* 0/1 data bits only     */
    void (*front_end_status)(void *user, int status); /* NF_STATUS_*            */
    void (*timer_update)(void *user, int samples);    /* advance protocol timers */
    /* ECM: pull the next HDLC frame to stream over the active (fast) modem.
     * Return the frame length (>0) into buf, or 0 when the burst is complete.
     * Used only between nf_fax_begin_hdlc_stream() and burst end. */
    int  (*hdlc_get_frame)(void *user, uint8_t *buf, int maxlen);
} nf_fax_iface_t;

typedef struct nf_fax nf_fax_t;

nf_fax_t *nf_fax_init(int calling_party, const nf_fax_iface_t *iface);
void      nf_fax_free(nf_fax_t *s);

/* Protocol layer -> driver: select the active rx/tx modem. */
void nf_fax_set_rx_type(nf_fax_t *s, int type, int bit_rate, int short_train, int use_hdlc);
void nf_fax_set_tx_type(nf_fax_t *s, int type, int bit_rate, int short_train, int use_hdlc);

/* Queue an HDLC control frame for V.21 transmission (len < 0 = restart stream). */
void nf_fax_send_hdlc(nf_fax_t *s, const uint8_t *msg, int len);

/* Begin streaming HDLC frames over the current (already-selected, use_hdlc) modem.
 * The driver pulls frames via iface.hdlc_get_frame until it returns 0, then ends
 * the burst cleanly (NF_STATUS_SEND_STEP_COMPLETE). Used for ECM image transfer. */
void nf_fax_begin_hdlc_stream(nf_fax_t *s);

void nf_fax_set_transmit_on_idle(nf_fax_t *s, int on);

/* Sample pump. nf_fax_tx fills up to max_len samples (returns count);
 * nf_fax_rx consumes len samples. */
int  nf_fax_tx(nf_fax_t *s, int16_t *amp, int max_len);
int  nf_fax_rx(nf_fax_t *s, const int16_t *amp, int len);

/* ── Pluggable "physical layer" backend ──────────────────────────────────
 *
 * The T.30 engine (nf_t30) drives a backend through this vtable instead of
 * calling nf_fax_* directly, so the SAME protocol engine can run over either
 * the V-series audio modems (this nf_fax backend) or T.38/UDPTL (nf_t38). The
 * UP direction stays nf_fax_iface_t (both backends call the same nf_t30
 * callbacks). `be` is the opaque backend handle (an nf_fax_t* or nf_t38_t*).
 *
 * tx/rx move audio samples and exist only for the audio backend; the T.38
 * backend leaves them NULL and is pumped via its own UDPTL I/O instead.
 */
typedef struct {
    void (*set_rx_type)(void *be, int type, int bit_rate, int short_train, int use_hdlc);
    void (*set_tx_type)(void *be, int type, int bit_rate, int short_train, int use_hdlc);
    void (*send_hdlc)(void *be, const uint8_t *msg, int len);
    void (*begin_hdlc_stream)(void *be);
    void (*set_transmit_on_idle)(void *be, int on);
    int  (*tx)(void *be, int16_t *amp, int max_len);     /* audio backend only */
    int  (*rx)(void *be, const int16_t *amp, int len);   /* audio backend only */
    void (*free)(void *be);
} nf_modem_ops_t;

/* The audio (V-series modem) backend's vtable. */
const nf_modem_ops_t *nf_fax_ops(void);

#endif /* NF_FAX_H */
