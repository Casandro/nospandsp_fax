#include "nf_v17.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int v17dbg(void) { static int d = -1; if (d < 0) d = getenv("NFV17DBG") ? 1 : 0; return d; }

#define CARRIER_HZ          1800.0
#define BAUD_RATE           2400.0
#define TX_LEVEL            (-14.0f)

/* training segment boundaries in tx symbol steps (TEP never sent for fax) */
#define SEG_TEP_B           480
#define SEG_1               (SEG_TEP_B + 48)        /* 528: start of ABAB */
#define SEG_2               (SEG_1 + 256)           /* 784: start of CDBA */
#define SEG_3               (SEG_2 + 2976)          /* 3760: start of bridge */
#define SEG_4               (SEG_3 + 64)            /* 3824: start of ones */
#define SHORT_SEG_4         (SEG_2 + 38)            /* short train jump point */
#define TRAIN_END           (SEG_4 + 48)            /* 3872 */
#define SHUTDOWN_A          (TRAIN_END + 32)        /* 32 bauds of ones */
#define SHUTDOWN_END        (SHUTDOWN_A + 48)       /* then 48 of silence */

#define V17_BRIDGE_WORD     0x8880

/* rx training segment lengths */
#define SEG_1_LEN           256
#define SEG_2_LEN           2976
#define SHORT_SEG_2_LEN     38
#define SEG_3_LEN           64
#define SEG_4A_LEN          15
#define SEG_4_LEN           48

enum {
    STAGE_NORMAL = 0,
    STAGE_SYMBOL_ACQUISITION,
    STAGE_LOG_PHASE,
    STAGE_SHORT_WAIT_FOR_CDBA,
    STAGE_WAIT_FOR_CDBA,
    STAGE_COARSE_TRAIN_ON_CDBA,
    STAGE_FINE_TRAIN_ON_CDBA,
    STAGE_SHORT_TRAIN_ON_CDBA_AND_TEST,
    STAGE_TRAIN_ON_CDBA_AND_TEST,
    STAGE_BRIDGE,
    STAGE_TCM_WINDUP,
    STAGE_TEST_ONES,
    STAGE_PARKED
};

/* minimum distance between points of one TCM subset, per space map */
static const float constellation_spacing[4] = { 1.414f, 2.0f, 2.828f, 4.0f };

/* constellations (ITU-T V.17 figures 2-5; index low bits = diff<<1 | conv) */
static const nf_cpx_t v17_c14400[128] = {
    {  -8.0,  -3.0}, {   9.0,   2.0}, {   2.0,  -9.0}, {  -3.0,   8.0},
    {   8.0,   3.0}, {  -9.0,  -2.0}, {  -2.0,   9.0}, {   3.0,  -8.0},
    {  -8.0,   1.0}, {   9.0,  -2.0}, {  -2.0,  -9.0}, {   1.0,   8.0},
    {   8.0,  -1.0}, {  -9.0,   2.0}, {   2.0,   9.0}, {  -1.0,  -8.0},
    {  -4.0,  -3.0}, {   5.0,   2.0}, {   2.0,  -5.0}, {  -3.0,   4.0},
    {   4.0,   3.0}, {  -5.0,  -2.0}, {  -2.0,   5.0}, {   3.0,  -4.0},
    {  -4.0,   1.0}, {   5.0,  -2.0}, {  -2.0,  -5.0}, {   1.0,   4.0},
    {   4.0,  -1.0}, {  -5.0,   2.0}, {   2.0,   5.0}, {  -1.0,  -4.0},
    {   4.0,  -3.0}, {  -3.0,   2.0}, {   2.0,   3.0}, {  -3.0,  -4.0},
    {  -4.0,   3.0}, {   3.0,  -2.0}, {  -2.0,  -3.0}, {   3.0,   4.0},
    {   4.0,   1.0}, {  -3.0,  -2.0}, {  -2.0,   3.0}, {   1.0,  -4.0},
    {  -4.0,  -1.0}, {   3.0,   2.0}, {   2.0,  -3.0}, {  -1.0,   4.0},
    {   0.0,  -3.0}, {   1.0,   2.0}, {   2.0,  -1.0}, {  -3.0,   0.0},
    {   0.0,   3.0}, {  -1.0,  -2.0}, {  -2.0,   1.0}, {   3.0,   0.0},
    {   0.0,   1.0}, {   1.0,  -2.0}, {  -2.0,  -1.0}, {   1.0,   0.0},
    {   0.0,  -1.0}, {  -1.0,   2.0}, {   2.0,   1.0}, {  -1.0,   0.0},
    {   8.0,  -3.0}, {  -7.0,   2.0}, {   2.0,   7.0}, {  -3.0,  -8.0},
    {  -8.0,   3.0}, {   7.0,  -2.0}, {  -2.0,  -7.0}, {   3.0,   8.0},
    {   8.0,   1.0}, {  -7.0,  -2.0}, {  -2.0,   7.0}, {   1.0,  -8.0},
    {  -8.0,  -1.0}, {   7.0,   2.0}, {   2.0,  -7.0}, {  -1.0,   8.0},
    {  -4.0,  -7.0}, {   5.0,   6.0}, {   6.0,  -5.0}, {  -7.0,   4.0},
    {   4.0,   7.0}, {  -5.0,  -6.0}, {  -6.0,   5.0}, {   7.0,  -4.0},
    {  -4.0,   5.0}, {   5.0,  -6.0}, {  -6.0,  -5.0}, {   5.0,   4.0},
    {   4.0,  -5.0}, {  -5.0,   6.0}, {   6.0,   5.0}, {  -5.0,  -4.0},
    {   4.0,  -7.0}, {  -3.0,   6.0}, {   6.0,   3.0}, {  -7.0,  -4.0},
    {  -4.0,   7.0}, {   3.0,  -6.0}, {  -6.0,  -3.0}, {   7.0,   4.0},
    {   4.0,   5.0}, {  -3.0,  -6.0}, {  -6.0,   3.0}, {   5.0,  -4.0},
    {  -4.0,  -5.0}, {   3.0,   6.0}, {   6.0,  -3.0}, {  -5.0,   4.0},
    {   0.0,  -7.0}, {   1.0,   6.0}, {   6.0,  -1.0}, {  -7.0,   0.0},
    {   0.0,   7.0}, {  -1.0,  -6.0}, {  -6.0,   1.0}, {   7.0,   0.0},
    {   0.0,   5.0}, {   1.0,  -6.0}, {  -6.0,  -1.0}, {   5.0,   0.0},
    {   0.0,  -5.0}, {  -1.0,   6.0}, {   6.0,   1.0}, {  -5.0,   0.0}
};

