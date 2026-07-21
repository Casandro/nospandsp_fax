#include "nf_t4.h"
#include <stdlib.h>
#include <string.h>

/*
 * ITU-T T.4 (MH 1-D / MR 2-D) and T.6 (MMR) bilevel codec.
 *
 * The run-length Huffman tables and the encoder are transcribed from spandsp
 * 0.0.6 src/t4_tx.c so the 1-D output is bit-for-bit identical; the 2-D/T.6
 * encoder follows the same algorithm. The decoder is an independent prefix
 * decoder (spandsp uses a generated state table; a direct decode is simpler and
 * the codes are prefix-free). Correctness is pinned by the round-trip
 * cross-check against spandsp.
 *
 * Bit order on the wire: codes are emitted LSB-first from the (reversed) `code`
 * values below, which reproduces the canonical MSB-first T.4 bit sequence.
 */

typedef struct { uint16_t length; uint16_t code; int16_t run; } rte_t;

/* White run-length codes: index 0..63 = terminating runs 0..63;
 * index 63+(run>>6) = make-up runs 64..2560. (104 entries.) */
static const rte_t white_codes[] = {
    { 8,0x00AC,   0},{ 6,0x0038,   1},{ 4,0x000E,   2},{ 4,0x0001,   3},
    { 4,0x000D,   4},{ 4,0x0003,   5},{ 4,0x0007,   6},{ 4,0x000F,   7},
    { 5,0x0019,   8},{ 5,0x0005,   9},{ 5,0x001C,  10},{ 5,0x0002,  11},
    { 6,0x0004,  12},{ 6,0x0030,  13},{ 6,0x000B,  14},{ 6,0x002B,  15},
    { 6,0x0015,  16},{ 6,0x0035,  17},{ 7,0x0072,  18},{ 7,0x0018,  19},
    { 7,0x0008,  20},{ 7,0x0074,  21},{ 7,0x0060,  22},{ 7,0x0010,  23},
    { 7,0x000A,  24},{ 7,0x006A,  25},{ 7,0x0064,  26},{ 7,0x0012,  27},
    { 7,0x000C,  28},{ 8,0x0040,  29},{ 8,0x00C0,  30},{ 8,0x0058,  31},
    { 8,0x00D8,  32},{ 8,0x0048,  33},{ 8,0x00C8,  34},{ 8,0x0028,  35},
    { 8,0x00A8,  36},{ 8,0x0068,  37},{ 8,0x00E8,  38},{ 8,0x0014,  39},
    { 8,0x0094,  40},{ 8,0x0054,  41},{ 8,0x00D4,  42},{ 8,0x0034,  43},
    { 8,0x00B4,  44},{ 8,0x0020,  45},{ 8,0x00A0,  46},{ 8,0x0050,  47},
    { 8,0x00D0,  48},{ 8,0x004A,  49},{ 8,0x00CA,  50},{ 8,0x002A,  51},
    { 8,0x00AA,  52},{ 8,0x0024,  53},{ 8,0x00A4,  54},{ 8,0x001A,  55},
    { 8,0x009A,  56},{ 8,0x005A,  57},{ 8,0x00DA,  58},{ 8,0x0052,  59},
    { 8,0x00D2,  60},{ 8,0x004C,  61},{ 8,0x00CC,  62},{ 8,0x002C,  63},
    { 5,0x001B,  64},{ 5,0x0009, 128},{ 6,0x003A, 192},{ 7,0x0076, 256},
    { 8,0x006C, 320},{ 8,0x00EC, 384},{ 8,0x0026, 448},{ 8,0x00A6, 512},
    { 8,0x0016, 576},{ 8,0x00E6, 640},{ 9,0x0066, 704},{ 9,0x0166, 768},
    { 9,0x0096, 832},{ 9,0x0196, 896},{ 9,0x0056, 960},{ 9,0x0156,1024},
    { 9,0x00D6,1088},{ 9,0x01D6,1152},{ 9,0x0036,1216},{ 9,0x0136,1280},
    { 9,0x00B6,1344},{ 9,0x01B6,1408},{ 9,0x0032,1472},{ 9,0x0132,1536},
    { 9,0x00B2,1600},{ 6,0x0006,1664},{ 9,0x01B2,1728},{11,0x0080,1792},
    {11,0x0180,1856},{11,0x0580,1920},{12,0x0480,1984},{12,0x0C80,2048},
    {12,0x0280,2112},{12,0x0A80,2176},{12,0x0680,2240},{12,0x0E80,2304},
    {12,0x0380,2368},{12,0x0B80,2432},{12,0x0780,2496},{12,0x0F80,2560},
};

