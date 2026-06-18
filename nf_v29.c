#include "nf_v29.h"
#include <math.h>
#include <string.h>

#define CARRIER_HZ          1700.0
#define BAUD_RATE           2400.0
#define TX_LEVEL            (-14.0f)

/* training segment lengths (symbols); the optional TEP segment is not sent */
#define SEG_1               48          /* silence */
#define SEG_2               128         /* ABAB */
#define SEG_3               384         /* scrambled CDCD */
#define SEG_4               48          /* test ones */
#define SHUTDOWN            32          /* scrambled ones padding at the end */
/* cumulative tx step counts (no TEP) */
#define STEP_SEG_2          SEG_1
#define STEP_SEG_3          (STEP_SEG_2 + SEG_2)
#define STEP_SEG_4          (STEP_SEG_3 + SEG_3)
#define STEP_END            (STEP_SEG_4 + SEG_4)
#define STEP_SHUTDOWN_END   (STEP_END + SHUTDOWN)

/* rx training stages */
enum {
    STAGE_NORMAL = 0,
    STAGE_SYMBOL_ACQUISITION,
    STAGE_LOG_PHASE,
    STAGE_WAIT_FOR_CDCD,
    STAGE_TRAIN_ON_CDCD,
    STAGE_TRAIN_ON_CDCD_AND_TEST,
    STAGE_TEST_ONES,
    STAGE_PARKED
};

/* The V.29 constellation: 8 phases x 2 amplitudes, indexed amp<<3 | phase
 * (phase in 45 degree steps). 4800 bps uses the even low-amplitude points,
 * 7200 the low half, 9600 all 16. */
static const nf_cpx_t v29_constel[16] = {
    { 3.0f,  0.0f}, { 1.0f,  1.0f}, { 0.0f,  3.0f}, {-1.0f,  1.0f},
    {-3.0f,  0.0f}, {-1.0f, -1.0f}, { 0.0f, -3.0f}, { 1.0f, -1.0f},
    { 5.0f,  0.0f}, { 3.0f,  3.0f}, { 0.0f,  5.0f}, {-3.0f,  3.0f},
    {-5.0f,  0.0f}, {-3.0f, -3.0f}, { 0.0f, -5.0f}, { 3.0f, -3.0f}
};

/* segment 2 A/B and segment 3 C/D points per rate (offset 0/2/4) */
static const nf_cpx_t v29_abab[6] = {
    { 3.0f, -3.0f}, {-3.0f,  0.0f},     /* 9600 */
    { 1.0f, -1.0f}, {-3.0f,  0.0f},     /* 7200 */
    { 0.0f, -3.0f}, {-3.0f,  0.0f}      /* 4800 */
};
static const nf_cpx_t v29_cdcd[6] = {
    { 3.0f,  0.0f}, {-3.0f,  3.0f},     /* 9600: C, D */
    { 3.0f,  0.0f}, {-1.0f,  1.0f},     /* 7200 */
    { 3.0f,  0.0f}, { 0.0f,  3.0f}      /* 4800 */
};
/* constellation indices of the C/D points, per rate */
static const int cdcd_pos[6] = { 0, 11, 0, 3, 0, 2 };