static const nf_cpx_t v17_c12000[64] = {
    {   7.0,   1.0}, {  -5.0,  -1.0}, {  -1.0,   5.0}, {   1.0,  -7.0},
    {  -7.0,  -1.0}, {   5.0,   1.0}, {   1.0,  -5.0}, {  -1.0,   7.0},
    {   3.0,  -3.0}, {  -1.0,   3.0}, {   3.0,   1.0}, {  -3.0,  -3.0},
    {  -3.0,   3.0}, {   1.0,  -3.0}, {  -3.0,  -1.0}, {   3.0,   3.0},
    {   7.0,  -7.0}, {  -5.0,   7.0}, {   7.0,   5.0}, {  -7.0,  -7.0},
    {  -7.0,   7.0}, {   5.0,  -7.0}, {  -7.0,  -5.0}, {   7.0,   7.0},
    {  -1.0,  -7.0}, {   3.0,   7.0}, {   7.0,  -3.0}, {  -7.0,   1.0},
    {   1.0,   7.0}, {  -3.0,  -7.0}, {  -7.0,   3.0}, {   7.0,  -1.0},
    {   3.0,   5.0}, {  -1.0,  -5.0}, {  -5.0,   1.0}, {   5.0,  -3.0},
    {  -3.0,  -5.0}, {   1.0,   5.0}, {   5.0,  -1.0}, {  -5.0,   3.0},
    {  -1.0,   1.0}, {   3.0,  -1.0}, {  -1.0,  -3.0}, {   1.0,   1.0},
    {   1.0,  -1.0}, {  -3.0,   1.0}, {   1.0,   3.0}, {  -1.0,  -1.0},
    {  -5.0,   5.0}, {   7.0,  -5.0}, {  -5.0,  -7.0}, {   5.0,   5.0},
    {   5.0,  -5.0}, {  -7.0,   5.0}, {   5.0,   7.0}, {  -5.0,  -5.0},
    {  -5.0,  -3.0}, {   7.0,   3.0}, {   3.0,  -7.0}, {  -3.0,   5.0},
    {   5.0,   3.0}, {  -7.0,  -3.0}, {  -3.0,   7.0}, {   3.0,  -5.0}
};

static const nf_cpx_t v17_c9600[32] = {
    {  -8.0,   2.0}, {  -6.0,  -4.0}, {  -4.0,   6.0}, {   2.0,   8.0},
    {   8.0,  -2.0}, {   6.0,   4.0}, {   4.0,  -6.0}, {  -2.0,  -8.0},
    {   0.0,   2.0}, {  -6.0,   4.0}, {   4.0,   6.0}, {   2.0,   0.0},
    {   0.0,  -2.0}, {   6.0,  -4.0}, {  -4.0,  -6.0}, {  -2.0,   0.0},
    {   0.0,  -6.0}, {   2.0,  -4.0}, {  -4.0,  -2.0}, {  -6.0,   0.0},
    {   0.0,   6.0}, {  -2.0,   4.0}, {   4.0,   2.0}, {   6.0,   0.0},
    {   8.0,   2.0}, {   2.0,   4.0}, {   4.0,  -2.0}, {   2.0,  -8.0},
    {  -8.0,  -2.0}, {  -2.0,  -4.0}, {  -4.0,   2.0}, {  -2.0,   8.0}
};