/* Black run-length codes, same index layout. (104 entries.) */
static const rte_t black_codes[] = {
    {10,0x03B0,   0},{ 3,0x0002,   1},{ 2,0x0003,   2},{ 2,0x0001,   3},
    { 3,0x0006,   4},{ 4,0x000C,   5},{ 4,0x0004,   6},{ 5,0x0018,   7},
    { 6,0x0028,   8},{ 6,0x0008,   9},{ 7,0x0010,  10},{ 7,0x0050,  11},
    { 7,0x0070,  12},{ 8,0x0020,  13},{ 8,0x00E0,  14},{ 9,0x0030,  15},
    {10,0x03A0,  16},{10,0x0060,  17},{10,0x0040,  18},{11,0x0730,  19},
    {11,0x00B0,  20},{11,0x01B0,  21},{11,0x0760,  22},{11,0x00A0,  23},
    {11,0x0740,  24},{11,0x00C0,  25},{12,0x0530,  26},{12,0x0D30,  27},
    {12,0x0330,  28},{12,0x0B30,  29},{12,0x0160,  30},{12,0x0960,  31},
    {12,0x0560,  32},{12,0x0D60,  33},{12,0x04B0,  34},{12,0x0CB0,  35},
    {12,0x02B0,  36},{12,0x0AB0,  37},{12,0x06B0,  38},{12,0x0EB0,  39},
    {12,0x0360,  40},{12,0x0B60,  41},{12,0x05B0,  42},{12,0x0DB0,  43},
    {12,0x02A0,  44},{12,0x0AA0,  45},{12,0x06A0,  46},{12,0x0EA0,  47},
    {12,0x0260,  48},{12,0x0A60,  49},{12,0x04A0,  50},{12,0x0CA0,  51},
    {12,0x0240,  52},{12,0x0EC0,  53},{12,0x01C0,  54},{12,0x0E40,  55},
    {12,0x0140,  56},{12,0x01A0,  57},{12,0x09A0,  58},{12,0x0D40,  59},
    {12,0x0340,  60},{12,0x05A0,  61},{12,0x0660,  62},{12,0x0E60,  63},
    {10,0x03C0,  64},{12,0x0130, 128},{12,0x0930, 192},{12,0x0DA0, 256},
    {12,0x0CC0, 320},{12,0x02C0, 384},{12,0x0AC0, 448},{13,0x06C0, 512},
    {13,0x16C0, 576},{13,0x0A40, 640},{13,0x1A40, 704},{13,0x0640, 768},
    {13,0x1640, 832},{13,0x09C0, 896},{13,0x19C0, 960},{13,0x05C0,1024},
    {13,0x15C0,1088},{13,0x0DC0,1152},{13,0x1DC0,1216},{13,0x0940,1280},
    {13,0x1940,1344},{13,0x0540,1408},{13,0x1540,1472},{13,0x0B40,1536},
    {13,0x1B40,1600},{13,0x04C0,1664},{13,0x14C0,1728},{11,0x0080,1792},
    {11,0x0180,1856},{11,0x0580,1920},{12,0x0480,1984},{12,0x0C80,2048},
    {12,0x0280,2112},{12,0x0A80,2176},{12,0x0680,2240},{12,0x0E80,2304},
    {12,0x0380,2368},{12,0x0B80,2432},{12,0x0780,2496},{12,0x0F80,2560},
};

/* 2-D mode codes (vertical VR3..VL3, then horizontal, then pass). */
static const rte_t code_v[7]   = {
    { 7,0x60,0},{ 6,0x30,0},{ 3,0x06,0},{ 1,0x01,0},{ 3,0x02,0},{ 6,0x10,0},{ 7,0x20,0}
};                                  /* index = (a1b1 diff) + 3, range -3..3 */
static const rte_t code_horiz = { 3,0x04,0 };
static const rte_t code_pass  = { 4,0x08,0 };

