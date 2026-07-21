#include "nf_v27.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int v27dbg(void) { static int d = -1; if (d < 0) d = getenv("NFV27DBG") ? 1 : 0; return d; }

#define CARRIER_HZ          1800.0
#define TX_LEVEL            (-14.0f)

/* training segments (symbols). FAX never sends the optional TEP segment 1
 * (320 symbols of unmodulated carrier), so tx starts at SEG_2. */
#define SEG_2               320
#define SEG_3               (SEG_2 + 32)        /* 32 silence */
#define SEG_4               (SEG_3 + 50)        /* 50 reversals */
#define SEG_5               (SEG_4 + 1074)      /* 1074 scrambled reversals */
#define TRAIN_END           (SEG_5 + 8)         /* 8 ones */
#define SHUTDOWN_END        (TRAIN_END + 32)

#define SEG_3_LEN           50
#define SEG_5_LEN           1074
#define SEG_6_LEN           8

enum {
    STAGE_NORMAL = 0,
    STAGE_SYMBOL_ACQUISITION,
    STAGE_LOG_PHASE,
    STAGE_WAIT_FOR_HOP,
    STAGE_TRAIN_ON_ABAB,
    STAGE_TEST_ONES,
    STAGE_PARKED
};

/* 8-PSK constellation, 45 degree steps, constant power */
static const nf_cpx_t v27_constel[8] = {
    { 1.414f,  0.0f},   { 1.0f,  1.0f}, { 0.0f,  1.414f}, {-1.0f,  1.0f},
    {-1.414f,  0.0f},   {-1.0f, -1.0f}, { 0.0f, -1.414f}, { 1.0f, -1.0f}
};

/* ── pulse shapers (two symbol rates -> two table sets) ────────────── */

#define TX_TAPS 9
#define RX_TAPS 27
#define TX_SETS_4800 5
#define TX_SETS_2400 20
#define RX_SETS_4800 8
#define RX_SETS_2400 12
#define HALF_STEP_4800 (RX_SETS_4800 * 5 / 2)          /* 20 */
#define HALF_STEP_2400 (RX_SETS_2400 * 20 / (3 * 2))   /* 40 */

static float tx_shaper_4800[TX_SETS_4800 * TX_TAPS];
static float tx_shaper_2400[TX_SETS_2400 * TX_TAPS];
static float rx_re_4800[RX_SETS_4800 * RX_TAPS], rx_im_4800[RX_SETS_4800 * RX_TAPS];
static float rx_re_2400[RX_SETS_2400 * RX_TAPS], rx_im_2400[RX_SETS_2400 * RX_TAPS];
static int tables_ready;

static void make_tables(void)
{
    if (tables_ready)
        return;
    nf_rrc_design(1.0 / TX_SETS_4800, 0.5, TX_SETS_4800, TX_TAPS, 0.0, tx_shaper_4800, NULL);
    nf_rrc_design(1.0 / TX_SETS_2400, 0.5, TX_SETS_2400, TX_TAPS, 0.0, tx_shaper_2400, NULL);
    nf_rrc_design(1600.0 / (RX_SETS_4800 * 8000.0), 0.5, RX_SETS_4800, RX_TAPS,
                  CARRIER_HZ, rx_re_4800, rx_im_4800);
    nf_rrc_design(1200.0 / (RX_SETS_2400 * 8000.0), 0.5, RX_SETS_2400, RX_TAPS,
                  CARRIER_HZ, rx_re_2400, rx_im_2400);
    tables_ready = 1;
}

/* ── tx ────────────────────────────────────────────────────────────── */

static int v27_scramble(nf_v27_tx_t *s, int in_bit)
{
    int out = (in_bit ^ (s->scramble_reg >> 5) ^ (s->scramble_reg >> 6)) & 1;
    if (s->scrambler_pattern_count >= 33) {
        out ^= 1;
        s->scrambler_pattern_count = 0;
    } else {
        if ((((s->scramble_reg >> 7) ^ (uint32_t) out)
           & ((s->scramble_reg >> 8) ^ (uint32_t) out)
           & ((s->scramble_reg >> 11) ^ (uint32_t) out) & 1))
            s->scrambler_pattern_count = 0;
        else
            s->scrambler_pattern_count++;
    }
    s->scramble_reg = (s->scramble_reg << 1) | (uint32_t) out;
    return out;
}

