/*
 * Stage 2a loopback harness for nf_fax: no T.30 yet. Pumps one engine's tx
 * samples into another's rx and checks the modem/HDLC/status glue.
 *   build: cc nf_faxloop.c nf_fax.c -lspandsp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nf_fax.h"

#define BLK 160

/* Per-engine test context (the iface user_data). */
struct ctx {
    const char *name;
    /* rx side */
    uint8_t  frame[300]; int frame_len; int frame_ok; int frames;
    int data_bits;
    int trained, carrier_up, carrier_down, step_complete;
    /* tx side: non-ECM get_bit source (TCF-like zeros, then end) */
    int tx_bits_left;
};

static void on_hdlc(void *u, const uint8_t *msg, int len, int ok)
{
    struct ctx *c = u;
    c->frames++;
    c->frame_ok = ok;
    c->frame_len = len > (int) sizeof c->frame ? (int) sizeof c->frame : len;
    memcpy(c->frame, msg, (size_t) c->frame_len);
}
static int on_get_bit(void *u)
{
    struct ctx *c = u;
    if (c->tx_bits_left-- <= 0) return NF_GET_BIT_END;
    return 0;                                   /* TCF: all zeros */
}
static void on_put_bit(void *u, int bit) { struct ctx *c = u; (void) bit; c->data_bits++; }
static void on_status(void *u, int st)
{
    struct ctx *c = u;
    switch (st) {
    case NF_STATUS_CARRIER_UP:          c->carrier_up++; break;
    case NF_STATUS_CARRIER_DOWN:        c->carrier_down++; break;
    case NF_STATUS_TRAINING_SUCCEEDED:  c->trained++; break;
    case NF_STATUS_SEND_STEP_COMPLETE:  c->step_complete++; break;
    }
}

static nf_fax_t *make(struct ctx *c, const char *name, int calling)
{
    memset(c, 0, sizeof *c);
    c->name = name;
    nf_fax_iface_t iface = {
        .user = c, .hdlc_accept = on_hdlc, .non_ecm_get_bit = on_get_bit,
        .non_ecm_put_bit = on_put_bit, .front_end_status = on_status, .timer_update = NULL,
    };
    nf_fax_t *f = nf_fax_init(calling, &iface);
    nf_fax_set_transmit_on_idle(f, 1);
    return f;
}

/* Pump A.tx -> B.rx for up to `blocks`, stopping when stop() is true. */
static int pump(nf_fax_t *atx, nf_fax_t *brx, int blocks, int (*stop)(void *), void *sd)
{
    int16_t buf[BLK];
    for (int i = 0; i < blocks; i++) {
        int n = nf_fax_tx(atx, buf, BLK);
        if (n < BLK) memset(buf + n, 0, (size_t) (BLK - n) * sizeof(int16_t));
        nf_fax_rx(brx, buf, BLK);
        if (stop && stop(sd)) return i;
    }
    return blocks;
}

static int got_frame(void *sd) { return ((struct ctx *) sd)->frames > 0; }
static int got_train(void *sd) { return ((struct ctx *) sd)->trained > 0; }

int main(void)
{
    struct ctx ca, cb;
    int fail = 0;

    /* TEST 1: V.21 HDLC control frame A -> B */
    {
        nf_fax_t *a = make(&ca, "A", 1), *b = make(&cb, "B", 0);
        nf_fax_set_rx_type(b, NF_MODEM_V21, 300, 0, 1);
        nf_fax_set_tx_type(a, NF_MODEM_V21, 300, 0, 1);
        uint8_t dis[] = { 0xFF, 0x13, 0x80, 0x00, 0xEE, 0xF8 };  /* DIS-like */
        nf_fax_send_hdlc(a, dis, sizeof dis);
        int blk = pump(a, b, 500, got_frame, &cb);
        int ok = cb.frames == 1 && cb.frame_ok && cb.frame_len == (int) sizeof dis
                 && memcmp(cb.frame, dis, sizeof dis) == 0;
        printf("TEST1 V.21 HDLC frame: frames=%d ok=%d len=%d match=%d (%d blocks) -> %s\n",
               cb.frames, cb.frame_ok, cb.frame_len,
               cb.frames ? (memcmp(cb.frame, dis, sizeof dis) == 0) : 0, blk,
               ok ? "PASS" : "FAIL");
        if (!ok) fail = 1;
        nf_fax_free(a); nf_fax_free(b);
    }

    /* TEST 2: V.17 9600 train A -> B (TCF-like zeros, then end) */
    {
        nf_fax_t *a = make(&ca, "A", 1), *b = make(&cb, "B", 0);
        ca.tx_bits_left = 9600 * 2;                 /* ~2 s of zeros at 9600 bps */
        nf_fax_set_rx_type(b, NF_MODEM_V17, 9600, 0, 0);
        nf_fax_set_tx_type(a, NF_MODEM_V17, 9600, 0, 0);
        int blk = pump(a, b, 600, got_train, &cb);
        /* let a few more blocks flow so carrier-down is seen after the data ends */
        pump(a, b, 200, NULL, NULL);
        int ok = cb.trained >= 1 && cb.data_bits > 1000;
        printf("TEST2 V.17 train: trained=%d data_bits=%d carrier_down=%d (%d blocks) -> %s\n",
               cb.trained, cb.data_bits, cb.carrier_down, blk, ok ? "PASS" : "FAIL");
        if (!ok) fail = 1;
        nf_fax_free(a); nf_fax_free(b);
    }

    printf("%s\n", fail ? "FAIL" : "ALL PASS");
    return fail ? 1 : 0;
}
