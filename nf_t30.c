#define _GNU_SOURCE
#include "nf_t30.h"
#include "nf_fax.h"
#include "nf_t38.h"
#include "nf_t4.h"
#include "nf_color.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    C_WAIT_DIS, C_SEND_DCS, C_DCS_PAUSE, C_SEND_TCF, C_WAIT_CFR, C_SEND_IMAGE, C_SEND_POST,
    C_WAIT_MCF, C_SEND_DCN, C_SEND_DTC,
    /* caller ECM */
    C_SEND_ECM, C_SEND_PPS, C_WAIT_PPS_RESP,
    /* answerer */
    A_SEND_CED, A_SEND_DIS, A_WAIT_DCS, A_RECV_TCF, A_SEND_CFR, A_SEND_FTT,
    A_RECV_IMAGE, A_WAIT_POST, A_SEND_MCF, A_SEND_RTN, A_WAIT_DCN,
    /* answerer ECM */
    A_RECV_ECM, A_WAIT_PPS, A_SEND_PPR, A_SEND_ECM_MCF,
    A_REACK,                       /* re-sending a response to a repeated command */
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

    char ident[24];
    int  supported_res;            /* NF_RES_* mask we advertise */

    uint8_t local_dis[24]; int local_dis_len;
    uint8_t dcs[24];       int dcs_len;
    uint8_t far_dis[24];   int far_dis_len;
    int dis_received;              /* X bit for our outbound frames */

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
    int  drop_rx[64], n_drop_rx;
    uint8_t dropped_tx[256], dropped_rx[256];

    /* timers */
    long timeout;                  /* samples remaining; <=0 = disarmed */

    void (*phase_b)(void *); void *phase_b_user;
    void (*phase_e)(void *, int); void *phase_e_user;

    char log_tag[256];             /* optional per-call log prefix (e.g. Call-ID) */
    nf_t30_stats_t stats;
};

static void vlog(nf_t30_t *s, const char *fmt, ...)
{
    if (!s->verbose) return;
    va_list ap; va_start(ap, fmt);
    if (s->log_tag[0]) fprintf(stderr, "[%s] ", s->log_tag);
    fprintf(stderr, "[%s] ", s->calling ? "CALL" : "ANSW");
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
}

static void eval_tcf(nf_t30_t *s);

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

static void tx_v21_frames(nf_t30_t *s) { s->mops->set_tx_type(s->be, NF_MODEM_V21, 300, 0, 1); }
static void rx_v21(nf_t30_t *s)        { s->mops->set_rx_type(s->be, NF_MODEM_V21, 300, 0, 1); }
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

static void arm_timeout(nf_t30_t *s, int ms) { s->timeout = (long) ms * 8; }
static void disarm_timeout(nf_t30_t *s) { s->timeout = 0; }