static int get_scrambled_bit(nf_v27_tx_t *s)
{
    int bit = s->current_get_bit(s->current_user);
    if (bit < 0) {
        s->current_get_bit = nf_fake_get_bit;
        s->current_user = NULL;
        s->in_training = 1;
        bit = 1;
    }
    return v27_scramble(s, bit);
}

static nf_cpx_t v27_getbaud(void *user)
{
    static const int phase_steps_4800[8] = { 1, 0, 2, 3, 6, 7, 5, 4 };
    static const int phase_steps_2400[4] = { 0, 2, 6, 4 };
    nf_v27_tx_t *s = user;
    int bits;

    if (s->in_training) {
        if (++s->training_step <= SEG_5) {
            if (s->training_step <= SEG_4) {
                if (s->training_step <= SEG_3)
                    return nf_cpx(0.0f, 0.0f);          /* segment 2: silence */
                /* segment 3: regular reversals */
                s->constellation_state = (s->constellation_state + 4) & 7;
                return v27_constel[s->constellation_state];
            }
            /* segment 4: scrambled reversals - every third scrambled bit */
            bits = get_scrambled_bit(s) << 2;
            get_scrambled_bit(s);
            get_scrambled_bit(s);
            s->constellation_state = (s->constellation_state + bits) & 7;
            return v27_constel[s->constellation_state];
        }
        /* segment 5 (8 ones) and the shutdown padding run below */
        if (s->training_step == TRAIN_END + 1) {
            s->current_get_bit = s->get_bit;
            s->current_user = s->get_user;
            s->in_training = 0;
        }
        if (s->training_step >= SHUTDOWN_END)
            s->qam.done = 1;
    }
    if (s->bit_rate == 4800) {
        bits = get_scrambled_bit(s);
        bits = (bits << 1) | get_scrambled_bit(s);
        bits = (bits << 1) | get_scrambled_bit(s);
        bits = phase_steps_4800[bits];
    } else {
        bits = get_scrambled_bit(s);
        bits = (bits << 1) | get_scrambled_bit(s);
        bits = phase_steps_2400[bits];
    }
    s->constellation_state = (s->constellation_state + bits) & 7;
    return v27_constel[s->constellation_state];
}

int nf_v27_tx_restart(nf_v27_tx_t *s, int bit_rate)
{
    if (bit_rate != 4800 && bit_rate != 2400)
        return -1;
    s->bit_rate = bit_rate;
    s->scramble_reg = 0x3C;
    s->scrambler_pattern_count = 0;
    s->in_training = 1;
    s->training_step = SEG_2;           /* no TEP */
    s->constellation_state = 0;
    s->current_get_bit = nf_fake_get_bit;
    s->current_user = NULL;
    if (bit_rate == 4800)
        nf_qam_tx_init(&s->qam, tx_shaper_4800, TX_SETS_4800, TX_TAPS, 1,
                       CARRIER_HZ, v27_getbaud, s);
    else
        nf_qam_tx_init(&s->qam, tx_shaper_2400, TX_SETS_2400, TX_TAPS, 3,
                       CARRIER_HZ, v27_getbaud, s);
    /* 0.70711: calibrate our unit-DC-gain RRC tables to spandsp's level */
    nf_qam_tx_restart(&s->qam,
        0.70711f * powf(10.0f, (TX_LEVEL - 3.14f) / 20.0f) * 32768.0f);
    return 0;
}

void nf_v27_tx_init(nf_v27_tx_t *s, int bit_rate, int (*get_bit)(void *), void *user)
{
    memset(s, 0, sizeof(*s));
    make_tables();
    s->get_bit = get_bit;
    s->get_user = user;
    nf_v27_tx_restart(s, bit_rate);
}

void nf_v27_tx_set_get_bit(nf_v27_tx_t *s, int (*get_bit)(void *), void *user)
{
    if (s->get_bit == s->current_get_bit) {
        s->current_get_bit = get_bit;
        s->current_user = user;
    }
    s->get_bit = get_bit;
    s->get_user = user;
}

