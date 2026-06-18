#ifndef NF_COLOR_H
#define NF_COLOR_H

#include <stdint.h>
#include <stddef.h>

/*
 * nf_color - continuous-tone colour codec for ITU-T T.30 Annex E colour fax.
 *
 * It maps between packed 8-bit interleaved sRGB images and a T.42-style
 * codestream: the sRGB pixels are converted to CIELAB (L*a*b*, illuminant D50,
 * T.42 8-bit quantization) and coded with T.81 baseline JPEG (via libjpeg, in
 * opaque 3-component mode so libjpeg performs no colour transform of its own).
 * The byte stream produced is what the engine carries over ECM.
 *
 * Colour is lossy (JPEG + sRGB<->Lab rounding): a decode of an encode is close,
 * not identical. The codestream itself round-trips byte-exact over ECM.
 */

/* Encode a w*h packed sRGB image (3 bytes/pixel, row-major, no row padding) into
 * a T.42/T.81 codestream. Allocates *out (caller frees with free()).
 * quality is the JPEG quality 1..100. Returns 0 on success, -1 on error. */
int nf_color_encode(const uint8_t *rgb, int w, int h, int quality,
                    uint8_t **out, size_t *out_len);

/* Decode a T.42/T.81 codestream back to packed sRGB. Allocates *rgb (caller
 * frees) and sets *w,*h from the JPEG frame header. Returns 0 / -1. */
int nf_color_decode(const uint8_t *bytes, size_t len,
                    uint8_t **rgb, int *w, int *h);

/* Greyscale (T.30 Annex E grey mode): a single-component T.81 JPEG whose sample
 * is the CIELAB L* lightness. The encoder takes a packed sRGB image (3 B/px) and
 * reduces it to L*; the decoder returns one grey byte per pixel (sRGB grey). */
int nf_gray_encode(const uint8_t *rgb, int w, int h, int quality,
                   uint8_t **out, size_t *out_len);
int nf_gray_decode(const uint8_t *bytes, size_t len,
                   uint8_t **gray, int *w, int *h);

#endif /* NF_COLOR_H */