#define TABLE_LEN 104

/* MSB-first within byte, 1 = black. */
static inline int px_black(const uint8_t *r, int i) { return (r[i >> 3] >> (7 - (i & 7))) & 1; }
static inline void set_black(uint8_t *r, int i)      { r[i >> 3] |= (uint8_t)(0x80 >> (i & 7)); }
static inline void set_range(uint8_t *r, int a, int b) { for (int i = a; i < b; i++) set_black(r, i); }

/* Cumulative changing-element positions of a packed row, line starting white;
 * the list always ends with `width`. Returns the count. */
static int row_runs(uint32_t *list, const uint8_t *row, int width)
{
    int n = 0, color = 0;
    for (int i = 0; i < width; i++) {
        int b = px_black(row, i);
        if (b != color) { list[n++] = (uint32_t) i; color = b; }
    }
    list[n++] = (uint32_t) width;
    return n;
}

/* ── Encoder ───────────────────────────────────────────────────────── */

struct nf_t4_enc {
    int width, comp, k;
    uint8_t *buf; size_t len, cap;
    uint32_t acc; int nbits;        /* bit accumulator, LSB-first */
    int row_bits;
    int row_is_2d, rows_to_next_1d, max_rows_to_next_1d;
    uint32_t *ref_runs, *cur_runs; int ref_steps;
    int min_row_bits;          /* T.4 FILL target; 0 = no padding */
};

static int eput(nf_t4_enc_t *e, uint32_t bits, int length)
{
    e->acc |= (bits << e->nbits);
    e->nbits += length;
    e->row_bits += length;
    while (e->nbits >= 8) {
        if (e->len >= e->cap) {
            size_t nc = e->cap ? e->cap * 2 : 4096;
            uint8_t *t = realloc(e->buf, nc);
            if (!t) return -1;
            e->buf = t; e->cap = nc;
        }
        e->buf[e->len++] = (uint8_t) (e->acc & 0xFF);
        e->acc >>= 8;
        e->nbits -= 8;
    }
    return 0;
}

static void put_1d_span(nf_t4_enc_t *e, int32_t span, const rte_t *tab)
{
    const rte_t *te = &tab[63 + (2560 >> 6)];
    while (span >= 2560 + 64) { eput(e, te->code, te->length); span -= te->run; }
    te = &tab[63 + (span >> 6)];
    if (span >= 64) { eput(e, te->code, te->length); span -= te->run; }
    eput(e, tab[span].code, tab[span].length);
}

static void encode_eol(nf_t4_enc_t *e)
{
    uint32_t code; int length;
    /* T.4 FILL: zeros inserted before an EOL stretch the line to the
     * receiver's minimum scan line time. Pad only after actual row data
     * (row_bits > an EOL's worth), so RTC's back-to-back EOLs stay clean. */
    if (e->min_row_bits > 0 && e->row_bits > 13 && e->row_bits < e->min_row_bits) {
        int fill = e->min_row_bits - e->row_bits;
        while (fill > 0) {
            int n = fill > 24 ? 24 : fill;
            eput(e, 0, n);
            fill -= n;
        }
    }
    if (e->comp == NF_T4_COMPRESSION_2D) {
        code = 0x0800 | ((uint32_t) (!e->row_is_2d) << 12);
        length = 13;
    } else {
        code = 0x0800; length = 12;        /* 1-D EOL or T.6 EOFB element */
    }
    eput(e, code, length);
    e->row_bits = 0;
}

static void encode_1d_row(nf_t4_enc_t *e, const uint8_t *row)
{
    e->ref_steps = row_runs(e->ref_runs, row, e->width);
    put_1d_span(e, (int32_t) e->ref_runs[0], white_codes);
    for (int i = 1; i < e->ref_steps; i++)
        put_1d_span(e, (int32_t) (e->ref_runs[i] - e->ref_runs[i - 1]),
                    (i & 1) ? black_codes : white_codes);
    e->ref_runs[e->ref_steps] = e->ref_runs[e->ref_steps + 1] =
        e->ref_runs[e->ref_steps + 2] = e->ref_runs[e->ref_steps - 1];
}

