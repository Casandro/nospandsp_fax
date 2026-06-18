/*
 * Stage 2b/2c full-duplex loopback: a caller (sender) and answerer (receiver)
 * nf_t30 engine, pumping each other's audio, running a complete non-ECM fax.
 *   build: cc nf_faxloop2.c nf_t30.c nf_fax.c nf_t4.c -lspandsp -ltiff
 *   run:   ./nf_faxloop2 <in.tiff> <out.tiff> [verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nf_t30.h"

#define BLK 160

struct done { int done, result; };
static void phe(void *u, int r) { struct done *d = u; d->done = 1; d->result = r; }

int main(int argc, char **argv)
{
    const char *infile  = argc > 1 ? argv[1] : "doc-fine.tiff";
    const char *outfile = argc > 2 ? argv[2] : "loop_out.tiff";
    int verbose = argc > 3;

    struct done da = {0,0}, db = {0,0};
    nf_t30_t *ca = nf_t30_init(1);   /* caller / sender   */
    nf_t30_t *cb = nf_t30_init(0);   /* answerer / receiver */
    int allres = NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400;
    nf_t30_set_supported_resolutions(ca, allres);
    nf_t30_set_supported_resolutions(cb, allres);
    nf_t30_set_tx_file(ca, infile);
    nf_t30_set_rx_file(cb, outfile);
    nf_t30_set_phase_e_handler(ca, phe, &da);
    nf_t30_set_phase_e_handler(cb, phe, &db);
    nf_t30_set_transmit_on_idle(ca, 1);
    nf_t30_set_transmit_on_idle(cb, 1);
    if (verbose) { nf_t30_set_verbose(ca, 1); nf_t30_set_verbose(cb, 1); }

    int16_t a[BLK], b[BLK];
    int i, maxblk = 60 * 50 + 400;          /* ~60 s cap */
    for (i = 0; i < maxblk && !(da.done && db.done); i++) {
        int na = nf_t30_tx(ca, a, BLK); if (na < BLK) memset(a + na, 0, (size_t)(BLK - na) * 2);
        nf_t30_rx(cb, a, BLK);
        int nb = nf_t30_tx(cb, b, BLK); if (nb < BLK) memset(b + nb, 0, (size_t)(BLK - nb) * 2);
        nf_t30_rx(ca, b, BLK);
    }

    nf_t30_stats_t sa, sb;
    nf_t30_get_stats(ca, &sa); nf_t30_get_stats(cb, &sb);
    printf("caller done=%d result=%d ; answerer done=%d result=%d ; pages_rx=%d %dx%d %ddpi ; blocks=%d (%.1fs)\n",
           da.done, da.result, db.done, db.result, sb.pages_rx, sb.width, sb.length, sb.y_resolution,
           i, i * 0.02);
    int ok = da.done && db.done && da.result == 0 && db.result == 0 && sb.pages_rx >= 1;
    nf_t30_free(ca); nf_t30_free(cb);
    printf("%s\n", ok ? "CALL OK" : "CALL FAIL");
    return ok ? 0 : 1;
}