static void finish(nf_t30_t *s, int result)
{
    if (s->finished) return;
    s->finished = 1;
    s->result = result;
    s->substate = ST_DONE;
    tx_none(s); rx_none(s);
    s->mops->set_tx_type(s->be, NF_MODEM_DONE, 0, 0, 0);
    vlog(s, "Phase E: result=%d pages tx=%d rx=%d", result, s->stats.pages_tx, s->stats.pages_rx);
    if (s->phase_e) s->phase_e(s->phase_e_user, result);
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
    if (s->rxrow_n >= s->rxrow_cap) {
        s->rxrow_cap = s->rxrow_cap ? s->rxrow_cap * 2 : 256;
        s->rxrows = realloc(s->rxrows, (size_t) s->rxrow_cap * s->rxstride);
    }
    memcpy(s->rxrows + (size_t) s->rxrow_n * s->rxstride, rowp, s->rxstride);
    s->rxrow_n++;
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

static void parse_dcs(nf_t30_t *s, const uint8_t *msg, int len)
{
    memset(s->far_dis, 0, sizeof s->far_dis);
    int n = len > 24 ? 24 : len;
    memcpy(s->far_dis, msg, n); s->far_dis_len = n;   /* reuse buffer for far DCS */

    uint8_t modembits = msg[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3);
    s->fallback = FB_V27_START + 1;
    for (size_t i = 0; i < sizeof FB / sizeof FB[0]; i++)
        if (FB[i].dcs == modembits) { s->fallback = (int) i; break; }
    s->modem = FB[s->fallback].modem;
    s->bit_rate = FB[s->fallback].rate;
    s->ecm = test_bit(s->far_dis, n, 27) ? 1 : 0;
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
    vlog(s, "DCS: modem=%d rate=%d enc=%d res=%d width=%d",
         s->modem, s->bit_rate, s->encoding, s->nf_res, s->image_width);
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
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, s->dcs, s->dcs_len);
    s->substate = C_SEND_DCS;            /* await send-step-complete -> TCF */
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
    if (s->phase_b) s->phase_b(s->phase_b_user);
    /* When serving a poll, the caller must have advertised it can receive. */
    if (s->poll_serve && !test_bit(s->far_dis, n, 10)) {
        vlog(s, "poll: caller is not ready to receive");
        send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_INCOMPAT); return;
    }
    int fb = next_allowed_fb(parse_dis_pick_fallback(s));
    if (fb < 0) { send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_INCOMPAT); return; }
    s->fallback = fb;
    s->modem = FB[fb].modem; s->bit_rate = FB[fb].rate;
    s->stats.bit_rate = s->bit_rate;
    /* ECM is used only if both ends advertise it; T.6 only inside ECM. */
    s->ecm = (s->want_ecm && test_bit(s->far_dis, n, 27)) ? 1 : 0;
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
            send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_INCOMPAT); return;
        }
    } else if (s->tx_is_gray) {
        if (s->ecm && test_bit(s->far_dis, n, 68)) {
            s->kind = PK_GRAY;
            s->nf_res = NF_RES_FINE; res_to_dpi(s->nf_res, &s->x_res, &s->y_res);
        } else {
            vlog(s, "far end cannot receive greyscale JPEG (ECM=%d)", s->ecm);
            send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_INCOMPAT); return;
        }
    } else if (s->tx_is_file) {
        if (s->ecm && test_bit(s->far_dis, n, 53)) {
            s->kind = PK_FILE;
        } else {
            vlog(s, "far end cannot receive a binary file (ECM=%d)", s->ecm);
            send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_INCOMPAT); return;
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
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, s->local_dis, s->local_dis_len);
    s->substate = A_SEND_DIS;
}

/* Caller polling for a document: send our capabilities as a DTC (build_dis with
 * dis_received already set emits FCF 0x81 and our "ready to receive" bit), then
 * wait for the answerer's DCS and receive like a normal answerer would. */
static void poll_send_dtc(nf_t30_t *s)
{
    build_dis(s);
    tx_v21_frames(s);
    s->mops->send_hdlc(s->be, s->local_dis, s->local_dis_len);
    s->substate = C_SEND_DTC;
}

/* ── ECM transmit (caller) ─────────────────────────────────────────── */

/* Carve the next "partial page" (a block of up to 256 FCD frames) out of the
 * encoded page (s->enc) starting at ecm_tx_base. */
