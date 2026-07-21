#define _GNU_SOURCE
#include "nf_fax.h"
#include "nf_dsp.h"
#include "nf_hdlc.h"
#include "nf_v21.h"
#include "nf_v29.h"
#include "nf_v27.h"
#include "nf_v17.h"
#include "nf_v8.h"
#include "nf_v34.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int nffax_dbg(void) { static int d = -1; return nf_cached_env_flag(&d, "NFFAXDBG"); }
#define DBG(...) do { if (nffax_dbg()) fprintf(stderr, "  <fax> " __VA_ARGS__); } while (0)

/*
 * The fax driver. Fills the role of spandsp's fax.c + fax_modems.c: it owns
 * the V-series modems (nf_v17/nf_v29/nf_v27/nf_v21), HDLC and tones - all our
 * own code now - switches the active modem when the protocol layer asks,
 * routes modem bits to/from HDLC or the non-ECM image path, and translates
 * modem status into NF_STATUS_* events. No spandsp anywhere below here.
 */

#define NF_MS(ms) ((ms) * 8)            /* 8 samples per ms at 8 kHz */

typedef int (*nf_rx_fn)(void *user, const int16_t *amp, int len);
typedef int (*nf_tx_fn)(void *user, int16_t *amp, int max_len);

/* ── own tone / silence generators (no spandsp) ─────────────────────── */

/* Silence source. Additive set (like spandsp's silence_gen_alter): pending
 * silence accumulates; the handler returns short once drained, which is how
 * nf_fax_tx detects the end of a tx step. */
struct nf_silence {
    int remaining;
};

static void nf_silence_alter(struct nf_silence *s, int samples)
{
    s->remaining += samples;
    if (s->remaining < 0) s->remaining = 0;
}

static int nf_silence_tx(void *user, int16_t *amp, int max_len)
{
    struct nf_silence *s = user;
    int len = max_len;
    if (len >= s->remaining) len = s->remaining;
    s->remaining -= len;
    memset(amp, 0, (size_t) len * sizeof(int16_t));
    return len;
}

/* CED / CNG fax tones (T.30: both at -11 dBm0).
 * CED: 200 ms silence then 2.6 s of 2100 Hz, then the source ends (one-shot).
 * CNG: 0.5 s of 1100 Hz + 3 s of silence, repeating forever (T.30 moves on
 * by protocol timer, not by tone completion). */
struct nf_tonegen {
    int cng;                   /* 1 = CNG cadence, 0 = CED one-shot */
    uint32_t phase;
    int32_t rate;
    float level;
    int timer;                 /* CED: samples left; CNG: cadence countdown */
};

static void nf_tonegen_init(struct nf_tonegen *t, int cng)
{
    t->cng = cng;
    t->phase = 0;
    t->rate = nf_dds_phase_rate(cng ? 1100.0 : 2100.0);
    t->level = nf_dbm0_scaling(-11);
    t->timer = cng ? NF_MS(500 + 3000) : NF_MS(200 + 2600);
}

static int nf_tone_tx(void *user, int16_t *amp, int max_len)
{
    struct nf_tonegen *t = user;
    int i = 0;

    if (t->cng) {
        /* timer counts down from 3.5 s; tone while in the first 500 ms */
        for (i = 0; i < max_len; i++) {
            amp[i] = (t->timer > NF_MS(3000))
                   ? nf_dds_mod(&t->phase, t->rate, t->level) : 0;
            if (--t->timer <= 0) t->timer = NF_MS(500 + 3000);
        }
        return max_len;
    }
    /* CED: leading silence while timer is still above the tone duration */
    int len = max_len;
    if (len >= t->timer) len = t->timer;
    if (t->timer > NF_MS(2600)) {
        i = t->timer - NF_MS(2600);
        if (i > len) i = len;
        memset(amp, 0, (size_t) i * sizeof(int16_t));
    }
    for (; i < len; i++)
        amp[i] = nf_dds_mod(&t->phase, t->rate, t->level);
    t->timer -= len;
    return len;
}

enum {                                  /* rx dispatch mode */
    RX_NONE = 0, RX_V21, RX_FAST_PARALLEL, RX_FAST_ONLY, RX_V21_ONLY,
    RX_FAST_AND_V21,     /* ECM image: fast modem (HDLC) + V.21 (own HDLC) parallel */
    RX_V8,               /* T.30 Annex F handshake (full duplex, see nf_v8.h)       */
    RX_V34               /* V.34 half-duplex session (see nf_v34.h)                 */
};

