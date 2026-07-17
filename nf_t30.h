#ifndef NF_T30_H
#define NF_T30_H

#include <stdint.h>
#include <stddef.h>   /* size_t */

/*
 * nf_t30 - a compact, non-ECM ITU-T T.30 fax protocol engine plus the small
 * wrapper that ties it to the nf_fax driver. It reproduces spandsp's t30.c on
 * the wire (same DIS/DCS bits, frames, TCF and modem-switch timing) but with a
 * simpler internal state machine. It owns an nf_fax instance and drives a page
 * through nf_t4 (image codec) + libtiff (page I/O), so this object is the whole
 * fax engine the application talks to - the analogue of spandsp's fax_state.
 *
 * Non-ECM only; ECM is not advertised/negotiated.
 */

/* Supported-resolution mask (our own; mapped to T.30 DIS/DCS bits internally). */
#define NF_RES_STANDARD   0x01     /* 204 x  98 dpi, 1728 wide */
#define NF_RES_FINE       0x02     /* 204 x 196 dpi, 1728 wide */
#define NF_RES_SUPERFINE  0x04     /* 204 x 391 dpi, 1728 wide */
#define NF_RES_300        0x08     /* 300 x 300 dpi, 2592 wide */
#define NF_RES_400        0x10     /* 400 x 400 dpi, 3456 wide */

/* Completion codes (subset; OK is 0). */
#define NF_T30_OK              0
#define NF_T30_ERR_GENERAL     1
#define NF_T30_ERR_INCOMPAT    2   /* no common modem/capability */
#define NF_T30_ERR_TIMEOUT     3
#define NF_T30_ERR_NORESSUPPORT 4
#define NF_T30_ERR_FILE        5

typedef struct {
    int pages_tx, pages_rx;
    int bit_rate;
    int width, length;             /* most recent page, pixels */
    int x_resolution, y_resolution;/* dpi */
    int v34;                       /* 1 if this call ran over V.34 (Super G3) */
    int ecm;                       /* 1 if the transfer used ECM */
    char rx_ident[24];             /* remote station id from TSI/CSI/CIG ("" if none) */
} nf_t30_stats_t;

typedef struct nf_t30 nf_t30_t;

/* calling_party = 1 for the side that places the call (the image sender, in
 * this tool); 0 for the answerer (the image receiver). */
nf_t30_t *nf_t30_init(int calling_party);
void      nf_t30_free(nf_t30_t *s);

void nf_t30_set_tx_ident(nf_t30_t *s, const char *ident);
void nf_t30_set_supported_resolutions(nf_t30_t *s, int mask);
void nf_t30_set_tx_file(nf_t30_t *s, const char *path);   /* multipage TIFF in   */
void nf_t30_set_rx_file(nf_t30_t *s, const char *path);   /* multipage TIFF out  */
void nf_t30_set_transmit_on_idle(nf_t30_t *s, int on);
void nf_t30_set_verbose(nf_t30_t *s, int on);
/* Optional prefix prepended (as "[tag] ") to every verbose log line, so the
 * output of concurrent calls can be told apart (e.g. set to the SIP Call-ID).
 * Empty/NULL = no prefix (the default). */
void nf_t30_set_log_tag(nf_t30_t *s, const char *tag);
/* Error Correction Mode (T.30 Annex A). On by default; used only when the far
 * end also advertises ECM, otherwise the call falls back to non-ECM. */
void nf_t30_set_ecm(nf_t30_t *s, int on);

/* ── V.34 fax (T.30 Annex F "Super G3") ──
 * Off by default (the classic V.21+V.17 path is untouched). When enabled on
 * the audio backend, V.34 half-duplex is offered in the V.8 CM/JM
 * modulation set; if both ends negotiate it, the session runs Annex F: all
 * T.30 frames over the V.34 control channel (no TCF - CFR straight after
 * DCS, DCS speed bits 0), the page as ECM frames over the 24000 bit/s
 * primary channel. ECM is implied. Also enabled by env NF_T30_V34=1. */
void nf_t30_set_v34(nf_t30_t *s, int on);
/* Abort the call (send DCN, fail) before transmitting any page data if V.8 did
 * not negotiate V.34 (Super G3). Use to fail fast instead of falling back to
 * classic G3. Only meaningful on the sending side. */
void nf_t30_set_require_v34(nf_t30_t *s, int on);

/* ── Colour fax (T.30 Annex E / T.42 / T.81 JPEG) ──
 * Colour requires ECM and is negotiated only when both ends support it. */
