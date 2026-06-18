/*
 * Offline cross-check for nf_t4 against spandsp's t4_tx/t4_rx.
 * Builds standalone: cc nf_t4check.c nf_t4.c -lspandsp -ltiff
 *
 * For a bilevel page (from a PAM) and each mode {1D,2D,T6} it checks:
 *   - self round-trip:   nf_decode(nf_encode(img)) == img
 *   - MH byte-identity:  nf_encode(1D) bytes == spandsp t4_tx(1D) bytes
 *   - interop A:         nf_decode(spandsp_encode) == img
 *   - interop B:         spandsp_decode(nf_encode) == img
 * Exit code 0 iff every check passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <tiffio.h>
#include <spandsp.h>
#include "nf_t4.h"

#define WIDTH 1728

/* ── PAM / TIFF helpers (rows: packed MSB-first, 1=black) ───────────── */

static uint8_t *read_pam(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    char line[256];
    int width = -1, height = -1, depth = -1, maxval = -1;
    if (!fgets(line, sizeof line, f)) { fclose(f); return NULL; }   /* P7 */
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "ENDHDR", 6)) break;
        char k[32]; int v;
        if (sscanf(line, "%31s %d", k, &v) == 2) {
            if (!strcmp(k, "WIDTH")) width = v;
            else if (!strcmp(k, "HEIGHT")) height = v;
            else if (!strcmp(k, "DEPTH")) depth = v;
            else if (!strcmp(k, "MAXVAL")) maxval = v;
        }
    }
    if (width != WIDTH || depth != 1 || maxval != 1) {
        fprintf(stderr, "PAM must be %d-wide bilevel\n", WIDTH); fclose(f); return NULL;
    }
    int stride = (width + 7) / 8;
    uint8_t *rows = calloc((size_t) height * stride, 1);
    uint8_t *line_buf = malloc(width);
    for (int y = 0; y < height; y++) {
        if (fread(line_buf, 1, width, f) != (size_t) width) { free(rows); free(line_buf); fclose(f); return NULL; }
        for (int x = 0; x < width; x++)
            if (line_buf[x] == 0) rows[y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7)); /* 0=black */
    }
    free(line_buf); fclose(f);
    *w = width; *h = height;
    return rows;
}

static int write_fax_tiff(const char *path, const uint8_t *rows, int w, int h)
{
    TIFF *t = TIFFOpen(path, "w");
    if (!t) return -1;
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 1);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_CCITT_T6);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
    TIFFSetField(t, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
    TIFFSetField(t, TIFFTAG_XRESOLUTION, 204.0f);
    TIFFSetField(t, TIFFTAG_YRESOLUTION, 196.0f);
    TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(t, 0));
    int stride = (w + 7) / 8;
    for (int y = 0; y < h; y++)
        if (TIFFWriteScanline(t, (void *) (rows + y * stride), y, 0) < 0) { TIFFClose(t); return -1; }
    TIFFClose(t);
    return 0;
}

static uint8_t *read_fax_tiff(const char *path, int *w, int *h)
{
    TIFF *t = TIFFOpen(path, "r");
    if (!t) return NULL;
    uint32_t W = 0, H = 0;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &W);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &H);
    int stride = ((int) W + 7) / 8;
    uint8_t *rows = calloc((size_t) H * stride, 1);
    for (uint32_t y = 0; y < H; y++)
        TIFFReadScanline(t, rows + y * stride, y, 0);
    TIFFClose(t);
    *w = (int) W; *h = (int) H;
    return rows;
}

/* ── nf_t4 encode / decode wrappers ────────────────────────────────── */

static uint8_t *nf_encode(const uint8_t *rows, int w, int h, int mode, int k, size_t *out_len)
{
    nf_t4_enc_t *e = nf_t4_enc_init(w, mode, k);
    int stride = (w + 7) / 8;
    for (int y = 0; y < h; y++) nf_t4_enc_row(e, rows + y * stride);
    nf_t4_enc_end_page(e);
    size_t len; const uint8_t *d = nf_t4_enc_data(e, &len);
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, d, len);
    *out_len = len;
    nf_t4_enc_free(e);
    return copy;
}

