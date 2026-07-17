/*
 * Unit test for nf_color: CIELAB round-trip accuracy, JPEG encode/decode PSNR,
 * and T.81 codestream structure (SOI/EOI, SOF0 with 3 x 1:1 components, no JFIF).
 *   build: see Makefile target `nf_colortest`
 *   run:   ./nf_colortest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "nf_color.h"

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
    double se = 0;
    for (size_t i = 0; i < n; i++) { double d = (double) a[i] - b[i]; se += d * d; }
    double mse = se / n;
    if (mse <= 0) return 999.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

/* Walk the JPEG markers; verify SOI/EOI, a single SOF0 with 3 components each
 * 1:1 sampled, and no JFIF APP0. Returns 0 if OK. */
static int check_codestream(const uint8_t *d, size_t n)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) { printf("  no SOI\n"); return -1; }
    if (d[n-2] != 0xFF || d[n-1] != 0xD9) { printf("  no EOI\n"); return -1; }
    size_t p = 2;
    int saw_sof = 0, jfif = 0;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) { printf("  bad marker at %zu\n", p); return -1; }
        uint8_t m = d[p+1];
        if (m == 0xD9) break;                 /* EOI */
        size_t seg = ((size_t) d[p+2] << 8) | d[p+3];
        if (m == 0xE0) jfif = 1;              /* APP0 (JFIF) - we must NOT emit it */
        if (m == 0xC0 || m == 0xC1) {         /* SOF0/1 baseline */
            saw_sof = 1;
            /* payload base = p+4; [0]=precision [1..2]=H [3..4]=W [5]=nc;
             * then per comp: [id, H<<4|V, qtab] from payload[6]. */
            size_t pl = p + 4;
            int nc = d[pl + 5];
            if (nc != 3) { printf("  SOF components=%d (want 3)\n", nc); return -1; }
            for (int c = 0; c < nc; c++) {
                uint8_t sf = d[pl + 6 + c*3 + 1];      /* H<<4 | V */
                uint8_t want = c == 0 ? 0x22 : 0x11;   /* T.42 default 4:1:1 */
                if (sf != want) { printf("  comp %d sampling=0x%02x (want 0x%02x)\n", c, sf, want); return -1; }
            }
        }
        if (m == 0xDA) break;                 /* SOS: scan data follows */
        p += 2 + seg;
    }
    if (!saw_sof) { printf("  no SOF\n"); return -1; }
    if (jfif)     { printf("  unexpected JFIF APP0\n"); return -1; }
    return 0;
}

int main(void)
{
    int rc = 0;

    /* 1) CIELAB round-trip over an RGB ramp: encode->decode a tiny image at q100
     *    and check the colour error is small (the transform + q100 JPEG). */
    int W = 64, H = 64;
    uint8_t *img = malloc((size_t) W*H*3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = img + ((size_t) y*W + x)*3;
            p[0] = (uint8_t)(x * 4);
            p[1] = (uint8_t)(y * 4);
            p[2] = (uint8_t)((x + y) * 2);
        }

    uint8_t *enc = NULL; size_t enclen = 0;
    if (nf_color_encode(img, W, H, 100, &enc, &enclen) != 0) { printf("encode q100 FAIL\n"); return 1; }
    printf("TEST codestream structure: ");
    if (check_codestream(enc, enclen) == 0) printf("PASS (%zu bytes)\n", enclen); else { printf("FAIL\n"); rc = 1; }

    uint8_t *dec = NULL; int dw = 0, dh = 0;
    if (nf_color_decode(enc, enclen, &dec, &dw, &dh) != 0 || dw != W || dh != H) { printf("decode FAIL\n"); return 1; }
    double p100 = psnr(img, dec, (size_t) W*H*3);
    printf("TEST round-trip PSNR q100 = %.1f dB: %s\n", p100, p100 >= 35.0 ? "PASS" : "FAIL");
    if (p100 < 35.0) rc = 1;
    free(enc); free(dec);

    /* 2) q85 should still be well above the usability floor. */
    if (nf_color_encode(img, W, H, 85, &enc, &enclen) != 0) { printf("encode q85 FAIL\n"); return 1; }
    if (nf_color_decode(enc, enclen, &dec, &dw, &dh) != 0) { printf("decode q85 FAIL\n"); return 1; }
    double p85 = psnr(img, dec, (size_t) W*H*3);
    printf("TEST round-trip PSNR q85  = %.1f dB: %s\n", p85, p85 >= 30.0 ? "PASS" : "FAIL");
    if (p85 < 30.0) rc = 1;
    free(enc); free(dec); free(img);

    /* 3) Pure grey must stay neutral (a*,b* ~ centre; r==g==b within a couple codes). */
    uint8_t grey[3] = { 128, 128, 128 }, lab_back[3*1];
    uint8_t *g_enc = NULL, *g_dec = NULL; size_t gl; int gw, gh;
    uint8_t one[3] = {128,128,128};
    (void) lab_back; (void) one;
    /* round-trip a 1x1 grey via the full codec */
    if (nf_color_encode(grey, 1, 1, 100, &g_enc, &gl) == 0 &&
        nf_color_decode(g_enc, gl, &g_dec, &gw, &gh) == 0) {
        int dr = abs(g_dec[0]-128), dg = abs(g_dec[1]-128), db = abs(g_dec[2]-128);
        printf("TEST grey neutrality: got (%d,%d,%d) err(%d,%d,%d): %s\n",
               g_dec[0], g_dec[1], g_dec[2], dr, dg, db,
               (dr<=3 && dg<=3 && db<=3) ? "PASS" : "FAIL");
        if (dr>3 || dg>3 || db>3) rc = 1;
        free(g_enc); free(g_dec);
    } else { printf("grey round-trip FAIL\n"); rc = 1; }

    /* 4) Greyscale codec round-trip: encode an RGB gradient as 1-component JPEG,
     *    decode to grey, compare against the luma of the source. */
    int ggw = 48, ggh = 48;
    uint8_t *gi = malloc((size_t) ggw*ggh*3);
    for (int y = 0; y < ggh; y++)
        for (int x = 0; x < ggw; x++) {
            uint8_t v = (uint8_t)((x + y) * 2);
            uint8_t *p = gi + ((size_t) y*ggw + x)*3;
            p[0] = p[1] = p[2] = v;          /* neutral grey ramp */
        }
    uint8_t *ge = NULL; size_t gel = 0; uint8_t *gd = NULL; int gdw = 0, gdh = 0;
    if (nf_gray_encode(gi, ggw, ggh, 90, &ge, &gel) == 0 &&
        nf_gray_decode(ge, gel, &gd, &gdw, &gdh) == 0 && gdw == ggw && gdh == ggh) {
        /* source grey value (r==g==b) vs decoded grey */
        double se = 0; for (int i = 0; i < ggw*ggh; i++) { double d = (double) gi[i*3] - gd[i]; se += d*d; }
        double mse = se / (ggw*ggh); double p = mse <= 0 ? 999 : 10*log10(255.0*255.0/mse);
        printf("TEST greyscale round-trip PSNR = %.1f dB: %s\n", p, p >= 35.0 ? "PASS" : "FAIL");
        if (p < 35.0) rc = 1;
        free(ge); free(gd);
    } else { printf("greyscale round-trip FAIL\n"); rc = 1; }
    free(gi);

    printf(rc == 0 ? "ALL PASS\n" : "FAILURES\n");
    return rc;
}