void nf_t30_set_color(nf_t30_t *s, int tx_is_color);   /* sender: tx doc is colour */
void nf_t30_set_gray(nf_t30_t *s, int tx_is_gray);     /* sender: tx doc as greyscale JPEG */
void nf_t30_set_color_capable(nf_t30_t *s, int on);    /* receiver: advertise colour+grey */
void nf_t30_set_color_quality(nf_t30_t *s, int q);     /* JPEG quality 1..100 (def 85) */

/* ── Arbitrary binary file transfer (private nf<->nf profile, requires ECM) ── */
void nf_t30_set_file_tx(nf_t30_t *s, const char *path); /* sender: send this file */
void nf_t30_set_file_rx(nf_t30_t *s, const char *path); /* receiver: write incoming file here */
void nf_t30_set_file_capable(nf_t30_t *s, int on);      /* receiver: advertise BFT */

/* ── Polling (T.30 DTC): the document flows from the answering station to the
 * calling station, the reverse of a normal call. The poll server answers, holds
 * the tx document, advertises "ready to transmit" (DIS bit 9), and starts
 * sending when the caller polls it with a DTC. The poll receiver places the
 * call, sends a DTC carrying its receive capabilities, then receives. Set on
 * the answering (serve) and calling (receive) engine respectively. ── */
void nf_t30_set_poll_serve(nf_t30_t *s, int on);    /* answerer: transmit when polled */
void nf_t30_set_poll_receive(nf_t30_t *s, int on);  /* caller: poll, then receive     */

/* Phase-B handler fires once the remote DIS/DTC has been received (caller side),
 * before DCS is built - the hook for resolution selection. From inside it,
 * nf_t30_remote_supports() reports what the remote advertised. */
void nf_t30_set_phase_b_handler(nf_t30_t *s, void (*h)(void *user), void *user);
int  nf_t30_remote_supports(nf_t30_t *s, int nf_res);     /* NF_RES_* -> 0/1     */
/* Annex E JPEG capability of the remote (phase-B queries, both imply ECM). */
int  nf_t30_remote_supports_color(nf_t30_t *s);
int  nf_t30_remote_supports_gray(nf_t30_t *s);
/* Choose which tx file to actually send; callable from the phase-B handler. */
void nf_t30_select_tx_file(nf_t30_t *s, const char *path);

void nf_t30_set_phase_e_handler(nf_t30_t *s, void (*h)(void *user, int result), void *user);

int  nf_t30_get_result(nf_t30_t *s);
void nf_t30_get_stats(nf_t30_t *s, nf_t30_stats_t *st);
const char *nf_t30_completion_to_str(int result);

/* Transmit progress, for a sender's progress meter. Returns 1 while image data
 * is being sent (and fills the outputs) or 0 during handshake/post-message.
 * `sent`/`total` are bytes of the current page's encoded image; `page` is
 * 0-based, `pages` the total; `bit_rate` is the negotiated modem rate (bps).
 * Any output pointer may be NULL. */
int  nf_t30_tx_progress(nf_t30_t *s, int *page, int *pages,
                        size_t *sent, size_t *total, int *bit_rate);

/* Name of the image modem selected for the current page ("V.17"/"V.29"/
 * "V.27ter"/"V.34"), for progress display. */
const char *nf_t30_modem_name(const nf_t30_t *s);

/* Sample pump (forwarded to the driver). Audio backend only. */
int  nf_t30_tx(nf_t30_t *s, int16_t *amp, int max_len);
int  nf_t30_rx(nf_t30_t *s, const int16_t *amp, int len);

/* ── T.38 backend ────────────────────────────────────────────────────────
 * Switch this engine from the audio modem backend to a T.38/UDPTL backend.
 * Call once, right after nf_t30_init() and before driving media. `send`
 * transmits a built UDPTL datagram on the wire. Then drive the engine with
 * nf_t30_t38_rx_datagram() (for each received datagram) and nf_t30_t38_pump()
 * on a periodic tick, instead of nf_t30_tx()/nf_t30_rx(). */
void nf_t30_t38_enable(nf_t30_t *s, int redundancy, int far_max_datagram,
                       void (*send)(void *user, const uint8_t *dgram, int len),
                       void *send_user);
void nf_t30_t38_rx_datagram(nf_t30_t *s, const uint8_t *dgram, int len);
void nf_t30_t38_pump(nf_t30_t *s, int ms);

#endif /* NF_T30_H */