int nf_v27_tx(nf_v27_tx_t *s, int16_t *amp, int max_len)
{
    return nf_qam_tx(&s->qam, amp, max_len);
}

/* ── rx ────────────────────────────────────────────────────────────── */

static int find_quadrant(const nf_cpx_t *z)
{
    int b1 = z->im > z->re;
    int b2 = z->im < -z->re;
    return (b2 << 1) | (b1 ^ b2);
}

static int find_octant(const nf_cpx_t *z)
{
    float abs_re = fabsf(z->re);
    float abs_im = fabsf(z->im);
    int b1, b2;
    if (abs_im > abs_re * 0.4142136f && abs_im < abs_re * 2.4142136f) {
        /* split the space along the two axes */
        b1 = z->re < 0.0f;
        b2 = z->im < 0.0f;
        return (b2 << 2) | ((b1 ^ b2) << 1) | 1;
    }
    /* split the space along the two diagonals */
    b1 = z->im > z->re;
    b2 = z->im < -z->re;
    return (b2 << 2) | ((b1 ^ b2) << 1);
}

/* The rx descrambler doubles as the training sequence generator: while
 * training (stage between NORMAL and TEST_ONES) it feeds back its own output
 * so that descrambling a stream of ones replays the tx scrambler. */
static int v27_descramble(nf_v27_rx_t *s, int in_bit)
{
    int training = s->training_stage > STAGE_NORMAL
                && s->training_stage < STAGE_TEST_ONES;
    in_bit &= 1;
    int out = (in_bit ^ (s->scramble_reg >> 5) ^ (s->scramble_reg >> 6)) & 1;
    if (s->scrambler_pattern_count >= 33) {
        out ^= 1;
        s->scrambler_pattern_count = 0;
    } else if (training) {
        s->scrambler_pattern_count = 0;
    } else {
        if ((((s->scramble_reg >> 7) ^ (uint32_t) in_bit)
           & ((s->scramble_reg >> 8) ^ (uint32_t) in_bit)
           & ((s->scramble_reg >> 11) ^ (uint32_t) in_bit) & 1))
            s->scrambler_pattern_count = 0;
        else
            s->scrambler_pattern_count++;
    }
    s->scramble_reg <<= 1;
    s->scramble_reg |= (uint32_t) (training ? out : in_bit);
    return out;
}

static void v27_put_bit(nf_v27_rx_t *s, int bit)
{
    int out = v27_descramble(s, bit);
    if (s->training_stage == STAGE_NORMAL)
        s->put_bit(s->put_user, out);
}

static int v27_decode_baud(nf_v27_rx_t *s, const nf_cpx_t *z)
{
    static const uint8_t phase_steps_4800[8] = { 4, 0, 2, 6, 7, 3, 1, 5 };
    static const uint8_t phase_steps_2400[4] = { 0, 2, 3, 1 };
    int nearest, raw_bits;

    if (s->bit_rate == 2400) {
        nearest = find_quadrant(z);
        raw_bits = phase_steps_2400[(nearest - s->constellation_state) & 3];
        v27_put_bit(s, raw_bits);
        v27_put_bit(s, raw_bits >> 1);
        s->constellation_state = nearest;
        nearest <<= 1;
    } else {
        nearest = find_octant(z);
        raw_bits = phase_steps_4800[(nearest - s->constellation_state) & 7];
        v27_put_bit(s, raw_bits);
        v27_put_bit(s, raw_bits >> 1);
        v27_put_bit(s, raw_bits >> 2);
        s->constellation_state = nearest;
    }
    nf_qam_track_carrier(&s->qam, z, &v27_constel[nearest]);
    if (--s->eq_skip <= 0) {
        s->eq_skip = 100;
        nf_qam_tune_eq(&s->qam, z, &v27_constel[nearest]);
    }
    return nearest;
}

