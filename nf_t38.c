#include "nf_t38.h"
#include "nf_udptl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Wire-level tracing of IFP indicators/fields, gated by NF_T38_DBG=1. */
static int t38dbg(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NF_T38_DBG"); v = (e && *e) ? 1 : 0; }
    return v;
}
static const char *ind_name(int i)
{
    static const char *n[] = {"NO_SIGNAL","CNG","CED","V21",
        "V27_2400","V27_4800","V29_7200","V29_9600",
        "V17_7200S","V17_7200L","V17_9600S","V17_9600L",
        "V17_12000S","V17_12000L","V17_14400S","V17_14400L"};
    return (i >= 0 && i < 16) ? n[i] : "?";
}
static const char *ft_name(int f)
{
    static const char *n[] = {"HDLC_DATA","HDLC_SIG_END","FCS_OK","FCS_BAD",
        "FCS_OK_SIG_END","FCS_BAD_SIG_END","NONECM_DATA","NONECM_SIG_END"};
    return (f >= 0 && f < 8) ? n[f] : "?";
}

/* ── T.38 IFP constants (version 0) ──────────────────────────────────────── */

enum {   /* indicators (T38_IND_*) */
    IND_NO_SIGNAL = 0, IND_CNG, IND_CED, IND_V21_PREAMBLE,
    IND_V27_2400_TRN, IND_V27_4800_TRN, IND_V29_7200_TRN, IND_V29_9600_TRN,
    IND_V17_7200_SHORT, IND_V17_7200_LONG, IND_V17_9600_SHORT, IND_V17_9600_LONG,
    IND_V17_12000_SHORT, IND_V17_12000_LONG, IND_V17_14400_SHORT, IND_V17_14400_LONG
};
enum {   /* data types (T38_DATA_*) */
    DT_V21 = 0, DT_V27_2400, DT_V27_4800, DT_V29_7200, DT_V29_9600,
    DT_V17_7200, DT_V17_9600, DT_V17_12000, DT_V17_14400
};
enum {   /* field types (T38_FIELD_*) */
    FT_HDLC_DATA = 0, FT_HDLC_SIG_END, FT_HDLC_FCS_OK, FT_HDLC_FCS_BAD,
    FT_HDLC_FCS_OK_SIG_END, FT_HDLC_FCS_BAD_SIG_END,
    FT_T4_NON_ECM_DATA, FT_T4_NON_ECM_SIG_END
};

/* tx burst phases */
enum {
    TX_IDLE = 0, TX_IND, TX_IND_WAIT, TX_HDLC_DATA, TX_HDLC_FCS, TX_HDLC_DRAIN,
    TX_HDLC_END, TX_NONECM, TX_NONECM_TRAIL, TX_NONECM_END,
    TX_TONE_WAIT, TX_PAUSE_WAIT, TX_COMPLETE
};

/* Delay (ms) to allow after an indicator before sending data, so a T.38 gateway
 * can play out the modem training / HDLC flag preamble to a real fax machine.
 * Values approximate the T.38 modem startup times. */
static int indicator_delay(int ind)
{
    switch (ind) {
    case IND_V21_PREAMBLE:    return 200;
    case IND_V27_2400_TRN:    return 900;
    case IND_V27_4800_TRN:    return 700;
    case IND_V29_7200_TRN:
    case IND_V29_9600_TRN:    return 270;
    case IND_V17_7200_SHORT:  case IND_V17_9600_SHORT:
    case IND_V17_12000_SHORT: case IND_V17_14400_SHORT: return 300;
    case IND_V17_7200_LONG:   case IND_V17_9600_LONG:
    case IND_V17_12000_LONG:  case IND_V17_14400_LONG:  return 1400;
    default:                  return 0;
    }
}

struct nf_t38 {
    nf_fax_iface_t up;
    int calling;

    void (*send)(void *user, const uint8_t *dgram, int len);
    void *send_user;
    nf_udptl_t udptl;

