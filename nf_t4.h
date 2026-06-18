#ifndef NF_T4_H
#define NF_T4_H

#include <stdint.h>
#include <stddef.h>

/*
 * nf_t4 - a from-scratch ITU-T T.4 / T.6 bilevel image codec, replacing
 * spandsp's t4_tx/t4_rx. It deals only in pixel rows and compressed bits; TIFF
 * file handling stays in the caller (libtiff). Symbols are nf_-prefixed so they
 * never clash with the still-linked libspandsp.
 *
 * Row format: packed bits, MSB-first within each byte, 1 = black, 0 = white
 * (i.e. the TIFF PHOTOMETRIC_MINISWHITE / FILLORDER_MSB2LSB scanline this tool
 * already produces). stride = (width + 7) / 8 bytes per row.
 *
 * Compression modes (values match spandsp's t4_image_compression_t):
 */
#define NF_T4_COMPRESSION_1D   1   /* T.4 1-D, Modified Huffman (MH)   */
#define NF_T4_COMPRESSION_2D   2   /* T.4 2-D, Modified READ (MR)      */
#define NF_T4_COMPRESSION_T6   3   /* T.6, Modified Modified READ (MMR)*/

/* ── Encoder: rows -> compressed bitstream ─────────────────────────── */

typedef struct nf_t4_enc nf_t4_enc_t;

/* Create an encoder for `width`-pixel rows using `compression`. `k_param` is the
 * T.4 2-D K factor (rows between forced 1-D rows; ignored for 1-D and T.6). Pass
 * 0 to use the spandsp-compatible default (2 for standard res, 4 otherwise -
 * the caller picks based on vertical resolution). */
nf_t4_enc_t *nf_t4_enc_init(int width, int compression, int k_param);

/* Non-ECM T.4 only: pad each row with FILL bits (zeros before the next EOL)
 * so that EOL+data+fill is at least `bits` long - the receiver's minimum
 * scan line time at the negotiated modem rate. 0 (default) disables. */
void nf_t4_enc_set_min_row_bits(nf_t4_enc_t *e, int bits);

/* Encode one row (packed, see above). Returns 0 on success, -1 on error. */
int nf_t4_enc_row(nf_t4_enc_t *e, const uint8_t *row);

/* Finish the page: append RTC (T.4) or EOFB (T.6) and flush the final byte. */
int nf_t4_enc_end_page(nf_t4_enc_t *e);

/* Borrow the accumulated bitstream (valid until free). */
const uint8_t *nf_t4_enc_data(const nf_t4_enc_t *e, size_t *len);

void nf_t4_enc_free(nf_t4_enc_t *e);

/* ── Decoder: compressed bitstream -> rows ─────────────────────────── */

typedef struct nf_t4_dec nf_t4_dec_t;

/* row_handler is called with each decoded row (packed, `width` pixels). */
typedef void (*nf_t4_row_handler_t)(void *user_data, const uint8_t *row, int width);

nf_t4_dec_t *nf_t4_dec_init(int width, int compression,
                            nf_t4_row_handler_t row_handler, void *user_data);

/* Push compressed bytes. Returns 1 once the page has ended (RTC/EOFB seen),
 * 0 if more data is expected, -1 on a decode error. */
int nf_t4_dec_put(nf_t4_dec_t *d, const uint8_t *data, size_t len);

/* Rows emitted, and how many of them were damaged and concealed (T.4 only:
 * a defective row is replaced by the previous good one and the decoder
 * resynchronises at the next EOL). */
int nf_t4_dec_rows(const nf_t4_dec_t *d);
int nf_t4_dec_bad_rows(const nf_t4_dec_t *d);

void nf_t4_dec_free(nf_t4_dec_t *d);

#endif /* NF_T4_H */