struct nf_fax {
    int calling;
    nf_fax_iface_t up;

    nf_v21_tx_t v21_tx;
    nf_v21_rx_t v21_rx;
    nf_v17_tx_t v17_tx;         nf_v17_rx_t v17_rx;
    nf_v29_tx_t v29_tx;         nf_v29_rx_t v29_rx;
    nf_v27_tx_t v27_tx;         nf_v27_rx_t v27_rx;
    nf_hdlc_tx_t hdlc_tx;
    nf_hdlc_rx_t hdlc_rx;        /* fast-modem HDLC (ECM FCD/RCP frames)        */
    nf_hdlc_rx_t hdlc_rx_v21;    /* V.21's own HDLC, so it can run in parallel  */
    struct nf_tonegen tone;
    struct nf_silence silence;
    nf_v8_t v8;
    nf_v34_sess_t *v34;             /* V.34 half-duplex session, lazily made  */
    uint32_t v8_our_modulations;    /* offered, set by nf_fax_set_v8_caps()   */
    int32_t  v8_our_call_function;
    uint32_t v8_result_modulations; /* negotiated, valid once V.8 resolves    */
    int32_t  v8_result_call_function;

    /* rx state */
    int rx_mode;
    /* last ~1 s of raw rx audio, kept so a freshly started V.34 session can
     * be primed with the V.8 tail it would otherwise never have heard (the
     * answerer's single INFO0a can start before our CJ hold ends) */
    int16_t rx_hist[8000];
    int rx_hist_n;
    int fast_rx_type;          /* NF_MODEM_V17/V29/V27TER while parallel/fast */
    int fast_bit_rate;         /* rate/train of the active fast rx (for re-acquire) */
    int fast_short_train;
    int rx_frame_received;     /* a V.21 HDLC frame arrived (parallel switch)  */
    int v21_frame_received;    /* a V.21 HDLC frame arrived (ECM parallel)     */

    /* tx state (silence -> modem chaining, mirrors fax.c) */
    nf_tx_fn tx_handler;       void *tx_user;
    nf_tx_fn next_tx_handler;  void *next_tx_user;
    int transmit;
    int transmit_on_idle;
    int current_tx_type;
    int hdlc_stream;           /* ECM: pull frames from iface.hdlc_get_frame */
};

/* ── bit routing wrappers ──────────────────────────────────────────── */

static int nf_get_bit(void *user)
{
    nf_fax_t *s = user;
    int b = s->up.non_ecm_get_bit ? s->up.non_ecm_get_bit(s->up.user) : NF_GET_BIT_END;
    return (b == NF_GET_BIT_END) ? NF_SIG_END_OF_DATA : b;
}

static void nf_put_bit(void *user, int bit)
{
    nf_fax_t *s = user;
    if (bit < 0) return;                 /* status arrives via status handlers */
    if (s->up.non_ecm_put_bit) s->up.non_ecm_put_bit(s->up.user, bit);
}

static void emit_status(nf_fax_t *s, int nf_status)
{
    if (s->up.front_end_status) s->up.front_end_status(s->up.user, nf_status);
}

static int xlat_status(int sig)
{
    switch (sig) {
    case NF_SIG_CARRIER_UP:         return NF_STATUS_CARRIER_UP;
    case NF_SIG_CARRIER_DOWN:       return NF_STATUS_CARRIER_DOWN;
    case NF_SIG_TRAINING_SUCCEEDED: return NF_STATUS_TRAINING_SUCCEEDED;
    case NF_SIG_TRAINING_FAILED:    return NF_STATUS_TRAINING_FAILED;
    case NF_SIG_SEND_COMPLETE:      return NF_STATUS_SEND_STEP_COMPLETE;
    default:                        return 0;
    }
}

/* ── HDLC handlers ─────────────────────────────────────────────────── */

static void hdlc_frame_handler(void *user, const uint8_t *msg, int len, int ok)
{
    nf_fax_t *s = user;
    if (len < 0) {                       /* a status report (msg NULL), not a frame */
        int nf = xlat_status(len);
        if (nf) emit_status(s, nf);
        return;
    }
    /* Only a GOOD frame means the far end is really on V.21 - so only then do we
     * abandon a parallel fast modem for V.21. A bad-CRC "frame" is just the V.21
     * demod chewing on a fast-modem (e.g. TCF) signal, especially over A-law. */
    if (ok) s->rx_frame_received = 1;
    if (s->up.hdlc_accept) s->up.hdlc_accept(s->up.user, msg, len, ok);
}

