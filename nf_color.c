#define _GNU_SOURCE
#include "nf_color.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <jpeglib.h>

/*
 * sRGB <-> CIELAB(D50) <-> 8-bit T.42 components, plus T.81 baseline JPEG via
 * libjpeg. See nf_color.h. The JPEG is driven in JCS_UNKNOWN (opaque) mode so
 * libjpeg never applies an RGB<->YCbCr transform to our L*a*b* samples, and with
 * 1:1:1 sampling (no chroma subsampling).
 */

/* ── colour space: sRGB <-> CIELAB (D50) ───────────────────────────── */

/* Bradford-adapted sRGB<->XYZ matrices for the D50 white point (Lindbloom). */
static const double M_RGB2XYZ[3][3] = {
    { 0.4360747, 0.3850649, 0.1430804 },
    { 0.2225045, 0.7168786, 0.0606169 },
    { 0.0139322, 0.0971045, 0.7141733 },
};
static const double M_XYZ2RGB[3][3] = {
    {  3.1338561, -1.6168667, -0.4906146 },
    { -0.9787684,  1.9161415,  0.0334540 },
    {  0.0719453, -0.2289914,  1.4052427 },
};
/* D50 reference white */
#define XN 0.96422
#define YN 1.00000
#define ZN 0.82521