static void sink_row(void *u, const uint8_t *r, int wd)
{
    (void) u; (void) r; (void) wd;
}

struct collector { uint8_t *rows; int n, cap, stride, width; };
static void collect_row(void *u, const uint8_t *row, int width)
{
    struct collector *c = u;
    if (c->n >= c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->rows = realloc(c->rows, (size_t) c->cap * c->stride); }
    memcpy(c->rows + (size_t) c->n * c->stride, row, c->stride);
    c->n++; (void) width;
}

static uint8_t *nf_decode(const uint8_t *buf, size_t len, int w, int mode, int *nrows)
{
    struct collector c = { NULL, 0, 0, (w + 7) / 8, w };
    nf_t4_dec_t *d = nf_t4_dec_init(w, mode, collect_row, &c);
    nf_t4_dec_put(d, buf, len);
    nf_t4_dec_free(d);
    *nrows = c.n;
    return c.rows;
}

/* ── spandsp encode / decode ───────────────────────────────────────── */

static uint8_t *sp_encode(const char *tiff, int mode, size_t *out_len)
{
    t4_tx_state_t *s = t4_tx_init(NULL, tiff, -1, -1);
    if (!s) return NULL;
    t4_tx_set_tx_encoding(s, mode);
    t4_tx_start_page(s);
    uint8_t *buf = NULL; size_t len = 0, cap = 0; int b;
    while ((b = t4_tx_get_byte(s)) < 0x100) {
        if (len >= cap) { cap = cap ? cap * 2 : 4096; buf = realloc(buf, cap); }
        buf[len++] = (uint8_t) b;
    }
    t4_tx_release(s); t4_tx_free(s);
    *out_len = len;
    return buf;
}

static int sp_decode(const uint8_t *buf, size_t len, int w, int mode, const char *out_tiff)
{
    t4_rx_state_t *s = t4_rx_init(NULL, out_tiff, T4_COMPRESSION_ITU_T6);
    if (!s) return -1;
    t4_rx_set_rx_encoding(s, mode);
    t4_rx_set_image_width(s, w);
    t4_rx_start_page(s);
    for (size_t i = 0; i < len; i++) t4_rx_put_byte(s, buf[i]);
    t4_rx_end_page(s);
    t4_rx_release(s); t4_rx_free(s);
    return 0;
}

/* ── compare ───────────────────────────────────────────────────────── */