/* nearest-constellation-point map over a 20x20 grid of (re,im) in 0.5 steps */
static const uint8_t space_map_9600[20][20] = {
    {13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11},
    {13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 11},
    {13, 13, 13, 13, 13, 13, 13,  4,  4,  4,  4,  4,  4, 11, 11, 11, 11, 11, 11, 11},
    {13, 13, 13, 13, 13, 13, 13,  4,  4,  4,  4,  4,  4, 11, 11, 11, 11, 11, 11, 11},
    {13, 13, 13, 13, 13, 13, 13,  4,  4,  4,  4,  4,  4, 11, 11, 11, 11, 11, 11, 11},
    {13, 13, 13, 13, 13, 13, 13,  5,  4,  4,  4,  4,  3, 11, 11, 11, 11, 11, 11, 11},
    {14, 13, 13, 13, 13, 13,  5,  5,  5,  5,  3,  3,  3,  3, 11, 11, 11, 11, 11, 10},
    {14, 14,  6,  6,  6,  5,  5,  5,  5,  5,  3,  3,  3,  3,  3,  2,  2,  2, 10, 10},
    {14, 14,  6,  6,  6,  6,  5,  5,  5,  5,  3,  3,  3,  3,  2,  2,  2,  2, 10, 10},
    {14, 14,  6,  6,  6,  6,  5,  5,  5,  5,  3,  3,  3,  3,  2,  2,  2,  2, 10, 10},
    {14, 14,  6,  6,  6,  6,  7,  7,  7,  7,  1,  1,  1,  1,  2,  2,  2,  2, 10, 10},
    {14, 14,  6,  6,  6,  6,  7,  7,  7,  7,  1,  1,  1,  1,  2,  2,  2,  2, 10, 10},
    {14, 14,  6,  6,  6,  7,  7,  7,  7,  7,  1,  1,  1,  1,  1,  2,  2,  2, 10, 10},
    {14, 15, 15, 15, 15, 15,  7,  7,  7,  7,  1,  1,  1,  1,  9,  9,  9,  9,  9, 10},
    {15, 15, 15, 15, 15, 15, 15,  7,  0,  0,  0,  0,  1,  9,  9,  9,  9,  9,  9,  9},
    {15, 15, 15, 15, 15, 15, 15,  0,  0,  0,  0,  0,  0,  9,  9,  9,  9,  9,  9,  9},
    {15, 15, 15, 15, 15, 15, 15,  0,  0,  0,  0,  0,  0,  9,  9,  9,  9,  9,  9,  9},
    {15, 15, 15, 15, 15, 15, 15,  0,  0,  0,  0,  0,  0,  9,  9,  9,  9,  9,  9,  9},
    {15, 15, 15, 15, 15, 15, 15,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9},
    {15, 15, 15, 15, 15, 15,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9}
};

/* ── shared pulse-shaper tables (built once) ───────────────────────── */

#define TX_SETS 10
#define TX_TAPS 9
#define RX_SETS 48
#define RX_TAPS 27
#define HALF_BAUD_STEP (RX_SETS * 10 / (3 * 2))     /* T/2 in 1/RX_SETS units */

static float tx_shaper[TX_SETS * TX_TAPS];
static float rx_shaper_re[RX_SETS * RX_TAPS];
static float rx_shaper_im[RX_SETS * RX_TAPS];
static int tables_ready;

static void make_tables(void)
{
    if (tables_ready)
        return;
    nf_rrc_design(1.0 / TX_SETS, 0.25, TX_SETS, TX_TAPS, 0.0, tx_shaper, NULL);
    nf_rrc_design(BAUD_RATE / (RX_SETS * 8000.0), 0.5, RX_SETS, RX_TAPS,
                  CARRIER_HZ, rx_shaper_re, rx_shaper_im);
    tables_ready = 1;
}

/* ── tx ────────────────────────────────────────────────────────────── */

static int fake_get_bit(void *user)
{
    (void) user;
    return 1;
}

static int get_scrambled_bit(nf_v29_tx_t *s)
{
    int bit = s->current_get_bit(s->current_user);
    if (bit < 0) {                  /* end of real data: shut down on ones */
        s->current_get_bit = fake_get_bit;
        s->current_user = NULL;
        s->in_training = 1;
        bit = 1;
    }
    int out = (bit ^ (s->scramble_reg >> 17) ^ (s->scramble_reg >> 22)) & 1;
    s->scramble_reg = (s->scramble_reg << 1) | (uint32_t) out;
    return out;
}

static nf_cpx_t v29_getbaud(void *user)
{
    static const int phase_steps_9600[8] = { 1, 0, 2, 3, 6, 7, 5, 4 };
    static const int phase_steps_4800[4] = { 0, 2, 6, 4 };
    nf_v29_tx_t *s = user;
    int bits, amp, bit;

    if (s->in_training) {
        if (++s->training_step <= STEP_SEG_4) {
            if (s->training_step <= STEP_SEG_3) {
                if (s->training_step <= STEP_SEG_2)
                    return nf_cpx(0.0f, 0.0f);          /* segment 1: silence */
                /* segment 2: ABAB... */
                return v29_abab[(s->training_step & 1) + s->training_offset];
            }
            /* segment 3: CDCD..., 1 + x^-6 + x^-7 training scrambler */
            bit = s->training_scramble_reg & 1;
            s->training_scramble_reg >>= 1;
            s->training_scramble_reg |= (uint8_t) (((bit ^ s->training_scramble_reg) & 1) << 6);
            return v29_cdcd[bit + s->training_offset];
        }
        /* segment 4 (test ones) and the shutdown padding both run here */
        if (s->training_step == STEP_END + 1) {
            s->current_get_bit = s->get_bit;
            s->current_user = s->get_user;
            s->in_training = 0;
        }
        if (s->training_step >= STEP_SHUTDOWN_END)
            s->qam.done = 1;
    }
    amp = 0;
    if (s->bit_rate == 9600 && get_scrambled_bit(s))
        amp = 8;
    bits = get_scrambled_bit(s);
    bits = (bits << 1) | get_scrambled_bit(s);
    if (s->bit_rate == 4800) {
        bits = phase_steps_4800[bits];
    } else {
        bits = (bits << 1) | get_scrambled_bit(s);
        bits = phase_steps_9600[bits];
    }
    s->constellation_state = (s->constellation_state + bits) & 7;
    return v29_constel[amp | s->constellation_state];
}