/* V.21's HDLC receiver (separate from the fast modem's). Distinguished so the
 * ECM parallel mode can tell a real V.21 control frame (e.g. a PPS) from the
 * fast modem's FCD frames and hand the line over to V.21 when one arrives. */
static void hdlc_v21_frame_handler(void *user, const uint8_t *msg, int len, int ok)
{
    nf_fax_t *s = user;
    if (len < 0) {                       /* a status report (msg NULL), not a frame */
        int nf = xlat_status(len);
        if (nf) emit_status(s, nf);
        return;
    }
    if (ok) { s->rx_frame_received = 1; s->v21_frame_received = 1; }
    if (s->up.hdlc_accept) s->up.hdlc_accept(s->up.user, msg, len, ok);
}

static void hdlc_underflow_handler(void *user)
{
    nf_fax_t *s = user;
    /* For a single V.21 control frame (hdlc_stream == 0) this is a no-op: the
     * buffer is empty but the closing flags are still being sent. The burst ends
     * cleanly when tx_end yields END_OF_DATA and the modem shuts down; the tx pump
     * then reports NF_STATUS_SEND_STEP_COMPLETE. Acting here would cut the closing
     * flags and the far end would drop the frame.
     *
     * For an ECM burst (hdlc_stream) we pull the next frame to keep the high-speed
     * HDLC carrier busy with back-to-back frames, and end the burst when the
     * protocol layer has no more frames. */
    if (!s->hdlc_stream || !s->up.hdlc_get_frame) return;
    uint8_t buf[4 + 256 + 8];
    int n = s->up.hdlc_get_frame(s->up.user, buf, (int) sizeof buf);
    if (n > 0) {
        nf_hdlc_tx_frame(&s->hdlc_tx, buf, n);
    } else {
        s->hdlc_stream = 0;
        nf_hdlc_tx_frame(&s->hdlc_tx, buf, 0);   /* mark tx_end -> carrier drops */
    }
}

/* ── modem status handlers ─────────────────────────────────────────── */

/* Re-arm the current fast receive modem for another training attempt, keeping
 * its rate/train. Used to re-acquire a continuing high-speed carrier whose
 * training did not take (a gateway re-presenting the image carrier). */
static void restart_fast_rx(nf_fax_t *s)
{
    switch (s->fast_rx_type) {
    case NF_MODEM_V17: nf_v17_rx_restart(&s->v17_rx, s->fast_bit_rate, s->fast_short_train); break;
    case NF_MODEM_V29: nf_v29_rx_restart(&s->v29_rx, s->fast_bit_rate); break;
    case NF_MODEM_V27TER: nf_v27_rx_restart(&s->v27_rx, s->fast_bit_rate); break;
    }
}

static void fast_rx_status(void *user, int status)
{
    nf_fax_t *s = user;
    if (status == NF_SIG_TRAINING_SUCCEEDED) {
        s->rx_mode = RX_FAST_ONLY;       /* drop V.21, listen to the fast modem */
        emit_status(s, NF_STATUS_TRAINING_SUCCEEDED);
        return;
    }
    /* During ECM parallel receive, a fast-modem training failure on a still-
     * present carrier means the sender re-presented the image carrier and our
     * short retrain didn't take. Re-arm the fast modem at once to try the next
     * preamble; V.21 keeps watching for a PPS in parallel. The failure is not
     * forwarded to the protocol layer (the modem owns re-acquisition). */
    if (s->rx_mode == RX_FAST_AND_V21 && status == NF_SIG_TRAINING_FAILED) {
        restart_fast_rx(s);
        return;
    }
    /* Only forward further events once the fast modem actually owns the line;
     * while still running in parallel its TRAINING_FAILED/carrier noise is just
     * it failing to lock onto a V.21 control carrier and must be ignored. */
    if (s->rx_mode == RX_FAST_ONLY) {
        int nf = xlat_status(status);
        if (nf) emit_status(s, nf);
    }
}

static void v21_rx_status(void *user, int status)
{
    /* Empty, as in spandsp: control-phase progress is driven by received frames
     * and the protocol timers, not by V.21 carrier transitions. */
    nf_fax_t *s = user;
    DBG("%c v21 rx status=%d\n", s->calling ? 'C' : 'A', status);
}

/* T.30 Annex F handshake result: reuses the TRAINING_SUCCEEDED/FAILED status
 * events (V.8 negotiated / didn't) so nf_t30 can react through the same path
 * it already uses for every other modem's training outcome. */
