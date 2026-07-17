/*
 * ECM rate-fallback (T.30 Annex A.4.3) loopback: two nf_t30 engines run a full
 * ECM fax. The env NF_ECM_DROP_ABOVE=<bps> makes the sender withhold every FCD
 * frame while the modem rate is above that threshold, so the block cannot clear
 * until the CTC/CTR ladder drops the rate below it. This exercises our own
 * transmitter (CTC/EOR) against our own receiver (CTR/ERR) in one process.
 *
 *   run:  NF_ECM_DROP_ABOVE=13000 ./nf_ctctest <in.tiff> <out.tiff> [verbose]
 *   exit: 0 if both sides completed cleanly (then compare pixels), 1 otherwise.
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
    const char *infile  = argc > 1 ? argv[1] : "_in.tiff";
    const char *outfile = argc > 2 ? argv[2] : "_ctc_out.tiff";
    int verbose = argc > 3;

    struct done da = {0,0}, db = {0,0};
    nf_t30_t *ca = nf_t30_init(1);   /* caller / sender     */
    nf_t30_t *cb = nf_t30_init(0);   /* answerer / receiver */
    int allres = NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400;
    nf_t30_set_supported_resolutions(ca, allres);
    nf_t30_set_supported_resolutions(cb, allres);
    nf_t30_set_ecm(ca, 1);
    nf_t30_set_ecm(cb, 1);
    nf_t30_set_tx_file(ca, infile);
    nf_t30_set_rx_file(cb, outfile);
    nf_t30_set_phase_e_handler(ca, phe, &da);
    nf_t30_set_phase_e_handler(cb, phe, &db);
    nf_t30_set_transmit_on_idle(ca, 1);
    nf_t30_set_transmit_on_idle(cb, 1);
    if (verbose) { nf_t30_set_verbose(ca, 1); nf_t30_set_verbose(cb, 1); }

    int16_t a[BLK], b[BLK];
    int i, maxblk = 200 * 50;                 /* ~200 s cap (the ladder adds rounds) */
    for (i = 0; i < maxblk && !(da.done && db.done); i++) {
        int na = nf_t30_tx(ca, a, BLK); if (na < BLK) memset(a + na, 0, (size_t)(BLK - na) * 2);
        nf_t30_rx(cb, a, BLK);
        int nb = nf_t30_tx(cb, b, BLK); if (nb < BLK) memset(b + nb, 0, (size_t)(BLK - nb) * 2);
        nf_t30_rx(ca, b, BLK);
    }

    nf_t30_stats_t sa, sb;
    nf_t30_get_stats(ca, &sa); nf_t30_get_stats(cb, &sb);
    printf("caller done=%d result=%d (%dbps) ; answerer done=%d result=%d ; pages_rx=%d %dx%d ; blocks=%d (%.1fs)\n",
           da.done, da.result, sa.bit_rate, db.done, db.result, sb.pages_rx, sb.width, sb.length,
           i, i * 0.02);
    int ok = da.done && db.done && da.result == 0 && db.result == 0 && sb.pages_rx >= 1;
    nf_t30_free(ca); nf_t30_free(cb);
    printf("%s\n", ok ? "CTC CALL OK" : "CTC CALL FAIL");
    return ok ? 0 : 1;
}