static float v29_tx_gain(int bit_rate)
{
    /* The constellation power varies with rate; scale to keep -14 dBm0.
     * The 0.70711 calibrates our unit-DC-gain RRC tables to spandsp's
     * measured tx level. */
    float base = 0.70711f * powf(10.0f, (TX_LEVEL - 3.14f) / 20.0f) * 32768.0f;
    switch (bit_rate) {
    case 9600: return 0.387f * base;
    case 7200: return 0.605f * base;
    default:   return 0.470f * base;    /* 4800 */
    }
}

int nf_v29_tx_restart(nf_v29_tx_t *s, int bit_rate)
{
    switch (bit_rate) {
    case 9600: s->training_offset = 0; break;
    case 7200: s->training_offset = 2; break;
    case 4800: s->training_offset = 4; break;
    default: return -1;
    }
    s->bit_rate = bit_rate;
    s->scramble_reg = 0;
    s->training_scramble_reg = 0x2A;
    s->in_training = 1;
    s->training_step = 0;
    s->constellation_state = 0;
    s->current_get_bit = fake_get_bit;
    s->current_user = NULL;
    nf_qam_tx_restart(&s->qam, v29_tx_gain(bit_rate));
    return 0;
}

void nf_v29_tx_init(nf_v29_tx_t *s, int bit_rate, int (*get_bit)(void *), void *user)
{
    memset(s, 0, sizeof(*s));
    make_tables();
    s->get_bit = get_bit;
    s->get_user = user;
    nf_qam_tx_init(&s->qam, tx_shaper, TX_SETS, TX_TAPS, 3, CARRIER_HZ,
                   v29_getbaud, s);
    nf_v29_tx_restart(s, bit_rate);
}

void nf_v29_tx_set_get_bit(nf_v29_tx_t *s, int (*get_bit)(void *), void *user)
{
    if (s->get_bit == s->current_get_bit)
        s->current_get_bit = get_bit, s->current_user = user;
    s->get_bit = get_bit;
    s->get_user = user;
}

int nf_v29_tx(nf_v29_tx_t *s, int16_t *amp, int max_len)
{
    return nf_qam_tx(&s->qam, amp, max_len);
}

/* ── rx ────────────────────────────────────────────────────────────── */

static int scrambled_training_bit(nf_v29_rx_t *s)
{
    int bit = s->training_scramble_reg & 1;
    s->training_scramble_reg >>= 1;
    if (bit ^ (s->training_scramble_reg & 1))
        s->training_scramble_reg |= 0x40;
    return bit;
}

static void v29_put_bit(nf_v29_rx_t *s, int bit)
{
    /* descramble */
    bit &= 1;
    int out = (bit ^ (s->scramble_reg >> 17) ^ (s->scramble_reg >> 22)) & 1;
    s->scramble_reg = (s->scramble_reg << 1) | (uint32_t) bit;
    /* strip the final test-ones stage of training */
    if (s->training_stage == STAGE_NORMAL)
        s->put_bit(s->put_user, out);
}

static int find_quadrant(const nf_cpx_t *z)
{
    int b1 = z->im > z->re;
    int b2 = z->im < -z->re;
    return (b2 << 1) | (b1 ^ b2);
}