static const nf_cpx_t v17_c7200[16] = {
    {   6.0,  -6.0}, {  -2.0,   6.0}, {   6.0,   2.0}, {  -6.0,  -6.0},
    {  -6.0,   6.0}, {   2.0,  -6.0}, {  -6.0,  -2.0}, {   6.0,   6.0},
    {  -2.0,   2.0}, {   6.0,  -2.0}, {  -2.0,  -6.0}, {   2.0,   2.0},
    {   2.0,  -2.0}, {  -6.0,   2.0}, {   2.0,   6.0}, {  -2.0,  -2.0}
};

static const nf_cpx_t v17_abcd[4] = {
    {  -6.0,  -2.0}, {   2.0,  -6.0}, {   6.0,   2.0}, {  -2.0,   6.0}
};

/* ── tables built once at init ─────────────────────────────────────── */

#define TX_SETS 10
#define TX_TAPS 9
#define RX_SETS 192
#define RX_TAPS 27
#define HALF_BAUD_STEP (RX_SETS * 10 / (3 * 2))     /* 320 */

static float tx_shaper[TX_SETS * TX_TAPS];
static float rx_shaper_re[RX_SETS * RX_TAPS];
static float rx_shaper_im[RX_SETS * RX_TAPS];
/* nearest constellation point per 0.5-unit grid cell and TCM subset */
static uint8_t constel_maps[4][36][36][8];
static int tables_ready;

static void make_space_map(int map, const nf_cpx_t *constel, int n)
{
    for (int ire = 0; ire < 36; ire++) {
        double re = (ire - 18) / 2.0 + 0.25;
        for (int iim = 0; iim < 36; iim++) {
            double im = (iim - 18) / 2.0 + 0.25;
            for (int i = 0; i < 8; i++) {
                int best = 0;
                double best_distance = 1e9;
                for (int l = i; l < n; l += 8) {
                    double d = (re - constel[l].re) * (re - constel[l].re)
                             + (im - constel[l].im) * (im - constel[l].im);
                    if (d <= best_distance) {
                        best = l;
                        best_distance = d;
                    }
                }
                constel_maps[map][ire][iim][i] = (uint8_t) best;
            }
        }
    }
}

static void make_tables(void)
{
    if (tables_ready)
        return;
    nf_rrc_design(1.0 / TX_SETS, 0.25, TX_SETS, TX_TAPS, 0.0, tx_shaper, NULL);
    nf_rrc_design(BAUD_RATE / (RX_SETS * 8000.0), 0.5, RX_SETS, RX_TAPS,
                  CARRIER_HZ, rx_shaper_re, rx_shaper_im);
    make_space_map(0, v17_c14400, 128);
    make_space_map(1, v17_c12000, 64);
    make_space_map(2, v17_c9600, 32);
    make_space_map(3, v17_c7200, 16);
    tables_ready = 1;
}

static void rate_params(int bit_rate, int *bits_per_symbol, int *space_map,
                        const nf_cpx_t **constel)
{
    switch (bit_rate) {
    case 14400: *bits_per_symbol = 6; *space_map = 0; *constel = v17_c14400; break;
    case 12000: *bits_per_symbol = 5; *space_map = 1; *constel = v17_c12000; break;
    case 9600:  *bits_per_symbol = 4; *space_map = 2; *constel = v17_c9600;  break;
    default:    *bits_per_symbol = 3; *space_map = 3; *constel = v17_c7200;  break;
    }
}

/* ── tx ────────────────────────────────────────────────────────────── */

static int fake_get_bit(void *user)
{
    (void) user;
    return 1;
}

static int v17_scramble(uint32_t *reg, int in_bit)
{
    int out = (in_bit ^ (*reg >> 17) ^ (*reg >> 22)) & 1;
    *reg = (*reg << 1) | (uint32_t) out;
    return out;
}

static nf_cpx_t training_get(nf_v17_tx_t *s)
{
    static const int cdba_to_abcd[4] = { 2, 3, 1, 0 };
    static const int dibit_to_step[4] = { 1, 0, 2, 3 };
    int bits, shift;

    if (++s->training_step <= SEG_3) {
        if (s->training_step <= SEG_2) {
            /* segment 1: ABAB... (the TEP segments are never sent) */
            return v17_abcd[(s->training_step & 1) ^ 1];
        }
        /* segment 2: scrambled CDBA */
        bits = v17_scramble(&s->scramble_reg, 1);
        bits = (bits << 1) | v17_scramble(&s->scramble_reg, 1);
        s->constellation_state = cdba_to_abcd[bits];
        if (s->short_train && s->training_step == SHORT_SEG_4) {
            /* short train: go straight to the ones test */
            s->training_step = SEG_4;
        }
        return v17_abcd[s->constellation_state];
    }
    /* segment 3: bridge */
    shift = ((s->training_step - SEG_3 - 1) & 0x7) << 1;
    bits = v17_scramble(&s->scramble_reg, V17_BRIDGE_WORD >> shift);
    bits = (bits << 1) | v17_scramble(&s->scramble_reg, V17_BRIDGE_WORD >> (shift + 1));
    s->constellation_state = (s->constellation_state + dibit_to_step[bits]) & 3;
    return v17_abcd[s->constellation_state];
}