static void encode_2d_row(nf_t4_enc_t *e, const uint8_t *row)
{
    int a0, a1, a2, b1, b2, diff, a_cursor, b_cursor, cur_steps;
    uint32_t *p;

    cur_steps = row_runs(e->cur_runs, row, e->width);
    e->cur_runs[cur_steps] = e->cur_runs[cur_steps + 1] =
        e->cur_runs[cur_steps + 2] = e->cur_runs[cur_steps - 1];

    a0 = 0;
    a1 = (int) e->cur_runs[0];
    b1 = (int) e->ref_runs[0];
    a_cursor = 0; b_cursor = 0;
    for (;;) {
        b2 = (int) e->ref_runs[b_cursor + 1];
        if (b2 >= a1) {
            diff = b1 - a1;
            if (abs(diff) <= 3) {
                eput(e, code_v[diff + 3].code, code_v[diff + 3].length);
                a0 = a1; a_cursor++;
            } else {
                a2 = (int) e->cur_runs[a_cursor + 1];
                eput(e, code_horiz.code, code_horiz.length);
                if (a0 + a1 == 0 || px_black(row, a0) == 0) {
                    put_1d_span(e, a1 - a0, white_codes);
                    put_1d_span(e, a2 - a1, black_codes);
                } else {
                    put_1d_span(e, a1 - a0, black_codes);
                    put_1d_span(e, a2 - a1, white_codes);
                }
                a0 = a2; a_cursor += 2;
            }
            if (a0 >= e->width) break;
            if (a_cursor >= cur_steps) a_cursor = cur_steps - 1;
            a1 = (int) e->cur_runs[a_cursor];
        } else {
            eput(e, code_pass.code, code_pass.length);
            a0 = b2;
            if (a0 >= e->width) break;
        }
        if (px_black(row, a0)) b_cursor |= 1; else b_cursor &= ~1;
        if (a0 < (int) e->ref_runs[b_cursor]) {
            for (; b_cursor >= 0; b_cursor -= 2)
                if (a0 >= (int) e->ref_runs[b_cursor]) break;
            b_cursor += 2;
        } else {
            for (; b_cursor < e->ref_steps; b_cursor += 2)
                if (a0 < (int) e->ref_runs[b_cursor]) break;
            if (b_cursor >= e->ref_steps) b_cursor = e->ref_steps - 1;
        }
        b1 = (int) e->ref_runs[b_cursor];
    }
    e->ref_steps = cur_steps;
    p = e->cur_runs; e->cur_runs = e->ref_runs; e->ref_runs = p;
}

nf_t4_enc_t *nf_t4_enc_init(int width, int compression, int k_param)
{
    nf_t4_enc_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->width = width;
    e->comp = compression;
    e->max_rows_to_next_1d = k_param > 0 ? k_param : 2;
    e->ref_runs = malloc((size_t) (width + 4) * sizeof(uint32_t));
    e->cur_runs = malloc((size_t) (width + 4) * sizeof(uint32_t));
    if (!e->ref_runs || !e->cur_runs) { nf_t4_enc_free(e); return NULL; }
    e->ref_runs[0] = e->ref_runs[1] = e->ref_runs[2] = e->ref_runs[3] = (uint32_t) width;
    e->ref_steps = 1;
    e->row_is_2d = (compression == NF_T4_COMPRESSION_T6);
    e->rows_to_next_1d = e->max_rows_to_next_1d - 1;
    return e;
}

void nf_t4_enc_set_min_row_bits(nf_t4_enc_t *e, int bits)
{
    e->min_row_bits = bits;
}

int nf_t4_enc_row(nf_t4_enc_t *e, const uint8_t *row)
{
    switch (e->comp) {
    case NF_T4_COMPRESSION_T6:
        e->row_bits = 0;
        encode_2d_row(e, row);
        break;
    case NF_T4_COMPRESSION_2D:
        encode_eol(e);
        if (e->row_is_2d) { encode_2d_row(e, row); e->rows_to_next_1d--; }
        else { encode_1d_row(e, row); e->row_is_2d = 1; }
        if (e->rows_to_next_1d <= 0) {
            e->row_is_2d = 0;
            e->rows_to_next_1d = e->max_rows_to_next_1d - 1;
        }
        break;
    default:
        encode_eol(e);
        encode_1d_row(e, row);
        break;
    }
    return 0;
}