static void v8_status(void *user, int status, const nf_v8_result_t *r)
{
    nf_fax_t *s = user;
    s->v8_result_modulations = r->modulations;
    s->v8_result_call_function = r->call_function;
    DBG("%c v8 status=%d cf=%d mod=0x%04X answer-tone=%s\n", s->calling ? 'C' : 'A',
        status, r->call_function, r->modulations,
        r->ansam_am == 1 ? "ANSam(AM,V.8 offered)" :
        r->ansam_am == 0 ? "plain-CED(no-AM,no-V.8)" : "n/a");
    emit_status(s, status == NF_V8_STATUS_V8_CALL
                   ? NF_STATUS_TRAINING_SUCCEEDED : NF_STATUS_TRAINING_FAILED);
}

/* ── V.34 half-duplex session glue (T.30 Annex F) ──────────────────────
 * The session engine (nf_v34.c) reports NF_SIG_* statuses and delivers
 * FCS-checked HDLC frames from both the control and primary channels; both
 * are translated onto the same up-interface every other modem uses. */

static void v34_status_handler(void *user, int status)
{
    nf_fax_t *s = user;
    int nf = xlat_status(status);

    DBG("%c v34 status=%d\n", s->calling ? 'C' : 'A', status);
    if (nf) emit_status(s, nf);
}

static void v34_frame_handler(void *user, const uint8_t *msg, int len, int ok)
{
    nf_fax_t *s = user;

    if (len >= 0 && ok) s->rx_frame_received = 1;
    if (len >= 0 && s->up.hdlc_accept) s->up.hdlc_accept(s->up.user, msg, len, ok);
}

static int v34_get_frame_shim(void *user, uint8_t *buf, int maxlen)
{
    nf_fax_t *s = user;

    return s->up.hdlc_get_frame ? s->up.hdlc_get_frame(s->up.user, buf, maxlen) : 0;
}

static nf_v34_sess_t *ensure_v34(nf_fax_t *s)
{
    if (!s->v34)
        s->v34 = nf_v34_sess_alloc(s->calling, v34_status_handler,
                                   v34_frame_handler, s);
    return s->v34;
}

static int v34_sub_mode(int bit_rate)
{
    /* nf_t30 selects the channel, not the rate: anything above the 2400
     * bit/s control-channel ceiling means "primary channel" (the actual
     * primary rate is negotiated inside the session via MPh - see
     * nf_fax_v34_bit_rate) */
    if (bit_rate > 2400)  return NF_V34_SESS_PRI;
    if (bit_rate > 0)     return NF_V34_SESS_CC;
    return NF_V34_SESS_STARTUP;
}

/* ── construction ──────────────────────────────────────────────────── */

nf_fax_t *nf_fax_init(int calling_party, const nf_fax_iface_t *iface)
{
    nf_fax_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->calling = calling_party;
    s->up = *iface;

    nf_hdlc_tx_init(&s->hdlc_tx, 2, hdlc_underflow_handler, s);
    nf_hdlc_rx_init(&s->hdlc_rx, 5, hdlc_frame_handler, s);
    nf_hdlc_rx_init(&s->hdlc_rx_v21, 5, hdlc_v21_frame_handler, s);

    nf_v21_tx_init(&s->v21_tx, (int (*)(void *)) nf_hdlc_tx_get_bit, &s->hdlc_tx);
    nf_v21_rx_init(&s->v21_rx, (void (*)(void *, int)) nf_hdlc_rx_put_bit, &s->hdlc_rx_v21);

    nf_v17_tx_init(&s->v17_tx, 14400, nf_get_bit, s);
    nf_v17_rx_init(&s->v17_rx, 14400, nf_put_bit, s);
    nf_v29_tx_init(&s->v29_tx, 9600, nf_get_bit, s);
    nf_v29_rx_init(&s->v29_rx, 9600, nf_put_bit, s);
    nf_v27_tx_init(&s->v27_tx, 4800, nf_get_bit, s);
    nf_v27_rx_init(&s->v27_rx, 4800, nf_put_bit, s);

    nf_tonegen_init(&s->tone, 1);
    s->silence.remaining = 0;

    /* status handlers (data put_bit stays the image/HDLC sink) */
    nf_v17_rx_set_status_handler(&s->v17_rx, fast_rx_status, s);
    nf_v29_rx_set_status_handler(&s->v29_rx, fast_rx_status, s);
    nf_v27_rx_set_status_handler(&s->v27_rx, fast_rx_status, s);
    nf_v21_rx_set_status_handler(&s->v21_rx, v21_rx_status, s);

    s->rx_mode = RX_NONE;
    s->tx_handler = nf_silence_tx;
    s->tx_user = &s->silence;
    s->transmit = 0;
    return s;
}