static int diff_and_convolutional_encode(nf_v17_tx_t *s, int q)
{
    static const uint8_t v17_differential_encoder[4][4] = {
        {0, 1, 2, 3}, {1, 2, 3, 0}, {2, 3, 0, 1}, {3, 0, 1, 2}
    };
    static const uint8_t v17_convolutional_encoder[8][4] = {
        {0, 2, 3, 1}, {4, 7, 5, 6}, {1, 3, 2, 0}, {7, 4, 6, 5},
        {2, 0, 1, 3}, {6, 5, 7, 4}, {3, 1, 0, 2}, {5, 6, 4, 7}
    };

    s->diff = v17_differential_encoder[s->diff][q & 0x03];
    s->convolution = v17_convolutional_encoder[s->convolution][s->diff];
    return ((q << 1) & 0x78) | (s->diff << 1) | ((s->convolution >> 2) & 1);
}

static nf_cpx_t v17_getbaud(void *user)
{
    nf_v17_tx_t *s = user;
    int bits = 0;

    if (s->in_training) {
        if (s->training_step <= TRAIN_END) {
            if (s->training_step < SEG_4)
                return training_get(s);
            /* the last training step is to send some 1's */
            if (++s->training_step > TRAIN_END) {
                /* training finished - commence normal operation */
                s->current_get_bit = s->get_bit;
                s->current_user = s->get_user;
                s->in_training = 0;
            }
        } else {
            if (++s->training_step > SHUTDOWN_A) {
                /* shutdown is 32 bauds of ones, then 48 bauds of silence */
                if (s->training_step >= SHUTDOWN_END)
                    s->qam.done = 1;
                return nf_cpx(0.0f, 0.0f);
            }
        }
    }
    for (int i = 0; i < s->bits_per_symbol; i++) {
        int bit = s->current_get_bit(s->current_user);
        if (bit < 0) {
            /* end of real data: shut down on scrambled ones */
            s->current_get_bit = fake_get_bit;
            s->current_user = NULL;
            s->in_training = 1;
            bit = 1;
        }
        bits |= v17_scramble(&s->scramble_reg, bit) << i;
    }
    return s->constellation[diff_and_convolutional_encode(s, bits)];
}

int nf_v17_tx_restart(nf_v17_tx_t *s, int bit_rate, int short_train)
{
    int space_map;
    switch (bit_rate) {
    case 14400: case 12000: case 9600: case 7200:
        break;
    default:
        return -1;
    }
    rate_params(bit_rate, &s->bits_per_symbol, &space_map, &s->constellation);
    s->bit_rate = bit_rate;
    /* NB: some modems seem to use 3 instead of 1 for long training */
    s->diff = short_train ? 0 : 1;
    s->convolution = 0;
    s->scramble_reg = 0x2ECDD5;
    s->in_training = 1;
    s->short_train = short_train;
    s->training_step = SEG_1;           /* no TEP */
    s->constellation_state = 0;
    s->current_get_bit = fake_get_bit;
    s->current_user = NULL;
    /* the constellation design keeps the average power constant per rate;
     * the 0.70711 calibrates our unit-DC-gain RRC tables to spandsp's
     * measured tx level (it is exactly 3.02 dB below our uncalibrated
     * output, across all three fast modems) */
    nf_qam_tx_restart(&s->qam,
        0.223f * 0.70711f * powf(10.0f, (TX_LEVEL - 3.14f) / 20.0f) * 32768.0f);
    return 0;
}

void nf_v17_tx_init(nf_v17_tx_t *s, int bit_rate, int (*get_bit)(void *), void *user)
{
    memset(s, 0, sizeof(*s));
    make_tables();
    s->get_bit = get_bit;
    s->get_user = user;
    nf_qam_tx_init(&s->qam, tx_shaper, TX_SETS, TX_TAPS, 3, CARRIER_HZ,
                   v17_getbaud, s);
    nf_v17_tx_restart(s, bit_rate, 0);
}

void nf_v17_tx_set_get_bit(nf_v17_tx_t *s, int (*get_bit)(void *), void *user)
{
    if (s->get_bit == s->current_get_bit) {
        s->current_get_bit = get_bit;
        s->current_user = user;
    }
    s->get_bit = get_bit;
    s->get_user = user;
}

int nf_v17_tx(nf_v17_tx_t *s, int16_t *amp, int max_len)
{
    return nf_qam_tx(&s->qam, amp, max_len);
}

/* ── rx ────────────────────────────────────────────────────────────── */