static double srgb2lin(double c)   /* c in [0,1] */
{
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double lin2srgb(double v)
{
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return (v <= 0.0031308) ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
static double labf(double t)
{
    const double d = 6.0 / 29.0;
    return (t > d * d * d) ? cbrt(t) : t / (3 * d * d) + 4.0 / 29.0;
}
static double labfinv(double t)
{
    const double d = 6.0 / 29.0;
    return (t > d) ? t * t * t : 3 * d * d * (t - 4.0 / 29.0);
}
static int clamp255(double x)
{
    long v = lround(x);
    return v < 0 ? 0 : (v > 255 ? 255 : (int) v);
}

/* one sRGB pixel -> 8-bit T.42 L*a*b* (L:0..255 over 0..100; a:+128/170; b:+96/200) */
static void srgb_to_lab8(const uint8_t rgb[3], uint8_t lab[3])
{
    double r = srgb2lin(rgb[0] / 255.0);
    double g = srgb2lin(rgb[1] / 255.0);
    double b = srgb2lin(rgb[2] / 255.0);
    double X = M_RGB2XYZ[0][0]*r + M_RGB2XYZ[0][1]*g + M_RGB2XYZ[0][2]*b;
    double Y = M_RGB2XYZ[1][0]*r + M_RGB2XYZ[1][1]*g + M_RGB2XYZ[1][2]*b;
    double Z = M_RGB2XYZ[2][0]*r + M_RGB2XYZ[2][1]*g + M_RGB2XYZ[2][2]*b;
    double fx = labf(X / XN), fy = labf(Y / YN), fz = labf(Z / ZN);
    double L = 116.0 * fy - 16.0;
    double A = 500.0 * (fx - fy);
    double B = 200.0 * (fy - fz);
    lab[0] = clamp255(L * 255.0 / 100.0);
    lab[1] = clamp255(A * 255.0 / 170.0 + 128.0);
    lab[2] = clamp255(B * 255.0 / 200.0 + 96.0);
}

static void lab8_to_srgb(const uint8_t lab[3], uint8_t rgb[3])
{
    double L = lab[0] * 100.0 / 255.0;
    double A = (lab[1] - 128.0) * 170.0 / 255.0;
    double B = (lab[2] - 96.0) * 200.0 / 255.0;
    double fy = (L + 16.0) / 116.0;
    double fx = fy + A / 500.0;
    double fz = fy - B / 200.0;
    double X = XN * labfinv(fx), Y = YN * labfinv(fy), Z = ZN * labfinv(fz);
    double r = M_XYZ2RGB[0][0]*X + M_XYZ2RGB[0][1]*Y + M_XYZ2RGB[0][2]*Z;
    double g = M_XYZ2RGB[1][0]*X + M_XYZ2RGB[1][1]*Y + M_XYZ2RGB[1][2]*Z;
    double b = M_XYZ2RGB[2][0]*X + M_XYZ2RGB[2][1]*Y + M_XYZ2RGB[2][2]*Z;
    rgb[0] = clamp255(lin2srgb(r) * 255.0);
    rgb[1] = clamp255(lin2srgb(g) * 255.0);
    rgb[2] = clamp255(lin2srgb(b) * 255.0);
}

/* ── libjpeg error handling (longjmp instead of exit) ──────────────── */

struct nf_jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void nf_jpeg_error_exit(j_common_ptr ci)
{
    struct nf_jerr *e = (struct nf_jerr *) ci->err;
    longjmp(e->jb, 1);
}

/* T.42-style identifying marker (we use the default D50 illuminant / gamut). */
static const uint8_t G3FAX_MARKER[] = { 'G','3','F','A','X', 0x00, 0x01 };

/* ── encode ────────────────────────────────────────────────────────── */

int nf_color_encode(const uint8_t *rgb, int w, int h, int quality,
                    uint8_t **out, size_t *out_len)
{
    if (!rgb || w <= 0 || h <= 0 || !out || !out_len) return -1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    /* sRGB -> L*a*b* (same packed layout) */
    uint8_t *lab = malloc((size_t) w * h * 3);
    if (!lab) return -1;
    for (size_t i = 0; i < (size_t) w * h; i++)
        srgb_to_lab8(rgb + i * 3, lab + i * 3);

    struct jpeg_compress_struct cinfo;
    struct nf_jerr jerr;
    unsigned char *mem = NULL;
    unsigned long memlen = 0;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = nf_jpeg_error_exit;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_compress(&cinfo);
        free(lab);
        free(mem);
        return -1;
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &memlen);
    cinfo.image_width = (JDIMENSION) w;
    cinfo.image_height = (JDIMENSION) h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_UNKNOWN;        /* opaque: no colour transform */
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_UNKNOWN);  /* no JFIF/Adobe markers */
    for (int c = 0; c < 3; c++) {
        cinfo.comp_info[c].h_samp_factor = 1;  /* 1:1:1, no subsampling */
        cinfo.comp_info[c].v_samp_factor = 1;
    }
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    jpeg_write_marker(&cinfo, JPEG_APP0 + 1, G3FAX_MARKER, sizeof G3FAX_MARKER);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW) (lab + (size_t) cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(lab);

    /* mem was malloc'd by jpeg_mem_dest; hand it to the caller. */
    *out = mem;
    *out_len = (size_t) memlen;
    return 0;
}

/* ── decode ────────────────────────────────────────────────────────── */

int nf_color_decode(const uint8_t *bytes, size_t len,
                    uint8_t **rgb, int *w, int *h)
{
    if (!bytes || len == 0 || !rgb || !w || !h) return -1;

    struct jpeg_decompress_struct dinfo;
    struct nf_jerr jerr;
    uint8_t *lab = NULL;

    dinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = nf_jpeg_error_exit;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&dinfo);
        free(lab);
        return -1;
    }
    jpeg_create_decompress(&dinfo);
    jpeg_mem_src(&dinfo, bytes, (unsigned long) len);
    jpeg_save_markers(&dinfo, JPEG_APP0 + 1, 0xFFFF);   /* keep G3FAX if present */
    if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK) { longjmp(jerr.jb, 1); }
    /* A 3-component stream with no JFIF/Adobe marker is assumed YCbCr by libjpeg;
     * tell it the stored components are opaque so it does an identity transform. */
    dinfo.jpeg_color_space = JCS_UNKNOWN;
    dinfo.out_color_space = JCS_UNKNOWN;                /* opaque, 3 components */
    jpeg_start_decompress(&dinfo);
    int W = (int) dinfo.output_width, H = (int) dinfo.output_height;
    if (dinfo.output_components != 3) { longjmp(jerr.jb, 1); }
    lab = malloc((size_t) W * H * 3);
    if (!lab) { longjmp(jerr.jb, 1); }
    while (dinfo.output_scanline < dinfo.output_height) {
        JSAMPROW row = (JSAMPROW) (lab + (size_t) dinfo.output_scanline * W * 3);
        jpeg_read_scanlines(&dinfo, &row, 1);
    }
    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);

    /* L*a*b* -> sRGB (in place reuse: write to a fresh rgb buffer) */
    uint8_t *out = malloc((size_t) W * H * 3);
    if (!out) { free(lab); return -1; }
    for (size_t i = 0; i < (size_t) W * H; i++)
        lab8_to_srgb(lab + i * 3, out + i * 3);
    free(lab);
    *rgb = out; *w = W; *h = H;
    return 0;
}

