#define _GNU_SOURCE
#include "nf_t30.h"
#include "nf_fax.h"
#include "nf_v8.h"
#include "nf_t38.h"
#include "nf_t4.h"
#include "nf_color.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tiffio.h>

/*
 * Compact non-ECM T.30 engine. Wire-compatible with spandsp (same DIS/DCS bit
 * layout, frames, TCF and modem-switch timing) but with a small internal state
 * machine. Owns an nf_fax driver and runs a page through nf_t4 + libtiff.
 */

#define ADDR            0xFF
#define CTL_FINAL       0x13
#define CTL_NONFINAL    0x03

/* FCF base values (t30_fcf.h). Dispatch is on (fcf | 0x01) to ignore the X bit. */
#define FCF_DIS 0x80
#define FCF_DCS 0x82
#define FCF_CSI 0x40
#define FCF_CIG 0x41     /* calling subscriber id (before DTC, polling)      */
#define FCF_TSI 0x42
#define FCF_CFR 0x84
#define FCF_FTT 0x44
#define FCF_MCF 0x8C
#define FCF_RTP 0xCC     /* retrain positive: page OK, but retrain          */
#define FCF_RTN 0x4C     /* retrain negative: page rejected, retransmit     */
#define FCF_EOP 0x2E
#define FCF_MPS 0x4E
#define FCF_EOM 0x8E
#define FCF_DCN 0xFA
/* ECM (T.30 Annex A) */
#define FCF_PPS 0xBE     /* partial page signal                       */
#define FCF_PPR 0xBC     /* partial page request (32-octet frame map) */
#define FCF_RNR 0xEC     /* receiver not ready                        */
#define FCF_RR  0x6E     /* receiver ready                            */
#define FCF_CTC 0x12     /* continue to correct                       */
#define FCF_CTR 0xC4     /* response to CTC                           */
#define FCF_EOR 0xCE     /* end of retransmission                     */
#define FCF_ERR 0x1C     /* response to EOR                           */
#define FCF_NULL 0x00
#define T4_FCD  0x06     /* facsimile coded data (high-speed HDLC)    */
#define T4_RCP  0x86     /* return to control for partial page        */
#define ECM_OCTETS 256   /* octets of image data per FCD frame        */

/* Hard ceiling on one reassembled received page (ECM payload or raw image
 * bits). Bounds memory against a peer that streams endless ECM blocks
 * (PPS-NULL forever) or holds the image carrier open - far above any real
 * fax page, so legitimate transfers are unaffected. */
#define NF_RX_MAX_PAGE (64u * 1024u * 1024u)

#define DISBIT1 0x01
#define DISBIT3 0x04
#define DISBIT4 0x08
#define DISBIT5 0x10
#define DISBIT6 0x20

#define set_bit(f,b)      ((f)[3 + ((b)-1)/8] |= (uint8_t)(1u << (((b)-1)%8)))
#define set_bits(f,v,b)   ((f)[3 + ((b)-1)/8] |= (uint8_t)((v) << (((b)-1)%8)))
static int test_bit(const uint8_t *f, int len, int b)
{
    int idx = 3 + (b - 1) / 8;
    if (idx >= len) return 0;
    return (f[idx] >> ((b - 1) % 8)) & 1;
}

/* fallback table: DCS modem bits -> rate/modem. */
static const struct { int rate, modem; uint8_t dcs; } FB[] = {
    { 14400, NF_MODEM_V17,    DISBIT6 },
    { 12000, NF_MODEM_V17,    DISBIT6 | DISBIT4 },
    {  9600, NF_MODEM_V17,    DISBIT6 | DISBIT3 },
    {  9600, NF_MODEM_V29,    DISBIT3 },
    {  7200, NF_MODEM_V17,    DISBIT6 | DISBIT4 | DISBIT3 },
    {  7200, NF_MODEM_V29,    DISBIT4 | DISBIT3 },
    {  4800, NF_MODEM_V27TER, DISBIT4 },
    {  2400, NF_MODEM_V27TER, 0 },
};
#define FB_V17_START 0
#define FB_V29_START 3
#define FB_V27_START 6
#define FB_LEN ((int) (sizeof FB / sizeof FB[0]))

/* NF_T30_MODEMS (e.g. "v29,v27") restricts which modems we advertise and
 * negotiate - used by the regression harnesses to pin a specific modem. */
static unsigned modem_allow_mask(void)
{
    static int mask = -1;
    if (mask < 0) {
        const char *e = getenv("NF_T30_MODEMS");
        mask = 0;
        if (e && *e) {
            if (strstr(e, "v17")) mask |= 1 << NF_MODEM_V17;
            if (strstr(e, "v29")) mask |= 1 << NF_MODEM_V29;
            if (strstr(e, "v27")) mask |= 1 << NF_MODEM_V27TER;
        }
        if (!mask)
            mask = (1 << NF_MODEM_V17) | (1 << NF_MODEM_V29) | (1 << NF_MODEM_V27TER);
    }
    return (unsigned) mask;
}

static int modem_allowed(int modem)
{
    return (modem_allow_mask() >> modem) & 1;
}

/* T.30 Annex F: the CM/JM modulation set we offer - the same classic-G3
 * ladder build_dis()/build_dcs() already advertise via DIS bits, plus V.21
 * (mandatory per V.8; it's also always our control channel). V.34
 * half-duplex is added only when the application opted in via
 * nf_t30_set_v34() - see s->v34_enable. */
struct nf_t30;
static uint32_t v8_capability_mask(const struct nf_t30 *s);

/* first fallback entry at or after i whose modem is allowed, or -1 */
static int next_allowed_fb(int i)
{
    if (i < 0) return -1;
    while (i < FB_LEN && !modem_allowed(FB[i].modem))
        i++;
    return i < FB_LEN ? i : -1;
}

/* substates */
enum {
    ST_INIT = 0,
    /* caller */
    C_WAIT_V8,                     /* T.30 Annex F: caller side of the handshake */
    C_WAIT_DIS, C_SEND_DCS, C_DCS_PAUSE, C_SEND_TCF, C_WAIT_CFR, C_SEND_IMAGE, C_SEND_POST,
    C_WAIT_MCF, C_SEND_DCN, C_SEND_DTC,
    /* caller ECM */
    C_SEND_ECM, C_SEND_PPS, C_WAIT_PPS_RESP,
    C_SEND_CTC, C_WAIT_CTR,        /* ECM rate fallback (T.30 Annex A.4.3)        */
    C_SEND_EOR, C_WAIT_EOR,        /* ECM end-of-retransmission at the lowest rate */
    C_WAIT_V34,                    /* T.30 Annex F: V.34 clause-12 startup */
    /* answerer */
    A_SEND_V8,                     /* T.30 Annex F: answerer side of the handshake */
    A_SEND_CED, A_SEND_DIS, A_WAIT_DCS, A_RECV_TCF, A_SEND_CFR, A_SEND_FTT,
    A_RECV_IMAGE, A_WAIT_POST, A_SEND_MCF, A_SEND_RTN, A_WAIT_DCN,
    /* answerer ECM */
    A_RECV_ECM, A_WAIT_PPS, A_SEND_PPR, A_SEND_ECM_MCF,
    A_SEND_CTR, A_SEND_ERR,        /* ECM rate-fallback / end-of-retransmission acks */
    A_REACK,                       /* re-sending a response to a repeated command */
    A_WAIT_V34,                    /* T.30 Annex F: V.34 clause-12 startup */
    ST_DONE
};

/* Payload kind negotiated for a call. Bilevel uses nf_t4; colour uses nf_color
 * (T.42/JPEG); file is a private nf<->nf raw binary transfer. Colour and file
 * both ride the ECM byte transport and require ECM. */
enum { PK_BILEVEL = 0, PK_COLOR, PK_GRAY, PK_FILE };

struct nf_t30 {
    int calling;
    int verbose;
    const nf_modem_ops_t *mops;   /* physical-layer backend vtable (audio or T.38) */
    void                 *be;     /* opaque backend handle (nf_fax_t* / nf_t38_t*) */
    int substate;
    int finished, result;

    char ident[24];               /* local station id (TSI/CSI/CIG); "" = send none */
    char far_ident[24];           /* remote station id captured from TSI/CSI/CIG    */
    int  supported_res;            /* NF_RES_* mask we advertise */

    uint8_t local_dis[24]; int local_dis_len;
    uint8_t dcs[24];       int dcs_len;
    uint8_t far_dis[24];   int far_dis_len;
    int dis_received;              /* X bit for our outbound frames */

    /* control burst: an optional station-id frame (CSI/TSI/CIG, non-final)
     * queued ahead of DIS/DCS/DTC so both go out in one carrier (T.30 phase B).
     * Served by on_get_frame via the streaming path; empty ident = single frame. */
    uint8_t cburst[2][24]; int cburst_len[2]; int cburst_n, cburst_i;

    /* negotiated */
    int fallback, modem, bit_rate, encoding /*NF_T4_COMPRESSION_*/;
    int y_res, x_res, image_width, nf_res;

    /* ── payload kind: colour (T.42/JPEG) and raw file transfer ── */
    int kind;                      /* PK_* negotiated this call */
    int color_capable, file_capable;/* advertise in DIS (receiver side)         */
    int poll_serve;                 /* answerer: hold a doc and transmit when polled */
    int poll_receive;               /* caller: poll (send DTC) then receive       */
    int tx_is_color, tx_is_gray, tx_is_file; /* the selected tx document's kind   */
    int color_quality;             /* JPEG quality for colour tx (1..100)        */
    uint8_t *color_rgb; int color_w, color_h;  /* decoded colour page (rx)       */

    /* tx document */
    char tx_file[512];
    TIFF *tif_in; int pages_total, cur_page;
    uint8_t *enc; size_t enc_len; size_t enc_bit;   /* encoded page + LSB cursor */
    int last_post_fcf;             /* MPS or EOP we sent */
    int post_retries;              /* post-message (EOP/PPS) retransmits so far (T.30: 3) */
    int dis_retries;               /* DIS repeats while awaiting DCS (T.30 T1: ~3 s apart) */

    /* rx document */
    char rx_file[512];
    TIFF *tif_out;
    uint8_t *rxbuf; size_t rxcap, rxbits;           /* accumulated image bits   */
    nf_t4_dec_t *dec;
    uint8_t *rxrows; int rxrow_n, rxrow_cap, rxstride;

    /* TCF */
    int tcf_left;                  /* tx */
    long tcf_zeros, tcf_most;      /* rx */
    int rx_page_bad;               /* rx: last non-ECM page failed copy quality */
    int rtn_count;                 /* tx: RTN-driven retransmits of this page */
    int rx_flip_n, rx_flip_ctr;    /* test hook: flip every Nth rx image bit  */

    /* ── V.34 (T.30 Annex F) ── */
    int v34_enable;                /* config: offer V.34 half-duplex in V.8      */
    int v34;                       /* this call negotiated V.34 (Annex F active) */
    int require_v34;               /* config: abort (DCN) if V.34 was not negotiated */

    /* ── ECM (T.30 Annex A) ── */
    int want_ecm;                  /* config: advertise + use ECM if far supports */
    int ecm;                       /* negotiated this call                        */
    int ecm_block;                 /* block index within the page                 */
    int ecm_at_page_end;           /* current block completes the page            */
    int next_tx_step;              /* PPS fcf2 at page end: MPS / EOP / EOM        */
    /* tx side: a "partial page" = up to 256 FCD frames of ECM_OCTETS each */
    size_t ecm_tx_base;            /* offset into s->enc of the current block      */
    int  ecm_frames;              /* frames in the current block (tx) / expected (rx) */
    uint8_t ecm_send[256];        /* tx: 1 = (re)send this frame                  */
    int  ecm_cursor;              /* tx: streaming frame cursor                   */
    int  ecm_rcp_left;            /* tx: RCP frames still to send (3..0)          */
    int  ecm_burst;               /* tx: frames actually sent this burst          */
    int  ppr_count;               /* tx: consecutive PPRs at this rate            */
    int  ctc_retries;             /* tx: CTC retransmits while awaiting CTR       */
    /* rx side */
    int16_t ecm_len[256];         /* rx: payload length per frame, -1 = missing   */
    uint8_t ecm_data[256][ECM_OCTETS]; /* rx: frame payloads                      */
    int  acked_page8, acked_block8; /* page/block (mod 256) of the last MCF we sent;
                                       -1 = none. Used to spot a repeated PPS (the
                                       sender missed our MCF) and re-acknowledge.  */
    int  last_resp_fcf;           /* rx: last post-message response sent (MCF/RTN) */
    int  reack_return;            /* substate to resume after an A_REACK re-send   */
    int  last_pps_fcf2;           /* rx: post-message in the last PPS             */
    uint8_t ecm_map[3 + 32];      /* PPR frame-map scratch                        */
    size_t rx_ecm_len;            /* rx: accumulated image bytes for current page */
    /* fault injection (testing): drop a frame once on first (re)transmit/receipt */
    int  drop_tx[64], n_drop_tx;
    int  drop_above_rate;         /* test: withhold every FCD frame while the
                                   * modem rate exceeds this (bps) - forces the
                                   * CTC rate-fallback path. 0 = disabled.       */
    int  drop_rx[64], n_drop_rx;
    uint8_t dropped_tx[256], dropped_rx[256];

    /* timers */
    long timeout;                  /* samples remaining; <=0 = disarmed */

    void (*phase_b)(void *); void *phase_b_user;
    void (*phase_e)(void *, int); void *phase_e_user;

    char log_tag[256];             /* optional per-call log prefix (e.g. Call-ID) */
    nf_t30_stats_t stats;
};

/* Optional wall-clock prefix ("HH:MM:SS.mmm ") for correlating T.30 events with
 * the SIP trace under sip_fax --debug (NF_LOG_TS). Cached; no-op when unset. */