static int v17_descramble(nf_v17_rx_t *s, int in_bit)
{
    int training = s->training_stage > STAGE_NORMAL
                && s->training_stage < STAGE_TCM_WINDUP;
    in_bit &= 1;
    int out = (in_bit ^ (s->scramble_reg >> 17) ^ (s->scramble_reg >> 22)) & 1;
    s->scramble_reg <<= 1;
    s->scramble_reg |= (uint32_t) (training ? out : in_bit);
    return out;
}

static void v17_put_bit(nf_v17_rx_t *s, int bit)
{
    int out = v17_descramble(s, bit);
    /* strip the trailing parts of the training before passing data up */
    if (s->training_stage == STAGE_NORMAL)
        s->put_bit(s->put_user, out);
}

static float dist_sq(const nf_cpx_t *x, const nf_cpx_t *y)
{
    return (x->re - y->re) * (x->re - y->re) + (x->im - y->im) * (x->im - y->im);
}

static int v17_decode_baud(nf_v17_rx_t *s, const nf_cpx_t *z)
{
    static const uint8_t v17_differential_decoder[4][4] = {
        {0, 1, 2, 3}, {3, 0, 1, 2}, {2, 3, 0, 1}, {1, 2, 3, 0}
    };
    static const uint8_t tcm_paths[8][4] = {
        {0, 6, 2, 4}, {6, 0, 4, 2}, {2, 4, 0, 6}, {4, 2, 6, 0},
        {1, 3, 7, 5}, {5, 7, 3, 1}, {7, 5, 1, 3}, {3, 1, 5, 7}
    };
    float distances[8], new_distances[8], min;
    int i, j, k, re, im, raw, min_index, set, nearest, constellation_state;

    re = (int) ((z->re + 9.0f) * 2.0f);
    im = (int) ((z->im + 9.0f) * 2.0f);
    if (re > 35) re = 35; else if (re < 0) re = 0;
    if (im > 35) im = 35; else if (im < 0) im = 0;

    /* the 8 candidate points nearest the target, one per TCM subset */
    min = 9999999.0f;
    min_index = 0;
    for (i = 0; i < 8; i++) {
        nearest = constel_maps[s->space_map][re][im][i];
        distances[i] = dist_sq(&s->constellation[nearest], z);
        if (min > distances[i]) {
            min = distances[i];
            min_index = i;
        }
    }
    /* Use the nearest of the soft decisions for carrier tracking (the
     * traceback output would put too much lag into the feedback path). */
    constellation_state = constel_maps[s->space_map][re][im][min_index];
    nf_qam_track_carrier(&s->qam, z, &s->constellation[constellation_state]);

    /* trellis: update the minimum accumulated distance to all 8 states */
    if (++s->trellis_ptr >= NF_V17_TRELLIS_DEPTH)
        s->trellis_ptr = 0;
    for (i = 0; i < 8; i++) {
        set = i >> 2;
        min = distances[tcm_paths[i][0]] + s->distances[set];
        min_index = 0;
        for (j = 1; j < 4; j++) {
            k = (j << 1) + set;
            if (min > distances[tcm_paths[i][j]] + s->distances[k]) {
                min = distances[tcm_paths[i][j]] + s->distances[k];
                min_index = j;
            }
        }
        k = (min_index << 1) + set;
        /* an elementary IIR filter tracks the distance to date */
        new_distances[i] = s->distances[k] * 0.9f + distances[tcm_paths[i][min_index]] * 0.1f;
        s->full_path_to_past_state_locations[s->trellis_ptr][i] =
            constel_maps[s->space_map][re][im][tcm_paths[i][min_index]];
        s->past_state_locations[s->trellis_ptr][i] = (uint8_t) k;
    }
    memcpy(s->distances, new_distances, sizeof(s->distances));

    /* trace back from the current minimum-distance state */
    min = s->distances[0];
    min_index = 0;
    for (i = 1; i < 8; i++) {
        if (min > s->distances[i]) {
            min = s->distances[i];
            min_index = i;
        }
    }
    k = min_index;
    for (i = 0, j = s->trellis_ptr; i < NF_V17_TRELLIS_DEPTH - 1; i++) {
        k = s->past_state_locations[j][k];
        if (--j < 0)
            j = NF_V17_TRELLIS_DEPTH - 1;
    }
    nearest = s->full_path_to_past_state_locations[j][k] >> 1;

    /* differentially decode */
    raw = (nearest & 0x3C) | v17_differential_decoder[s->diff][nearest & 0x03];
    s->diff = nearest & 0x03;
    for (i = 0; i < s->bits_per_symbol; i++) {
        v17_put_bit(s, raw);
        raw >>= 1;
    }
    return constellation_state;
}