int nf_t4_enc_end_page(nf_t4_enc_t *e)
{
    int eols = (e->comp == NF_T4_COMPRESSION_T6) ? 2 : 6;
    if (e->comp != NF_T4_COMPRESSION_T6) e->row_is_2d = 0;
    for (int i = 0; i < eols; i++) encode_eol(e);
    eput(e, 0xFF, 7);              /* flush trailing partial byte with ones */
    return 0;
}

const uint8_t *nf_t4_enc_data(const nf_t4_enc_t *e, size_t *len) { *len = e->len; return e->buf; }

void nf_t4_enc_free(nf_t4_enc_t *e)
{
    if (!e) return;
    free(e->ref_runs); free(e->cur_runs); free(e->buf); free(e);
}

/* ── Decoder ───────────────────────────────────────────────────────── */

struct nf_t4_dec {
    int width, comp;
    nf_t4_row_handler_t cb; void *cb_user;
    const uint8_t *data; size_t len; size_t bitpos;   /* set per put() call */
    uint8_t *row;                                      /* current decoded row */
    uint32_t *ref; int ref_n;                          /* reference transitions */
    uint8_t *good_row;                                 /* last good row (concealment) */
    int rows;
    int bad_rows;
    int ended;
    int started_2d;                                    /* T.4-2D: first row done */
};

static inline int bits_left(const nf_t4_dec_t *d) { return (int) (d->len * 8 - d->bitpos); }

/* Next transmitted bit (LSB-first within byte), or -1 if exhausted. */
static inline int get_bit(nf_t4_dec_t *d)
{
    if (d->bitpos >= d->len * 8) return -1;
    int byte = d->data[d->bitpos >> 3];
    int bit = (byte >> (d->bitpos & 7)) & 1;
    d->bitpos++;
    return bit;
}

/* Decode one run-length code from `tab`. Builds the canonical (MSB-first) value
 * incrementally and matches against the prefix-free table. Returns the run, or
 * -1 on error. */
static int read_code(nf_t4_dec_t *d, const rte_t *tab)
{
    uint32_t v = 0; int n = 0;
    while (n < 14) {
        int b = get_bit(d);
        if (b < 0) return -1;
        v = (v << 1) | (uint32_t) b;
        n++;
        for (int i = 0; i < TABLE_LEN; i++) {
            if (tab[i].length != n) continue;
            /* canonical code = bit-reversed stored code over `length` bits */
            uint32_t canon = 0, c = tab[i].code;
            for (int k = 0; k < n; k++) { canon = (canon << 1) | (c & 1); c >>= 1; }
            if (canon == v) return tab[i].run;
        }
    }
    return -1;
}

/* A complete run (make-up codes + terminating). Returns total run, -1 on error. */
static int read_run(nf_t4_dec_t *d, int black)
{
    int total = 0, r;
    const rte_t *tab = black ? black_codes : white_codes;
    do {
        r = read_code(d, tab);
        if (r < 0) return -1;
        total += r;
        /* Saturate. A crafted stream of nothing but make-up codes would grow
         * total without bound and eventually wrap negative; the cap is far
         * above any legal run (max line width 3456) so valid input is never
         * affected, and the caller clamps to the line width regardless. */
        if (total > (1 << 20)) total = 1 << 20;
    } while (r >= 64);
    return total;
}

static int consume_eol_tag(nf_t4_dec_t *d, int *tag_1d);

/* Consume a 1-D EOL (>=11 zero bits then a 1) if present at the cursor.
 * Returns 1 if an EOL was consumed, 0 otherwise. Leaves the cursor unchanged
 * when no EOL is present. */
static int consume_eol(nf_t4_dec_t *d)
{
    int tag = 0;                            /* tag bit consumed but not needed */
    return consume_eol_tag(d, &tag);
}