static void log_ts(FILE *f)
{
    static int en = -1;
    if (en < 0) en = getenv("NF_LOG_TS") != NULL;
    if (!en) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d.%03ld ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

static void vlog(nf_t30_t *s, const char *fmt, ...)
{
    if (!s->verbose) return;
    va_list ap; va_start(ap, fmt);
    log_ts(stderr);
    if (s->log_tag[0]) fprintf(stderr, "[%s] ", s->log_tag);
    fprintf(stderr, "[%s] ", s->calling ? "CALL" : "ANSW");
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
}

static void eval_tcf(nf_t30_t *s);

static uint32_t v8_capability_mask(const struct nf_t30 *s)
{
    uint32_t m = NF_V8_MOD_V21;
    if (modem_allowed(NF_MODEM_V17))    m |= NF_V8_MOD_V17;
    if (modem_allowed(NF_MODEM_V29))    m |= NF_V8_MOD_V29;
    if (modem_allowed(NF_MODEM_V27TER)) m |= NF_V8_MOD_V27TER;
    if (s->v34_enable)                  m |= NF_V8_MOD_V34HDX;
    return m;
}

/* Minimum scan line time the receiver demands (T.30 DIS bits 21-23 + the
 * bit-46 fine/superfine halving), translated to the DCS code we will use.
 * Codes: 0=20ms 1=5ms 2=10ms 4=40ms 7=0ms. ECM always runs at 0 ms. */
static const int min_scan_ms[8] = { 20, 5, 10, 0, 40, 0, 0, 0 };

static int min_scan_code(nf_t30_t *s)
{
    static const uint8_t xlat[3][8] = {
        { 0, 1, 2, 0, 4, 4, 2, 7 },   /* standard resolution */
        { 0, 1, 2, 2, 4, 0, 1, 7 },   /* fine / inch-based */
        { 2, 1, 1, 1, 0, 2, 1, 7 },   /* superfine, when halving allowed */
    };
    if (s->ecm)
        return 7;
    int field = (s->far_dis_len > 5) ? ((s->far_dis[5] >> 4) & 7) : 0;
    int row;
    switch (s->nf_res) {
    case NF_RES_SUPERFINE:
    case NF_RES_400:
        row = test_bit(s->far_dis, s->far_dis_len, 46) ? 2 : 1;
        break;
    case NF_RES_FINE:
    case NF_RES_300:
        row = 1;
        break;
    default:
        row = 0;
        break;
    }
    return xlat[row][field];
}

/* ── modem control shortcuts ───────────────────────────────────────── */

/* T.30 control channel: V.21 classically, the V.34 1200 bit/s control
 * channel in an Annex F (V.34) call. */
static void tx_v21_frames(nf_t30_t *s)
{
    if (s->v34) s->mops->set_tx_type(s->be, NF_MODEM_V34, 1200, 0, 1);
    else        s->mops->set_tx_type(s->be, NF_MODEM_V21, 300, 0, 1);
}
static void rx_v21(nf_t30_t *s)
{
    if (s->v34) s->mops->set_rx_type(s->be, NF_MODEM_V34, 1200, 0, 1);
    else        s->mops->set_rx_type(s->be, NF_MODEM_V21, 300, 0, 1);
}
static void tx_none(nf_t30_t *s)       { s->mops->set_tx_type(s->be, NF_MODEM_NONE, 0, 0, 0); }
static void rx_none(nf_t30_t *s)       { s->mops->set_rx_type(s->be, NF_MODEM_NONE, 0, 0, 0); }
static void tx_image(nf_t30_t *s, int shorttrain)
    { s->mops->set_tx_type(s->be, s->modem, s->bit_rate, shorttrain, 0); }
static void rx_image(nf_t30_t *s, int shorttrain)
    { s->mops->set_rx_type(s->be, s->modem, s->bit_rate, shorttrain, 0); }

static void send_simple(nf_t30_t *s, int fcf)
{
    uint8_t f[3] = { ADDR, CTL_FINAL, (uint8_t)(fcf | s->dis_received) };
    s->mops->send_hdlc(s->be, f, 3);
}

/* Build a station-identifier frame (CSI/TSI/CIG, non-final): the 20-octet FIF
 * is the identifier in REVERSE character order, then space-padded (T.30 §5.2.2 /
 * spandsp convention - verified against a real machine whose id "91182" arrives
 * in the frame as "28119    "). The per-octet bit order is untouched: the HDLC
 * layer already handles the LSB-first wire order (same reason DIS bytes are
 * stored as-sent). */
static int build_id_frame(nf_t30_t *s, int fcf, uint8_t *out)
{
    out[0] = ADDR;
    out[1] = CTL_NONFINAL;
    out[2] = (uint8_t)(fcf | s->dis_received);
    int idl = (int) strlen(s->ident);
    if (idl > 20) idl = 20;
    for (int i = 0; i < idl; i++)
        out[3 + i] = (uint8_t) s->ident[idl - 1 - i];   /* reversed */
    memset(out + 3 + idl, ' ', (size_t)(20 - idl));
    return 3 + 20;
}

/* Send a phase-B control frame (DIS/DCS/DTC), prefixing our station-id frame
 * (id_fcf = CSI/TSI/CIG) in the SAME carrier burst when an identifier is set.
 * With no identifier this is the original single-frame path, byte for byte. */
static void send_ctrl_burst(nf_t30_t *s, int id_fcf,
                            const uint8_t *frame, int len, int next_substate)
{
    tx_v21_frames(s);
    /* Prepend our station-id frame (CSI/TSI/CIG) over classic G3 and T.38 only.
     * Over the V.34 control channel a real SG3 machine (Brother **1) fails to
     * answer a two-frame TSI+DCS burst with CFR - it times out - so send the
     * DCS/DIS/DTC alone there, exactly the proven single-frame path. We still
     * parse the remote's CSI/TSI over V.34; only our own id is withheld. */
    if (s->ident[0] && !s->v34) {
        /* audio / T.38: send_hdlc is one-frame-per-burst, so stream the id frame
         * then the control frame together via on_get_frame. */
        s->cburst_len[0] = build_id_frame(s, id_fcf, s->cburst[0]);
        int n = len > (int) sizeof s->cburst[1] ? (int) sizeof s->cburst[1] : len;
        memcpy(s->cburst[1], frame, (size_t) n);
        s->cburst_len[1] = n;
        s->cburst_n = 2; s->cburst_i = 0;
        s->mops->begin_hdlc_stream(s->be);   /* on_get_frame streams id then frame */
    } else {
        s->mops->send_hdlc(s->be, frame, len);   /* no id, or V.34: single frame */
    }
    s->substate = next_substate;
}

static void arm_timeout(nf_t30_t *s, int ms) { s->timeout = (long) ms * 8; }
static void disarm_timeout(nf_t30_t *s) { s->timeout = 0; }

/* T.30 Annex F: the actual page rate lives inside the V.34 session (MPh
 * negotiation incl. automatic fallback) - refresh the reported stat from
 * there whenever it might have changed. */
static void v34_refresh_bit_rate(nf_t30_t *s)
{
    if (s->v34 && s->mops == nf_fax_ops()) {
        int r = nf_fax_v34_bit_rate((nf_fax_t *) s->be);
        if (r > 0)
            s->stats.bit_rate = r;
    }
}

static void finish(nf_t30_t *s, int result)
{
    if (s->finished) return;
    s->finished = 1;
    s->result = result;
    v34_refresh_bit_rate(s);
    s->substate = ST_DONE;
    tx_none(s); rx_none(s);
    s->mops->set_tx_type(s->be, NF_MODEM_DONE, 0, 0, 0);
    vlog(s, "Phase E: result=%d pages tx=%d rx=%d", result, s->stats.pages_tx, s->stats.pages_rx);
    if (s->phase_e) s->phase_e(s->phase_e_user, result);
}

/* Refuse the call: send DCN and finish with the given error. */
static void abort_dcn(nf_t30_t *s, int result)
{
    send_simple(s, FCF_DCN);
    finish(s, result);
}

/* ── resolution helpers ────────────────────────────────────────────── */

static int res_dis_bit(int nf_res)        /* DIS/DCS bit advertising a resolution */
{
    switch (nf_res) {
    case NF_RES_FINE:      return 15;
    case NF_RES_SUPERFINE: return 41;
    case NF_RES_300:       return 42;
    case NF_RES_400:       return 43;
    default:               return 0;      /* standard: no bit */
    }
}

int nf_t30_remote_supports(nf_t30_t *s, int nf_res)
{
    int b = res_dis_bit(nf_res);
    return (b == 0) ? 1 : test_bit(s->far_dis, s->far_dis_len, b);
}

/* Phase-B queries: can the remote take T.30 Annex E JPEG payloads? Both ride
 * the ECM transport, so the remote must advertise ECM too (and we must be
 * willing to use it). Colour needs DIS bits 68+69, greyscale just 68. */
int nf_t30_remote_supports_color(nf_t30_t *s)
{
    return s->want_ecm
        && test_bit(s->far_dis, s->far_dis_len, 27)
        && test_bit(s->far_dis, s->far_dis_len, 68)
        && test_bit(s->far_dis, s->far_dis_len, 69);
}

int nf_t30_remote_supports_gray(nf_t30_t *s)
{
    return s->want_ecm
        && test_bit(s->far_dis, s->far_dis_len, 27)
        && test_bit(s->far_dis, s->far_dis_len, 68);
}

/* Classify a TIFF page's tags into NF_RES_*, and fill y/x dpi + width. */
static int classify_res(int width, double ydpi)
{
    if (width >= 3000) return NF_RES_400;
    if (width >= 2200) return NF_RES_300;
    if (ydpi > 300)    return NF_RES_SUPERFINE;
    if (ydpi > 150)    return NF_RES_FINE;
    return NF_RES_STANDARD;
}
static void res_to_dpi(int nf_res, int *xr, int *yr)
{
    switch (nf_res) {
    case NF_RES_FINE:      *xr = 204; *yr = 196; break;
    case NF_RES_SUPERFINE: *xr = 204; *yr = 391; break;
    case NF_RES_300:       *xr = 300; *yr = 300; break;
    case NF_RES_400:       *xr = 400; *yr = 400; break;
    default:               *xr = 204; *yr = 98;  break;
    }
}
static int res_width(int nf_res)
{
    if (nf_res == NF_RES_300) return 2592;
    if (nf_res == NF_RES_400) return 3456;
    return 1728;
}

/* ── DIS / DCS build ───────────────────────────────────────────────── */

static void build_dis(nf_t30_t *s)
{
    uint8_t *f = s->local_dis;
    memset(f, 0, sizeof s->local_dis);
    f[0] = ADDR; f[1] = CTL_FINAL; f[2] = (uint8_t)(FCF_DIS | s->dis_received);
    /* With dis_received set, the same skeleton goes out as a DTC (0x81). */
    set_bit(f, 10);                       /* ready to receive a fax document */
    if (s->v34) set_bit(f, 6);            /* Annex F: V.34 capable (mirrors the
                                           * reference capture's DIS ff138020..) */
    if (s->poll_serve) set_bit(f, 9);     /* a document is available for polling */
    if (modem_allowed(NF_MODEM_V29))    set_bit(f, 11);
    if (modem_allowed(NF_MODEM_V27TER)) set_bit(f, 12);
    if (modem_allowed(NF_MODEM_V17))    /* V.17 (forces V.29/V.27ter too) */
        f[4] |= (DISBIT6 | DISBIT4 | DISBIT3);
    if (s->supported_res & NF_RES_FINE)      set_bit(f, 15);
    set_bit(f, 16);                       /* T.4 2-D capable */
    set_bits(f, 7, 21);                   /* min scan line time = 0 ms */
    if (s->want_ecm) {
        set_bit(f, 27);                   /* ECM capable (256-octet frames) */
        set_bit(f, 31);                   /* T.6 coding capable (ECM only)  */
    }
    if (s->supported_res & NF_RES_SUPERFINE) set_bit(f, 41);
    if (s->supported_res & NF_RES_300)       set_bit(f, 42);
    if (s->supported_res & NF_RES_400)       set_bit(f, 43);
    set_bit(f, 45);                       /* metric resolution preferred */
    if (s->want_ecm && s->color_capable && !getenv("NF_T30_RX_NO_JPEG")) {
        /* T.30 Annex E continuous-tone colour (test hooks: NF_T30_RX_NO_JPEG
         * drops both JPEG bits, NF_T30_RX_NO_COLOR leaves greyscale only) */
        set_bit(f, 15);                   /* fine-res anchor for colour */
        set_bit(f, 68);                   /* T.81 (JPEG) coding capable */
        if (!getenv("NF_T30_RX_NO_COLOR"))
            set_bit(f, 69);               /* full colour mode */
    }
    if (s->want_ecm && s->file_capable)   /* binary file transfer (private profile) */
        set_bit(f, 53);                   /* BFT capable */
    /* prune: trim trailing empty octets and set extension bits */
    int i;
    for (i = 18; i >= 6; i--) { f[i] &= 0x7F; if (f[i]) break; }
    s->local_dis_len = i + 1;
    f[i] &= 0x7F;
    for (i--; i > 4; i--) f[i] |= 0x80;
}

static void build_dcs(nf_t30_t *s)
{
    uint8_t *f = s->dcs;
    memset(f, 0, sizeof s->dcs);
    f[0] = ADDR; f[1] = CTL_FINAL; f[2] = (uint8_t)(FCF_DCS | s->dis_received);
    if (!s->v34)                          /* Annex F: DCS speed bits all 0 -
                                           * the rate is governed by V.34 */
        f[4] |= FB[s->fallback].dcs;
    set_bits(f, (unsigned) min_scan_code(s), 21);  /* min scan line time */
    set_bit(f, 10);                       /* receiver: go into receive mode */
    if (s->kind == PK_COLOR) {            /* T.30 Annex E colour (always ECM) */
        set_bit(f, 15);                   /* fine-res anchor */
        set_bit(f, 27);                   /* ECM */
        set_bit(f, 68);                   /* T.81 (JPEG) */
        set_bit(f, 69);                   /* full colour mode */
    } else if (s->kind == PK_GRAY) {      /* T.30 Annex E greyscale (always ECM) */
        set_bit(f, 15);                   /* fine-res anchor */
        set_bit(f, 27);                   /* ECM */
        set_bit(f, 68);                   /* T.81 (JPEG); bit 69 clear = grey only */
    } else if (s->kind == PK_FILE) {      /* binary file transfer (always ECM) */
        set_bit(f, 27);                   /* ECM */
        set_bit(f, 53);                   /* BFT */
    } else {                              /* bilevel */
        if (s->encoding == NF_T4_COMPRESSION_2D) set_bit(f, 16);
        if (s->ecm) {
            set_bit(f, 27);               /* ECM (bit 28 clear = 256-octet frames) */
            if (s->encoding == NF_T4_COMPRESSION_T6) set_bit(f, 31);   /* T.6 */
        }
        int b = res_dis_bit(s->nf_res);
        if (b) set_bit(f, b);
    }
    int i;
    for (i = 18; i >= 6; i--) { f[i] &= 0x7F; if (f[i]) break; }
    s->dcs_len = i + 1;
    f[i] &= 0x7F;
    for (i--; i > 4; i--) f[i] |= 0x80;
}

/* ── tx document (caller) ──────────────────────────────────────────── */

static void set_enc(nf_t30_t *s, uint8_t *buf, size_t len)
{
    free(s->enc);
    s->enc = buf;
    s->enc_len = len;
    s->enc_bit = 0;
}

/* Read the current TIFF directory as packed sRGB (3 B/px) via TIFFReadRGBAImage
 * (so any RGB-ish TIFF works). Caller frees *rgb. */
static int read_rgb_page(nf_t30_t *s, uint8_t **rgb, uint32_t *wp, uint32_t *hp)
{
    if (!TIFFSetDirectory(s->tif_in, (uint16_t) s->cur_page)) return -1;
    uint32_t w = 0, h = 0;
    TIFFGetField(s->tif_in, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(s->tif_in, TIFFTAG_IMAGELENGTH, &h);
    if (!w || !h) return -1;
    uint32_t *raster = malloc((size_t) w * h * sizeof(uint32_t));
    if (!raster) return -1;
    if (!TIFFReadRGBAImageOriented(s->tif_in, w, h, raster, ORIENTATION_TOPLEFT, 0)) {
        free(raster); return -1;
    }
    uint8_t *out = malloc((size_t) w * h * 3);
    if (!out) { free(raster); return -1; }
    for (size_t i = 0; i < (size_t) w * h; i++) {
        uint32_t px = raster[i];
        out[i*3+0] = (uint8_t) TIFFGetR(px);
        out[i*3+1] = (uint8_t) TIFFGetG(px);
        out[i*3+2] = (uint8_t) TIFFGetB(px);
    }
    free(raster);
    *rgb = out; *wp = w; *hp = h;
    return 0;
}

/* Colour/greyscale page: read the TIFF as sRGB and produce a T.42/JPEG
 * codestream into s->enc (colour = 3-component, greyscale = L* only). */
static int encode_page_jpeg(nf_t30_t *s, int gray)
{
    uint8_t *rgb = NULL; uint32_t w = 0, h = 0;
    if (read_rgb_page(s, &rgb, &w, &h) != 0) return -1;
    uint8_t *out = NULL; size_t outlen = 0;
    int q = s->color_quality ? s->color_quality : 85;
    int rc = gray ? nf_gray_encode(rgb, (int) w, (int) h, q, &out, &outlen)
                  : nf_color_encode(rgb, (int) w, (int) h, q, &out, &outlen);
    free(rgb);
    if (rc != 0) return -1;
    set_enc(s, out, outlen);
    s->image_width = (int) w;
    s->nf_res = NF_RES_FINE; res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
    s->stats.width = (int) w; s->stats.length = (int) h;
    s->stats.x_resolution = s->x_res; s->stats.y_resolution = s->y_res;
    vlog(s, "encoded %s page %d: %ux%u -> %zu JPEG bytes",
         gray ? "grey" : "colour", s->cur_page, w, h, outlen);
    return 0;
}

/* File payload: header "NFFX1"|name_len|name|u64 length + raw file bytes -> s->enc. */
static int encode_file_payload(nf_t30_t *s)
{
    FILE *f = fopen(s->tx_file, "rb");
    if (!f) { vlog(s, "cannot open tx file %s", s->tx_file); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    /* base name only */
    const char *base = strrchr(s->tx_file, '/');
    base = base ? base + 1 : s->tx_file;
    size_t nl = strlen(base); if (nl > 255) nl = 255;
    size_t hdr = 5 + 1 + nl + 8;
    uint8_t *buf = malloc(hdr + (size_t) sz);
    if (!buf) { fclose(f); return -1; }
    memcpy(buf, "NFFX1", 5);
    buf[5] = (uint8_t) nl;
    memcpy(buf + 6, base, nl);
    uint64_t len64 = (uint64_t) sz;
    for (int i = 0; i < 8; i++) buf[6 + nl + i] = (uint8_t) (len64 >> (8 * i));
    size_t got = fread(buf + hdr, 1, (size_t) sz, f);
    fclose(f);
    if (got != (size_t) sz) { free(buf); return -1; }
    set_enc(s, buf, hdr + (size_t) sz);
    s->stats.width = (int) sz; s->stats.length = 1;
    vlog(s, "encoded file '%s' %ld bytes (+%zu hdr)", base, sz, hdr);
    return 0;
}

/* Read one bilevel TIFF page into a freshly encoded nf_t4 buffer. Returns 0. */
static int encode_current_page(nf_t30_t *s)
{
    if (s->kind == PK_COLOR) return encode_page_jpeg(s, 0);
    if (s->kind == PK_GRAY)  return encode_page_jpeg(s, 1);
    if (s->kind == PK_FILE)  return encode_file_payload(s);
    if (!TIFFSetDirectory(s->tif_in, (uint16_t) s->cur_page)) return -1;
    uint32_t w = 0, h = 0; float yr = 0; uint16_t unit = RESUNIT_INCH;
    TIFFGetField(s->tif_in, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(s->tif_in, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(s->tif_in, TIFFTAG_RESOLUTIONUNIT, &unit);
    TIFFGetField(s->tif_in, TIFFTAG_YRESOLUTION, &yr);
    double ydpi = (unit == RESUNIT_CENTIMETER) ? yr * 2.54 : yr;

    s->nf_res = classify_res((int) w, ydpi);
    res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
    s->image_width = (int) w;

    /* nf_t4 expects 1 = black (MINISWHITE). Invert if the source is MINISBLACK. */
    uint16_t photo = PHOTOMETRIC_MINISWHITE;
    TIFFGetFieldDefaulted(s->tif_in, TIFFTAG_PHOTOMETRIC, &photo);
    int invert = (photo == PHOTOMETRIC_MINISBLACK);

    nf_t4_enc_t *e = nf_t4_enc_init((int) w, s->encoding, 0);
    if (!s->ecm) {
        int ms = min_scan_ms[min_scan_code(s)];
        nf_t4_enc_set_min_row_bits(e, s->bit_rate * ms / 1000);
        if (ms)
            vlog(s, "min scan line time %d ms -> %d bits/row at %d bps",
                 ms, s->bit_rate * ms / 1000, s->bit_rate);
    }
    int stride = ((int) w + 7) / 8;
    uint8_t *row = malloc(stride);
    for (uint32_t y = 0; y < h; y++) {
        memset(row, 0, stride);
        TIFFReadScanline(s->tif_in, row, y, 0);
        if (invert) for (int i = 0; i < stride; i++) row[i] ^= 0xFF;
        nf_t4_enc_row(e, row);
    }
    free(row);
    nf_t4_enc_end_page(e);
    size_t len; const uint8_t *d = nf_t4_enc_data(e, &len);
    free(s->enc);
    s->enc = malloc(len ? len : 1);
    memcpy(s->enc, d, len);
    s->enc_len = len; s->enc_bit = 0;
    nf_t4_enc_free(e);
    s->stats.width = (int) w; s->stats.length = (int) h;
    s->stats.x_resolution = s->x_res; s->stats.y_resolution = s->y_res;
    vlog(s, "encoded page %d: %ux%u res=%d enc=%zu bytes", s->cur_page, w, h, s->nf_res, len);
    return 0;
}

/* ── rx document (answerer) ────────────────────────────────────────── */

static void rx_row(void *user, const uint8_t *rowp, int width)
{
    nf_t30_t *s = user;
    s->rxstride = (width + 7) / 8;
    if ((size_t) s->rxrow_n * s->rxstride >= NF_RX_MAX_PAGE) return;   /* cap */
    if (s->rxrow_n >= s->rxrow_cap) {
        int ncap = s->rxrow_cap ? s->rxrow_cap * 2 : 256;
        uint8_t *nb = realloc(s->rxrows, (size_t) ncap * s->rxstride);
        if (!nb) return;                    /* OOM: keep the old buffer, drop row */
        s->rxrows = nb;
        s->rxrow_cap = ncap;
    }
    memcpy(s->rxrows + (size_t) s->rxrow_n * s->rxstride, rowp, s->rxstride);
    s->rxrow_n++;
}

/* Write fax metadata (sender station id, receive time, negotiated call params)
 * into the current TIFF directory's standard descriptive tags. */
static void set_rx_meta_tags(nf_t30_t *s)
{
    s->stats.ecm = s->ecm;
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char dt[24] = "", iso[40] = "", desc[256];
    if (tmv) {
        strftime(dt,  sizeof dt,  "%Y:%m:%d %H:%M:%S", tmv);   /* TIFF DATETIME  */
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", tmv);   /* ISO 8601       */
    }
    snprintf(desc, sizeof desc,
             "From=%s Received=%s Rate=%dbps Mode=%s ECM=%s",
             s->far_ident[0] ? s->far_ident : "(none)", iso,
             s->stats.bit_rate, s->stats.v34 ? "Super-G3/V.34" : "G3",
             s->ecm ? "on" : "off");
    TIFFSetField(s->tif_out, TIFFTAG_IMAGEDESCRIPTION, desc);
    if (dt[0]) TIFFSetField(s->tif_out, TIFFTAG_DATETIME, dt);
    TIFFSetField(s->tif_out, TIFFTAG_SOFTWARE, s->ident[0] ? s->ident : "nf_fax");
}

static void write_rx_page(nf_t30_t *s)
{
    if (!s->tif_out) {
        s->tif_out = TIFFOpen(s->rx_file, "w");
        if (!s->tif_out) { vlog(s, "cannot open rx file %s", s->rx_file); return; }
    }
    if (s->kind == PK_COLOR || s->kind == PK_GRAY) {  /* JPEG page from nf_color */
        int gray = (s->kind == PK_GRAY);
        int w = s->color_w, h = s->color_h, spp = gray ? 1 : 3;
        TIFFSetField(s->tif_out, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(s->tif_out, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(s->tif_out, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(s->tif_out, TIFFTAG_SAMPLESPERPIXEL, spp);
        TIFFSetField(s->tif_out, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        TIFFSetField(s->tif_out, TIFFTAG_PHOTOMETRIC,
                     gray ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
        TIFFSetField(s->tif_out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(s->tif_out, TIFFTAG_XRESOLUTION, 200.0f);
        TIFFSetField(s->tif_out, TIFFTAG_YRESOLUTION, 200.0f);
        TIFFSetField(s->tif_out, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        TIFFSetField(s->tif_out, TIFFTAG_ROWSPERSTRIP, h ? h : 1);
        TIFFSetField(s->tif_out, TIFFTAG_PAGENUMBER, s->stats.pages_rx, 0);
        set_rx_meta_tags(s);
        for (int y = 0; y < h; y++)
            TIFFWriteScanline(s->tif_out, s->color_rgb + (size_t) y * w * spp, y, 0);
        TIFFWriteDirectory(s->tif_out);
        vlog(s, "wrote rx %s page %d: %dx%d", gray ? "grey" : "colour", s->stats.pages_rx, w, h);
        return;
    }
    int w = s->image_width, h = s->rxrow_n;
    TIFFSetField(s->tif_out, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(s->tif_out, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(s->tif_out, TIFFTAG_BITSPERSAMPLE, 1);
    TIFFSetField(s->tif_out, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(s->tif_out, TIFFTAG_COMPRESSION, COMPRESSION_CCITT_T6);
    TIFFSetField(s->tif_out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
    TIFFSetField(s->tif_out, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
    TIFFSetField(s->tif_out, TIFFTAG_XRESOLUTION, (float) s->x_res);
    TIFFSetField(s->tif_out, TIFFTAG_YRESOLUTION, (float) s->y_res);
    TIFFSetField(s->tif_out, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(s->tif_out, TIFFTAG_ROWSPERSTRIP, h ? h : 1);
    TIFFSetField(s->tif_out, TIFFTAG_PAGENUMBER, s->stats.pages_rx, 0);
    set_rx_meta_tags(s);
    for (int y = 0; y < h; y++)
        TIFFWriteScanline(s->tif_out, s->rxrows + (size_t) y * s->rxstride, y, 0);
    TIFFWriteDirectory(s->tif_out);
    vlog(s, "wrote rx page %d: %dx%d", s->stats.pages_rx, w, h);
}

static void start_rx_page(nf_t30_t *s)
{
    nf_t4_dec_free(s->dec);
    s->dec = nf_t4_dec_init(s->image_width, s->encoding, rx_row, s);
    s->rxbits = 0;
    s->rxrow_n = 0;
}

static void finish_rx_page(nf_t30_t *s)
{
    if (s->rxbuf && s->rxbits)
        nf_t4_dec_put(s->dec, s->rxbuf, (s->rxbits + 7) / 8);
    /* Copy quality: spandsp's accept/reject boundary (TSB-85 derived) is
     * 15% defective rows; a zero-row page is a total failure. Damaged rows
     * were concealed by the decoder, so an accepted page keeps its geometry. */
    int rows = s->dec ? nf_t4_dec_rows(s->dec) : 0;
    int bad = s->dec ? nf_t4_dec_bad_rows(s->dec) : 0;
    s->rx_flip_n = 0;              /* the test hook damages one attempt only */
    s->rx_page_bad = (rows == 0 || bad * 20 >= rows * 3);
    if (s->rx_page_bad) {
        vlog(s, "copy quality: %d of %d rows bad -> reject page (RTN)", bad, rows);
        return;                              /* page is not written or counted */
    }
    if (bad)
        vlog(s, "copy quality: %d of %d rows bad -> accept page", bad, rows);
    s->stats.pages_rx++;
    s->stats.width = s->image_width; s->stats.length = s->rxrow_n;
    write_rx_page(s);
}

/* ── DCS/DIS parse + negotiation ───────────────────────────────────── */

/* Decode a received DIS/DTC (a peer's receive capabilities) into a readable
 * --verbose line: modems/speed, resolutions, coding, colour/grey/BFT, ECM,
 * and its minimum scan-line time. Reads the same bits build_dis() sets. */
static void log_far_caps(nf_t30_t *s, const char *what)
{
    if (!s->verbose) return;
    const uint8_t *f = s->far_dis;
    int n = s->far_dis_len;
    char b[300];
    int p = 0;
#define CAP(...) do { if (p < (int) sizeof b) \
        p += snprintf(b + p, sizeof b - (size_t) p, __VA_ARGS__); } while (0)
    CAP("%s:", what);
    if (test_bit(f, n, 6)) CAP(" V.34(SuperG3)");
    uint8_t m = (n > 4) ? (f[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3)) : 0;
    if (m == (DISBIT6 | DISBIT4 | DISBIT3))              CAP(" V.17<=14400");
    else if (m == (DISBIT4 | DISBIT3) || m == DISBIT3)   CAP(" V.29<=9600");
    else if (m == DISBIT4)                               CAP(" V.27ter<=4800");
    else                                                 CAP(" V.27ter<=2400");
    CAP(" res=std");
    if (test_bit(f, n, 15)) CAP(",fine");
    if (test_bit(f, n, 41)) CAP(",superfine");
    if (test_bit(f, n, 42)) CAP(",300dpi");
    if (test_bit(f, n, 43)) CAP(",400dpi");
    if (test_bit(f, n, 16)) CAP(" 2D");
    if (test_bit(f, n, 27)) CAP(" ECM");
    if (test_bit(f, n, 31)) CAP(" T.6");
    if (test_bit(f, n, 68) && test_bit(f, n, 69)) CAP(" colour(T.42/JPEG)");
    else if (test_bit(f, n, 68))                  CAP(" greyscale(JPEG)");
    if (test_bit(f, n, 53)) CAP(" BFT");
    CAP(" min-scan=%dms", min_scan_ms[(n > 5) ? ((f[5] >> 4) & 7) : 0]);
#undef CAP
    vlog(s, "%s", b);
}

/* Log a peer's non-standard facilities frame (NSF/NSC/NSS, FCF 0x20-0x22):
 * vendor-proprietary, so just surface the T.35 country byte and the raw FIF. */
static void log_nsf(nf_t30_t *s, const uint8_t *msg, int len)
{
    if (!s->verbose || len < 4) return;
    char hex[3 * 16 + 1];
    int p = 0, fl = len - 3;
    if (fl > 16) fl = 16;
    for (int i = 0; i < fl; i++)
        p += snprintf(hex + p, sizeof hex - (size_t) p, "%02x ", msg[3 + i]);
    vlog(s, "peer non-standard frame fcf=0x%02x country=0x%02x fif=%s%s",
         msg[2], msg[3], hex, (len - 3 > 16) ? "..." : "");
}

static void parse_dcs(nf_t30_t *s, const uint8_t *msg, int len)
{
    memset(s->far_dis, 0, sizeof s->far_dis);
    int n = len > 24 ? 24 : len;
    memcpy(s->far_dis, msg, n); s->far_dis_len = n;   /* reuse buffer for far DCS */

    /* Read the modem bits from the zero-padded copy, not the raw frame: a runt
     * DCS (len < 5) must see 0 here, not stale bytes past the received data. */
    uint8_t modembits = s->far_dis[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3);
    s->fallback = FB_V27_START + 1;
    for (size_t i = 0; i < sizeof FB / sizeof FB[0]; i++)
        if (FB[i].dcs == modembits) { s->fallback = (int) i; break; }
    s->modem = FB[s->fallback].modem;
    s->bit_rate = FB[s->fallback].rate;
    s->ecm = test_bit(s->far_dis, n, 27) ? 1 : 0;
    if (s->v34) {                 /* Annex F: modem fixed, ECM mandatory */
        s->modem = NF_MODEM_V34;
        s->bit_rate = 24000;
        s->ecm = 1;
    }
    if (s->ecm && test_bit(s->far_dis, n, 68) && test_bit(s->far_dis, n, 69))
        s->kind = PK_COLOR;
    else if (s->ecm && test_bit(s->far_dis, n, 68))   /* 68 without 69 = greyscale */
        s->kind = PK_GRAY;
    else if (s->ecm && test_bit(s->far_dis, n, 53))
        s->kind = PK_FILE;
    else
        s->kind = PK_BILEVEL;
    if (s->ecm && test_bit(s->far_dis, n, 31))
        s->encoding = NF_T4_COMPRESSION_T6;
    else
        s->encoding = test_bit(s->far_dis, n, 16) ? NF_T4_COMPRESSION_2D : NF_T4_COMPRESSION_1D;

    if (test_bit(s->far_dis, n, 43))      s->nf_res = NF_RES_400;
    else if (test_bit(s->far_dis, n, 42)) s->nf_res = NF_RES_300;
    else if (test_bit(s->far_dis, n, 41)) s->nf_res = NF_RES_SUPERFINE;
    else if (test_bit(s->far_dis, n, 15)) s->nf_res = NF_RES_FINE;
    else                                  s->nf_res = NF_RES_STANDARD;
    res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
    s->image_width = res_width(s->nf_res);
    s->stats.bit_rate = s->bit_rate;
    s->stats.x_resolution = s->x_res; s->stats.y_resolution = s->y_res;
    static const char *kind_name[] = { "bilevel", "colour", "greyscale", "file" };
    const char *enc = s->encoding == NF_T4_COMPRESSION_T6 ? "T.6" :
                      s->encoding == NF_T4_COMPRESSION_2D ? "MR(2D)" : "MH(1D)";
    vlog(s, "DCS (peer selected): %s %dbps ECM=%s coding=%s res=%dx%ddpi (%dpx) kind=%s",
         nf_t30_modem_name(s), s->bit_rate, s->ecm ? "on" : "off", enc,
         s->x_res, s->y_res, s->image_width,
         (s->kind >= 0 && s->kind < 4) ? kind_name[s->kind] : "?");
}

/* Caller: pick the fallback start from the far DIS modem bits. */
static int parse_dis_pick_fallback(nf_t30_t *s)
{
    uint8_t m = s->far_dis[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3);
    if (m == (DISBIT6 | DISBIT4 | DISBIT3)) return FB_V17_START;   /* V.17 */
    if (m == (DISBIT4 | DISBIT3))           return FB_V29_START;   /* V.29 */
    if (m == DISBIT4)                       return FB_V27_START;   /* V.27ter 4800 */
    if (m == 0)                             return FB_V27_START + 1;
    if (m == DISBIT3)                       return FB_V29_START;
    return -1;
}

/* ── the protocol event handlers ───────────────────────────────────── */

static void send_dcs_then_tcf(nf_t30_t *s)
{
    build_dcs(s);
    /* TSI (transmitting station id) precedes DCS in the same burst. */
    send_ctrl_burst(s, FCF_TSI, s->dcs, s->dcs_len, C_SEND_DCS); /* -> TCF */
}

static void begin_image_tx(nf_t30_t *s)
{
    if (encode_current_page(s) != 0) { finish(s, NF_T30_ERR_FILE); return; }
    tx_image(s, 1);                      /* short training for the image */
    s->substate = C_SEND_IMAGE;
}

/* The far end's capabilities (a DIS for a normal caller, or a DTC when we are
 * being polled) have arrived: store them, run the phase-B selection hook, pick
 * the modem/ECM/encoding and payload kind, then send DCS + TCF. Shared by the
 * normal caller (C_WAIT_DIS) and the poll server (DTC in A_WAIT_DCS). */
static void recv_caps_begin_tx(nf_t30_t *s, const uint8_t *msg, int len)
{
    int n = len > 24 ? 24 : len;
    memcpy(s->far_dis, msg, n); s->far_dis_len = n;
    s->dis_received = 1;
    log_far_caps(s, "peer caps (DIS)");
    /* --require-v34: if V.8 did not settle on V.34 (Annex F), refuse to send any
     * page data. We know the negotiated modem by now (s->v34 was set during the
     * V.8/DIS handshake), and we are already in the V.21 message phase, so send
     * DCN cleanly and fail the call before the first pixel goes out. */
    if (s->require_v34 && !s->v34) {
        vlog(s, "--require-v34: V.34 (Super G3) not negotiated - aborting with DCN");
        abort_dcn(s, NF_T30_ERR_INCOMPAT); return;
    }
    if (s->phase_b) s->phase_b(s->phase_b_user);
    /* When serving a poll, the caller must have advertised it can receive. */
    if (s->poll_serve && !test_bit(s->far_dis, n, 10)) {
        vlog(s, "poll: caller is not ready to receive");
        abort_dcn(s, NF_T30_ERR_INCOMPAT); return;
    }
    if (s->v34) {
        /* T.30 Annex F: no modem ladder - the V.34 primary channel carries
         * the pages (rate governed below T.30), and ECM is mandatory. */
        if (!test_bit(s->far_dis, n, 6))
            vlog(s, "warning: V.34 session but far DIS lacks bit 6");
        s->modem = NF_MODEM_V34;
        s->bit_rate = 24000;
        s->ecm = 1;
    } else {
        int fb = next_allowed_fb(parse_dis_pick_fallback(s));
        if (fb < 0) { abort_dcn(s, NF_T30_ERR_INCOMPAT); return; }
        s->fallback = fb;
        s->modem = FB[fb].modem; s->bit_rate = FB[fb].rate;
        /* ECM is used only if both ends advertise it; T.6 only inside ECM. */
        s->ecm = (s->want_ecm && test_bit(s->far_dis, n, 27)) ? 1 : 0;
    }
    s->stats.bit_rate = s->bit_rate;
    if (s->ecm && test_bit(s->far_dis, n, 31))
        s->encoding = NF_T4_COMPRESSION_T6;
    else
        s->encoding = test_bit(s->far_dis, n, 16) ? NF_T4_COMPRESSION_2D
                                                  : NF_T4_COMPRESSION_1D;
    /* Pick the payload kind. Colour and file both require ECM and the far end's
     * capability bits; otherwise we can't proceed. */
    if (s->tx_is_color) {
        if (s->ecm && test_bit(s->far_dis, n, 68) && test_bit(s->far_dis, n, 69)) {
            s->kind = PK_COLOR;
            s->nf_res = NF_RES_FINE; res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
        } else {
            vlog(s, "far end cannot receive colour (ECM=%d)", s->ecm);
            abort_dcn(s, NF_T30_ERR_INCOMPAT); return;
        }
    } else if (s->tx_is_gray) {
        if (s->ecm && test_bit(s->far_dis, n, 68)) {
            s->kind = PK_GRAY;
            s->nf_res = NF_RES_FINE; res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
        } else {
            vlog(s, "far end cannot receive greyscale JPEG (ECM=%d)", s->ecm);
            abort_dcn(s, NF_T30_ERR_INCOMPAT); return;
        }
    } else if (s->tx_is_file) {
        if (s->ecm && test_bit(s->far_dis, n, 53)) {
            s->kind = PK_FILE;
        } else {
            vlog(s, "far end cannot receive a binary file (ECM=%d)", s->ecm);
            abort_dcn(s, NF_T30_ERR_INCOMPAT); return;
        }
    } else {
        s->kind = PK_BILEVEL;
    }
    vlog(s, "negotiated ECM=%d kind=%d encoding=%d", s->ecm, s->kind, s->encoding);
    send_dcs_then_tcf(s);
}

static void answer_send_dis(nf_t30_t *s)
{
    build_dis(s);
    /* CSI (called station id) precedes DIS in the same burst. */
    send_ctrl_burst(s, FCF_CSI, s->local_dis, s->local_dis_len, A_SEND_DIS);
}

/* Caller polling for a document: send our capabilities as a DTC (build_dis with
 * dis_received already set emits FCF 0x81 and our "ready to receive" bit), then
 * wait for the answerer's DCS and receive like a normal answerer would. */
static void poll_send_dtc(nf_t30_t *s)
{
    build_dis(s);
    /* CIG (calling station id) precedes DTC in the same burst. */
    send_ctrl_burst(s, FCF_CIG, s->local_dis, s->local_dis_len, C_SEND_DTC);
}

/* ── ECM transmit (caller) ─────────────────────────────────────────── */

/* Carve the next "partial page" (a block of up to 256 FCD frames) out of the
 * encoded page (s->enc) starting at ecm_tx_base. */
static void ecm_build_block(nf_t30_t *s)
{
    size_t remaining = s->enc_len - s->ecm_tx_base;
    size_t cap = (size_t) 256 * ECM_OCTETS;
    size_t this_block;
    /* T.30 Annex F: at the lower V.34 rates a full 256-frame block exceeds
     * the session's 30 s rx-gate force-process guard (256 frames is ~23 s
     * at 24000 bit/s but ~38 s at 14400), so size the block to ~24 s of
     * carrier at the currently negotiated rate. A partial block is
     * perfectly legal ECM. */
    if (s->v34 && s->mops == nf_fax_ops()) {
        int r = nf_fax_v34_bit_rate((nf_fax_t *) s->be);
        if (r > 0) {
            long fmax = (long) r * 24 / ((ECM_OCTETS + 5) * 8);
            if (fmax < 16)
                fmax = 16;
            if (fmax < 256)
                cap = (size_t) fmax * ECM_OCTETS;
        }
    }
    /* Diagnostic: hard-cap FCD frames per block (interop experiments). */
    {
        const char *e = getenv("NF_ECM_MAXFR");
        long fmax = e ? atol(e) : 0;
        if (fmax >= 1 && fmax <= 256 && cap > (size_t) fmax * ECM_OCTETS)
            cap = (size_t) fmax * ECM_OCTETS;
    }
    this_block = remaining < cap ? remaining : cap;
    int frames = (int) ((this_block + ECM_OCTETS - 1) / ECM_OCTETS);
    if (frames < 1) frames = 1;                 /* always at least one (padded) frame */
    s->ecm_frames = frames;
    for (int i = 0; i < frames; i++) s->ecm_send[i] = 1;
    s->ecm_at_page_end = (s->ecm_tx_base + this_block >= s->enc_len);
    s->ppr_count = 0;
    vlog(s, "ECM tx block %d: %d frames, page_end=%d", s->ecm_block, frames, s->ecm_at_page_end);
}

/* Start (or restart, after a PPR) the high-speed HDLC burst for the current block. */
static void ecm_start_burst(nf_t30_t *s)
{
    s->ecm_cursor = 0;
    s->ecm_rcp_left = 3;
    s->ecm_burst = 0;
    s->mops->set_tx_type(s->be, s->modem, s->bit_rate, 1 /*short train*/, 1 /*hdlc*/);
    s->mops->begin_hdlc_stream(s->be);
    s->substate = C_SEND_ECM;
}

static void begin_ecm_image_tx(nf_t30_t *s)
{
    if (encode_current_page(s) != 0) { finish(s, NF_T30_ERR_FILE); return; }
    s->ecm_block = 0;
    s->ecm_tx_base = 0;
    s->next_tx_step = (s->cur_page + 1 < s->pages_total) ? FCF_MPS : FCF_EOP;
    ecm_build_block(s);
    ecm_start_burst(s);
}

/* Pull the next HDLC frame for the driver to stream (FCD frames then 3 RCP). */
static int on_get_frame(void *user, uint8_t *buf, int maxlen)
{
    nf_t30_t *s = user;
    if (s->cburst_n > 0) {                  /* phase-B: station-id frame then DIS/DCS/DTC */
        if (s->cburst_i < s->cburst_n) {
            int i = s->cburst_i++;
            int n = s->cburst_len[i] > maxlen ? maxlen : s->cburst_len[i];
            memcpy(buf, s->cburst[i], (size_t) n);
            return n;
        }
        s->cburst_n = s->cburst_i = 0;      /* both frames sent -> end the burst */
        return 0;
    }
    if (s->substate != C_SEND_ECM) return 0;
    while (s->ecm_cursor < s->ecm_frames) {
        int i = s->ecm_cursor++;
        if (!s->ecm_send[i]) continue;
        /* This frame is part of the burst, so it counts in the PPS frame total
         * (the receiver locks the block size from the first PPS) even if we then
         * drop it to simulate transit loss. */
        s->ecm_burst++;
        if (s->drop_above_rate && s->bit_rate > s->drop_above_rate) {
            vlog(s, "[fault] withhold FCD frame %d at %d bps (> %d) - forcing CTC",
                 i, s->bit_rate, s->drop_above_rate);
            continue;                           /* delivered only once CTC drops the rate */
        }
        int drop = 0;
        if (!s->dropped_tx[i])
            for (int k = 0; k < s->n_drop_tx; k++)
                if (s->drop_tx[k] == i) { s->dropped_tx[i] = 1; drop = 1; break; }
        if (drop) { vlog(s, "[fault] drop tx FCD frame %d (lost in transit)", i); continue; }
        size_t off = s->ecm_tx_base + (size_t) i * ECM_OCTETS;
        int n = 0;
        buf[n++] = ADDR; buf[n++] = CTL_NONFINAL; buf[n++] = T4_FCD; buf[n++] = (uint8_t) i;
        for (int o = 0; o < ECM_OCTETS && n < maxlen; o++) {
            size_t p = off + (size_t) o;
            buf[n++] = (p < s->enc_len) ? s->enc[p] : 0x00;     /* pad tail with zeros */
        }
        return n;                                               /* 4 + 256 octets */
    }
    if (s->ecm_rcp_left > 0) {                                   /* T.4/A: 3 RCP frames */
        s->ecm_rcp_left--;
        buf[0] = ADDR; buf[1] = CTL_NONFINAL; buf[2] = T4_RCP;
        return 3;
    }
    return 0;                                                   /* burst done */
}

static void ecm_send_pps(nf_t30_t *s)
{
    uint8_t f[7];
    f[0] = ADDR; f[1] = CTL_FINAL; f[2] = (uint8_t) (FCF_PPS | s->dis_received);
    f[3] = s->ecm_at_page_end ? (uint8_t) (s->next_tx_step | s->dis_received) : FCF_NULL;
    f[4] = (uint8_t) (s->cur_page & 0xFF);
    f[5] = (uint8_t) (s->ecm_block & 0xFF);
    f[6] = (uint8_t) (s->ecm_burst ? s->ecm_burst - 1 : 0);
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, f, 7);
    vlog(s, "PPS fcf2=0x%02x page=%d block=%d frames=%d", f[3], f[4], f[5], s->ecm_burst);
    s->substate = C_SEND_PPS;
}

/* Sender got a PPR: clear frames the receiver is happy with, keep the rest to resend. */
static void ecm_apply_ppr(nf_t30_t *s, const uint8_t *msg, int len)
{
    if (len < 3 + 32) return;
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++) {
            int fno = (i << 3) + j;
            if (fno >= s->ecm_frames) continue;
            s->ecm_send[fno] = (msg[3 + i] >> j) & 1;          /* bit set -> resend */
        }
}

/* Map a DCS-style speed octet (bits 11-14, as carried in DCS FIF octet 2 and in
 * a CTC frame) back to a fallback-table index, or -1 if unknown. */
static int fb_from_dcs_byte(uint8_t b)
{
    uint8_t bits = b & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3);
    for (int i = 0; i < FB_LEN; i++)
        if (FB[i].dcs == bits) return i;
    return -1;
}

/* T.30 Annex A.4.3: after repeated PPRs the transmitter drops a modem step and
 * asks the receiver to continue correcting the block at the lower rate. The CTC
 * FIF is two octets: octet 1 is zero, octet 2 carries the new speed in the same
 * coding as DCS bits 11-14 (our fallback table's `dcs` byte). s->fallback/modem/
 * bit_rate must already be set to the target rate. */
static void ecm_send_ctc(nf_t30_t *s)
{
    uint8_t f[5];
    f[0] = ADDR; f[1] = CTL_FINAL; f[2] = (uint8_t) (FCF_CTC | s->dis_received);
    f[3] = 0x00; f[4] = FB[s->fallback].dcs;
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, f, 5);
    vlog(s, "CTC: block %d still bad -> retransmit at %d bps (%s)",
         s->ecm_block, s->bit_rate, nf_t30_modem_name(s));
    s->substate = C_SEND_CTC;
}

/* T.30 Annex A: already at the lowest rate and the block still won't clear -
 * end retransmission. The EOR FIF octet 2 carries the post-message command that
 * would have followed a good block (NULL mid-page, else MPS/EOP). The receiver
 * acknowledges with ERR. */
static void ecm_send_eor(nf_t30_t *s)
{
    uint8_t f[4];
    f[0] = ADDR; f[1] = CTL_FINAL; f[2] = (uint8_t) (FCF_EOR | s->dis_received);
    f[3] = (uint8_t) ((s->ecm_at_page_end ? s->next_tx_step : FCF_NULL) | s->dis_received);
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, f, 4);
    vlog(s, "EOR: block %d cannot be delivered at %d bps (fcf2=0x%02x)",
         s->ecm_block, s->bit_rate, f[3]);
    s->substate = C_SEND_EOR;
}

/* ── ECM receive (answerer) ────────────────────────────────────────── */

static void ecm_rx_begin_page(nf_t30_t *s)
{
    for (int i = 0; i < 256; i++) s->ecm_len[i] = -1;
    s->ecm_frames = -1;                  /* learned from the first PPS of the block */
    s->ecm_block = 0;
    s->rx_ecm_len = 0;
}

static void ecm_rx_listen(nf_t30_t *s)   /* receive FCD/RCP over the fast modem (HDLC) */
{
    s->mops->set_rx_type(s->be, s->modem, s->bit_rate, 1 /*short train*/, 1 /*hdlc*/);
    s->substate = A_RECV_ECM;
    /* Bound the block: nf_fax now runs V.21 in parallel and silently re-acquires
     * the fast carrier, so a totally dead/untrainable line no longer surfaces a
     * carrier-down to move us on. Disarmed the instant a frame arrives (on_hdlc),
     * so a healthy block is unaffected; on expiry on_timer disconnects.
     * V.34: the whole block is delivered in one batch when the burst ends (a
     * full 256-frame block at 24000 bit/s is ~23 s of carrier), so the bound
     * must cover the entire burst. */
    arm_timeout(s, s->v34 ? 45000 : 15000);
}

static void ecm_store_fcd(nf_t30_t *s, const uint8_t *msg, int len)
{
    int fno = msg[3];
    int plen = len - 4;
    if (fno < 0 || fno > 255 || plen < 0) return;
    if (plen > ECM_OCTETS) plen = ECM_OCTETS;
    if (!s->dropped_rx[fno])
        for (int k = 0; k < s->n_drop_rx; k++)
            if (s->drop_rx[k] == fno) {
                s->dropped_rx[fno] = 1;
                vlog(s, "[fault] drop rx FCD frame %d", fno);
                return;
            }
    memcpy(s->ecm_data[fno], msg + 4, (size_t) plen);
    s->ecm_len[fno] = (int16_t) plen;
}

/* Ensure *buf holds at least `need` bytes, doubling *cap from an 8 KB floor.
 * On success updates buf and cap and returns 0; on OOM the old buffer is left
 * intact and it returns -1. */
static int grow_bytes(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;
    size_t ncap = *cap ? *cap : 8192;
    while (ncap < need) ncap *= 2;
    uint8_t *nb = realloc(*buf, ncap);
    if (!nb) return -1;
    *buf = nb;
    *cap = ncap;
    return 0;
}

static void ecm_rx_append(nf_t30_t *s, const uint8_t *data, int n)
{
    if (n <= 0) return;
    if (s->rx_ecm_len + (size_t) n > NF_RX_MAX_PAGE) {   /* cap runaway growth */
        vlog(s, "rx page exceeds %u bytes - dropping data (possible attack)",
             NF_RX_MAX_PAGE);
        return;
    }
    if (grow_bytes(&s->rxbuf, &s->rxcap, s->rx_ecm_len + (size_t) n) != 0)
        return;                             /* OOM: drop data, keep old buffer */
    memcpy(s->rxbuf + s->rx_ecm_len, data, (size_t) n);
    s->rx_ecm_len += (size_t) n;
}

/* Reassembled ECM payload (s->rxbuf) -> output file (PK_FILE). */
static void write_rx_file(nf_t30_t *s)
{
    const uint8_t *b = s->rxbuf; size_t n = s->rx_ecm_len;
    if (n < 6 + 8 || memcmp(b, "NFFX1", 5) != 0) { vlog(s, "bad file header"); return; }
    int nl = b[5];
    if ((size_t) (6 + nl + 8) > n) { vlog(s, "truncated file header"); return; }
    uint64_t len = 0;
    for (int i = 0; i < 8; i++) len |= (uint64_t) b[6 + nl + i] << (8 * i);
    size_t off = (size_t) 6 + nl + 8;
    if (off + len > n) len = n - off;            /* clamp to what we actually have */
    char name[256]; int cn = nl < 255 ? nl : 255;
    memcpy(name, b + 6, cn); name[cn] = '\0';
    FILE *f = fopen(s->rx_file, "wb");
    if (!f) { vlog(s, "cannot open rx file %s", s->rx_file); return; }
    fwrite(b + off, 1, (size_t) len, f);
    fclose(f);
    vlog(s, "wrote rx file %s (sent name '%s', %llu bytes)",
         s->rx_file, name, (unsigned long long) len);
}

static void ecm_finish_page(nf_t30_t *s)
{
    if (s->kind == PK_COLOR || s->kind == PK_GRAY) {
        int gray = (s->kind == PK_GRAY);
        free(s->color_rgb); s->color_rgb = NULL;
        int rc = gray ? nf_gray_decode(s->rxbuf, s->rx_ecm_len, &s->color_rgb, &s->color_w, &s->color_h)
                      : nf_color_decode(s->rxbuf, s->rx_ecm_len, &s->color_rgb, &s->color_w, &s->color_h);
        if (rc == 0) {
            s->stats.pages_rx++;
            s->stats.width = s->color_w; s->stats.length = s->color_h;
            write_rx_page(s);
            free(s->color_rgb); s->color_rgb = NULL;
            vlog(s, "ECM %s page %d (%zu bytes -> %dx%d)",
                 gray ? "grey" : "colour", s->stats.pages_rx, s->rx_ecm_len, s->color_w, s->color_h);
        } else {
            vlog(s, "%s decode failed (%zu bytes)", gray ? "grey" : "colour", s->rx_ecm_len);
        }
        s->rx_ecm_len = 0;
        return;
    }
    if (s->kind == PK_FILE) {
        write_rx_file(s);
        s->stats.pages_rx++;
        s->rx_ecm_len = 0;
        return;
    }
    start_rx_page(s);                    /* inits decoder for s->image_width/encoding */
    if (s->rxbuf && s->rx_ecm_len)
        nf_t4_dec_put(s->dec, s->rxbuf, s->rx_ecm_len);
    s->stats.pages_rx++;
    s->stats.width = s->image_width; s->stats.length = s->rxrow_n;
    write_rx_page(s);
    vlog(s, "ECM page %d complete (%zu bytes -> %d rows)", s->stats.pages_rx, s->rx_ecm_len, s->rxrow_n);
    s->rx_ecm_len = 0;
}

static void ecm_send_ppr(nf_t30_t *s)
{
    uint8_t *m = s->ecm_map;
    m[0] = ADDR; m[1] = CTL_FINAL; m[2] = (uint8_t) (FCF_PPR | s->dis_received);
    for (int i = 0; i < 32; i++) {
        uint8_t b = 0;
        for (int j = 0; j < 8; j++)
            if (s->ecm_len[(i << 3) + j] < 0) b |= (uint8_t) (1u << j);   /* missing */
        m[3 + i] = b;
    }
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, m, 3 + 32);
    s->substate = A_SEND_PPR;
}

/* Re-send our last post-message response (MCF/RTN) to a command the far end has
 * repeated because it missed the response (T.30 §5: the receiver answers a
 * repeated command). `resume` is the substate to return to once the re-send has
 * been played out (we don't re-process the page/block). */
static void reack(nf_t30_t *s, int resume)
{
    s->reack_return = resume;
    tx_v21_frames(s);
    send_simple(s, s->last_resp_fcf ? s->last_resp_fcf : FCF_MCF);
    s->substate = A_REACK;
}

static void ecm_process_pps(nf_t30_t *s, const uint8_t *msg, int len)
{
    if (len < 7) return;
    int fcf2 = msg[3] & 0xFE;
    int frames = msg[6] + 1;
    int page = msg[4], block = msg[5];

    /* A PPS for the block we just acknowledged means the sender missed our MCF
     * and is repeating the command — re-acknowledge rather than treat the (now
     * cleared) block as a fresh, all-frames-missing one and answer with a PPR. */
    if (page == s->acked_page8 && block == s->acked_block8) {
        vlog(s, "repeated PPS for acked page %d block %d -> re-send MCF", page, block);
        reack(s, A_RECV_ECM);
        return;
    }

    if (s->ecm_frames < 0) s->ecm_frames = frames;        /* lock block size */
    s->last_pps_fcf2 = fcf2;

    int first_bad = 256;
    for (int i = 0; i < s->ecm_frames; i++)
        if (s->ecm_len[i] < 0) { first_bad = i; break; }

    if (first_bad >= s->ecm_frames) {                     /* block fully received */
        for (int i = 0; i < s->ecm_frames; i++)
            ecm_rx_append(s, s->ecm_data[i], s->ecm_len[i]);
        for (int i = 0; i < 256; i++) s->ecm_len[i] = -1;
        s->ecm_block++;
        s->ecm_frames = -1;
        s->acked_page8 = page; s->acked_block8 = block; s->last_resp_fcf = FCF_MCF;
        vlog(s, "ECM block OK, send MCF (fcf2=0x%02x)", fcf2);
        tx_v21_frames(s);
        send_simple(s, FCF_MCF);
        s->substate = A_SEND_ECM_MCF;
    } else {
        vlog(s, "ECM block incomplete (first_bad=%d), send PPR", first_bad);
        ecm_send_ppr(s);
    }
}

/* Receiver side of T.30 Annex A.4.3: the sender is dropping to a lower modem
 * rate to keep correcting the current block. Adopt the new rate (from the CTC
 * speed octet), acknowledge with CTR, then keep receiving the block - the
 * already-good frames are retained, only the still-missing ones come again. */
static void ecm_process_ctc(nf_t30_t *s, const uint8_t *msg, int len)
{
    if (len >= 5) {
        int fb = fb_from_dcs_byte(msg[4]);
        if (fb >= 0) {
            s->fallback = fb;
            s->modem = FB[fb].modem;
            s->bit_rate = FB[fb].rate;
            s->stats.bit_rate = s->bit_rate;
            vlog(s, "CTC: retransmission continues at %d bps (%s)",
                 s->bit_rate, nf_t30_modem_name(s));
        }
    }
    tx_v21_frames(s);
    send_simple(s, FCF_CTR);
    s->substate = A_SEND_CTR;
}

/* Receiver side: the sender ended retransmission of a block it could not get
 * through (EOR). Keep whatever frames did arrive and acknowledge with ERR; the
 * page is left with uncorrected errors and the sender's next command (DCN, or
 * the next block/page) drives what follows. */
static void ecm_process_eor(nf_t30_t *s, const uint8_t *msg, int len)
{
    (void) msg; (void) len;
    for (int i = 0; i < s->ecm_frames && i < 256; i++)
        if (s->ecm_len[i] >= 0) ecm_rx_append(s, s->ecm_data[i], s->ecm_len[i]);
    vlog(s, "EOR: sender ended retransmission; page %d has uncorrected errors",
         s->ecm_block);
    tx_v21_frames(s);
    send_simple(s, FCF_ERR);
    s->substate = A_SEND_ERR;
}

/* Capture the remote station id from a received CSI/TSI/CIG FIF. The FIF holds
 * the id in REVERSE character order, space-padded (T.30 §5.2.2), so read the
 * octets back-to-front and trim surrounding spaces. Mirrors into the stats. */
static void capture_far_ident(nf_t30_t *s, const uint8_t *msg, int len)
{
    int fl = len - 3;
    if (fl < 0) fl = 0;
    if (fl > 20) fl = 20;
    /* un-reverse: last FIF octet is the first id character */
    int start = 0, end = fl;
    while (start < end && msg[3 + start] == ' ') start++;      /* trailing pad */
    while (end > start && msg[3 + end - 1] == ' ') end--;      /* leading pad  */
    int n = 0;
    for (int i = end - 1; i >= start && n < (int) sizeof s->far_ident - 1; i--)
        s->far_ident[n++] = (char) msg[3 + i];
    s->far_ident[n] = '\0';
    memcpy(s->stats.rx_ident, s->far_ident, sizeof s->stats.rx_ident);
    vlog(s, "remote station id: \"%s\"", s->far_ident);
}

static void on_hdlc(void *user, const uint8_t *msg, int len, int ok)
{
    nf_t30_t *s = user;
    vlog(s, "rx frame fcf=0x%02x len=%d ok=%d (state %d)", len>=3?msg[2]:0, len, ok, s->substate);
    if (!ok || len < 3) return;
    int fcf = msg[2] | 0x01;             /* ignore X bit for dispatch */
    disarm_timeout(s);

    /* Non-standard facilities (NSF/NSC/NSS, FCF 0x20-0x22): vendor-proprietary,
     * we don't act on them, but surface them under --verbose. */
    if ((msg[2] & 0xFC) == 0x20) log_nsf(s, msg, len);

    /* ECM high-speed image frames arrive only while receiving an ECM block. */
    if (s->substate == A_RECV_ECM) {
        int raw = msg[2] & 0xFE;
        if (raw == T4_FCD) { ecm_store_fcd(s, msg, len); return; }
        if (raw == T4_RCP) return;       /* block end is driven by carrier-down */
    }

    switch (s->substate) {
    case C_WAIT_DIS:
        if (fcf == (FCF_CSI | 1)) {       /* called station id, precedes DIS */
            capture_far_ident(s, msg, len);
        } else if (fcf == (FCF_DIS | 1)) {
            if (s->poll_receive) {
                /* We called to pull a document. The answerer must offer one
                 * (DIS bit 9); then send our DTC and switch to receiving. */
                int n = len > 24 ? 24 : len;
                memcpy(s->far_dis, msg, n); s->far_dis_len = n;
                s->dis_received = 1;
                log_far_caps(s, "peer caps (DIS, polled)");
                if (s->phase_b) s->phase_b(s->phase_b_user);
                if (!test_bit(s->far_dis, n, 9)) {
                    vlog(s, "poll: answerer has no document to transmit");
                    abort_dcn(s, NF_T30_ERR_INCOMPAT); return;
                }
                poll_send_dtc(s);
            } else {
                recv_caps_begin_tx(s, msg, len);
            }
        }
        break;
    case C_WAIT_CFR:
        if (fcf == (FCF_CFR | 1)) { if (s->ecm) begin_ecm_image_tx(s); else begin_image_tx(s); }
        else if (fcf == (FCF_FTT | 1)) {
            int nfb = next_allowed_fb(s->fallback + 1);
            if (nfb < 0) {
                abort_dcn(s, NF_T30_ERR_GENERAL); return;
            }
            s->fallback = nfb; s->modem = FB[s->fallback].modem; s->bit_rate = FB[s->fallback].rate;
            s->stats.bit_rate = s->bit_rate;
            send_dcs_then_tcf(s);
        }
        break;
    case C_WAIT_MCF:
        if (fcf == (FCF_MCF | 1) || fcf == (FCF_RTP | 1)) {
            /* RTP: the page was accepted too, the receiver merely asks for a
             * retrain - which it gets anyway before any next page. */
            s->rtn_count = 0;
            if (s->last_post_fcf == FCF_MPS) {
                s->cur_page++;
                begin_image_tx(s);       /* next page */
            } else {
                s->stats.pages_tx = s->cur_page + 1;
                tx_v21_frames(s);
                send_simple(s, FCF_DCN);
                s->substate = C_SEND_DCN;   /* let DCN flush before finishing */
            }
        } else if (fcf == (FCF_RTN | 1)) {
            /* Page rejected: retrain at the next lower rate and resend the
             * same page (cur_page is not advanced). */
            int nfb = next_allowed_fb(s->fallback + 1);
            if (++s->rtn_count > 2 || nfb < 0) {
                tx_v21_frames(s);
                send_simple(s, FCF_DCN);
                finish(s, NF_T30_ERR_GENERAL);
                return;
            }
            vlog(s, "RTN: retransmit page %d at %d bps", s->cur_page, FB[nfb].rate);
            s->fallback = nfb; s->modem = FB[nfb].modem; s->bit_rate = FB[nfb].rate;
            s->stats.bit_rate = s->bit_rate;
            send_dcs_then_tcf(s);
        }
        break;

    case C_WAIT_PPS_RESP:
        if (fcf == (FCF_MCF | 1)) {
            if (!s->ecm_at_page_end) {              /* more blocks in this page */
                s->ecm_tx_base += (size_t) s->ecm_frames * ECM_OCTETS;
                s->ecm_block++;
                ecm_build_block(s);
                ecm_start_burst(s);
            } else if (s->next_tx_step == FCF_MPS) { /* next page */
                s->stats.pages_tx = s->cur_page + 1;
                s->cur_page++;
                begin_ecm_image_tx(s);
            } else {                                 /* EOP: done */
                s->stats.pages_tx = s->cur_page + 1;
                tx_v21_frames(s);
                send_simple(s, FCF_DCN);
                s->substate = C_SEND_DCN;
            }
        } else if (fcf == (FCF_PPR | 1)) {
            ecm_apply_ppr(s, msg, len);
            s->ppr_count++;
            if (s->v34) {
                /* V.34 (Annex F) negotiates its own primary-channel rate; the
                 * classic CTC modem ladder doesn't apply. Resend, bounded. */
                if (s->ppr_count > 16) {
                    tx_v21_frames(s); send_simple(s, FCF_DCN);
                    finish(s, NF_T30_ERR_GENERAL);
                    return;
                }
                ecm_start_burst(s);
            } else if (s->ppr_count >= 4) {
                /* T.30 Annex A.4.3: after 4 consecutive PPRs, stop hammering the
                 * block at this rate. Drop a modem step (CTC) so the receiver can
                 * still correct it; at the lowest rate, end retransmission (EOR). */
                int nfb = next_allowed_fb(s->fallback + 1);
                if (nfb >= 0) {
                    s->fallback = nfb;
                    s->modem = FB[nfb].modem;
                    s->bit_rate = FB[nfb].rate;
                    s->stats.bit_rate = s->bit_rate;
                    s->ppr_count = 0;
                    s->ctc_retries = 0;
                    ecm_send_ctc(s);
                } else {
                    ecm_send_eor(s);
                }
            } else {
                ecm_start_burst(s);                  /* resend the still-bad frames */
            }
        } else if (fcf == (FCF_RNR | 1)) {
            ecm_send_pps(s);                         /* receiver busy: re-poll with PPS */
        }
        break;

    case C_WAIT_CTR:
        /* Receiver agreed to continue correcting at the new rate: resend the
         * still-bad frames (already marked in ecm_send[]) at s->bit_rate. */
        if (fcf == (FCF_CTR | 1)) {
            vlog(s, "CTR: resuming block %d at %d bps", s->ecm_block, s->bit_rate);
            ecm_start_burst(s);
        }
        break;

    case C_WAIT_EOR:
        /* Receiver acknowledged the end of retransmission: the block could not
         * be delivered, so the transfer failed. Disconnect. */
        if (fcf == (FCF_ERR | 1)) {
            vlog(s, "ERR: block %d abandoned; disconnecting", s->ecm_block);
            tx_v21_frames(s); send_simple(s, FCF_DCN);
            finish(s, NF_T30_ERR_GENERAL);
        }
        break;

    case A_WAIT_DCS:
    case A_RECV_TCF:                     /* caller may repeat DCS+TCF if we
                                          * stayed silent on a failed TCF */
        if (fcf == (FCF_TSI | 1) || fcf == (FCF_CIG | 1)) {
            capture_far_ident(s, msg, len);   /* sender's TSI / poller's CIG */
        } else if (fcf == (FCF_DCS | 1)) {
            parse_dcs(s, msg, len);
            if (s->v34) {
                /* T.30 Annex F F.3.2: no TCF - the V.34 training already
                 * proved the channel, answer CFR straight away. */
                vlog(s, "Annex F: DCS on V.34 control channel -> CFR (no TCF)");
                tx_v21_frames(s);
                send_simple(s, FCF_CFR);
                s->substate = A_SEND_CFR;
                return;
            }
            start_rx_page(s);
            rx_image(s, 0);              /* receive TCF (long train) */
            s->tcf_zeros = s->tcf_most = 0;
            arm_timeout(s, 7000);        /* T2-style: evaluate even if the
                                          * modem never trains (-> FTT) */
            s->substate = A_RECV_TCF;
        } else if (s->poll_serve && fcf == (FCF_DIS | 1)) {
            /* The caller polled us (DTC, FCF 0x81): we hold the document, so we
             * become the transmitter and run the normal Phase B-D send path. */
            recv_caps_begin_tx(s, msg, len);
        } else if (fcf == (FCF_DCN | 1)) {
            /* e.g. the sender declines to retransmit after our RTN */
            finish(s, NF_T30_OK);
        }
        break;
    case A_WAIT_POST:
        /* The page was already decoded/written on image carrier-down; here we
         * just acknowledge. MPS = next page, same params; EOM = next page,
         * renegotiate (back to Phase B); EOP = end. */
        if (fcf == (FCF_EOP | 1) || fcf == (FCF_MPS | 1) || fcf == (FCF_EOM | 1)) {
            s->last_post_fcf = (fcf == (FCF_MPS | 1)) ? FCF_MPS
                             : (fcf == (FCF_EOM | 1)) ? FCF_EOM : FCF_EOP;
            tx_v21_frames(s);
            if (s->rx_page_bad) {
                /* too many damaged lines: reject the page; the sender should
                 * retrain (fresh DCS + TCF) and send it again */
                s->last_resp_fcf = FCF_RTN;
                send_simple(s, FCF_RTN);
                s->substate = A_SEND_RTN;
            } else {
                s->last_resp_fcf = FCF_MCF;
                send_simple(s, FCF_MCF);
                s->substate = A_SEND_MCF;
            }
        } else if (fcf == (FCF_DCN | 1)) {
            finish(s, NF_T30_OK);
        }
        break;
    case A_RECV_ECM:   /* With parallel V.21 receive a post-message control frame
                        * can land while we are still armed for the high-speed
                        * image carrier (FCD/RCP were handled above and returned),
                        * e.g. a gateway that PPS-signals after a training the
                        * fast modem could not take. Treat it like A_WAIT_PPS. */
    case A_WAIT_PPS:
        if ((msg[2] & 0xFE) == FCF_PPS)      ecm_process_pps(s, msg, len);
        else if ((msg[2] & 0xFE) == FCF_CTC) ecm_process_ctc(s, msg, len);
        else if ((msg[2] & 0xFE) == FCF_EOR) ecm_process_eor(s, msg, len);
        else if (fcf == (FCF_DCN | 1))       finish(s, NF_T30_OK);
        break;

    case A_WAIT_DCN:
        if (fcf == (FCF_DCN | 1)) {
            finish(s, NF_T30_OK);
        } else if ((msg[2] & 0xFE) == FCF_PPS ||           /* ECM post-message */
                   fcf == (FCF_EOP | 1) || fcf == (FCF_MPS | 1) || fcf == (FCF_EOM | 1)) {
            /* The sender repeated its end-of-page command: our MCF was lost.
             * Re-acknowledge and keep waiting for the DCN. */
            vlog(s, "repeated end-of-page command in A_WAIT_DCN -> re-send response");
            reack(s, A_WAIT_DCN);
        }
        break;
    }
}

static void on_status(void *user, int status)
{
    nf_t30_t *s = user;
    vlog(s, "status=%d (state %d)", status, s->substate);
    /* T.30 Annex F: NF_MODEM_V8 reports its one-shot result via the same
     * TRAINING_SUCCEEDED/FAILED events every other modem uses. If V.34 was
     * both offered (nf_t30_set_v34) and negotiated, the branch below hands
     * the call to the V.34 session engine; otherwise V.8 success only means
     * "negotiation completed" and the classic-G3 flow continues unchanged. */
    /* T.30 Annex F over V.34: if both ends negotiated V.34 half-duplex in
     * V.8 (and the application opted in), hand the line to the V.34 session
     * engine for the clause-12 startup instead of the classic flow. */
    if (status == NF_STATUS_TRAINING_SUCCEEDED &&
        (s->substate == C_WAIT_V8 || s->substate == A_SEND_V8) &&
        s->v34_enable && s->mops == nf_fax_ops() &&
        (nf_fax_v8_modulations((nf_fax_t *) s->be) & NF_V8_MOD_V34HDX)) {
        vlog(s, "V8 negotiated V.34 half-duplex (mod=0x%04X) - T.30 Annex F",
             nf_fax_v8_modulations((nf_fax_t *) s->be));
        s->v34 = 1;
        s->stats.v34 = 1;
        s->want_ecm = 1;                    /* Annex F requires ECM */
        s->ecm = 1;
        s->modem = NF_MODEM_V34;
        s->bit_rate = 24000;
        s->stats.bit_rate = s->bit_rate;
        s->mops->set_tx_type(s->be, NF_MODEM_V34, 0, 0, 0);   /* startup */
        s->mops->set_rx_type(s->be, NF_MODEM_V34, 0, 0, 0);
        arm_timeout(s, 40000);
        s->substate = s->calling ? C_WAIT_V34 : A_WAIT_V34;
        return;
    }
    if (status == NF_STATUS_TRAINING_SUCCEEDED && s->substate == C_WAIT_V34) {
        v34_refresh_bit_rate(s);
        vlog(s, "V.34 control channel established (%d bps); awaiting DIS",
             s->stats.bit_rate);
        disarm_timeout(s);
        rx_v21(s); arm_timeout(s, 35000); s->substate = C_WAIT_DIS;
        return;
    }
    if (status == NF_STATUS_TRAINING_SUCCEEDED && s->substate == A_WAIT_V34) {
        v34_refresh_bit_rate(s);
        vlog(s, "V.34 control channel established (%d bps); sending DIS",
             s->stats.bit_rate);
        disarm_timeout(s);
        answer_send_dis(s);
        return;
    }
    if (status == NF_STATUS_TRAINING_FAILED &&
        (s->substate == C_WAIT_V34 || s->substate == A_WAIT_V34)) {
        vlog(s, "V.34 startup failed");
        finish(s, NF_T30_ERR_GENERAL);
        return;
    }
    if (status == NF_STATUS_TRAINING_SUCCEEDED && s->substate == C_WAIT_V8) {
        vlog(s, "V8 negotiated cf=%d mod=0x%04X", nf_fax_v8_call_function((nf_fax_t *) s->be),
             nf_fax_v8_modulations((nf_fax_t *) s->be));
        rx_v21(s); arm_timeout(s, 35000); s->substate = C_WAIT_DIS;
        return;
    }
    if (status == NF_STATUS_TRAINING_FAILED && s->substate == C_WAIT_V8) {
        vlog(s, "V8 not negotiated (non-V8 peer or timeout), falling back to CNG/DIS");
        s->mops->set_tx_type(s->be, NF_MODEM_CNG, 0, 0, 0);
        rx_v21(s); arm_timeout(s, 35000); s->substate = C_WAIT_DIS;
        return;
    }
    if (status == NF_STATUS_TRAINING_SUCCEEDED && s->substate == A_SEND_V8) {
        vlog(s, "V8 negotiated cf=%d mod=0x%04X", nf_fax_v8_call_function((nf_fax_t *) s->be),
             nf_fax_v8_modulations((nf_fax_t *) s->be));
        answer_send_dis(s);       /* V.8 already served CED's role */
        return;
    }
    if (status == NF_STATUS_TRAINING_FAILED && s->substate == A_SEND_V8) {
        vlog(s, "V8 not negotiated (non-V8 peer or timeout), falling back to CED/DIS");
        s->mops->set_tx_type(s->be, NF_MODEM_CED, 0, 0, 0);
        s->substate = A_SEND_CED;
        return;
    }
    if (status == NF_STATUS_SEND_STEP_COMPLETE) {
        switch (s->substate) {
        case A_SEND_CED:
            answer_send_dis(s);
            break;
        case A_SEND_DIS:
            /* T.30 phase B: DIS is repeated every ~3 s within T1 until a
             * command arrives - a V.8-capable caller may still be classifying
             * our CED when the first DIS goes by. */
            rx_v21(s); arm_timeout(s, 3000); s->substate = A_WAIT_DCS;
            break;
        case C_SEND_DCS:
            if (s->v34) {
                /* Annex F: no TCF - await CFR on the control channel */
                rx_v21(s); arm_timeout(s, 10000); s->substate = C_WAIT_CFR;
                break;
            }
            /* Brief gap so the V.21 closing flags fully clear at the far end
             * before the image-modem TCF carrier starts. */
            s->mops->set_tx_type(s->be, NF_MODEM_PAUSE, 0, 200, 0);
            s->substate = C_DCS_PAUSE;
            break;
        case C_DCS_PAUSE:
            s->tcf_left = (3 * s->bit_rate) / 2;
            tx_image(s, 0);             /* long training + zeros */
            s->substate = C_SEND_TCF;
            break;
        case C_SEND_TCF:
            rx_v21(s); arm_timeout(s, 7000); s->substate = C_WAIT_CFR;
            break;
        case C_SEND_IMAGE:
            /* image sent -> post-message: EOP (last) or MPS (more pages) */
            tx_v21_frames(s);
            s->last_post_fcf = (s->cur_page + 1 < s->pages_total) ? FCF_MPS : FCF_EOP;
            send_simple(s, s->last_post_fcf);
            s->post_retries = 0;
            s->substate = C_SEND_POST;
            break;
        case C_SEND_POST:
            rx_v21(s); arm_timeout(s, 6000); s->substate = C_WAIT_MCF;
            break;
        case C_SEND_DCN:
            finish(s, NF_T30_OK);
            break;
        case C_SEND_DTC:
            /* Poll request sent; await the answerer's DCS and receive. */
            rx_v21(s); arm_timeout(s, 35000); s->substate = A_WAIT_DCS;
            break;
        case C_SEND_ECM:                 /* high-speed block fully streamed */
            s->post_retries = 0;
            ecm_send_pps(s);
            break;
        case C_SEND_PPS:
            rx_v21(s); arm_timeout(s, 6000); s->substate = C_WAIT_PPS_RESP;
            break;
        case C_SEND_CTC:
            rx_v21(s); arm_timeout(s, 6000); s->substate = C_WAIT_CTR;
            break;
        case C_SEND_EOR:
            rx_v21(s); arm_timeout(s, 6000); s->substate = C_WAIT_EOR;
            break;
        case A_SEND_CFR:
            if (s->ecm) { ecm_rx_begin_page(s); ecm_rx_listen(s); }
            else { rx_image(s, 1); s->substate = A_RECV_IMAGE; }
            break;
        case A_SEND_FTT:
        case A_SEND_RTN:
            rx_v21(s); arm_timeout(s, 35000); s->substate = A_WAIT_DCS;
            break;
        case A_SEND_MCF:
            if (s->last_post_fcf == FCF_MPS) {
                start_rx_page(s);
                rx_image(s, 1); s->substate = A_RECV_IMAGE;   /* next page, same params */
            } else if (s->last_post_fcf == FCF_EOM) {
                answer_send_dis(s);                            /* renegotiate (Phase B) */
            } else {
                rx_v21(s); arm_timeout(s, 7000); s->substate = A_WAIT_DCN;
            }
            break;
        case A_SEND_PPR:
            ecm_rx_listen(s);            /* go back and receive the resent frames */
            break;
        case A_SEND_CTR:
            ecm_rx_listen(s);            /* resume receiving the block at the new rate */
            break;
        case A_SEND_ERR:
            /* End of retransmission acknowledged: await the sender's next move
             * (DCN, or the next block/page). */
            rx_v21(s); arm_timeout(s, 7000); s->substate = A_WAIT_PPS;
            break;
        case A_SEND_ECM_MCF:
            if (s->last_pps_fcf2 == FCF_NULL) {
                ecm_rx_listen(s);        /* another block of the same page */
            } else {
                ecm_finish_page(s);
                if (s->last_pps_fcf2 == FCF_MPS)      { ecm_rx_begin_page(s); ecm_rx_listen(s); }
                else if (s->last_pps_fcf2 == FCF_EOM) { answer_send_dis(s); }
                else { rx_v21(s); arm_timeout(s, 7000); s->substate = A_WAIT_DCN; }  /* EOP */
            }
            break;
        case A_REACK:                    /* a repeated command was re-acknowledged */
            if (s->reack_return == A_RECV_ECM)
                ecm_rx_listen(s);                       /* resume receiving the block */
            else
                { rx_v21(s); arm_timeout(s, 7000); s->substate = A_WAIT_DCN; }
            break;
        }
        return;
    }
    if (status == NF_STATUS_CARRIER_DOWN) {
        if (s->substate == A_RECV_TCF) {
            eval_tcf(s);
        } else if (s->substate == A_RECV_IMAGE) {
            finish_rx_page(s);
            rx_v21(s); arm_timeout(s, 7000); s->substate = A_WAIT_POST;
        } else if (s->substate == A_RECV_ECM) {
            /* High-speed block finished; drop to V.21 to receive the PPS. */
            rx_v21(s); arm_timeout(s, 7000); s->substate = A_WAIT_PPS;
        }
    }
}

/* Judge the received TCF and answer CFR or FTT. Called on image carrier-down
 * and, when training failed so no carrier-down ever reaches the protocol (the
 * fast modem stays in the parallel-rx arrangement), by the T2-style timeout. */
static void eval_tcf(nf_t30_t *s)
{
    disarm_timeout(s);
    if (s->tcf_zeros > s->tcf_most) s->tcf_most = s->tcf_zeros;
    int pass = s->tcf_most >= s->bit_rate;
    vlog(s, "TCF most_zeros=%ld need>=%d -> %s", s->tcf_most, s->bit_rate,
         pass ? "CFR" : "FTT");
    tx_v21_frames(s);
    if (pass) { send_simple(s, FCF_CFR); s->substate = A_SEND_CFR; }
    else      { send_simple(s, FCF_FTT); s->substate = A_SEND_FTT; }
}

static int on_get_bit(void *user)
{
    nf_t30_t *s = user;
    if (s->substate == C_SEND_TCF) {
        if (s->tcf_left-- <= 0) return NF_GET_BIT_END;
        return 0;
    }
    if (s->substate == C_SEND_IMAGE) {
        if (s->enc_bit >= s->enc_len * 8) return NF_GET_BIT_END;
        int bit = (s->enc[s->enc_bit >> 3] >> (s->enc_bit & 7)) & 1;
        s->enc_bit++;
        return bit;
    }
    return NF_GET_BIT_END;
}

static void on_put_bit(void *user, int bit)
{
    nf_t30_t *s = user;
    if (s->substate == A_RECV_TCF) {
        if (bit) { if (s->tcf_zeros > s->tcf_most) s->tcf_most = s->tcf_zeros; s->tcf_zeros = 0; }
        else s->tcf_zeros++;
        return;
    }
    if (s->substate == A_RECV_IMAGE) {
        /* NF_T30_RX_FLIP test hook: corrupt the first page attempt so the
         * copy-quality/RTN/retransmit path can be exercised deterministically */
        if (s->rx_flip_n > 0 && ++s->rx_flip_ctr >= s->rx_flip_n) {
            s->rx_flip_ctr = 0;
            bit ^= 1;
        }
        size_t byte = s->rxbits >> 3;
        if (byte >= NF_RX_MAX_PAGE) return;   /* cap runaway image growth */
        if (grow_bytes(&s->rxbuf, &s->rxcap, byte + 1) != 0)
            return;                           /* OOM: drop bit, keep old buffer */
        if ((s->rxbits & 7) == 0) s->rxbuf[byte] = 0;
        s->rxbuf[byte] |= (uint8_t)(bit << (s->rxbits & 7));
        s->rxbits++;
    }
}

static void on_timer(void *user, int samples)
{
    nf_t30_t *s = user;
    if (s->timeout > 0) {
        s->timeout -= samples;
        if (s->timeout <= 0) {
            s->timeout = 0;
            vlog(s, "timeout in state %d", s->substate);
            if (s->substate == A_RECV_TCF) {
                eval_tcf(s);             /* training never happened: FTT */
                return;
            }
            /* T.30 post-message command retransmission (T4 timer, up to 3 tries):
             * the far end's MCF/PPR can be delayed, especially over a T.38 gateway
             * that adds modem-playout latency. Resend EOP/MPS (non-ECM) or PPS
             * (ECM) before giving up. */
            if (s->substate == C_WAIT_MCF && s->post_retries < 3) {
                s->post_retries++;
                vlog(s, "no MCF; resending post-message (try %d)", s->post_retries);
                tx_v21_frames(s);
                send_simple(s, s->last_post_fcf);
                s->substate = C_SEND_POST;   /* step-complete re-arms the wait */
                return;
            }
            if (s->substate == C_WAIT_PPS_RESP && s->post_retries < 3) {
                s->post_retries++;
                vlog(s, "no PPS response; resending PPS (try %d)", s->post_retries);
                ecm_send_pps(s);             /* -> C_SEND_PPS -> re-arm on step-complete */
                return;
            }
            if (s->substate == C_WAIT_CTR && s->ctc_retries < 3) {
                s->ctc_retries++;
                vlog(s, "no CTR; resending CTC (try %d)", s->ctc_retries);
                ecm_send_ctc(s);             /* -> C_SEND_CTC -> re-arm on step-complete */
                return;
            }
            /* T.30 T1: the answerer repeats DIS every ~3 s until it hears a
             * command (not when polling - that wait is for a DCS after DTC). */
            if (s->substate == A_WAIT_DCS && !s->poll_receive && s->dis_retries < 10) {
                s->dis_retries++;
                vlog(s, "no DCS; repeating DIS (try %d)", s->dis_retries);
                answer_send_dis(s);          /* -> A_SEND_DIS -> re-arm on step-complete */
                return;
            }
            send_simple(s, FCF_DCN);
            finish(s, NF_T30_ERR_TIMEOUT);
        }
    }
}

/* ── public API ────────────────────────────────────────────────────── */

/* Parse a comma-separated list of frame numbers (for ECM fault injection). */
static void parse_drop_list(const char *env, int *arr, int *n)
{
    *n = 0;
    if (!env) return;
    for (const char *p = env; *p && *n < 64; ) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        arr[(*n)++] = atoi(p);
        while (*p && *p != ',') p++;
    }
}

nf_t30_t *nf_t30_init(int calling_party)
{
    nf_t30_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->calling = calling_party;
    s->acked_page8 = s->acked_block8 = -1;   /* nothing acknowledged yet */
    s->supported_res = NF_RES_STANDARD | NF_RES_FINE;
    s->want_ecm = 1;                     /* advertise + use ECM when the far end can */
    strcpy(s->ident, "nf_fax");
    nf_fax_iface_t iface = {
        .user = s, .hdlc_accept = on_hdlc, .non_ecm_get_bit = on_get_bit,
        .non_ecm_put_bit = on_put_bit, .front_end_status = on_status, .timer_update = on_timer,
        .hdlc_get_frame = on_get_frame,
    };
    s->mops = nf_fax_ops();                       /* audio (V-series modem) backend */
    s->be   = nf_fax_init(calling_party, &iface);
    s->encoding = NF_T4_COMPRESSION_2D;
    /* Optional fault injection for testing ECM retransmission. */
    parse_drop_list(getenv("NF_ECM_DROP_TX"), s->drop_tx, &s->n_drop_tx);
    parse_drop_list(getenv("NF_ECM_DROP_RX"), s->drop_rx, &s->n_drop_rx);
    if (getenv("NF_ECM_DROP_ABOVE"))
        s->drop_above_rate = atoi(getenv("NF_ECM_DROP_ABOVE"));
    if (getenv("NF_T30_RX_FLIP"))
        s->rx_flip_n = atoi(getenv("NF_T30_RX_FLIP"));
    if (getenv("NF_T30_V34"))
        s->v34_enable = atoi(getenv("NF_T30_V34")) != 0;
    return s;
}

void nf_t30_free(nf_t30_t *s)
{
    if (!s) return;
    if (s->tif_in) TIFFClose(s->tif_in);
    if (s->tif_out) TIFFClose(s->tif_out);
    nf_t4_dec_free(s->dec);
    free(s->enc); free(s->rxbuf); free(s->rxrows); free(s->color_rgb);
    s->mops->free(s->be);
    free(s);
}

void nf_t30_set_tx_ident(nf_t30_t *s, const char *id) { strncpy(s->ident, id, sizeof s->ident - 1); }
void nf_t30_set_supported_resolutions(nf_t30_t *s, int mask) { s->supported_res = mask | NF_RES_STANDARD; }
void nf_t30_set_transmit_on_idle(nf_t30_t *s, int on) { s->mops->set_transmit_on_idle(s->be, on); }
void nf_t30_set_verbose(nf_t30_t *s, int on) { s->verbose = on; }
void nf_t30_set_log_tag(nf_t30_t *s, const char *tag)
{
    if (tag) { strncpy(s->log_tag, tag, sizeof s->log_tag - 1); s->log_tag[sizeof s->log_tag - 1] = '\0'; }
    else     { s->log_tag[0] = '\0'; }
}
void nf_t30_set_ecm(nf_t30_t *s, int on) { s->want_ecm = on; }
void nf_t30_set_v34(nf_t30_t *s, int on) { s->v34_enable = on; }
void nf_t30_set_require_v34(nf_t30_t *s, int on) { s->require_v34 = on; }
void nf_t30_set_color(nf_t30_t *s, int tx_is_color) { s->tx_is_color = tx_is_color; }
void nf_t30_set_gray(nf_t30_t *s, int tx_is_gray) { s->tx_is_gray = tx_is_gray; }
void nf_t30_set_color_capable(nf_t30_t *s, int on) { s->color_capable = on; }
void nf_t30_set_color_quality(nf_t30_t *s, int q) { s->color_quality = q; }
void nf_t30_set_file_capable(nf_t30_t *s, int on) { s->file_capable = on; }
void nf_t30_set_poll_serve(nf_t30_t *s, int on) { s->poll_serve = on; }
void nf_t30_set_poll_receive(nf_t30_t *s, int on) { s->poll_receive = on; }
void nf_t30_set_phase_b_handler(nf_t30_t *s, void (*h)(void *), void *u) { s->phase_b = h; s->phase_b_user = u; }
void nf_t30_set_phase_e_handler(nf_t30_t *s, void (*h)(void *, int), void *u) { s->phase_e = h; s->phase_e_user = u; }
int  nf_t30_get_result(nf_t30_t *s) { return s->result; }
void nf_t30_get_stats(nf_t30_t *s, nf_t30_stats_t *st) { *st = s->stats; }
const char *nf_t30_completion_to_str(int r) { return r == NF_T30_OK ? "OK" : "error"; }

/* Human-readable name of the image modem currently selected for the page. */
const char *nf_t30_modem_name(const nf_t30_t *s)
{
    if (s->v34) return "V.34";
    switch (s->modem) {
    case NF_MODEM_V17:    return "V.17";
    case NF_MODEM_V29:    return "V.29";
    case NF_MODEM_V27TER: return "V.27ter";
    default:              return "?";
    }
}

int nf_t30_tx_progress(nf_t30_t *s, int *page, int *pages,
                       size_t *sent, size_t *total, int *bit_rate)
{
    size_t done;
    if (s->substate == C_SEND_ECM) {
        done = s->ecm_tx_base + (size_t) s->ecm_cursor * ECM_OCTETS;
        if (done > s->enc_len) done = s->enc_len;
    } else if (s->substate == C_SEND_IMAGE) {
        done = s->enc_bit / 8;
    } else {
        return 0;                      /* not transmitting image data right now */
    }
    if (s->v34) v34_refresh_bit_rate(s);   /* the real V.34 rate lives in the session */
    if (page)     *page     = s->cur_page;
    if (pages)    *pages    = s->pages_total;
    if (sent)     *sent     = done;
    if (total)    *total    = s->enc_len;
    /* For V.34 the negotiated primary rate is in stats.bit_rate (s->bit_rate
     * stays the placeholder 24000); classic G3 keeps its rate in bit_rate. */
    if (bit_rate) *bit_rate = (s->v34 && s->stats.bit_rate > 0) ? s->stats.bit_rate
                                                                : s->bit_rate;
    return 1;
}

static void open_tx(nf_t30_t *s, const char *path)
{
    strncpy(s->tx_file, path, sizeof s->tx_file - 1);
    if (s->tif_in) TIFFClose(s->tif_in);
    s->tif_in = TIFFOpen(path, "r");
    s->pages_total = 0; s->cur_page = 0;
    if (s->tif_in) {
        do s->pages_total++; while (TIFFReadDirectory(s->tif_in));
        TIFFSetDirectory(s->tif_in, 0);
        /* Classify resolution from page 0 so DCS (built before the page is
         * encoded) carries the right resolution bit. */
        uint32_t w = 0; float yr = 0; uint16_t unit = RESUNIT_INCH;
        TIFFGetField(s->tif_in, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetFieldDefaulted(s->tif_in, TIFFTAG_RESOLUTIONUNIT, &unit);
        TIFFGetField(s->tif_in, TIFFTAG_YRESOLUTION, &yr);
        double ydpi = (unit == RESUNIT_CENTIMETER) ? yr * 2.54 : yr;
        s->nf_res = classify_res((int) w, ydpi);
        res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
        s->image_width = (int) w;
    }
}

void nf_t30_set_tx_file(nf_t30_t *s, const char *path) { open_tx(s, path); }
void nf_t30_select_tx_file(nf_t30_t *s, const char *path) { open_tx(s, path); }
void nf_t30_set_rx_file(nf_t30_t *s, const char *path) { strncpy(s->rx_file, path, sizeof s->rx_file - 1); }

/* Binary file transfer: the tx document is an arbitrary file (not a TIFF). */
void nf_t30_set_file_tx(nf_t30_t *s, const char *path)
{
    strncpy(s->tx_file, path, sizeof s->tx_file - 1);
    if (s->tif_in) { TIFFClose(s->tif_in); s->tif_in = NULL; }
    s->pages_total = 1; s->cur_page = 0;
    s->tx_is_file = 1;
}
void nf_t30_set_file_rx(nf_t30_t *s, const char *path)
{
    strncpy(s->rx_file, path, sizeof s->rx_file - 1);
    s->file_capable = 1;
}

/* Kick off the call on the first tx/rx pump. T.30 Annex F (V.8) is only
 * attempted on the audio backend, and only when V.34 is enabled: V.8 exists
 * solely to select V.34 half-duplex, and a caller that answers ANSam with a
 * classic-only CM confuses real SG3 machines (a Brother goes mute and drops
 * the call instead of falling back to DIS). Without V.34 we behave as a
 * plain-G3 terminal - ignore ANSam, send CNG, wait for V.21 DIS - which is
 * the fallback path every SG3 answerer already handles. */
static void start(nf_t30_t *s)
{
    if (s->mops == nf_fax_ops() && s->v34_enable) {
        nf_fax_set_v8_caps((nf_fax_t *) s->be, v8_capability_mask(s),
                           s->poll_receive ? NF_V8_CALL_T30_RX : NF_V8_CALL_T30_TX);
        s->mops->set_tx_type(s->be, NF_MODEM_V8, 0, 0, 0);
        s->mops->set_rx_type(s->be, NF_MODEM_V8, 0, 0, 0);
        s->substate = s->calling ? C_WAIT_V8 : A_SEND_V8;
        return;
    }
    if (s->calling) {
        s->mops->set_tx_type(s->be, NF_MODEM_CNG, 0, 0, 0);
        rx_v21(s); arm_timeout(s, 35000);
        s->substate = C_WAIT_DIS;
    } else {
        s->mops->set_tx_type(s->be, NF_MODEM_CED, 0, 0, 0);
        s->substate = A_SEND_CED;
    }
}

int nf_t30_tx(nf_t30_t *s, int16_t *amp, int max_len)
{
    if (s->substate == ST_INIT) start(s);
    return s->mops->tx(s->be, amp, max_len);
}
int nf_t30_rx(nf_t30_t *s, const int16_t *amp, int len)
{
    if (s->substate == ST_INIT) start(s);
    return s->mops->rx(s->be, amp, len);
}

/* ── T.38 backend ────────────────────────────────────────────────────────── */

void nf_t30_t38_enable(nf_t30_t *s, int redundancy, int far_max_datagram,
                       void (*send)(void *user, const uint8_t *dgram, int len),
                       void *send_user)
{
    if (s->be && s->mops && s->mops->free) s->mops->free(s->be);
    /* Same up-callbacks as the audio backend; only the physical layer changes. */
    nf_fax_iface_t iface = {
        .user = s, .hdlc_accept = on_hdlc, .non_ecm_get_bit = on_get_bit,
        .non_ecm_put_bit = on_put_bit, .front_end_status = on_status,
        .timer_update = on_timer, .hdlc_get_frame = on_get_frame,
    };
    s->be = nf_t38_init(s->calling, &iface, redundancy, far_max_datagram, send, send_user);
    s->mops = nf_t38_ops();
}

void nf_t30_t38_rx_datagram(nf_t30_t *s, const uint8_t *dgram, int len)
{
    if (s->substate == ST_INIT) start(s);
    nf_t38_rx_datagram(s->be, dgram, len);
}

void nf_t30_t38_pump(nf_t30_t *s, int ms)
{
    if (s->substate == ST_INIT) start(s);
    nf_t38_pump(s->be, ms);
}