static void v17_process_baud(void *user, const nf_cpx_t *z)
{
    /* the CDBA training points, in scrambled-dibit order */
    static const nf_cpx_t cdba[4] = {
        { 6.0f,  2.0f}, {-2.0f,  6.0f}, { 2.0f, -6.0f}, {-6.0f, -2.0f}
    };
    nf_v17_rx_t *s = user;
    nf_qam_rx_t *q = &s->qam;
    const nf_cpx_t zero = nf_cpx(0.0f, 0.0f);
    if (v17dbg())
        fprintf(stderr, "v17rx stage=%d cnt=%d z=(%.3f,%.3f) err=%.2f rate=%d\n",
                s->training_stage, s->training_count, z->re, z->im,
                (double) s->training_error, q->carrier_phase_rate);
    const nf_cpx_t *target = &zero;
    int32_t angle, ang;
    uint32_t phase_step;
    int i, j, bit;
    int constellation_state = 0;

    switch (s->training_stage) {
    case STAGE_NORMAL:
        constellation_state = v17_decode_baud(s, z);
        target = &s->constellation[constellation_state];
        break;
    case STAGE_SYMBOL_ACQUISITION:
        /* allow time for the symbol timing to settle */
        if (++s->training_count >= 100) {
            s->training_stage = STAGE_LOG_PHASE;
            memset(s->diff_angles, 0, sizeof(s->diff_angles));
            s->last_angles[0] = nf_angle32(z->im, z->re);
            nf_qam_lock_agc(q);
        }
        break;
    case STAGE_LOG_PHASE:
        angle = nf_angle32(z->im, z->re);
        s->training_count = 1;
        if (s->short_train) {
            if (v17dbg())
                fprintf(stderr, "v17rx LOG short: last=%d angle=%d diff=%u br=%d\n",
                        s->last_angles[0], angle,
                        (uint32_t) (angle - s->last_angles[0]),
                        (uint32_t) (angle - s->last_angles[0]) < (uint32_t) NF_PHASE(180.0));
            /* We know the carrier frequency from the long train; only the
             * phase needs sorting out. Decide if we just saw A or B.
             * atan(1/3) = 18.433 degrees. */
            if ((uint32_t) (angle - s->last_angles[0]) < (uint32_t) NF_PHASE(180.0)) {
                angle = s->last_angles[0];
                s->last_angles[0] = NF_PHASE(270.0 + 18.433);
                s->last_angles[1] = NF_PHASE(180.0 + 18.433);
            } else {
                s->last_angles[0] = NF_PHASE(180.0 + 18.433);
                s->last_angles[1] = NF_PHASE(270.0 + 18.433);
            }
            /* angle is now the difference between where A is and where it
             * should be; spin the constellation into line */
            phase_step = (uint32_t) angle - (uint32_t) NF_PHASE(180.0 + 18.433);
            nf_qam_spin(q, (int32_t) phase_step);
            q->carrier_track_p = 500000.0f;
            s->training_stage = STAGE_SHORT_WAIT_FOR_CDBA;
        } else {
            s->last_angles[1] = angle;
            s->training_stage = STAGE_WAIT_FOR_CDBA;
        }
        break;
    case STAGE_WAIT_FOR_CDBA:
        angle = nf_angle32(z->im, z->re);
        /* the ABAB reversal pattern breaks when the CDBA section starts */
        i = s->training_count + 1;
        ang = angle - s->last_angles[i & 1];
        s->last_angles[i & 1] = angle;
        s->diff_angles[i & 0xF] = s->diff_angles[(i - 2) & 0xF] + (ang >> 4);
        if ((ang > NF_PHASE(90.0) || ang < NF_PHASE(-90.0)) && s->training_count >= 13) {
            /* slam the carrier frequency into line from the drift */
            i = (s->training_count - 8) & ~1;
            if (i > 1) {
                j = i & 0xF;
                ang = (s->diff_angles[j] + s->diff_angles[j | 1]) / (i - 1);
                q->carrier_phase_rate += 3 * 16 * (ang / 20);
            }
            if (q->carrier_phase_rate < nf_dds_phase_rate(CARRIER_HZ - 20.0)
                || q->carrier_phase_rate > nf_dds_phase_rate(CARRIER_HZ + 20.0)) {
                s->training_stage = STAGE_PARKED;
                nf_qam_park(q);
                break;
            }
            /* angle is the difference between where C is and where it
             * should be (18.433 degrees); spin the constellation in */
            phase_step = (uint32_t) angle - (uint32_t) NF_PHASE(18.433);
            nf_qam_spin(q, (int32_t) phase_step);
            /* we just saw the first symbol of the scrambled sequence */
            bit = v17_descramble(s, 1);
            bit = (bit << 1) | v17_descramble(s, 1);
            target = &cdba[bit];
            s->training_count = 1;
            s->training_stage = STAGE_COARSE_TRAIN_ON_CDBA;
            break;
        }
        if (++s->training_count > SEG_1_LEN) {
            s->training_stage = STAGE_PARKED;
            nf_qam_park(q);
        }
        break;
    case STAGE_COARSE_TRAIN_ON_CDBA:
        bit = v17_descramble(s, 1);
        bit = (bit << 1) | v17_descramble(s, 1);
        target = &cdba[bit];
        nf_qam_track_carrier(q, z, target);
        nf_qam_tune_eq(q, z, target);
        if (++s->training_count == SEG_2_LEN - 2000) {
            /* slow the equalizer down or it will never tune well on noise */
            q->eq_delta = 0.1f * 0.21f / q->eq_len;
            q->carrier_track_i = 1000.0f;
            s->training_stage = STAGE_FINE_TRAIN_ON_CDBA;
        }
        break;
    case STAGE_FINE_TRAIN_ON_CDBA:
        bit = v17_descramble(s, 1);
        bit = (bit << 1) | v17_descramble(s, 1);
        target = &cdba[bit];
        nf_qam_track_carrier(q, z, target);
        nf_qam_tune_eq(q, z, target);
        if (++s->training_count >= SEG_2_LEN - 48) {
            s->training_error = 0.0f;
            q->carrier_track_i = 100.0f;
            q->carrier_track_p = 500000.0f;
            s->training_stage = STAGE_TRAIN_ON_CDBA_AND_TEST;
        }
        break;
    case STAGE_TRAIN_ON_CDBA_AND_TEST:
        bit = v17_descramble(s, 1);
        bit = (bit << 1) | v17_descramble(s, 1);
        target = &cdba[bit];
        /* ignore the last few symbols; some modems end this part badly */
        if (++s->training_count < SEG_2_LEN - 20) {
            nf_qam_track_carrier(q, z, target);
            nf_qam_tune_eq(q, z, target);
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        } else if (s->training_count >= SEG_2_LEN) {
            if (s->training_error < 20.0f * 1.414f * constellation_spacing[s->space_map]) {
                s->training_error = 0.0f;
                s->training_count = 0;
                s->training_stage = STAGE_BRIDGE;
            } else {
                s->training_stage = STAGE_PARKED;
                nf_qam_park(q);
            }
        }
        break;
    case STAGE_BRIDGE:
        v17_descramble(s, V17_BRIDGE_WORD >> ((s->training_count & 0x7) << 1));
        v17_descramble(s, V17_BRIDGE_WORD >> (((s->training_count & 0x7) << 1) + 1));
        target = z;
        if (++s->training_count >= SEG_3_LEN) {
            s->training_error = 0.0f;
            s->training_count = 0;
            s->training_stage = STAGE_TCM_WINDUP;
        }
        break;
    case STAGE_SHORT_WAIT_FOR_CDBA:
        angle = nf_angle32(z->im, z->re);
        ang = angle - s->last_angles[s->training_count & 1];
        if (v17dbg())
            fprintf(stderr, "  shortwait cnt=%d angle=%.1f last[%d]=%.1f ang=%.1f\n",
                    s->training_count, angle * 360.0 / 4294967296.0,
                    s->training_count & 1,
                    s->last_angles[s->training_count & 1] * 360.0 / 4294967296.0,
                    ang * 360.0 / 4294967296.0);
        if (ang > NF_PHASE(90.0) || ang < NF_PHASE(-90.0)) {
            /* a phase reversal: the first symbol of the scrambled sequence */
            bit = v17_descramble(s, 1);
            bit = (bit << 1) | v17_descramble(s, 1);
            target = &cdba[bit];
            s->training_error = 0.0f;
            s->training_count = 1;
            s->training_stage = STAGE_SHORT_TRAIN_ON_CDBA_AND_TEST;
            break;
        }
        target = &cdba[(s->training_count & 1) + 2];
        nf_qam_track_carrier(q, z, target);
        if (++s->training_count > SEG_1_LEN) {
            /* parked, but keep the saved equalizer for the next attempt */
            s->training_stage = STAGE_PARKED;
            q->parked = 1;
            nf_qam_report(q, NF_SIG_TRAINING_FAILED);
        }
        break;
    case STAGE_SHORT_TRAIN_ON_CDBA_AND_TEST:
        /* short retrain on the scrambled CDBA, measuring quality, with NO
         * equalizer adaptation (the saved equalizer is the asset here) */
        bit = v17_descramble(s, 1);
        bit = (bit << 1) | v17_descramble(s, 1);
        target = &cdba[bit];
        nf_qam_track_carrier(q, z, target);
        if (s->training_count > 8) {
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        }
        if (++s->training_count >= SHORT_SEG_2_LEN) {
            q->carrier_track_i = 100.0f;
            q->carrier_track_p = 500000.0f;
            if (s->training_error
                < (SHORT_SEG_2_LEN - 8) * 4.0f * constellation_spacing[s->space_map]) {
                s->training_count = 0;
                s->training_stage = STAGE_TCM_WINDUP;
            } else {
                s->training_stage = STAGE_PARKED;
                q->parked = 1;
                nf_qam_report(q, NF_SIG_TRAINING_FAILED);
            }
        }
        break;
    case STAGE_TCM_WINDUP:
        /* wait for the trellis to fill up */
        constellation_state = v17_decode_baud(s, z);
        target = &s->constellation[constellation_state];
        {
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        }
        if (++s->training_count >= SEG_4A_LEN) {
            s->training_error = 0.0f;
            s->training_count = 0;
            s->diff = s->short_train ? 0 : 1;   /* restart the diff decoder */
            s->training_stage = STAGE_TEST_ONES;
        }
        break;
    case STAGE_TEST_ONES:
        constellation_state = v17_decode_baud(s, z);
        target = &s->constellation[constellation_state];
        {
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        }
        if (++s->training_count >= SEG_4_LEN) {
            if (s->training_error
                < SEG_4_LEN * 1.0f * constellation_spacing[s->space_map]) {
                nf_qam_report(q, NF_SIG_TRAINING_SUCCEEDED);
                q->signal_present = 60;
                nf_qam_save_state(q);
                /* once trained, restarts default to short training */
                s->short_train = 1;
                s->training_stage = STAGE_NORMAL;
            } else {
                if (!s->short_train)
                    q->agc_scaling_save = 0.0f;
                s->training_stage = STAGE_PARKED;
                q->parked = 1;
                nf_qam_report(q, NF_SIG_TRAINING_FAILED);
            }
        }
        break;
    case STAGE_PARKED:
    default:
        break;
    }
    if (q->diag)
        q->diag(q->diag_user, z, target, constellation_state);
}