    /* ── tx ── */
    int  tx_phase;
    int  tx_next_phase;     /* phase to enter after the post-indicator wait */
    int  tx_ind;            /* indicator to emit for the current burst   */
    int  tx_dt;             /* data type for the current burst           */
    int  tx_bit_rate;       /* for pacing non-ECM data                   */
    int  tx_is_hdlc;        /* current carrier carries HDLC              */
    int  tx_is_stream;      /* ECM: pull frames via up.hdlc_get_frame    */
    int  tx_wait_ms;        /* remaining delay for tone/pause phases     */
    uint8_t tx_frame[600];  /* current HDLC frame, bit-reversed for the wire */
    int  tx_frame_len;
    int  tx_frame_off;      /* paced-emission cursor into tx_frame        */
    int  tx_have_frame;     /* a single control frame is queued          */
    int  tx_trailer;        /* non-ECM: zero-pad octets still to send at end */
    long tx_burst_octets;   /* HDLC data octets emitted in the current burst */
    long tx_burst_ms;       /* wall-clock ms elapsed sending the current burst */
    int  tx_suppress;       /* test only: drop this burst's datagrams (lose a frame) */

    /* ── rx ── */
    uint8_t rx_hdlc[600];   /* HDLC frame reassembly                     */
    int  rx_hdlc_len;
    int  rx_carrier;        /* a fast carrier is currently up            */
    int  rx_is_fast;        /* current carrier is a fast (image/TCF) one */
};

/* HDLC frames are MSB-first byte values inside nf_t30 (e.g. DIS FCF = 0x80) but
 * travel LSB-first on the fax wire, which is the order T.38 HDLC_DATA carries —
 * so each HDLC octet is bit-reversed across the T.38 boundary (0x80 <-> 0x01).
 * (Non-ECM image bits come from on_get_bit already in transmission order, so
 * they only need LSB-first packing, handled inline below.) */
static uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t) (((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t) (((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t) (((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* ── IFP encode (version 0) ──────────────────────────────────────────────── */

static int ifp_indicator(uint8_t *buf, int ind)
{
    buf[0] = (uint8_t) (ind << 1);          /* bit6=0 (indicator), value in 6..1 */
    return 1;
}

/* One data field. payload NULL/0 for the no-data field types. */
static int ifp_data(uint8_t *buf, int dt, int ft, const uint8_t *payload, int plen)
{
    int len = 0;
    buf[len++] = (uint8_t) (0x80 | 0x40 | (dt << 1));   /* data-field-present | data | dt */
    buf[len++] = 1;                                       /* number of fields = 1          */
    buf[len++] = (uint8_t) (((plen > 0) ? 0x80 : 0) | (ft << 4));
    if (plen > 0) {
        buf[len++] = (uint8_t) ((plen - 1) >> 8);
        buf[len++] = (uint8_t) ((plen - 1) & 0xFF);
        memcpy(&buf[len], payload, (size_t) plen);
        len += plen;
    }
    return len;
}

static void emit(nf_t38_t *s, const uint8_t *ifp, int ifp_len)
{
    if (s->tx_suppress) return;     /* test: this whole burst is "lost" on the wire */
    uint8_t dg[NF_UDPTL_MAXDGRAM];
    int n = nf_udptl_build(&s->udptl, ifp, ifp_len, dg, sizeof(dg));
    if (n > 0 && s->send) s->send(s->send_user, dg, n);
}

static void emit_indicator(nf_t38_t *s, int ind)
{
    uint8_t ifp[4];
    if (t38dbg()) fprintf(stderr, "[T38 TX] IND %s\n", ind_name(ind));
    emit(s, ifp, ifp_indicator(ifp, ind));
}

static void emit_data(nf_t38_t *s, int ft, const uint8_t *p, int plen)
{
    uint8_t ifp[NF_UDPTL_MAXIFP];
    int n = ifp_data(ifp, s->tx_dt, ft, p, plen);
    if (t38dbg()) fprintf(stderr, "[T38 TX] %s len=%d\n", ft_name(ft), plen);
    emit(s, ifp, n);
}

/* Load an HDLC frame into the paced-emission buffer, bit-reversing each octet
 * (HDLC travels LSB-first; nf_t30 frames are MSB-first). The frame is then sent
 * in carrier-rate-sized chunks across pump ticks (see TX_HDLC_DATA), so a slow
 * carrier (V.21 = 300 bps) isn't asked to play a whole frame instantly. */
static void load_hdlc_frame(nf_t38_t *s, const uint8_t *frame, int len)
{
    if (len > (int) sizeof(s->tx_frame)) len = sizeof(s->tx_frame);
    for (int i = 0; i < len; i++) s->tx_frame[i] = bitrev8(frame[i]);
    s->tx_frame_len = len;
    s->tx_frame_off = 0;
    s->tx_burst_octets += len + 4;   /* +flag/FCS overhead the modem also plays */
}

/* ── modem -> indicator/data-type mapping ───────────────────────────────── */

static void map_modem(int type, int rate, int short_train, int *ind, int *dt)
{
    *ind = IND_NO_SIGNAL; *dt = DT_V21;
    switch (type) {
    case NF_MODEM_V21: *ind = IND_V21_PREAMBLE; *dt = DT_V21; break;
    case NF_MODEM_V27TER:
        if (rate >= 4800) { *ind = IND_V27_4800_TRN; *dt = DT_V27_4800; }
        else              { *ind = IND_V27_2400_TRN; *dt = DT_V27_2400; }
        break;
    case NF_MODEM_V29:
        if (rate >= 9600) { *ind = IND_V29_9600_TRN; *dt = DT_V29_9600; }
        else              { *ind = IND_V29_7200_TRN; *dt = DT_V29_7200; }
        break;
    case NF_MODEM_V17:
        /* Honour the negotiated training length: T.30 uses LONG for TCF (cold
         * receiver) and SHORT for image bursts after CFR (warm receiver). A
         * gateway re-modulates the indicator faithfully, so sending LONG where
         * SHORT was agreed corrupts the far end's image. */
        switch (rate) {
        case 14400: *ind = short_train ? IND_V17_14400_SHORT : IND_V17_14400_LONG; *dt = DT_V17_14400; break;
        case 12000: *ind = short_train ? IND_V17_12000_SHORT : IND_V17_12000_LONG; *dt = DT_V17_12000; break;
        case 9600:  *ind = short_train ? IND_V17_9600_SHORT  : IND_V17_9600_LONG;  *dt = DT_V17_9600;  break;
        default:    *ind = short_train ? IND_V17_7200_SHORT  : IND_V17_7200_LONG;  *dt = DT_V17_7200;  break;
        }
        break;
    case NF_MODEM_CED: *ind = IND_CED; break;
    case NF_MODEM_CNG: *ind = IND_CNG; break;
    default: break;     /* NONE / PAUSE / DONE -> NO_SIGNAL */
    }
}

/* ── down: nf_modem_ops_t implementation ─────────────────────────────────── */

static void t38_set_tx_type(void *be, int type, int rate, int short_train, int use_hdlc)
{
    nf_t38_t *s = be;
    int ind, dt;
    map_modem(type, rate, short_train, &ind, &dt);
    s->tx_ind = ind;
    s->tx_dt = dt;
    s->tx_bit_rate = rate > 0 ? rate : 14400;
    s->tx_is_hdlc = use_hdlc;
    s->tx_is_stream = 0;
    s->tx_have_frame = 0;
    s->tx_trailer = 0;
    s->tx_burst_octets = 0; s->tx_burst_ms = 0;
    s->tx_frame_len = 0; s->tx_frame_off = 0;

    switch (type) {
    case NF_MODEM_NONE:
    case NF_MODEM_DONE:
        /* No carrier: announce no-signal, no completion step expected. */
        s->tx_ind = IND_NO_SIGNAL;
        s->tx_phase = TX_IND;
        s->tx_is_hdlc = -1;             /* -> go idle after the indicator */
        break;
    case NF_MODEM_PAUSE:
        s->tx_wait_ms = rate > 0 ? rate : short_train;   /* ms in short_train arg */
        if (s->tx_wait_ms <= 0) s->tx_wait_ms = short_train > 0 ? short_train : 75;
        s->tx_phase = TX_PAUSE_WAIT;
        break;
    case NF_MODEM_CED:
    case NF_MODEM_CNG:
        s->tx_wait_ms = 200;
        s->tx_phase = TX_IND;           /* emit tone indicator, then wait+complete */
        s->tx_is_hdlc = -2;             /* tone marker */
        break;
    default:
        s->tx_phase = TX_IND;           /* emit training/preamble, then data */
        break;
    }
}

static void t38_set_rx_type(void *be, int type, int rate, int short_train, int use_hdlc)
{
    (void) rate; (void) short_train; (void) use_hdlc;
    nf_t38_t *s = be;
    /* Receiving is driven entirely by incoming IFP; just note we're listening.
     * Reset any half-assembled HDLC frame when switching rx mode. */
    if (type == NF_MODEM_NONE) s->rx_carrier = 0;
    s->rx_hdlc_len = 0;
}

static void t38_send_hdlc(void *be, const uint8_t *msg, int len)
{
    nf_t38_t *s = be;
    if (len < 0) return;                /* stream restart marker: not used here */
    /* Test hook: NF_T38_DROP_MCF=N drops the next N MCF responses on the wire
     * (state still advances), to exercise the sender's command retransmit and
     * the receiver's re-acknowledge recovery. */
    if (len >= 3 && (msg[2] & 0xFE) == 0x8C /* FCF_MCF */) {
        static int mcf_drop = -1;
        if (mcf_drop < 0) { const char *e = getenv("NF_T38_DROP_MCF"); mcf_drop = e ? atoi(e) : 0; }
        if (mcf_drop > 0) { mcf_drop--; s->tx_suppress = 1; }
    }
    load_hdlc_frame(s, msg, len);       /* reversed, ready for paced emission */
    s->tx_have_frame = 1;               /* one control frame for this burst */
}

static void t38_begin_hdlc_stream(void *be)
{
    nf_t38_t *s = be;
    s->tx_is_stream = 1;                /* ECM: pump pulls FCD frames */
}

/* T.38 has no transmit-on-idle notion (we only emit IFP when there's something to
 * send), so the audio-modem idle flag is a no-op here. */
static void t38_set_idle(void *be, int on) { (void) be; (void) on; }
static void t38_free(void *be)             { free(be); }

static const nf_modem_ops_t NF_T38_OPS = {
    t38_set_rx_type, t38_set_tx_type, t38_send_hdlc, t38_begin_hdlc_stream,
    t38_set_idle, NULL /*tx*/, NULL /*rx*/, t38_free
};

const nf_modem_ops_t *nf_t38_ops(void) { return &NF_T38_OPS; }

/* ── tx pump ─────────────────────────────────────────────────────────────── */

static void step_complete(nf_t38_t *s)
{
    s->tx_phase = TX_IDLE;
    s->tx_suppress = 0;                 /* end of any test-suppressed burst */
    if (s->up.front_end_status)
        s->up.front_end_status(s->up.user, NF_STATUS_SEND_STEP_COMPLETE);
}

void nf_t38_pump(nf_t38_t *s, int ms)
{
    if (ms <= 0) ms = 30;

    if (s->tx_phase == TX_HDLC_DATA || s->tx_phase == TX_HDLC_FCS ||
        s->tx_phase == TX_HDLC_DRAIN)
        s->tx_burst_ms += ms;

    switch (s->tx_phase) {
    case TX_IDLE:
        break;

    case TX_IND:
        emit_indicator(s, s->tx_ind);
        if (s->tx_is_hdlc == -1) {           /* NO_SIGNAL: nothing follows */
            s->tx_phase = TX_IDLE;
        } else if (s->tx_is_hdlc == -2) {    /* CED/CNG tone */
            s->tx_phase = TX_TONE_WAIT;
        } else {
            /* Decide what follows the indicator, but first allow the training /
             * flag-preamble time so a gateway can play it out. */
            s->tx_next_phase = s->tx_is_hdlc
                ? (s->tx_is_stream ? TX_HDLC_DATA
                                   : (s->tx_have_frame ? TX_HDLC_DATA : TX_HDLC_END))
                : TX_NONECM;
            s->tx_wait_ms = indicator_delay(s->tx_ind);
            s->tx_phase = (s->tx_wait_ms > 0) ? TX_IND_WAIT : s->tx_next_phase;
        }
        break;

    case TX_IND_WAIT:
        s->tx_wait_ms -= ms;
        if (s->tx_wait_ms <= 0) s->tx_phase = s->tx_next_phase;
        break;

    case TX_HDLC_DATA: {
        /* Need a new frame? (start of burst, or previous frame fully emitted) */
        if (s->tx_frame_off >= s->tx_frame_len) {
            if (s->tx_is_stream) {
                uint8_t f[4 + 256 + 8];
                int n = s->up.hdlc_get_frame ? s->up.hdlc_get_frame(s->up.user, f, (int) sizeof f) : 0;
                if (n <= 0) { s->tx_phase = TX_HDLC_DRAIN; break; }   /* burst done */
                load_hdlc_frame(s, f, n);
            } else {
                s->tx_phase = TX_HDLC_DRAIN;   /* single control frame fully sent */
                break;
            }
        }
        /* Emit one carrier-rate-sized chunk of the (already bit-reversed) frame. */
        int want = s->tx_bit_rate * ms / 8000;
        if (want < 1) want = 1;
        int avail = s->tx_frame_len - s->tx_frame_off;
        if (want > avail) want = avail;
        emit_data(s, FT_HDLC_DATA, &s->tx_frame[s->tx_frame_off], want);
        s->tx_frame_off += want;
        if (s->tx_frame_off >= s->tx_frame_len) s->tx_phase = TX_HDLC_FCS;
        break;
    }

    case TX_HDLC_FCS:
        emit_data(s, FT_HDLC_FCS_OK, NULL, 0);
        s->tx_phase = s->tx_is_stream ? TX_HDLC_DATA : TX_HDLC_DRAIN;
        break;

    case TX_HDLC_DRAIN: {
        /* Keep the carrier up until the modem could actually have played out the
         * whole HDLC burst before signalling end. We emit a frame's octets in one
         * IFP, but the gateway re-modulates at the (possibly slow) carrier rate —
         * V.21 is only 300 bps, so a 7-octet PPS needs ~190 ms on the wire. Cut
         * the carrier too soon and the gateway truncates the frame. */
        long needed = s->tx_burst_octets * 8 * 1000 / (s->tx_bit_rate > 0 ? s->tx_bit_rate : 300);
        if (s->tx_burst_ms < needed) break;     /* still draining */
        s->tx_phase = TX_HDLC_END;
        break;
    }

    case TX_HDLC_END:
        emit_data(s, FT_HDLC_SIG_END, NULL, 0);
        emit_indicator(s, IND_NO_SIGNAL);   /* cleanly drop carrier (gateways expect it) */
        step_complete(s);
        break;

    case TX_NONECM: {
        int want = s->tx_bit_rate * ms / 8000;       /* octets to pace this tick */
        if (want < 8) want = 8;
        if (want > NF_UDPTL_MAXIFP - 8) want = NF_UDPTL_MAXIFP - 8;
        uint8_t chunk[NF_UDPTL_MAXIFP];
        int oct = 0, ended = 0;
        while (oct < want) {
            int byte = 0;
            for (int b = 0; b < 8; b++) {
                int bit = s->up.non_ecm_get_bit ? s->up.non_ecm_get_bit(s->up.user) : NF_GET_BIT_END;
                if (bit == NF_GET_BIT_END) { ended = 1; break; }
                byte |= (bit & 1) << (7 - b);         /* MSB-first (T.38 wire order) */
            }
            if (ended) break;
            chunk[oct++] = (uint8_t) byte;
        }
        if (oct > 0) emit_data(s, FT_T4_NON_ECM_DATA, chunk, oct);
        if (ended) {
            /* End of image data. Don't stop abruptly: a gateway re-modulating to
             * a real fax machine loses/corrupts the last rows if the carrier ends
             * right at the final EOL (the modem shuts down before flushing). Pad
             * the carrier tail with ~200 ms of zero octets before SIG_END — this
             * is exactly what spandsp's t38_terminal does for the same reason. */
            s->tx_trailer = s->tx_bit_rate * 200 / 8000;
            s->tx_phase = TX_NONECM_TRAIL;
        }
        break;
    }

    case TX_NONECM_TRAIL: {
        int want = s->tx_bit_rate * ms / 8000;
        if (want < 8) want = 8;
        if (want > s->tx_trailer) want = s->tx_trailer;
        if (want > NF_UDPTL_MAXIFP - 8) want = NF_UDPTL_MAXIFP - 8;
        uint8_t z[NF_UDPTL_MAXIFP];
        memset(z, 0, (size_t) want);
        emit_data(s, FT_T4_NON_ECM_DATA, z, want);
        s->tx_trailer -= want;
        if (s->tx_trailer <= 0) s->tx_phase = TX_NONECM_END;
        break;
    }

    case TX_NONECM_END:
        emit_data(s, FT_T4_NON_ECM_SIG_END, NULL, 0);
        emit_indicator(s, IND_NO_SIGNAL);   /* cleanly drop carrier (gateways expect it) */
        step_complete(s);
        break;

    case TX_TONE_WAIT:
        s->tx_wait_ms -= ms;
        if (s->tx_wait_ms <= 0) step_complete(s);
        break;

    case TX_PAUSE_WAIT:
        s->tx_wait_ms -= ms;
        if (s->tx_wait_ms <= 0) step_complete(s);
        break;

    case TX_COMPLETE:
        step_complete(s);
        break;
    }

    if (s->up.timer_update) s->up.timer_update(s->up.user, ms * 8);
}

/* ── rx: IFP -> nf_t30 callbacks ─────────────────────────────────────────── */

/* Drop the current carrier. nf_t30 only acts on carrier-down for the FAST
 * (TCF / non-ECM image / ECM image) carrier — that's what drives eval_tcf,
 * page finalisation and the A_RECV_ECM -> A_WAIT_PPS step. V.21 control frames
 * are frame-driven (on_hdlc), so their carrier end must NOT raise a spurious
 * CARRIER_DOWN (it would, e.g., fire eval_tcf right after DCS). */
static void rx_carrier_down(nf_t38_t *s)
{
    if (s->rx_carrier && s->rx_is_fast && s->up.front_end_status)
        s->up.front_end_status(s->up.user, NF_STATUS_CARRIER_DOWN);
    s->rx_carrier = 0;
}

static void rx_indicator(nf_t38_t *s, int ind)
{
    if (t38dbg()) fprintf(stderr, "[T38 RX] IND %s\n", ind_name(ind));
    rx_carrier_down(s);                 /* end any previous fast carrier */
    s->rx_hdlc_len = 0;
    if (ind == IND_NO_SIGNAL || ind == IND_CED || ind == IND_CNG) {
        s->rx_is_fast = 0;
        return;
    }
    s->rx_is_fast = (ind != IND_V21_PREAMBLE);
    s->rx_carrier = 1;
    if (s->rx_is_fast && s->up.front_end_status) {   /* fast: T.38 implies trained */
        s->up.front_end_status(s->up.user, NF_STATUS_CARRIER_UP);
        s->up.front_end_status(s->up.user, NF_STATUS_TRAINING_SUCCEEDED);
    }
}

static void rx_hdlc_frame(nf_t38_t *s, int ok)
{
    if (s->up.hdlc_accept && s->rx_hdlc_len > 0)
        s->up.hdlc_accept(s->up.user, s->rx_hdlc, s->rx_hdlc_len, ok);
    s->rx_hdlc_len = 0;
}

static void rx_data_field(nf_t38_t *s, int ft, const uint8_t *p, int plen)
{
    if (t38dbg()) fprintf(stderr, "[T38 RX] %s len=%d\n", ft_name(ft), plen);
    switch (ft) {
    case FT_HDLC_DATA:
        if (plen > 0 && s->rx_hdlc_len + plen <= (int) sizeof(s->rx_hdlc)) {
            for (int i = 0; i < plen; i++)       /* wire LSB-first -> nf_t30 MSB-first */
                s->rx_hdlc[s->rx_hdlc_len + i] = bitrev8(p[i]);
            s->rx_hdlc_len += plen;
        }
        break;
    case FT_HDLC_FCS_OK:          rx_hdlc_frame(s, 1); break;
    case FT_HDLC_FCS_BAD:         rx_hdlc_frame(s, 0); break;
    case FT_HDLC_FCS_OK_SIG_END:  rx_hdlc_frame(s, 1); rx_carrier_down(s); break;
    case FT_HDLC_FCS_BAD_SIG_END: rx_hdlc_frame(s, 0); rx_carrier_down(s); break;
    case FT_HDLC_SIG_END:         rx_hdlc_frame(s, 1); rx_carrier_down(s); break;
    case FT_T4_NON_ECM_DATA:
        if (s->up.non_ecm_put_bit)
            for (int i = 0; i < plen; i++)
                for (int b = 7; b >= 0; b--)         /* MSB-first (T.38 wire order) */
                    s->up.non_ecm_put_bit(s->up.user, (p[i] >> b) & 1);
        break;
    case FT_T4_NON_ECM_SIG_END:   rx_carrier_down(s); break;
    }
}

/* Decode one IFP packet (version 0) and dispatch to the handlers above. */
static void rx_ifp(nf_t38_t *s, const uint8_t *buf, int len)
{
    if (len < 1) return;
    uint8_t b0 = buf[0];
    int data_present = b0 & 0x80;
    int type = (b0 >> 6) & 1;

    if (type == 0) {                       /* indicator */
        if (data_present) return;          /* malformed */
        rx_indicator(s, (b0 >> 1) & 0x3F);
        return;
    }

    /* data packet: bits 1-5 are the modem data-type, but rx is driven entirely by
     * the field types below, so we don't need it. */
    int ptr = 1;
    if (ptr >= len) return;

    /* number of fields (PER length; small) */
    int count;
    if (buf[ptr] & 0x80) { if (ptr + 1 >= len) return; count = ((buf[ptr] & 0x3F) << 8) | buf[ptr + 1]; ptr += 2; }
    else                 { count = buf[ptr++]; }
    if (count < 0 || count > 64) return;

    for (int i = 0; i < count; i++) {
        if (ptr >= len) return;
        uint8_t fb = buf[ptr++];
        int fdp = fb & 0x80;
        int ft = (fb >> 4) & 0x07;
        const uint8_t *fp = NULL;
        int flen = 0;
        if (fdp) {
            if (ptr + 1 >= len) return;
            flen = ((buf[ptr] << 8) | buf[ptr + 1]) + 1;
            ptr += 2;
            if (ptr + flen > len) return;
            fp = &buf[ptr];
            ptr += flen;
        }
        rx_data_field(s, ft, fp, flen);
    }
}

/* UDPTL -> IFP fan-out (recovered gap-fillers + primary, in order). */
static void on_udptl_ifp(void *user, const uint8_t *ifp, int ifp_len, int seq)
{
    (void) seq;
    rx_ifp((nf_t38_t *) user, ifp, ifp_len);
}

void nf_t38_rx_datagram(nf_t38_t *s, const uint8_t *dgram, int len)
{
    nf_udptl_rx(&s->udptl, dgram, len, on_udptl_ifp, s);
}

/* ── construction ────────────────────────────────────────────────────────── */

nf_t38_t *nf_t38_init(int calling_party, const nf_fax_iface_t *iface,
                      int redundancy, int far_max_datagram,
                      void (*send)(void *user, const uint8_t *dgram, int len),
                      void *send_user)
{
    nf_t38_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->up = *iface;
    s->calling = calling_party;
    s->send = send;
    s->send_user = send_user;
    nf_udptl_init(&s->udptl, redundancy, far_max_datagram);
    s->tx_phase = TX_IDLE;
    return s;
}

void nf_t38_free(nf_t38_t *s) { free(s); }
