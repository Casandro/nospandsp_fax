/*
 * T.38 REMODULATION oracle: our nf_t30 T.38 sender -> spandsp t38_gateway
 * (T.38<->audio, exactly what a carrier gateway does) -> spandsp audio fax
 * receiver -> TIFF. Unlike nf_t38oracle (which terminates T.38 to a TIFF and so
 * never remodulates), this exercises the gateway re-modulating our IFP back into
 * a real V.17/V.29 modem signal for an audio fax machine -- the live topology.
 *
 *   run: ./nf_t38gw <in.tiff> <out.tiff> [noecm] [verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <spandsp.h>
#include "nf_t30.h"
#include "nf_udptl.h"

#define STEP 160                       /* 20 ms @ 8 kHz */

static nf_t30_t         *g_nf;          /* our T.38 sender                  */
static t38_core_state_t *g_gw_core;     /* gateway's T.38 core (IFP side)   */
static nf_udptl_t        g_dec;         /* decode our datagrams -> IFP      */
static nf_udptl_t        g_enc;         /* encode gateway IFP -> datagram   */

/* our sender produced a UDPTL datagram -> hand each IFP to the gateway */
static void on_ifp_to_gw(void *u, const uint8_t *ifp, int ifp_len, int seq)
{
    (void) u;
    t38_core_rx_ifp_packet(g_gw_core, ifp, ifp_len, (uint16_t) seq);
}
static void nf_send_cb(void *u, const uint8_t *dgram, int len)
{
    (void) u;
    nf_udptl_rx(&g_dec, dgram, len, on_ifp_to_gw, NULL);
}
/* gateway produced an IFP packet -> wrap in UDPTL and feed our sender */
static int gw_tx_handler(t38_core_state_t *s, void *u, const uint8_t *buf, int len, int count)
{
    (void) s; (void) u; (void) count;
    uint8_t dg[NF_UDPTL_MAXDGRAM];
    int n = nf_udptl_build(&g_enc, buf, len, dg, sizeof dg);
    if (n > 0) nf_t30_t38_rx_datagram(g_nf, dg, n);
    return 0;
}

/* Optional spandsp t38_terminal sender (baseline): cross-wire its T.38 core to
 * the gateway directly (IFP both ways, no UDPTL needed). */
static t38_core_state_t *g_spsnd_core;
static uint16_t g_seq_to_gw, g_seq_to_spsnd;
static int spsnd_tx_handler(t38_core_state_t *s, void *u, const uint8_t *buf, int len, int count)
{
    (void) s; (void) u; (void) count;
    t38_core_rx_ifp_packet(g_gw_core, buf, len, g_seq_to_gw++);
    return 0;
}
static int gw_tx_to_spsnd(t38_core_state_t *s, void *u, const uint8_t *buf, int len, int count)
{
    (void) s; (void) u; (void) count;
    t38_core_rx_ifp_packet(g_spsnd_core, buf, len, g_seq_to_spsnd++);
    return 0;
}

static int g_nf_done, g_nf_res, g_sp_done, g_sp_res;
static void nf_phe(void *u, int r) { (void) u; g_nf_done = 1; g_nf_res = r; }
static void sp_phe(t30_state_t *s, void *u, int r) { (void) s; (void) u; g_sp_done = 1; g_sp_res = r; }
static void spsnd_phe(t30_state_t *s, void *u, int r) { (void) s; (void) u; g_nf_done = 1; g_nf_res = r; }