void nf_fax_free(nf_fax_t *s)
{
    if (!s) return;
    nf_v34_sess_free(s->v34);
    free(s);
}

void nf_fax_set_transmit_on_idle(nf_fax_t *s, int on) { s->transmit_on_idle = on; }

void nf_fax_set_v8_caps(nf_fax_t *s, uint32_t modulations, int32_t call_function)
{
    s->v8_our_modulations = modulations;
    s->v8_our_call_function = call_function;
}
uint32_t nf_fax_v8_modulations(const nf_fax_t *s)   { return s->v8_result_modulations; }
int32_t  nf_fax_v8_call_function(const nf_fax_t *s) { return s->v8_result_call_function; }

int nf_fax_v34_bit_rate(const nf_fax_t *s)
{
    return s->v34 ? nf_v34_sess_data_rate(s->v34) : 0;
}

void nf_fax_send_hdlc(nf_fax_t *s, const uint8_t *msg, int len)
{
    if (s->current_tx_type == NF_MODEM_V34 && s->v34) {
        nf_v34_sess_queue_frame(s->v34, msg, len);      /* len < 0 clears */
        return;
    }
    if (len < 0) { nf_hdlc_tx_restart(&s->hdlc_tx); return; }
    nf_hdlc_tx_frame(&s->hdlc_tx, msg, len);
    /* Mark end-of-transmission so the carrier ends cleanly after the closing
     * flags (we send one control frame per burst). */
    nf_hdlc_tx_frame(&s->hdlc_tx, msg, 0);
}

void nf_fax_begin_hdlc_stream(nf_fax_t *s)
{
    if (s->current_tx_type == NF_MODEM_V34 && s->v34) {
        /* the session pulls the whole block on its next tx pump (after the
         * protocol layer's state is in place) and plays it as one burst */
        nf_v34_sess_begin_stream(s->v34, v34_get_frame_shim, s);
        return;
    }
    /* The current modem was just set up with use_hdlc; its flag preamble will
     * trigger the first underflow, which pulls frame 0, and so on. */
    s->hdlc_stream = 1;
}

/* ── rx ────────────────────────────────────────────────────────────── */

