/*
 * T.38 interop oracle: our nf_t30 engine (on the T.38 backend) SENDS a fax to
 * spandsp's real t38_terminal RECEIVER, over an in-process UDPTL bridge. This
 * validates that the IFP content we put on the wire (indicators, HDLC frame bit
 * order, non-ECM data packing) is decodable by a genuine T.38 implementation —
 * the same family as the carrier gateways we talk to live.
 *
 * Bridge: IFP is the interchange unit. Our sender emits UDPTL datagrams; we
 * decode them with our own nf_udptl and hand each IFP to spandsp via
 * t38_core_rx_ifp_packet(). spandsp emits raw IFP via its tx handler; we wrap
 * each in a UDPTL datagram (our nf_udptl) and feed nf_t30_t38_rx_datagram().
 *
 *   run: ./nf_t38oracle <send|recv> <in.tiff> <out.tiff> [noecm] [verbose]
 *        send: our nf_t30 sends -> spandsp receives (validates our IFP output)
 *        recv: spandsp sends -> our nf_t30 receives (validates our IFP decode)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <spandsp.h>
#include "nf_t30.h"
#include "nf_udptl.h"

#define TICK_MS 30

static nf_t30_t       *g_nf;            /* our sender                       */
static t38_core_state_t *g_sp_core;     /* spandsp receiver's T.38 core     */
static nf_udptl_t      g_dec_to_sp;     /* decode our datagrams -> IFP      */
static nf_udptl_t      g_enc_to_nf;     /* encode spandsp IFP -> datagram   */

/* our sender produced a UDPTL datagram: decode and feed each IFP to spandsp */
static void on_ifp_to_sp(void *u, const uint8_t *ifp, int ifp_len, int seq)
{
    (void) u;
    t38_core_rx_ifp_packet(g_sp_core, ifp, ifp_len, (uint16_t) seq);
}
static void nf_send_cb(void *u, const uint8_t *dgram, int len)
{
    (void) u;
    nf_udptl_rx(&g_dec_to_sp, dgram, len, on_ifp_to_sp, NULL);
}

/* spandsp produced an IFP packet: wrap it in UDPTL and feed it to our sender */
static int sp_tx_handler(t38_core_state_t *s, void *u, const uint8_t *buf, int len, int count)
{
    (void) s; (void) u; (void) count;
    uint8_t dg[NF_UDPTL_MAXDGRAM];
    int n = nf_udptl_build(&g_enc_to_nf, buf, len, dg, sizeof dg);
    if (n > 0) nf_t30_t38_rx_datagram(g_nf, dg, n);
    return 0;
}

static int g_nf_done, g_nf_res, g_sp_done, g_sp_res;
static void nf_phe(void *u, int r) { (void) u; g_nf_done = 1; g_nf_res = r; }
static void sp_phe(t30_state_t *s, void *u, int r) { (void) s; (void) u; g_sp_done = 1; g_sp_res = r; }

int main(int argc, char **argv)
{
    const char *mode    = argc > 1 ? argv[1] : "send";
    const char *infile  = argc > 2 ? argv[2] : "doc.pam";
    const char *outfile = argc > 3 ? argv[3] : "t38o_out.tiff";
    int nf_sends = (strcmp(mode, "recv") != 0);   /* default: nf is the sender */
    int noecm = 0, verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "noecm") == 0) noecm = 1;
        else if (strcmp(argv[i], "verbose") == 0) verbose = 1;
    }

    nf_udptl_init(&g_dec_to_sp, 2, NF_UDPTL_MAXDGRAM);
    nf_udptl_init(&g_enc_to_nf, 2, NF_UDPTL_MAXDGRAM);

    /* our nf_t30 on the T.38 backend (caller if it sends, answerer if it recvs) */
    g_nf = nf_t30_init(nf_sends ? 1 : 0);
    nf_t30_set_supported_resolutions(g_nf,
        NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400);
    if (nf_sends) nf_t30_set_tx_file(g_nf, infile);
    else          nf_t30_set_rx_file(g_nf, outfile);
    nf_t30_set_phase_e_handler(g_nf, nf_phe, NULL);
    if (noecm) nf_t30_set_ecm(g_nf, 0);
    if (verbose) nf_t30_set_verbose(g_nf, 1);
    nf_t30_t38_enable(g_nf, 2, NF_UDPTL_MAXDGRAM, nf_send_cb, NULL);

    /* spandsp t38_terminal as the opposite role */
    t38_terminal_state_t *term = t38_terminal_init(NULL, nf_sends ? FALSE : TRUE,
                                                   sp_tx_handler, NULL);
    t30_state_t *t30 = t38_terminal_get_t30_state(term);
    g_sp_core = t38_terminal_get_t38_core_state(term);
    t38_set_t38_version(g_sp_core, 0);
    t38_set_data_rate_management_method(g_sp_core, T38_DATA_RATE_MANAGEMENT_TRANSFERRED_TCF);
    t30_set_supported_modems(t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
    t30_set_supported_compressions(t30,
        T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION | T30_SUPPORT_T6_COMPRESSION);
    t30_set_supported_resolutions(t30, T30_SUPPORT_STANDARD_RESOLUTION
        | T30_SUPPORT_FINE_RESOLUTION | T30_SUPPORT_SUPERFINE_RESOLUTION);
    t30_set_ecm_capability(t30, noecm ? FALSE : TRUE);
    if (nf_sends) t30_set_rx_file(t30, outfile, -1);
    else          t30_set_tx_file(t30, infile, -1, -1);
    t30_set_phase_e_handler(t30, sp_phe, NULL);

    if (verbose) {
        logging_state_t *lg = t38_terminal_get_logging_state(term);
        span_log_set_level(lg, SPAN_LOG_FLOW | SPAN_LOG_SHOW_TAG);
        span_log_set_tag(lg, "spandsp-rx");
        lg = t38_core_get_logging_state(g_sp_core);
        span_log_set_level(lg, SPAN_LOG_FLOW | SPAN_LOG_SHOW_TAG);
        span_log_set_tag(lg, "spandsp-t38");
    }

    int i, maxtick = 90 * 1000 / TICK_MS;
    for (i = 0; i < maxtick && !(g_nf_done && g_sp_done); i++) {
        nf_t30_t38_pump(g_nf, TICK_MS);
        t38_terminal_send_timeout(term, TICK_MS * 8);   /* ms -> samples @8k */
    }

    printf("%s: nf done=%d res=%d ; spandsp done=%d res=%d ; ticks=%d (%.1fs)\n",
           nf_sends ? "nf->spandsp" : "spandsp->nf",
           g_nf_done, g_nf_res, g_sp_done, g_sp_res, i, i * (TICK_MS / 1000.0));
    int ok = g_nf_done && g_sp_done && g_nf_res == 0 && g_sp_res == 0;
    nf_t30_free(g_nf);
    t38_terminal_free(term);
    printf("%s\n", ok ? "T38 ORACLE OK" : "T38 ORACLE FAIL");
    return ok ? 0 : 1;
}