static void v27_process_baud(void *user, const nf_cpx_t *z)
{
    static const int abab_pos[2] = { 0, 4 };
    nf_v27_rx_t *s = user;
    if (v27dbg())
        fprintf(stderr, "v27rx stage=%d cnt=%d z=(%.3f,%.3f) agc=%g rate=%d\n",
                s->training_stage, s->training_count, z->re, z->im,
                (double) s->qam.agc_scaling, s->qam.carrier_phase_rate);
    nf_qam_rx_t *q = &s->qam;
    const nf_cpx_t zero = nf_cpx(0.0f, 0.0f);
    const nf_cpx_t *target = &zero;
    int32_t angle, ang;
    int i, j, nearest;

    switch (s->training_stage) {
    case STAGE_NORMAL:
        nearest = v27_decode_baud(s, z);
        target = &v27_constel[nearest];
        break;
    case STAGE_SYMBOL_ACQUISITION:
        /* allow time for the Gardner algorithm to settle the baud timing */
        if (++s->training_count >= 30) {
            q->gardner_step = 32;
            s->training_stage = STAGE_LOG_PHASE;
            memset(s->diff_angles, 0, sizeof(s->diff_angles));
            s->last_angles[0] = nf_angle32(z->im, z->re);
            nf_qam_lock_agc(q);
        }
        break;
    case STAGE_LOG_PHASE:
        s->last_angles[1] = nf_angle32(z->im, z->re);
        s->training_count = 1;
        s->training_stage = STAGE_WAIT_FOR_HOP;
        break;
    case STAGE_WAIT_FOR_HOP:
        angle = nf_angle32(z->im, z->re);
        /* the regular reversals deviate when the scrambled section starts */
        i = s->training_count + 1;
        ang = angle - s->last_angles[i & 1];
        s->last_angles[i & 1] = angle;
        s->diff_angles[i & 0xF] = s->diff_angles[(i - 2) & 0xF] + (ang >> 4);
        if ((ang > NF_PHASE(45.0) || ang < NF_PHASE(-45.0)) && s->training_count >= 13) {
            i = (s->training_count - 8) & ~1;
            if (i > 1) {
                j = i & 0xF;
                ang = (s->diff_angles[j] + s->diff_angles[j | 1]) / (i - 1);
                if (s->bit_rate == 4800)
                    q->carrier_phase_rate += 16 * (ang / 10);
                else
                    q->carrier_phase_rate += 3 * 16 * (ang / 40);
            }
            if (q->carrier_phase_rate < nf_dds_phase_rate(CARRIER_HZ - 20.0)
                || q->carrier_phase_rate > nf_dds_phase_rate(CARRIER_HZ + 20.0)) {
                s->training_stage = STAGE_PARKED;
                nf_qam_park(q);
                break;
            }
            /* spin the reversal target (B, at 180 degrees) onto A */
            angle += NF_PHASE(180.0);
            nf_qam_spin(q, angle);
            q->gardner_step = 2;
            /* we just saw the first element of the scrambled sequence */
            s->training_bc = 1;
            s->training_bc ^= v27_descramble(s, 1);
            v27_descramble(s, 1);
            v27_descramble(s, 1);
            s->constellation_state = abab_pos[s->training_bc];
            target = &v27_constel[s->constellation_state];
            s->training_count = 1;
            s->training_stage = STAGE_TRAIN_ON_ABAB;
            break;
        }
        if (++s->training_count > SEG_3_LEN) {
            s->training_stage = STAGE_PARKED;
            nf_qam_park(q);
        }
        break;
    case STAGE_TRAIN_ON_ABAB:
        s->training_bc ^= v27_descramble(s, 1);
        v27_descramble(s, 1);
        v27_descramble(s, 1);
        s->constellation_state = abab_pos[s->training_bc];
        target = &v27_constel[s->constellation_state];
        nf_qam_track_carrier(q, z, target);
        nf_qam_tune_eq(q, z, target);
        /* ramp the carrier tracking down as the training proceeds */
        q->carrier_track_i = 400.0f
            + (200000.0f - 400.0f) * (float) (SEG_5_LEN - s->training_count) / SEG_5_LEN;
        q->carrier_track_p = 1000000.0f
            + (10000000.0f - 1000000.0f) * (float) (SEG_5_LEN - s->training_count) / SEG_5_LEN;
        if (++s->training_count >= SEG_5_LEN) {
            s->constellation_state = (s->bit_rate == 4800) ? 4 : 2;
            s->training_count = 0;
            s->training_stage = STAGE_TEST_ONES;
        }
        break;
    case STAGE_TEST_ONES:
        nearest = v27_decode_baud(s, z);
        target = &v27_constel[nearest];
        {
            nf_cpx_t e = nf_cpx_sub(*z, *target);
            s->training_error += nf_cpx_power(e);
        }
        if (++s->training_count >= SEG_6_LEN) {
            /* symbols are 1.08 apart at 4800, 2.0 apart at 2400 */
            if ((s->bit_rate == 4800 && s->training_error < SEG_6_LEN * 0.25f)
                || (s->bit_rate == 2400 && s->training_error < SEG_6_LEN * 0.5f)) {
                nf_qam_report(q, NF_SIG_TRAINING_SUCCEEDED);
                q->signal_present = (s->bit_rate == 4800) ? 90 : 120;
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

static void v27_carrier_drop(void *user)
{
    nf_v27_rx_t *s = user;
    nf_v27_rx_restart(s, s->bit_rate);
}

int nf_v27_rx_restart(nf_v27_rx_t *s, int bit_rate)
{
    if (bit_rate != 4800 && bit_rate != 2400)
        return -1;
    s->bit_rate = bit_rate;
    s->scramble_reg = 0x3C;
    s->scrambler_pattern_count = 0;
    s->training_stage = STAGE_SYMBOL_ACQUISITION;
    s->training_bc = 0;
    s->training_count = 0;
    s->training_error = 0.0f;
    s->constellation_state = 0;
    s->eq_skip = 0;
    memset(s->diff_angles, 0, sizeof(s->diff_angles));
    {
        /* keep the hooks; re-point the tables for the selected rate */
        void (*status)(void *, int) = s->qam.status;
        void *status_user = s->qam.status_user;
        if (bit_rate == 4800)
            nf_qam_rx_init(&s->qam, rx_re_4800, rx_im_4800, RX_SETS_4800, RX_TAPS,
                           HALF_STEP_4800, 32, 16, 0.25f, 1.414f, CARRIER_HZ,
                           v27_process_baud, v27_carrier_drop, s);
        else
            nf_qam_rx_init(&s->qam, rx_re_2400, rx_im_2400, RX_SETS_2400, RX_TAPS,
                           HALF_STEP_2400, 32, 16, 0.25f, 1.414f, CARRIER_HZ,
                           v27_process_baud, v27_carrier_drop, s);
        nf_qam_rx_set_gardner(&s->qam, 512);
        nf_qam_rx_set_cutoff(&s->qam, -45.5f, 0.4f);
        /* V.27ter centres its T/2 equalizer one tap later, at the
         * constellation amplitude, and starts the put step a full half baud
         * out (spandsp v27ter_rx.c equalizer_reset) */
        s->qam.eq_centre_val = 1.414f;
        s->qam.eq_centre_idx = 16 + 1;
        s->qam.eq_put_init = s->qam.half_baud_step;
        s->qam.status = status;
        s->qam.status_user = status_user;
    }
    nf_qam_rx_restart(&s->qam, 0, 1.414f / 283.0f);
    s->qam.carrier_track_i = 200000.0f;
    s->qam.carrier_track_p = 10000000.0f;
    s->qam.gardner_step = 512;
    return 0;
}

void nf_v27_rx_init(nf_v27_rx_t *s, int bit_rate, void (*put_bit)(void *, int), void *user)
{
    memset(s, 0, sizeof(*s));
    make_tables();
    s->put_bit = put_bit;
    s->put_user = user;
    nf_v27_rx_restart(s, bit_rate);
}

void nf_v27_rx_set_put_bit(nf_v27_rx_t *s, void (*put_bit)(void *, int), void *user)
{
    s->put_bit = put_bit;
    s->put_user = user;
}

void nf_v27_rx_set_status_handler(nf_v27_rx_t *s, void (*status)(void *, int), void *user)
{
    s->qam.status = status;
    s->qam.status_user = user;
}

int nf_v27_rx(nf_v27_rx_t *s, const int16_t *amp, int len)
{
    return nf_qam_rx(&s->qam, amp, len);
}