static int v29_decode_baud(nf_v29_rx_t *s, const nf_cpx_t *z)
{
    static const uint8_t phase_steps_9600[8] = { 4, 0, 2, 6, 7, 3, 1, 5 };
    static const uint8_t phase_steps_4800[4] = { 0, 2, 3, 1 };
    int nearest, raw_bits;

    if (s->bit_rate == 4800) {
        nearest = find_quadrant(z) << 1;
        raw_bits = phase_steps_4800[((nearest - s->constellation_state) >> 1) & 3];
        v29_put_bit(s, raw_bits);
        v29_put_bit(s, raw_bits >> 1);
    } else {
        int re = (int) ((z->re + 5.0f) * 2.0f);
        int im = (int) ((z->im + 5.0f) * 2.0f);
        if (re > 19) re = 19; else if (re < 0) re = 0;
        if (im > 19) im = 19; else if (im < 0) im = 0;
        nearest = space_map_9600[re][im];
        if (s->bit_rate == 9600)
            v29_put_bit(s, nearest >> 3);       /* amplitude bit */
        else
            nearest &= 7;
        raw_bits = phase_steps_9600[(nearest - s->constellation_state) & 7];
        for (int i = 0; i < 3; i++) {
            v29_put_bit(s, raw_bits);
            raw_bits >>= 1;
        }
    }
    nf_qam_track_carrier(&s->qam, z, &v29_constel[nearest]);
    if (--s->eq_skip <= 0) {
        /* in data mode, only touch the equalizer occasionally */
        s->eq_skip = 10;
        nf_qam_tune_eq(&s->qam, z, &v29_constel[nearest]);
    }
    s->constellation_state = nearest;
    return nearest;
}

static void v29_process_baud(void *user, const nf_cpx_t *z)
{
    nf_v29_rx_t *s = user;
    nf_qam_rx_t *q = &s->qam;
    const nf_cpx_t zero = nf_cpx(0.0f, 0.0f);
    const nf_cpx_t *target = &zero;
    int32_t angle, ang;
    int i, j, bit;

    switch (s->training_stage) {
    case STAGE_NORMAL:
        v29_decode_baud(s, z);
        target = &v29_constel[s->constellation_state];
        break;
    case STAGE_SYMBOL_ACQUISITION:
        /* allow time for the symbol timing to settle on the ALT segment */
        if (++s->training_count >= 60) {
            s->training_stage = STAGE_LOG_PHASE;
            memset(s->diff_angles, 0, sizeof(s->diff_angles));
            s->last_angles[0] = nf_angle32(z->im, z->re);
            nf_qam_lock_agc(q);
        }
        break;
    case STAGE_LOG_PHASE:
        s->last_angles[1] = nf_angle32(z->im, z->re);
        s->training_count = 1;
        s->training_stage = STAGE_WAIT_FOR_CDCD;
        break;
    case STAGE_WAIT_FOR_CDCD:
        angle = nf_angle32(z->im, z->re);
        /* look for the phase reversal that starts the scrambled CDCD part */
        i = s->training_count + 1;
        ang = angle - s->last_angles[i & 1];
        s->last_angles[i & 1] = angle;
        s->diff_angles[i & 0xF] = s->diff_angles[(i - 2) & 0xF] + (ang >> 4);
        if ((ang > NF_PHASE(45.0) || ang < NF_PHASE(-45.0)) && s->training_count >= 13) {
            /* slam the carrier frequency into line from the drift, stepping
             * back a few symbols to avoid the ISI around the reversal */
            i = (s->training_count - 8) & ~1;
            if (i > 1) {
                j = i & 0xF;
                ang = (s->diff_angles[j] + s->diff_angles[j | 1]) / (i - 1);
                q->carrier_phase_rate += 3 * 16 * (ang / 20);
            }
            /* plausibility: within +-20 Hz of nominal */
            if (q->carrier_phase_rate < nf_dds_phase_rate(CARRIER_HZ - 20.0)
                || q->carrier_phase_rate > nf_dds_phase_rate(CARRIER_HZ + 20.0)) {
                s->training_stage = STAGE_PARKED;
                nf_qam_park(q);
                break;
            }
            /* hard phase spin to pull the constellation into line */
            nf_qam_spin(q, angle);
            /* we have just seen the first bit of the scrambled sequence */
            bit = scrambled_training_bit(s);
            s->constellation_state = cdcd_pos[s->training_cd + bit];
            target = &v29_constel[s->constellation_state];
            s->training_count = 1;
            s->training_stage = STAGE_TRAIN_ON_CDCD;
            break;
        }
        if (++s->training_count > SEG_2) {
            /* no ALT segment of this length exists in a real signal */
            s->training_stage = STAGE_PARKED;
            nf_qam_park(q);
        }
        break;
    case STAGE_TRAIN_ON_CDCD:
        bit = scrambled_training_bit(s);
        s->constellation_state = cdcd_pos[s->training_cd + bit];
        target = &v29_constel[s->constellation_state];
        nf_qam_track_carrier(q, z, target);
        nf_qam_tune_eq(q, z, target);
        if (++s->training_count >= SEG_3 - 48) {
            s->training_stage = STAGE_TRAIN_ON_CDCD_AND_TEST;
            s->training_error = 0.0f;
            q->carrier_track_i = 200.0f;
            q->carrier_track_p = 1000000.0f;
        }
        break;
    case STAGE_TRAIN_ON_CDCD_AND_TEST:
        bit = scrambled_training_bit(s);
        s->constellation_state = cdcd_pos[s->training_cd + bit];
        target = &v29_constel[s->constellation_state];
        nf_qam_track_carrier(q, z, target);
        nf_qam_tune_eq(q, z, target);
        {
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        }
        if (++s->training_count >= SEG_3) {
            if (s->training_error < 48.0f * 2.0f) {
                s->training_error = 0.0f;
                s->training_count = 0;
                s->constellation_state = 0;
                s->training_stage = STAGE_TEST_ONES;
            } else {
                s->training_stage = STAGE_PARKED;
                nf_qam_park(q);
            }
        }
        break;
    case STAGE_TEST_ONES:
        v29_decode_baud(s, z);
        target = &v29_constel[s->constellation_state];
        {
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        }
        if (++s->training_count >= SEG_4) {
            if (s->training_error < 48.0f * 1.0f) {
                nf_qam_report(q, NF_SIG_TRAINING_SUCCEEDED);
                /* lag the carrier-off so the last bits get pushed through */
                q->signal_present = 60;
                s->training_stage = STAGE_NORMAL;
                nf_qam_save_state(q);
            } else {
                s->training_stage = STAGE_PARKED;
                nf_qam_park(q);
            }
        }
        break;
    case STAGE_PARKED:
    default:
        break;
    }
    if (q->diag)
        q->diag(q->diag_user, z, target, s->constellation_state);
}