static void ecm_build_block(nf_t30_t *s)
{
    size_t remaining = s->enc_len - s->ecm_tx_base;
    size_t cap = (size_t) 256 * ECM_OCTETS;
    size_t this_block = remaining < cap ? remaining : cap;
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
    if (s->substate != C_SEND_ECM) return 0;
    while (s->ecm_cursor < s->ecm_frames) {
        int i = s->ecm_cursor++;
        if (!s->ecm_send[i]) continue;
        /* This frame is part of the burst, so it counts in the PPS frame total
         * (the receiver locks the block size from the first PPS) even if we then
         * drop it to simulate transit loss. */
        s->ecm_burst++;
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
     * so a healthy block is unaffected; on expiry on_timer disconnects. */
    arm_timeout(s, 15000);
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

static void ecm_rx_append(nf_t30_t *s, const uint8_t *data, int n)
{
    if (n <= 0) return;
    if (s->rx_ecm_len + (size_t) n > s->rxcap) {
        while (s->rxcap < s->rx_ecm_len + (size_t) n) s->rxcap = s->rxcap ? s->rxcap * 2 : 8192;
        s->rxbuf = realloc(s->rxbuf, s->rxcap);
    }
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

static void on_hdlc(void *user, const uint8_t *msg, int len, int ok)
{
    nf_t30_t *s = user;
    vlog(s, "rx frame fcf=0x%02x len=%d ok=%d (state %d)", len>=3?msg[2]:0, len, ok, s->substate);
    if (!ok || len < 3) return;
    int fcf = msg[2] | 0x01;             /* ignore X bit for dispatch */
    disarm_timeout(s);

    /* ECM high-speed image frames arrive only while receiving an ECM block. */
    if (s->substate == A_RECV_ECM) {
        int raw = msg[2] & 0xFE;
        if (raw == T4_FCD) { ecm_store_fcd(s, msg, len); return; }
        if (raw == T4_RCP) return;       /* block end is driven by carrier-down */
    }

    switch (s->substate) {
    case C_WAIT_DIS:
        if (fcf == (FCF_DIS | 1)) {
            if (s->poll_receive) {
                /* We called to pull a document. The answerer must offer one
                 * (DIS bit 9); then send our DTC and switch to receiving. */
                int n = len > 24 ? 24 : len;
                memcpy(s->far_dis, msg, n); s->far_dis_len = n;
                s->dis_received = 1;
                if (s->phase_b) s->phase_b(s->phase_b_user);
                if (!test_bit(s->far_dis, n, 9)) {
                    vlog(s, "poll: answerer has no document to transmit");
                    send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_INCOMPAT); return;
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
                send_simple(s, FCF_DCN); finish(s, NF_T30_ERR_GENERAL); return;
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
            if (++s->ppr_count > 16) {               /* avoid an endless retry loop */
                tx_v21_frames(s); send_simple(s, FCF_DCN);
                finish(s, NF_T30_ERR_GENERAL);
                return;
            }
            ecm_start_burst(s);                      /* resend the still-bad frames */
        } else if (fcf == (FCF_RNR | 1)) {
            ecm_send_pps(s);                         /* receiver busy: re-poll with PPS */
        }
        break;

    case A_WAIT_DCS:
    case A_RECV_TCF:                     /* caller may repeat DCS+TCF if we
                                          * stayed silent on a failed TCF */
        if (fcf == (FCF_DCS | 1)) {
            parse_dcs(s, msg, len);
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
    if (status == NF_STATUS_SEND_STEP_COMPLETE) {
        switch (s->substate) {
        case A_SEND_CED:
            answer_send_dis(s);
            break;
        case A_SEND_DIS:
            rx_v21(s); arm_timeout(s, 35000); s->substate = A_WAIT_DCS;
            break;
        case C_SEND_DCS:
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
        if (byte >= s->rxcap) {
            s->rxcap = s->rxcap ? s->rxcap * 2 : 8192;
            s->rxbuf = realloc(s->rxbuf, s->rxcap);
        }
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
    if (getenv("NF_T30_RX_FLIP"))
        s->rx_flip_n = atoi(getenv("NF_T30_RX_FLIP"));
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
    if (page)     *page     = s->cur_page;
    if (pages)    *pages    = s->pages_total;
    if (sent)     *sent     = done;
    if (total)    *total    = s->enc_len;
    if (bit_rate) *bit_rate = s->bit_rate;
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

/* Kick off the call on the first tx/rx pump. */
static void start(nf_t30_t *s)
{
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