/* Like consume_eol but reports the tag bit (1 => next row 1-D) for T.4-2D. */
/* Note: *tag_1d is only written when an EOL is actually found, so that the
 * caller's loop (which calls this once more than it succeeds) does not clobber
 * the tag from the last real EOL. The caller initialises tag_1d per row. */
static int consume_eol_tag(nf_t4_dec_t *d, int *tag_1d)
{
    size_t save = d->bitpos;
    int zeros = 0, b;
    while ((b = get_bit(d)) == 0) zeros++;
    if (b == 1 && zeros >= 11) {
        if (d->comp == NF_T4_COMPRESSION_2D) { int t = get_bit(d); *tag_1d = (t == 1); }
        return 1;
    }
    d->bitpos = save;
    return 0;
}

static int decode_1d_row(nf_t4_dec_t *d)
{
    memset(d->row, 0, (size_t) ((d->width + 7) / 8));
    int a = 0, black = 0;
    while (a < d->width) {
        int run = read_run(d, black);
        if (run < 0) return -1;
        if (a + run > d->width) run = d->width - a;
        if (black) set_range(d->row, a, a + run);
        a += run; black ^= 1;
    }
    return 0;
}

/* Reference changing element b1 to the right of a0 of colour opposite to the
 * current coding colour, plus b2 = the element after it. ref[] elements
 * alternate, with ref[k] beginning a run of colour (k even ? black : white). */
static void find_b(const nf_t4_dec_t *d, int a0, int color, int *b1, int *b2)
{
    int want = !color;                  /* colour of the run b1 begins */
    int k = 0;
    while (k < d->ref_n) {
        int begins = (k & 1) ? 0 : 1;   /* even -> black, odd -> white */
        if ((int) d->ref[k] > a0 && begins == want) break;
        k++;
    }
    *b1 = (k < d->ref_n) ? (int) d->ref[k] : d->width;
    *b2 = (k + 1 < d->ref_n) ? (int) d->ref[k + 1] : d->width;
}

static int decode_2d_row(nf_t4_dec_t *d)
{
    memset(d->row, 0, (size_t) ((d->width + 7) / 8));
    int a0 = -1, color = 0;             /* a0 just before line; start white */
    while (a0 < d->width) {
        int b1, b2;
        find_b(d, a0, color, &b1, &b2);
        /* read 2-D mode */
        uint32_t v = 0; int n = 0, mode = -1, vdiff = 0;
        while (n < 7 && mode < 0) {
            int bit = get_bit(d);
            if (bit < 0) return -1;
            v = (v << 1) | (uint32_t) bit; n++;
            if (n == 1 && v == 0x1) { mode = 1; vdiff = 0; }            /* V0 */
            else if (n == 3 && v == 0x1) mode = 0;                       /* H 001 */
            else if (n == 4 && v == 0x1) mode = 2;                       /* P 0001 */
            else if (n == 3 && v == 0x3) { mode = 1; vdiff = 1; }        /* VR1 011 */
            else if (n == 3 && v == 0x2) { mode = 1; vdiff = -1; }       /* VL1 010 */
            else if (n == 6 && v == 0x3) { mode = 1; vdiff = 2; }        /* VR2 000011 */
            else if (n == 6 && v == 0x2) { mode = 1; vdiff = -2; }       /* VL2 000010 */
            else if (n == 7 && v == 0x3) { mode = 1; vdiff = 3; }        /* VR3 0000011 */
            else if (n == 7 && v == 0x2) { mode = 1; vdiff = -3; }       /* VL3 0000010 */
        }
        if (mode < 0) return -1;
        int start = (a0 < 0) ? 0 : a0;
        if (mode == 2) {                                  /* pass */
            if (color) set_range(d->row, start, b2);
            a0 = b2;
        } else if (mode == 1) {                           /* vertical */
            int a1 = b1 + vdiff;
            if (a1 < 0) a1 = 0;
            if (a1 > d->width) a1 = d->width;
            if (color) set_range(d->row, start, a1);
            a0 = a1; color ^= 1;
        } else {                                          /* horizontal */
            int r1 = read_run(d, color);
            int r2 = read_run(d, !color);
            if (r1 < 0 || r2 < 0) return -1;
            int a1 = start + r1; if (a1 > d->width) a1 = d->width;
            int a2 = a1 + r2;    if (a2 > d->width) a2 = d->width;
            if (color) set_range(d->row, start, a1); else set_range(d->row, a1, a2);
            a0 = a2;
        }
    }
    return 0;
}