void nf_fax_set_rx_type(nf_fax_t *s, int type, int bit_rate, int short_train, int use_hdlc)
{
    DBG("%c set_rx_type type=%d rate=%d short=%d hdlc=%d\n", s->calling?'C':'A', type, bit_rate, short_train, use_hdlc);
    void (*put)(void *, int) = use_hdlc
        ? (void (*)(void *, int)) nf_hdlc_rx_put_bit : nf_put_bit;
    void *put_user = use_hdlc ? (void *) &s->hdlc_rx : (void *) s;

    if (use_hdlc) {
        nf_hdlc_rx_init(&s->hdlc_rx, 5, hdlc_frame_handler, s);
        /* ECM image: also run V.21 in parallel on its OWN HDLC receiver, so a
         * post-message control frame (a PPS) is caught even while we stay armed
         * for the high-speed image carrier. A standard sender that PPS-signals a
         * block the fast modem could not train on is then handled by PPR; a
         * gateway that simply re-presents the image carrier is caught by the fast
         * modem. (The two HDLC receivers are separate, so FCD and PPS never mix.) */
        nf_hdlc_rx_init(&s->hdlc_rx_v21, 5, hdlc_v21_frame_handler, s);
        nf_v21_rx_init(&s->v21_rx, (void (*)(void *, int)) nf_hdlc_rx_put_bit, &s->hdlc_rx_v21);
        nf_v21_rx_set_status_handler(&s->v21_rx, v21_rx_status, s);
    }
    s->rx_frame_received = 0;
    s->v21_frame_received = 0;
    s->fast_bit_rate = bit_rate;
    s->fast_short_train = short_train;

    switch (type) {
    case NF_MODEM_V21:
        nf_v21_rx_init(&s->v21_rx, (void (*)(void *, int)) nf_hdlc_rx_put_bit, &s->hdlc_rx_v21);
        nf_v21_rx_set_status_handler(&s->v21_rx, v21_rx_status, s);
        s->rx_mode = RX_V21;
        break;
    /* For ECM the image arrives as HDLC over the fast modem; V.21 runs alongside
     * it on its own HDLC receiver (RX_FAST_AND_V21). Non-ECM image keeps the
     * existing parallel V.21 watch. */
    case NF_MODEM_V27TER:
        nf_v27_rx_restart(&s->v27_rx, bit_rate);
        nf_v27_rx_set_put_bit(&s->v27_rx, put, put_user);
        nf_v27_rx_set_status_handler(&s->v27_rx, fast_rx_status, s);
        s->fast_rx_type = type; s->rx_mode = use_hdlc ? RX_FAST_AND_V21 : RX_FAST_PARALLEL;
        break;
    case NF_MODEM_V29:
        nf_v29_rx_restart(&s->v29_rx, bit_rate);
        nf_v29_rx_set_put_bit(&s->v29_rx, put, put_user);
        nf_v29_rx_set_status_handler(&s->v29_rx, fast_rx_status, s);
        s->fast_rx_type = type; s->rx_mode = use_hdlc ? RX_FAST_AND_V21 : RX_FAST_PARALLEL;
        break;
    case NF_MODEM_V17:
        nf_v17_rx_restart(&s->v17_rx, bit_rate, short_train);
        nf_v17_rx_set_put_bit(&s->v17_rx, put, put_user);
        nf_v17_rx_set_status_handler(&s->v17_rx, fast_rx_status, s);
        s->fast_rx_type = type; s->rx_mode = use_hdlc ? RX_FAST_AND_V21 : RX_FAST_PARALLEL;
        break;
    case NF_MODEM_V8:
        /* nf_v8_init() itself happens in nf_fax_set_tx_type() (also always
         * called for this phase, and it owns the one nf_v8_t engine); this
         * just points the rx dispatch at it. */
        s->rx_mode = RX_V8;
        break;
    case NF_MODEM_V34:
        ensure_v34(s);
        if (s->v34) {
            nf_v34_sess_set_rx_mode(s->v34, v34_sub_mode(bit_rate));
            /* new session: replay the rx history (the V.8 tail) into the
             * Phase-2 receiver; a no-op at any later set_rx_type */
            nf_v34_sess_rx_prime(s->v34, s->rx_hist, s->rx_hist_n);
            s->rx_mode = RX_V34;
        }
        break;
    default:
        s->rx_mode = RX_NONE;
        break;
    }
}

int nf_fax_rx(nf_fax_t *s, const int16_t *amp, int len)
{
    /* keep the rolling rx history current BEFORE dispatching, so a mode
     * switch triggered from inside a handler (V.8 completing mid-chunk)
     * can prime the V.34 session with everything up to and including the
     * present chunk */
    if (len > 0) {
        int cap = (int) (sizeof(s->rx_hist) / sizeof(s->rx_hist[0]));
        int n = len > cap ? cap : len;
        if (s->rx_hist_n + n > cap) {
            int keep = cap - n;
            memmove(s->rx_hist, s->rx_hist + s->rx_hist_n - keep,
                    (size_t) keep * sizeof(int16_t));
            s->rx_hist_n = keep;
        }
        memcpy(s->rx_hist + s->rx_hist_n, amp + (len - n),
               (size_t) n * sizeof(int16_t));
        s->rx_hist_n += n;
    }

    switch (s->rx_mode) {
    case RX_V21:
    case RX_V21_ONLY:
        nf_v21_rx(&s->v21_rx, amp, len);
        break;
    case RX_FAST_PARALLEL:
        switch (s->fast_rx_type) {
        case NF_MODEM_V17: nf_v17_rx(&s->v17_rx, amp, len); break;
        case NF_MODEM_V29: nf_v29_rx(&s->v29_rx, amp, len); break;
        case NF_MODEM_V27TER: nf_v27_rx(&s->v27_rx, amp, len); break;
        }
        nf_v21_rx(&s->v21_rx, amp, len);
        if (s->rx_frame_received) s->rx_mode = RX_V21_ONLY;   /* got a V.21 frame */
        break;
    case RX_FAST_AND_V21:
        switch (s->fast_rx_type) {
        case NF_MODEM_V17: nf_v17_rx(&s->v17_rx, amp, len); break;
        case NF_MODEM_V29: nf_v29_rx(&s->v29_rx, amp, len); break;
        case NF_MODEM_V27TER: nf_v27_rx(&s->v27_rx, amp, len); break;
        }
        nf_v21_rx(&s->v21_rx, amp, len);
        /* A V.21 control frame (PPS) means the block transmission ended without
         * the fast modem training; hand the line to V.21 so the protocol can
         * PPR. (Fast training success instead switches us to RX_FAST_ONLY via
         * fast_rx_status.) */
        if (s->v21_frame_received) s->rx_mode = RX_V21_ONLY;
        break;
    case RX_FAST_ONLY:
        switch (s->fast_rx_type) {
        case NF_MODEM_V17: nf_v17_rx(&s->v17_rx, amp, len); break;
        case NF_MODEM_V29: nf_v29_rx(&s->v29_rx, amp, len); break;
        case NF_MODEM_V27TER: nf_v27_rx(&s->v27_rx, amp, len); break;
        }
        break;
    case RX_V8:
        nf_v8_rx(&s->v8, amp, len);
        break;
    case RX_V34:
        nf_v34_sess_rx(s->v34, amp, len);
        break;
    default:
        break;
    }
    if (s->up.timer_update) s->up.timer_update(s->up.user, len);
    return 0;
}

