/*
 * ECM interop test: run nf_t30 (with ECM enabled) against spandsp's own fax
 * engine, ECM + T.6 enabled on both, over an in-process full-duplex sample loop.
 * Frame loss is injected via the NF_ECM_DROP_TX / NF_ECM_DROP_RX env vars, which
 * nf_t30 honours, so the test exercises PPR-driven retransmission against the
 * real spandsp implementation.
 *
 *   build: see Makefile target `nf_ecmtest`
 *   run:   ./nf_ecmtest nf2sp|sp2nf <in.tiff> <out.tiff>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <spandsp.h>
#include "nf_t30.h"

#define BLK 160

static int nf_done, nf_res, sp_done, sp_res;
static void nf_phe(void *u, int r) { (void)u; nf_done = 1; nf_res = r; }
static void sp_phe(t30_state_t *s, void *u, int r) { (void)s;(void)u; sp_done = 1; sp_res = r; }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "nf2sp";
    const char *infile = argc > 2 ? argv[2] : "doc-fine.tiff";
    const char *outfile = argc > 3 ? argv[3] : "ecm_out.tiff";
    int nf_is_caller = (strcmp(mode, "nf2sp") == 0);   /* nf sends, spandsp recvs */

    /* nf engine - ECM is on by default; fault injection via env. */
    nf_t30_t *nf = nf_t30_init(nf_is_caller);
    nf_t30_set_supported_resolutions(nf, NF_RES_STANDARD|NF_RES_FINE|NF_RES_SUPERFINE|NF_RES_300|NF_RES_400);
    nf_t30_set_ecm(nf, 1);
    nf_t30_set_transmit_on_idle(nf, 1);
    nf_t30_set_phase_e_handler(nf, nf_phe, NULL);
    if (getenv("NFV")) nf_t30_set_verbose(nf, 1);
    if (nf_is_caller) nf_t30_set_tx_file(nf, infile); else nf_t30_set_rx_file(nf, outfile);

    /* spandsp engine (the opposite role), ECM + T.6 enabled. */
    fax_state_t *fax = fax_init(NULL, nf_is_caller ? FALSE : TRUE);
    t30_state_t *t30 = fax_get_t30_state(fax);
    t30_set_supported_modems(t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
    t30_set_supported_compressions(t30, T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION
                                        | T30_SUPPORT_T6_COMPRESSION);
    t30_set_supported_resolutions(t30, T30_SUPPORT_STANDARD_RESOLUTION | T30_SUPPORT_FINE_RESOLUTION
                                       | T30_SUPPORT_SUPERFINE_RESOLUTION);
    t30_set_ecm_capability(t30, TRUE);
    t30_set_phase_e_handler(t30, sp_phe, NULL);
    fax_set_transmit_on_idle(fax, TRUE);
    if (nf_is_caller) t30_set_rx_file(t30, outfile, -1); else t30_set_tx_file(t30, infile, -1, -1);

    int16_t x[BLK], y[BLK];
    int i, maxblk = 90 * 60;
    for (i = 0; i < maxblk && !(nf_done && sp_done); i++) {
        int n;
        n = nf_t30_tx(nf, x, BLK); if (n < BLK) memset(x + n, 0, (size_t)(BLK - n) * 2);
        fax_rx(fax, x, BLK);
        n = fax_tx(fax, y, BLK); if (n < BLK) memset(y + n, 0, (size_t)(BLK - n) * 2);
        nf_t30_rx(nf, y, BLK);
    }
    printf("%s: nf done=%d res=%d ; spandsp done=%d res=%d ; blocks=%d (%.1fs)\n",
           mode, nf_done, nf_res, sp_done, sp_res, i, i * 0.02);

    fax_release(fax); fax_free(fax);
    nf_t30_free(nf);
    return (nf_done && sp_done && nf_res == 0 && sp_res == 0) ? 0 : 2;
}