static void v17_carrier_drop(void *user)
{
    nf_v17_rx_t *s = user;
    nf_v17_rx_restart(s, s->bit_rate, s->short_train);
}

int nf_v17_rx_restart(nf_v17_rx_t *s, int bit_rate, int short_train)
{
    switch (bit_rate) {
    case 14400: case 12000: case 9600: case 7200:
        break;
    default:
        return -1;
    }
    rate_params(bit_rate, &s->bits_per_symbol, &s->space_map, &s->constellation);
    s->bit_rate = bit_rate;
    s->diff = 1;
    s->scramble_reg = 0x2ECDD5;
    s->training_stage = STAGE_SYMBOL_ACQUISITION;
    s->training_count = 0;
    s->training_error = 0.0f;
    s->short_train = short_train;
    memset(s->last_angles, 0, sizeof(s->last_angles));
    memset(s->diff_angles, 0, sizeof(s->diff_angles));
    /* The accumulated distance vectors start at zero only for state zero,
     * forcing the initial paths to merge there. */
    for (int i = 0; i < 8; i++)
        s->distances[i] = 99.0f;
    s->distances[0] = 0.0f;
    memset(s->full_path_to_past_state_locations, 0,
           sizeof(s->full_path_to_past_state_locations));
    memset(s->past_state_locations, 0, sizeof(s->past_state_locations));
    s->trellis_ptr = 14;

    nf_qam_rx_restart(&s->qam, short_train, 2.17f / 735.0f);
    if (short_train && s->qam.agc_scaling_save != 0.0f) {
        /* no frequency correction at all until the phase is pulled in */
        s->qam.carrier_track_i = 0.0f;
        s->qam.carrier_track_p = 40000.0f;
        s->qam.eq_delta = 0.1f * 0.21f / s->qam.eq_len;
    } else {
        s->qam.carrier_track_i = 5000.0f;
        s->qam.carrier_track_p = 40000.0f;
        s->short_train = 0;     /* no saved state to restore: long train */
    }
    return 0;
}