/* ── tx ────────────────────────────────────────────────────────────── */

static void set_tx(nf_fax_t *s, nf_tx_fn h, void *u) { s->tx_handler = h; s->tx_user = u; }
static void set_next_tx(nf_fax_t *s, nf_tx_fn h, void *u) { s->next_tx_handler = h; s->next_tx_user = u; }

void nf_fax_set_tx_type(nf_fax_t *s, int type, int bit_rate, int short_train, int use_hdlc)
{
    DBG("%c set_tx_type type=%d rate=%d short=%d hdlc=%d\n", s->calling?'C':'A', type, bit_rate, short_train, use_hdlc);
    int (*get)(void *) = use_hdlc
        ? (int (*)(void *)) nf_hdlc_tx_get_bit : nf_get_bit;
    void *get_user = use_hdlc ? (void *) &s->hdlc_tx : (void *) s;
    s->hdlc_stream = 0;            /* a fresh tx step; ECM re-arms via begin_hdlc_stream */

    switch (type) {
    case NF_MODEM_PAUSE:
        nf_silence_alter(&s->silence, NF_MS(short_train));
        set_tx(s, nf_silence_tx, &s->silence);
        set_next_tx(s, NULL, NULL);
        s->transmit = 1;
        break;
    case NF_MODEM_CED:
    case NF_MODEM_CNG:
        nf_tonegen_init(&s->tone, type == NF_MODEM_CNG);
        set_tx(s, nf_tone_tx, &s->tone);
        set_next_tx(s, NULL, NULL);
        s->transmit = 1;
        break;
    case NF_MODEM_V21:
        nf_v21_tx_init(&s->v21_tx, get, get_user);
        nf_hdlc_tx_flags(&s->hdlc_tx, 32);
        nf_silence_alter(&s->silence, NF_MS(75));
        set_tx(s, nf_silence_tx, &s->silence);
        set_next_tx(s, (nf_tx_fn) nf_v21_tx, &s->v21_tx);
        s->transmit = 1;
        break;
    case NF_MODEM_V27TER:
        nf_silence_alter(&s->silence, NF_MS(75));
        if (use_hdlc) nf_hdlc_tx_flags(&s->hdlc_tx, bit_rate / (8 * 5));  /* 200ms preamble */
        nf_v27_tx_restart(&s->v27_tx, bit_rate);
        nf_v27_tx_set_get_bit(&s->v27_tx, get, get_user);
        set_tx(s, nf_silence_tx, &s->silence);
        set_next_tx(s, (nf_tx_fn) nf_v27_tx, &s->v27_tx);
        s->transmit = 1;
        break;
    case NF_MODEM_V29:
        nf_silence_alter(&s->silence, NF_MS(75));
        if (use_hdlc) nf_hdlc_tx_flags(&s->hdlc_tx, bit_rate / (8 * 5));
        nf_v29_tx_restart(&s->v29_tx, bit_rate);
        nf_v29_tx_set_get_bit(&s->v29_tx, get, get_user);
        set_tx(s, nf_silence_tx, &s->silence);
        set_next_tx(s, (nf_tx_fn) nf_v29_tx, &s->v29_tx);
        s->transmit = 1;
        break;
    case NF_MODEM_V17:
        nf_silence_alter(&s->silence, NF_MS(75));
        if (use_hdlc) nf_hdlc_tx_flags(&s->hdlc_tx, bit_rate / (8 * 5));
        nf_v17_tx_restart(&s->v17_tx, bit_rate, short_train);
        nf_v17_tx_set_get_bit(&s->v17_tx, get, get_user);
        set_tx(s, nf_silence_tx, &s->silence);
        set_next_tx(s, (nf_tx_fn) nf_v17_tx, &s->v17_tx);
        s->transmit = 1;
        break;
    case NF_MODEM_V8:
        /* Full duplex, no finite "burst": nf_v8_tx() always fills the buffer
         * (silence, ANSam, or FSK bits) and only ever stops being pumped when
         * nf_t30 switches to a different tx type on TRAINING_SUCCEEDED/FAILED
         * (see v8_status()) - same non-terminating shape as CED/CNG above. */
        nf_v8_init(&s->v8, s->calling, s->v8_our_modulations, s->v8_our_call_function,
                  v8_status, s);
        set_tx(s, (nf_tx_fn) nf_v8_tx, &s->v8);
        set_next_tx(s, NULL, NULL);
        s->transmit = 1;
        break;
    case NF_MODEM_V34:
        /* bit_rate 0 = clause-12 startup (infinite generator, like V.8);
         * 1200 = one control-channel burst; 24000 = one primary-channel
         * burst. The finite bursts return short when played out, which is
         * what yields NF_STATUS_SEND_STEP_COMPLETE through the generic tx
         * pump below. */
        ensure_v34(s);
        if (s->v34) {
            nf_v34_sess_set_tx_mode(s->v34, v34_sub_mode(bit_rate));
            set_tx(s, (nf_tx_fn) nf_v34_sess_tx, s->v34);
            set_next_tx(s, NULL, NULL);
            s->transmit = 1;
        }
        break;
    default:                            /* NONE / DONE */
        set_tx(s, nf_silence_tx, &s->silence);
        set_next_tx(s, NULL, NULL);
        s->transmit = 0;
        break;
    }
    s->current_tx_type = type;
}

