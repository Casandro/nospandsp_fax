/*
 * nf<->nf transfer harness for the colour (T.42/JPEG) and binary-file payload
 * kinds: a caller (sender) and answerer (receiver) nf_t30 engine pump each
 * other's audio through a full ECM exchange. Frame loss can be injected via
 * NF_ECM_DROP_TX / NF_ECM_DROP_RX (honoured by nf_t30) to exercise PPR.
 *   run:  ./nf_xfer color <in.tiff>  <out.tiff>   [verbose]
 *         ./nf_xfer gray  <in.tiff>  <out.tiff>   [verbose]
 *         ./nf_xfer file  <in.bin>   <out.bin>    [verbose]
 * Comparison (PSNR for colour, byte-exact for file) is done by the caller/Make.
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
    if (argc < 4) { fprintf(stderr, "usage: %s color|gray|file <in> <out> [verbose]\n", argv[0]); return 2; }
    const char *mode = argv[1], *infile = argv[2], *outfile = argv[3];
    int verbose = argc > 4;
    int is_color = (strcmp(mode, "color") == 0 || strcmp(mode, "colour") == 0);
    int is_gray  = (strcmp(mode, "gray") == 0 || strcmp(mode, "grey") == 0);

    struct done da = {0,0}, db = {0,0};
    nf_t30_t *ca = nf_t30_init(1);   /* caller / sender */
    nf_t30_t *cb = nf_t30_init(0);   /* answerer / receiver */

    if (is_color || is_gray) {
        if (is_gray) nf_t30_set_gray(ca, 1); else nf_t30_set_color(ca, 1);
        nf_t30_set_color_quality(ca, 85);
        nf_t30_set_tx_file(ca, infile);
        nf_t30_set_color_capable(cb, 1);   /* advertises 68+69; accepts colour & grey */
        nf_t30_set_rx_file(cb, outfile);
    } else {
        nf_t30_set_file_tx(ca, infile);
        nf_t30_set_file_rx(cb, outfile);
    }
    nf_t30_set_phase_e_handler(ca, phe, &da);
    nf_t30_set_phase_e_handler(cb, phe, &db);
    nf_t30_set_transmit_on_idle(ca, 1);
    nf_t30_set_transmit_on_idle(cb, 1);
    if (verbose) { nf_t30_set_verbose(ca, 1); nf_t30_set_verbose(cb, 1); }

    int16_t a[BLK], b[BLK];
    int i, maxblk = 90 * 60;
    for (i = 0; i < maxblk && !(da.done && db.done); i++) {
        int na = nf_t30_tx(ca, a, BLK); if (na < BLK) memset(a + na, 0, (size_t)(BLK - na) * 2);
        nf_t30_rx(cb, a, BLK);
        int nb = nf_t30_tx(cb, b, BLK); if (nb < BLK) memset(b + nb, 0, (size_t)(BLK - nb) * 2);
        nf_t30_rx(ca, b, BLK);
    }

    nf_t30_stats_t sb;
    nf_t30_get_stats(cb, &sb);
    printf("%s: caller done=%d result=%d ; answerer done=%d result=%d ; rx pages=%d %dx%d ; blocks=%d (%.1fs)\n",
           mode, da.done, da.result, db.done, db.result, sb.pages_rx, sb.width, sb.length, i, i * 0.02);
    int ok = da.done && db.done && da.result == 0 && db.result == 0;
    nf_t30_free(ca); nf_t30_free(cb);
    return ok ? 0 : 1;
}