void nf_v17_rx_init(nf_v17_rx_t *s, int bit_rate, void (*put_bit)(void *, int), void *user)
{
    memset(s, 0, sizeof(*s));
    make_tables();
    s->put_bit = put_bit;
    s->put_user = user;
    nf_qam_rx_init(&s->qam, rx_shaper_re, rx_shaper_im, RX_SETS, RX_TAPS,
                   HALF_BAUD_STEP, 33, 16, 0.21f, 2.17f, CARRIER_HZ,
                   v17_process_baud, v17_carrier_drop, s);
    nf_qam_rx_set_godard(&s->qam, CARRIER_HZ, BAUD_RATE, 0.99, 1000.0f, 100.0f, 15, 1);
    nf_qam_rx_set_cutoff(&s->qam, -45.5f, 0.4f);
    nf_v17_rx_restart(s, bit_rate, 0);
}

void nf_v17_rx_set_put_bit(nf_v17_rx_t *s, void (*put_bit)(void *, int), void *user)
{
    s->put_bit = put_bit;
    s->put_user = user;
}

void nf_v17_rx_set_status_handler(nf_v17_rx_t *s, void (*status)(void *, int), void *user)
{
    s->qam.status = status;
    s->qam.status_user = user;
}

int nf_v17_rx(nf_v17_rx_t *s, const int16_t *amp, int len)
{
    return nf_qam_rx(&s->qam, amp, len);
}
