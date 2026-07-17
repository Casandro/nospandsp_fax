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
 * the T.42 default 4:1:1 sampling (chroma at half resolution both ways).
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

/* Reject wildly out-of-range JPEG dimensions before allocating from them.
 * libjpeg permits up to 65500x65500; a real fax page is a few thousand
 * pixels each way. Without this, a tiny hostile JPEG forces a multi-gigabyte
 * allocation (DoS), and on a 32-bit build (size_t)W*H*comps overflows into an
 * undersized buffer that the scanline loop then writes past (heap overflow).
 * The per-dimension cap keeps W*H*comps well inside size_t on all platforms. */
#define NF_JPEG_MAX_DIM     10000
#define NF_JPEG_MAX_PIXELS  (64 * 1024 * 1024)   /* ~64 Mpx: covers A3 @ 400 dpi */

static int jpeg_dims_ok(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (w > NF_JPEG_MAX_DIM || h > NF_JPEG_MAX_DIM) return 0;
    if ((int64_t) w * h > NF_JPEG_MAX_PIXELS) return 0;
    return 1;
}

/* Compress `comps`-component `samples` (row stride w*comps) to a JPEG in a
 * freshly malloc'd buffer returned via out/out_len. `cs` is the colour space;
 * chroma_411 requests T.42 4:1:1 sub-sampling on the first component. The
 * caller owns `samples` and frees it regardless of the return. 0 ok, -1 fail. */
static int jpeg_compress(const uint8_t *samples, int w, int h, int comps,
                         J_COLOR_SPACE cs, int chroma_411, int quality,
                         uint8_t **out, size_t *out_len)
{
    struct jpeg_compress_struct cinfo;
    struct nf_jerr jerr;
    unsigned char *mem = NULL;
    unsigned long memlen = 0;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = nf_jpeg_error_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_compress(&cinfo); free(mem); return -1; }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &memlen);
    cinfo.image_width = (JDIMENSION) w;
    cinfo.image_height = (JDIMENSION) h;
    cinfo.input_components = comps;
    cinfo.in_color_space = cs;
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, cs);           /* no JFIF/Adobe markers */
    if (chroma_411) {
        /* T.42 default sub-sampling is 4:1:1 (chroma at half resolution both
         * ways); 1:1:1 is only allowed when the receiver advertises DIS bit 73,
         * so the default is the one mode every colour receiver must accept. */
        cinfo.comp_info[0].h_samp_factor = 2;
        cinfo.comp_info[0].v_samp_factor = 2;
        for (int c = 1; c < comps; c++) {
            cinfo.comp_info[c].h_samp_factor = 1;
            cinfo.comp_info[c].v_samp_factor = 1;
        }
    }
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    jpeg_write_marker(&cinfo, JPEG_APP0 + 1, G3FAX_MARKER, sizeof G3FAX_MARKER);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW) (samples + (size_t) cinfo.next_scanline * w * comps);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    *out = mem;
    *out_len = (size_t) memlen;
    return 0;
}

/* Decompress a JPEG to a freshly malloc'd `want_comps`-component planar buffer
 * (*planar, row stride W*want_comps) with the given output colour space. When
 * out_cs is JCS_UNKNOWN the stored components are treated as opaque (identity
 * transform - our L*a*b* streams). Rejects implausible dimensions and any
 * component count != want_comps. 0 ok, -1 fail. */
static int jpeg_decompress(const uint8_t *bytes, size_t len, int want_comps,
                           J_COLOR_SPACE out_cs, uint8_t **planar, int *w, int *h)
{
    struct jpeg_decompress_struct dinfo;
    struct nf_jerr jerr;
    uint8_t *buf = NULL;

    dinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = nf_jpeg_error_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&dinfo); free(buf); return -1; }
    jpeg_create_decompress(&dinfo);
    jpeg_mem_src(&dinfo, bytes, (unsigned long) len);
    jpeg_save_markers(&dinfo, JPEG_APP0 + 1, 0xFFFF);   /* keep G3FAX if present */
    if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK) longjmp(jerr.jb, 1);
    /* A stream with no JFIF/Adobe marker is assumed YCbCr by libjpeg; for our
     * opaque streams tell it the components are already final (no transform). */
    if (out_cs == JCS_UNKNOWN) dinfo.jpeg_color_space = JCS_UNKNOWN;
    dinfo.out_color_space = out_cs;
    jpeg_start_decompress(&dinfo);
    int W = (int) dinfo.output_width, H = (int) dinfo.output_height;
    if (dinfo.output_components != want_comps) longjmp(jerr.jb, 1);
    if (!jpeg_dims_ok(W, H)) longjmp(jerr.jb, 1);
    buf = malloc((size_t) W * H * want_comps);
    if (!buf) longjmp(jerr.jb, 1);
    while (dinfo.output_scanline < dinfo.output_height) {
        JSAMPROW row = (JSAMPROW) (buf + (size_t) dinfo.output_scanline * W * want_comps);
        jpeg_read_scanlines(&dinfo, &row, 1);
    }
    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);
    *planar = buf; *w = W; *h = H;
    return 0;
}

/* ── encode ────────────────────────────────────────────────────────── */

int nf_color_encode(const uint8_t *rgb, int w, int h, int quality,
                    uint8_t **out, size_t *out_len)
{
    if (!rgb || w <= 0 || h <= 0 || !out || !out_len) return -1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    /* sRGB -> L*a*b* (same packed layout), then a 3-component opaque JPEG. */
    uint8_t *lab = malloc((size_t) w * h * 3);
    if (!lab) return -1;
    for (size_t i = 0; i < (size_t) w * h; i++)
        srgb_to_lab8(rgb + i * 3, lab + i * 3);

    int rc = jpeg_compress(lab, w, h, 3, JCS_UNKNOWN, 1, quality, out, out_len);
    free(lab);
    return rc;
}

/* ── decode ────────────────────────────────────────────────────────── */

int nf_color_decode(const uint8_t *bytes, size_t len,
                    uint8_t **rgb, int *w, int *h)
{
    if (!bytes || len == 0 || !rgb || !w || !h) return -1;

    uint8_t *lab; int W, H;
    if (jpeg_decompress(bytes, len, 3, JCS_UNKNOWN, &lab, &W, &H) != 0) return -1;

    /* L*a*b* -> sRGB into a fresh buffer */
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

    int rc = jpeg_compress(lum, w, h, 1, JCS_GRAYSCALE, 0, quality, out, out_len);
    free(lum);
    return rc;
}

int nf_gray_decode(const uint8_t *bytes, size_t len,
                   uint8_t **gray, int *w, int *h)
{
    if (!bytes || len == 0 || !gray || !w || !h) return -1;

    uint8_t *lum; int W, H;
    if (jpeg_decompress(bytes, len, 1, JCS_GRAYSCALE, &lum, &W, &H) != 0) return -1;

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