int main(int argc, char **argv)
{
    int a0 = 1;
    int use_sp_sender = (argc > 1 && strcmp(argv[1], "sp") == 0);
    if (use_sp_sender || (argc > 1 && strcmp(argv[1], "nf") == 0)) a0 = 2;
    const char *infile  = argc > a0     ? argv[a0]     : "doc.pam";
    const char *outfile = argc > a0 + 1 ? argv[a0 + 1] : "t38gw_out.tiff";
    int noecm = 0, verbose = 0;
    for (int i = a0 + 2; i < argc; i++) {
        if (strcmp(argv[i], "noecm") == 0) noecm = 1;
        else if (strcmp(argv[i], "verbose") == 0) verbose = 1;
    }

    nf_udptl_init(&g_dec, 2, NF_UDPTL_MAXDGRAM);
    nf_udptl_init(&g_enc, 2, NF_UDPTL_MAXDGRAM);

    /* our T.38 sender (caller) — unless the spandsp baseline sender is selected */
    t38_terminal_state_t *spsnd = NULL;
    if (use_sp_sender) {
        spsnd = t38_terminal_init(NULL, TRUE, spsnd_tx_handler, NULL);
        t30_state_t *st = t38_terminal_get_t30_state(spsnd);
        g_spsnd_core = t38_terminal_get_t38_core_state(spsnd);
        t38_set_t38_version(g_spsnd_core, 0);
        t38_set_data_rate_management_method(g_spsnd_core, T38_DATA_RATE_MANAGEMENT_TRANSFERRED_TCF);
        t30_set_supported_modems(st, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
        t30_set_supported_compressions(st,
            T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION | T30_SUPPORT_T6_COMPRESSION);
        t30_set_ecm_capability(st, noecm ? FALSE : TRUE);
        t30_set_tx_file(st, infile, -1, -1);
        t30_set_phase_e_handler(st, spsnd_phe, NULL);
    } else {
        g_nf = nf_t30_init(1);
        nf_t30_set_supported_resolutions(g_nf,
            NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400);
        nf_t30_set_tx_file(g_nf, infile);
        nf_t30_set_phase_e_handler(g_nf, nf_phe, NULL);
        if (noecm) nf_t30_set_ecm(g_nf, 0);
        if (verbose) nf_t30_set_verbose(g_nf, 1);
        nf_t30_t38_enable(g_nf, 2, NF_UDPTL_MAXDGRAM, nf_send_cb, NULL);
    }

    /* spandsp T.38<->audio gateway */
    t38_gateway_state_t *gw = t38_gateway_init(NULL,
        use_sp_sender ? gw_tx_to_spsnd : gw_tx_handler, NULL);
    g_gw_core = t38_gateway_get_t38_core_state(gw);
    t38_set_t38_version(g_gw_core, 0);
    t38_set_data_rate_management_method(g_gw_core, T38_DATA_RATE_MANAGEMENT_TRANSFERRED_TCF);
    t38_gateway_set_supported_modems(gw, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
    t38_gateway_set_ecm_capability(gw, noecm ? FALSE : TRUE);
    t38_gateway_set_transmit_on_idle(gw, TRUE);

    /* spandsp audio fax receiver (answerer) */
    fax_state_t *fax = fax_init(NULL, FALSE);
    t30_state_t *t30 = fax_get_t30_state(fax);
    t30_set_supported_modems(t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
    t30_set_supported_compressions(t30,
        T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION | T30_SUPPORT_T6_COMPRESSION);
    t30_set_supported_resolutions(t30, T30_SUPPORT_STANDARD_RESOLUTION
        | T30_SUPPORT_FINE_RESOLUTION | T30_SUPPORT_SUPERFINE_RESOLUTION);
    t30_set_ecm_capability(t30, noecm ? FALSE : TRUE);
    t30_set_rx_file(t30, outfile, -1);
    t30_set_phase_e_handler(t30, sp_phe, NULL);
    fax_set_transmit_on_idle(fax, TRUE);

    if (verbose) {
        logging_state_t *lg = t38_gateway_get_logging_state(gw);
        span_log_set_level(lg, SPAN_LOG_FLOW | SPAN_LOG_SHOW_TAG); span_log_set_tag(lg, "gw");
        lg = t38_core_get_logging_state(g_gw_core);
        span_log_set_level(lg, SPAN_LOG_FLOW | SPAN_LOG_SHOW_TAG); span_log_set_tag(lg, "gw-t38");
        lg = t30_get_logging_state(t30);
        span_log_set_level(lg, SPAN_LOG_FLOW | SPAN_LOG_SHOW_TAG); span_log_set_tag(lg, "fax-rx");
    }

    int16_t a[STEP], b[STEP];
    int i, maxstep = 70 * 8000 / STEP;          /* ~70 s cap */
    for (i = 0; i < maxstep && !(g_nf_done && g_sp_done); i++) {
        if (use_sp_sender) t38_terminal_send_timeout(spsnd, STEP);
        else               nf_t30_t38_pump(g_nf, STEP / 8);   /* 20 ms */

        /* gateway -> audio -> fax receiver */
        int n = t38_gateway_tx(gw, a, STEP);
        for (int k = n; k < STEP; k++) a[k] = 0;
        fax_rx(fax, a, STEP);

        /* fax receiver -> audio -> gateway */
        int m = fax_tx(fax, b, STEP);
        for (int k = m; k < STEP; k++) b[k] = 0;
        t38_gateway_rx(gw, b, STEP);
    }

    printf("nf(send) done=%d res=%d ; spandsp-fax(recv) done=%d res=%d ; steps=%d (%.1fs)\n",
           g_nf_done, g_nf_res, g_sp_done, g_sp_res, i, i * (STEP / 8000.0));
    int ok = g_nf_done && g_sp_done && g_nf_res == 0 && g_sp_res == 0;
    nf_t30_free(g_nf);
    fax_release(fax);
    t38_gateway_release(gw);
    printf("%s\n", ok ? "T38 GW OK" : "T38 GW FAIL");
    return ok ? 0 : 1;
}