/* ── greyscale (single L* component) ───────────────────────────────── */

int nf_gray_encode(const uint8_t *rgb, int w, int h, int quality,
                   uint8_t **out, size_t *out_len)
{
    if (!rgb || w <= 0 || h <= 0 || !out || !out_len) return -1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    uint8_t *lum = malloc((size_t) w * h);     /* one L* byte per pixel */
    if (!lum) return -1;
    for (size_t i = 0; i < (size_t) w * h; i++) {
        uint8_t lab[3];
        srgb_to_lab8(rgb + i * 3, lab);
        lum[i] = lab[0];
    }

    struct jpeg_compress_struct cinfo;
    struct nf_jerr jerr;
    unsigned char *mem = NULL;
    unsigned long memlen = 0;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = nf_jpeg_error_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_compress(&cinfo); free(lum); free(mem); return -1; }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &memlen);
    cinfo.image_width = (JDIMENSION) w;
    cinfo.image_height = (JDIMENSION) h;
    cinfo.input_components = 1;
    cinfo.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_GRAYSCALE);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    jpeg_write_marker(&cinfo, JPEG_APP0 + 1, G3FAX_MARKER, sizeof G3FAX_MARKER);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW) (lum + (size_t) cinfo.next_scanline * w);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(lum);
    *out = mem;
    *out_len = (size_t) memlen;
    return 0;
}

int nf_gray_decode(const uint8_t *bytes, size_t len,
                   uint8_t **gray, int *w, int *h)
{
    if (!bytes || len == 0 || !gray || !w || !h) return -1;

    struct jpeg_decompress_struct dinfo;
    struct nf_jerr jerr;
    uint8_t *lum = NULL;

    dinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = nf_jpeg_error_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&dinfo); free(lum); return -1; }
    jpeg_create_decompress(&dinfo);
    jpeg_mem_src(&dinfo, bytes, (unsigned long) len);
    if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK) { longjmp(jerr.jb, 1); }
    dinfo.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&dinfo);
    int W = (int) dinfo.output_width, H = (int) dinfo.output_height;
    if (dinfo.output_components != 1) { longjmp(jerr.jb, 1); }
    lum = malloc((size_t) W * H);
    if (!lum) { longjmp(jerr.jb, 1); }
    while (dinfo.output_scanline < dinfo.output_height) {
        JSAMPROW row = (JSAMPROW) (lum + (size_t) dinfo.output_scanline * W);
        jpeg_read_scanlines(&dinfo, &row, 1);
    }
    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);

    /* L* byte -> neutral sRGB grey byte */
    uint8_t *out = malloc((size_t) W * H);
    if (!out) { free(lum); return -1; }
    for (size_t i = 0; i < (size_t) W * H; i++) {
        uint8_t lab[3] = { lum[i], 128, 96 }, rgb[3];
        lab8_to_srgb(lab, rgb);
        out[i] = rgb[0];
    }
    free(lum);
    *gray = out; *w = W; *h = H;
    return 0;
}