static long pixel_diff(const uint8_t *a, const uint8_t *b, int w, int h)
{
    int stride = (w + 7) / 8;
    long diff = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int pa = (a[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            int pb = (b[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            if (pa != pb) diff++;
        }
    return diff;
}

int main(int argc, char **argv)
{
    const char *pam = argc > 1 ? argv[1] : "doc.pam";
    int w, h;
    uint8_t *orig = read_pam(pam, &w, &h);
    if (!orig) return 2;
    fprintf(stderr, "loaded %s: %dx%d\n", pam, w, h);

    const char *tin = "/tmp/nf_t4check_in.tif";
    const char *tout = "/tmp/nf_t4check_out.tif";
    if (write_fax_tiff(tin, orig, w, h) != 0) { fprintf(stderr, "write tiff failed\n"); return 2; }

    struct { const char *name; int mode; int k; } modes[] = {
        { "1D", NF_T4_COMPRESSION_1D, 0 },
        { "2D", NF_T4_COMPRESSION_2D, 2 },
        { "T6", NF_T4_COMPRESSION_T6, 0 },
    };

    int fail = 0;
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        int mode = modes[m].mode, k = modes[m].k;
        const char *nm = modes[m].name;

        /* self round-trip */
        size_t nlen; uint8_t *nbuf = nf_encode(orig, w, h, mode, k, &nlen);
        int nr; uint8_t *ndec = nf_decode(nbuf, nlen, w, mode, &nr);
        long d_self = (nr == h) ? pixel_diff(orig, ndec, w, h) : -1;

        /* MH byte-identity (1D only) */
        char byteinfo[64] = "n/a";
        if (mode == NF_T4_COMPRESSION_1D) {
            size_t slen; uint8_t *sbuf = sp_encode(tin, mode, &slen);
            int same = (sbuf && slen == nlen && memcmp(sbuf, nbuf, nlen) == 0);
            snprintf(byteinfo, sizeof byteinfo, "%s (nf=%zu sp=%zu)",
                     same ? "IDENTICAL" : "DIFFER", nlen, slen);
            if (!same) fail = 1;
            free(sbuf);
        }

        /* interop A: spandsp encode -> nf decode */
        size_t slen2; uint8_t *sbuf2 = sp_encode(tin, mode, &slen2);
        int ar; uint8_t *adec = sbuf2 ? nf_decode(sbuf2, slen2, w, mode, &ar) : NULL;
        long d_a = (adec && ar == h) ? pixel_diff(orig, adec, w, h) : -1;

        /* interop B: nf encode -> spandsp decode */
        long d_b = -1;
        if (sp_decode(nbuf, nlen, w, mode, tout) == 0) {
            int bw, bh; uint8_t *bdec = read_fax_tiff(tout, &bw, &bh);
            if (bdec && bw == w && bh == h) d_b = pixel_diff(orig, bdec, w, h);
            free(bdec);
        }

        printf("%-3s  self_diff=%-6ld(nr=%d/%d)  MH_bytes=%-22s  sp->nf_diff=%-6ld(nr=%d)  nf->sp_diff=%-6ld\n",
               nm, d_self, nr, h, byteinfo, d_a, ar, d_b);
        if (d_self != 0 || d_a != 0 || d_b != 0) fail = 1;

        free(nbuf); free(ndec); free(sbuf2); free(adec);
    }

    /* Decoder robustness (T.4 only): flip bits mid-stream; the decoder must
     * finish the page, conceal the damaged rows (geometry preserved) and
     * count them; a clean stream must count zero. */
    for (size_t m = 0; m < 2; m++) {
        int mode = modes[m].mode, k = modes[m].k;
        const char *nm = modes[m].name;
        size_t nlen; uint8_t *nbuf = nf_encode(orig, w, h, mode, k, &nlen);

        nf_t4_dec_t *d = nf_t4_dec_init(w, mode, sink_row, NULL);
        nf_t4_dec_put(d, nbuf, nlen);
        int clean_bad = nf_t4_dec_bad_rows(d);
        nf_t4_dec_free(d);

        for (int i = 1; i <= 5; i++)            /* damage 5 spots mid-stream */
            nbuf[(nlen / 7) * i] ^= 0x24;
        d = nf_t4_dec_init(w, mode, sink_row, NULL);
        nf_t4_dec_put(d, nbuf, nlen);
        int rows = nf_t4_dec_rows(d);
        int bad = nf_t4_dec_bad_rows(d);
        nf_t4_dec_free(d);

        /* Each damaged spot may cost a row or two (the resync can land
         * inside an EOL) and 2-D errors can cascade briefly - what matters
         * is: no abort, damage counted, page geometry roughly intact. */
        int ok = (clean_bad == 0) && (bad > 0) && (rows >= (h * 4) / 5) && (rows <= h + 2);
        printf("%-3s  corrupt: clean_bad=%d rows=%d/%d bad=%d  %s\n",
               nm, clean_bad, rows, h, bad, ok ? "OK" : "FAIL");
        if (!ok) fail = 1;
        free(nbuf);
    }

    free(orig);
    printf("%s\n", fail ? "FAIL" : "ALL PASS");
    return fail ? 1 : 0;
}