static void emit_row(nf_t4_dec_t *d)
{
    d->cb(d->cb_user, d->row, d->width);
    d->rows++;
    /* This row becomes the reference for the next 2-D row. */
    d->ref_n = row_runs(d->ref, d->row, d->width);
}

nf_t4_dec_t *nf_t4_dec_init(int width, int compression,
                            nf_t4_row_handler_t row_handler, void *user_data)
{
    nf_t4_dec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->width = width; d->comp = compression;
    d->cb = row_handler; d->cb_user = user_data;
    d->row = malloc((size_t) ((width + 7) / 8));
    d->ref = malloc((size_t) (width + 4) * sizeof(uint32_t));
    d->good_row = calloc(1, (size_t) ((width + 7) / 8));   /* starts white */
    if (!d->row || !d->ref || !d->good_row) { nf_t4_dec_free(d); return NULL; }
    d->ref[0] = (uint32_t) width; d->ref_n = 1;      /* imaginary white reference line */
    return d;
}

/* The whole page bitstream is expected in one put() (matches how T.4 non-ECM
 * pages are assembled). Returns 1 on page end, -1 on error. */
int nf_t4_dec_put(nf_t4_dec_t *d, const uint8_t *data, size_t len)
{
    d->data = data; d->len = len; d->bitpos = 0;

    if (d->comp == NF_T4_COMPRESSION_T6) {
        for (;;) {
            /* EOFB = two EOLs; also stop if out of bits. */
            if (bits_left(d) < 8) { d->ended = 1; break; }
            size_t save = d->bitpos;
            if (consume_eol(d)) { d->ended = 1; break; }
            d->bitpos = save;
            if (decode_2d_row(d) < 0) return -1;
            emit_row(d);
            if (d->rows > 100000) break;             /* runaway guard */
        }
        return d->ended ? 1 : 0;
    }

    /* T.4 1-D / 2-D: rows separated by EOL; RTC (6 EOLs) ends the page. */
    for (;;) {
        int consecutive = 0, tag_1d = 0, got = 0;
        while (consume_eol_tag(d, &tag_1d)) { consecutive++; got = 1; if (consecutive >= 6) { d->ended = 1; break; } }
        if (d->ended) break;
        if (bits_left(d) < 8) { d->ended = 1; break; }
        /* No leading EOL? Tolerate it: the first row is treated as 1-D below. */
        int row_1d = (d->comp == NF_T4_COMPRESSION_1D) || tag_1d || (d->rows == 0 && !got);
        int rc = row_1d ? decode_1d_row(d) : decode_2d_row(d);
        if (rc < 0) {
            /* A damaged row: conceal it by repeating the last good row (it
             * also becomes the 2-D reference), count it, and resynchronise
             * at the next EOL, as spandsp's T.4 decoder does. */
            memcpy(d->row, d->good_row, (size_t) ((d->width + 7) / 8));
            emit_row(d);
            d->bad_rows++;
            int t;
            size_t save;
            for (;;) {
                if (bits_left(d) < 12) { d->ended = 1; break; }
                save = d->bitpos;
                if (consume_eol_tag(d, &t)) { d->bitpos = save; break; }
                d->bitpos = save + 1;
            }
            if (d->ended) break;
            continue;
        }
        memcpy(d->good_row, d->row, (size_t) ((d->width + 7) / 8));
        emit_row(d);
        if (d->rows > 100000) break;
    }
    return d->ended ? 1 : 0;
}

int nf_t4_dec_rows(const nf_t4_dec_t *d) { return d->rows; }

int nf_t4_dec_bad_rows(const nf_t4_dec_t *d) { return d->bad_rows; }

void nf_t4_dec_free(nf_t4_dec_t *d)
{
    if (!d) return;
    free(d->row); free(d->good_row); free(d->ref); free(d);
}