static void v29_carrier_drop(void *user)
{
    nf_v29_rx_t *s = user;
    nf_v29_rx_restart(s, s->bit_rate);
}

int nf_v29_rx_restart(nf_v29_rx_t *s, int bit_rate)
{
    switch (bit_rate) {
    case 9600: s->training_cd = 0; break;
    case 7200: s->training_cd = 2; break;
    case 4800: s->training_cd = 4; break;
    default: return -1;
    }
    s->bit_rate = bit_rate;
    s->scramble_reg = 0;
    s->training_scramble_reg = 0x2A;
    s->training_stage = STAGE_SYMBOL_ACQUISITION;
    s->training_count = 0;
    s->constellation_state = 0;
    s->training_error = 0.0f;
    s->eq_skip = 0;
    memset(s->diff_angles, 0, sizeof(s->diff_angles));
    nf_qam_rx_restart(&s->qam, 0, 1.25f / 735.0f);
    s->qam.carrier_track_i = 8000.0f;
    s->qam.carrier_track_p = 8000000.0f;
    return 0;
}

void nf_v29_rx_init(nf_v29_rx_t *s, int bit_rate, void (*put_bit)(void *, int), void *user)
{
    memset(s, 0, sizeof(*s));
    make_tables();
    s->put_bit = put_bit;
    s->put_user = user;
    nf_qam_rx_init(&s->qam, rx_shaper_re, rx_shaper_im, RX_SETS, RX_TAPS,
                   HALF_BAUD_STEP, 33, 16, 0.21f, 1.25f, CARRIER_HZ,
                   v29_process_baud, v29_carrier_drop, s);
    nf_qam_rx_set_godard(&s->qam, CARRIER_HZ, BAUD_RATE, 0.99, 1000.0f, 30.0f, 5, 1);
    /* spandsp's fax layer drops the V.29 cutoff from the spec's -26/-31 to
     * V.17's -43/-48 (fax_modems.c); match it for impairment parity */
    nf_qam_rx_set_cutoff(&s->qam, -45.5f, 0.4f);
    nf_v29_rx_restart(s, bit_rate);
}

void nf_v29_rx_set_put_bit(nf_v29_rx_t *s, void (*put_bit)(void *, int), void *user)
{
    s->put_bit = put_bit;
    s->put_user = user;
}

void nf_v29_rx_set_status_handler(nf_v29_rx_t *s, void (*status)(void *, int), void *user)
{
    s->qam.status = status;
    s->qam.status_user = user;
}

int nf_v29_rx(nf_v29_rx_t *s, const int16_t *amp, int len)
{
    return nf_qam_rx(&s->qam, amp, len);
}
