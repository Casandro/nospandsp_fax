/*
 * End-to-end fax over V.34 (T.30 Annex F "Super G3"): two nf_t30 engines in
 * audio loopback, both with V.34 enabled, transfer a page entirely over the
 * V.34 half-duplex session - V.8 (V.34HDX in CM/JM), clause-12 Phase 2
 * (INFO0c/INFO0a, tones + phase reversals, L1/L2 probing), INFOh, Phase 3
 * S/Sbar/PP + TRN training, control-channel startup (PPh/ALT/MPh/MPh/E both
 * directions), then T.30 over the 1200 bit/s control channel (DIS, DCS,
 * CFR - no TCF) and the page as ECM FCD frames over the 24000 bit/s primary
 * channel (PPS-NULL/MCF between blocks, PPS-EOP/MCF, DCN).
 *
 *   run: [NFV34DBG=1] ./nf_v34fax <in.tiff> <out.tiff> [verbose [maxrate]]
 *
 * Pixel-exactness of out.tiff vs in.tiff is asserted by the Makefile's
 * check-v34fax target (compare -metric AE); this harness asserts protocol
 * completion, the ECM page count and the negotiated rate: 33600 bit/s on
 * the clean loopback (the highest mutually-supported rate), or exactly
 * `maxrate` when the optional 4th argument caps the session's advertised
 * rate (exercising the 12.4.1.3 mask/max negotiation end to end).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "nf_t30.h"

#define BLK 160

struct done { int done, result; };
static void phe(void *u, int r) { struct done *d = u; d->done = 1; d->result = r; }

/* NFV34HIT="t0:t1:snr_db" - a transient AWGN hit on the caller->answerer audio
 * (which carries the primary-channel image) between t0 and t1 seconds, at
 * snr_db below the block's active RMS. Used to force a mid-call quality drop
 * that triggers a V.34 12.4 rate renegotiation (Sh short resync -> PPh/MPh)
 * without failing the transfer (ECM + the lower rate recover it pixel-exact). */
static unsigned hit_seed = 0xC0FFEE11;
static double hit_gauss(void)
{
    double u1, u2;
    hit_seed = hit_seed * 1103515245u + 12345u;
    u1 = ((hit_seed >> 8) & 0xffffff) / (double) 0x1000000 + 1e-12;
    hit_seed = hit_seed * 1103515245u + 12345u;
    u2 = ((hit_seed >> 8) & 0xffffff) / (double) 0x1000000;
    return sqrt(-2.0 * log(u1)) * cos(6.2831853071795864 * u2);
}
static void hit_apply(int16_t *a, int n, double snr_db)
{
    double rms = 0.0, sigma;
    int i, act = 0;
    for (i = 0; i < n; i++)
        if (a[i] > 200 || a[i] < -200) { rms += (double) a[i] * a[i]; act++; }
    if (!act) return;
    rms = sqrt(rms / act);
    sigma = rms * pow(10.0, -snr_db / 20.0);
    for (i = 0; i < n; i++) {
        double v = (double) a[i] + sigma * hit_gauss();
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        a[i] = (int16_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
    }
}

int main(int argc, char **argv)
{
    const char *infile  = argc > 1 ? argv[1] : "doc-fine.tiff";
    const char *outfile = argc > 2 ? argv[2] : "v34_out.tiff";
    int verbose = argc > 3;
    int maxrate = argc > 4 ? atoi(argv[4]) : 0;
    int expect_rate = 33600;

    if (maxrate > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", maxrate);
        setenv("NFV34MAXRATE", buf, 1);
        expect_rate = maxrate;
    }

    struct done da = {0,0}, db = {0,0};
    nf_t30_t *ca = nf_t30_init(1);   /* caller / source (sends the page)      */
    nf_t30_t *cb = nf_t30_init(0);   /* answerer / recipient (receives)       */
    int allres = NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400;
    nf_t30_set_supported_resolutions(ca, allres);
    nf_t30_set_supported_resolutions(cb, allres);
    nf_t30_set_v34(ca, 1);
    nf_t30_set_v34(cb, 1);
    nf_t30_set_tx_file(ca, infile);
    nf_t30_set_rx_file(cb, outfile);
    nf_t30_set_phase_e_handler(ca, phe, &da);
    nf_t30_set_phase_e_handler(cb, phe, &db);
    nf_t30_set_transmit_on_idle(ca, 1);
    nf_t30_set_transmit_on_idle(cb, 1);
    if (verbose) { nf_t30_set_verbose(ca, 1); nf_t30_set_verbose(cb, 1); }

    double hit_t0 = -1.0, hit_t1 = -1.0, hit_snr = 0.0;
    {
        const char *h = getenv("NFV34HIT");
        if (h && sscanf(h, "%lf:%lf:%lf", &hit_t0, &hit_t1, &hit_snr) == 3)
            fprintf(stderr, "NFV34HIT: transient hit %.1f-%.1f s at %.0f dB SNR\n",
                    hit_t0, hit_t1, hit_snr);
    }

    int16_t a[BLK], b[BLK];
    int i, maxblk = 180 * 50 + 400;         /* ~180 s cap (a full fine page
                                             * at a capped low rate is over
                                             * 90 s of legitimate call) */
    for (i = 0; i < maxblk && !(da.done && db.done); i++) {
        double t = i * (double) BLK / 8000.0;
        int na = nf_t30_tx(ca, a, BLK); if (na < BLK) memset(a + na, 0, (size_t)(BLK - na) * 2);
        if (hit_t0 >= 0.0 && t >= hit_t0 && t < hit_t1)
            hit_apply(a, BLK, hit_snr);     /* mid-call line hit on the image */
        nf_t30_rx(cb, a, BLK);
        int nb = nf_t30_tx(cb, b, BLK); if (nb < BLK) memset(b + nb, 0, (size_t)(BLK - nb) * 2);
        nf_t30_rx(ca, b, BLK);
    }

    nf_t30_stats_t sa, sb;
    nf_t30_get_stats(ca, &sa); nf_t30_get_stats(cb, &sb);
    printf("caller done=%d result=%d ; answerer done=%d result=%d ; pages_rx=%d %dx%d %ddpi ; rate=%d ; blocks=%d (%.1fs)\n",
           da.done, da.result, db.done, db.result, sb.pages_rx, sb.width, sb.length,
           sb.y_resolution, sb.bit_rate, i, i * 0.02);
    int ok = da.done && db.done && da.result == 0 && db.result == 0
          && sb.pages_rx >= 1;
    /* With a mid-call hit the transfer may recover by ECM retransmission at
     * the SAME rate once the transient passes, or by a 12.4 renegotiation
     * DOWN - both are correct; accept any completed rate. Otherwise require
     * the exact expected rate. */
    if (hit_t0 >= 0.0)
        ok = ok && sb.bit_rate > 0;
    else
        ok = ok && sb.bit_rate == expect_rate;
    nf_t30_free(ca); nf_t30_free(cb);
    printf("%s\n", ok ? "V34 CALL OK" : "V34 CALL FAIL");
    return ok ? 0 : 1;
}