/* Advance to the chained next tx handler (silence->modem). Returns 0 if it
 * switched, -1 if the current step is fully done. */
static int advance_tx(nf_fax_t *s)
{
    if (s->next_tx_handler) {
        set_tx(s, s->next_tx_handler, s->next_tx_user);
        set_next_tx(s, NULL, NULL);
        return 0;
    }
    set_tx(s, nf_silence_tx, &s->silence);
    set_next_tx(s, NULL, NULL);
    s->transmit = 0;
    return -1;
}

int nf_fax_tx(nf_fax_t *s, int16_t *amp, int max_len)
{
    int len = 0;
    if (s->transmit) {
        while ((len += s->tx_handler(s->tx_user, amp + len, max_len - len)) < max_len) {
            if (advance_tx(s) < 0 && s->current_tx_type != NF_MODEM_NONE
                                  && s->current_tx_type != NF_MODEM_DONE)
                emit_status(s, NF_STATUS_SEND_STEP_COMPLETE);
            if (!s->transmit) {
                if (s->transmit_on_idle) {
                    memset(amp + len, 0, (size_t) (max_len - len) * sizeof(int16_t));
                    len = max_len;
                }
                break;
            }
        }
    } else if (s->transmit_on_idle) {
        memset(amp, 0, (size_t) max_len * sizeof(int16_t));
        len = max_len;
    }
    return len;
}

/* ── Backend vtable (nf_modem_ops_t) ─────────────────────────────────────
 * Thin wrappers so nf_t30 can drive the audio backend through the generic
 * nf_modem_ops_t interface (void* handle). No behaviour change. */
static void be_set_rx(void *be, int t, int r, int s, int h) { nf_fax_set_rx_type(be, t, r, s, h); }
static void be_set_tx(void *be, int t, int r, int s, int h) { nf_fax_set_tx_type(be, t, r, s, h); }
static void be_send_hdlc(void *be, const uint8_t *m, int l)  { nf_fax_send_hdlc(be, m, l); }
static void be_begin_stream(void *be)                        { nf_fax_begin_hdlc_stream(be); }
static void be_idle(void *be, int on)                        { nf_fax_set_transmit_on_idle(be, on); }
static int  be_tx(void *be, int16_t *amp, int max)           { return nf_fax_tx(be, amp, max); }
static int  be_rx(void *be, const int16_t *amp, int len)     { return nf_fax_rx(be, amp, len); }
static void be_free(void *be)                                { nf_fax_free(be); }

static const nf_modem_ops_t NF_FAX_OPS = {
    be_set_rx, be_set_tx, be_send_hdlc, be_begin_stream, be_idle, be_tx, be_rx, be_free
};

const nf_modem_ops_t *nf_fax_ops(void) { return &NF_FAX_OPS; }
