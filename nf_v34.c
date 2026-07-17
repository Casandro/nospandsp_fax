#include "nf_v34.h"
#include "nf_v34_superconstellation.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ITU-T V.34 (02/98) Table 2 - "Carrier frequencies versus symbol rate" -
 * exact values from the recommendation text (not approximations). The
 * 3429-baud/1959Hz value was independently confirmed against the real
 * capture in references/v.34_modem_test.wav before this table was found,
 * which is what gave confidence the whole front end was already correct. */
const nf_v34_rate_t nf_v34_rates[6] = {
    { 2400, 1600.0, 1800.0 },
    { 2743, 1646.0, 1829.0 },
    { 2800, 1680.0, 1867.0 },
    { 3000, 1800.0, 2000.0 },
    { 3200, 1829.0, 1920.0 },
    { 3429, 1959.0, 1959.0 },
};

/* ═══ complete mode parameter tables (Tables 1/2/7/8/10) ═══════════════
 *
 * Transcribed from references/T-REC-V.34-199802-I!!PDF-E.pdf and verified
 * row-by-row by nf_v34_modeparams_check() (check-v34's `modetab` mode):
 * N = R*0.28/J integer, b = ceil(N/P), r = N-(b-1)P in [1,P], SWP re-derived
 * from the 8.2 counter algorithm, K/q per eq 9-1, M_min/M_exp per 9.2,
 * L = 4M*2^q (9-2) against the Table 10 L columns.
 *
 * One transcription ambiguity found and resolved by the cross-check: the
 * PDF's Table 8 cell for S=3000/R=14600 renders ambiguously as "3F7F" at
 * page-image resolution, but the text layer AND the 8.2 counter derivation
 * (r = 584 - 38*15 = 14 high frames of 15) both give 3FFF. */

const nf_v34_srate_t nf_v34_srates[6] = {
    /*        a  c  spsN spsD J  P    low d,e,cn,cd     high d,e,cn,cd   trn */
    { 2400,   1, 1, 10,  3,   7, 12, {{2,3, 1,  5},     {3,4, 9, 40}},   84 },
    { 2743,   8, 7, 35, 12,   8, 12, {{3,5, 36,175},    {2,3, 8, 35}},   96 },
    { 2800,   7, 6, 20,  7,   7, 14, {{3,5, 21,100},    {2,3, 7, 30}},   98 },
    { 3000,   5, 4,  8,  3,   7, 15, {{3,5, 9,  40},    {2,3, 1,  4}},  105 },
    { 3200,   4, 3,  5,  2,   7, 16, {{4,7, 8,  35},    {3,5, 6, 25}},  112 },
    { 3429,  10, 7,  7,  3,   8, 15, {{4,7, 12, 49},    {4,7, 12, 49}}, 120 },
};

/* Tables 8 + 10, all rows. aux = 1 marks the R = primary + 200 bit/s
 * auxiliary-channel variants (transcribed and checked, unusable until the
 * aux channel itself exists). */
static const nf_v34_rateparam_t v34_rates_2400[] = {
    /* rate  aux  b   swp     K   q  Mmin Mexp */
    {  2400, 0,   8, 0xFFF,   0,  0,  1,  1 },
    {  2600, 1,   9, 0x6DB,   0,  0,  1,  1 },
    {  4800, 0,  16, 0xFFF,   4,  0,  2,  2 },
    {  5000, 1,  17, 0x6DB,   5,  0,  2,  2 },
    {  7200, 0,  24, 0xFFF,  12,  0,  3,  4 },
    {  7400, 1,  25, 0x6DB,  13,  0,  4,  4 },
    {  9600, 0,  32, 0xFFF,  20,  0,  6,  7 },
    {  9800, 1,  33, 0x6DB,  21,  0,  7,  8 },
    { 12000, 0,  40, 0xFFF,  28,  0, 12, 14 },
    { 12200, 1,  41, 0x6DB,  29,  0, 13, 15 },
    { 14400, 0,  48, 0xFFF,  28,  1, 12, 14 },
    { 14600, 1,  49, 0x6DB,  29,  1, 13, 15 },
    { 16800, 0,  56, 0xFFF,  28,  2, 12, 14 },
    { 17000, 1,  57, 0x6DB,  29,  2, 13, 15 },
    { 19200, 0,  64, 0xFFF,  28,  3, 12, 14 },
    { 19400, 1,  65, 0x6DB,  29,  3, 13, 15 },
    { 21600, 0,  72, 0xFFF,  28,  4, 12, 14 },
    { 21800, 1,  73, 0x6DB,  29,  4, 13, 15 },
};
static const nf_v34_rateparam_t v34_rates_2743[] = {
    {  4800, 0,  14, 0xFFF,   2,  0,  2,  2 },
    {  5000, 1,  15, 0x56B,   3,  0,  2,  2 },
    {  7200, 0,  21, 0xFFF,   9,  0,  3,  3 },
    {  7400, 1,  22, 0x56B,  10,  0,  3,  3 },
    {  9600, 0,  28, 0xFFF,  16,  0,  4,  5 },
    {  9800, 1,  29, 0x56B,  17,  0,  5,  5 },
    { 12000, 0,  35, 0xFFF,  23,  0,  8,  9 },
    { 12200, 1,  36, 0x56B,  24,  0,  8, 10 },
    { 14400, 0,  42, 0xFFF,  30,  0, 14, 17 },
    { 14600, 1,  43, 0x56B,  31,  0, 15, 18 },
    { 16800, 0,  49, 0xFFF,  29,  1, 13, 15 },
    { 17000, 1,  50, 0x56B,  30,  1, 14, 17 },
    { 19200, 0,  56, 0xFFF,  28,  2, 12, 14 },
    { 19400, 1,  57, 0x56B,  29,  2, 13, 15 },
    { 21600, 0,  63, 0xFFF,  27,  3, 11, 13 },
    { 21800, 1,  64, 0x56B,  28,  3, 12, 14 },
    { 24000, 0,  70, 0xFFF,  26,  4, 10, 12 },
    { 24200, 1,  71, 0x56B,  27,  4, 11, 13 },
    { 26400, 0,  77, 0xFFF,  25,  5,  9, 11 },
    { 26600, 1,  78, 0x56B,  26,  5, 10, 12 },
};
static const nf_v34_rateparam_t v34_rates_2800[] = {
    {  4800, 0,  14, 0x1BB7,  2,  0,  2,  2 },
    {  5000, 1,  15, 0x0489,  3,  0,  2,  2 },
    {  7200, 0,  21, 0x15AB,  9,  0,  3,  3 },
    {  7400, 1,  22, 0x0081, 10,  0,  3,  3 },
    {  9600, 0,  28, 0x0A95, 16,  0,  4,  5 },
    {  9800, 1,  28, 0x3FFF, 16,  0,  4,  5 },
    { 12000, 0,  35, 0x0489, 23,  0,  8,  9 },
    { 12200, 1,  35, 0x1FBF, 23,  0,  8,  9 },
    { 14400, 0,  42, 0x0081, 30,  0, 14, 17 },
    { 14600, 1,  42, 0x1BB7, 30,  0, 14, 17 },
    { 16800, 0,  48, 0x3FFF, 28,  1, 12, 14 },
    { 17000, 1,  49, 0x15AB, 29,  1, 13, 15 },
    { 19200, 0,  55, 0x1FBF, 27,  2, 11, 13 },
    { 19400, 1,  56, 0x0A95, 28,  2, 12, 14 },
    { 21600, 0,  62, 0x1BB7, 26,  3, 10, 12 },
    { 21800, 1,  63, 0x0489, 27,  3, 11, 13 },
    { 24000, 0,  69, 0x15AB, 25,  4,  9, 11 },
    { 24200, 1,  70, 0x0081, 26,  4, 10, 12 },
    { 26400, 0,  76, 0x0A95, 24,  5,  8, 10 },
    { 26600, 1,  76, 0x3FFF, 24,  5,  8, 10 },
};
static const nf_v34_rateparam_t v34_rates_3000[] = {
    {  4800, 0,  13, 0x3DEF,  1,  0,  2,  2 },
    {  5000, 1,  14, 0x1249,  2,  0,  2,  2 },
    {  7200, 0,  20, 0x0421,  8,  0,  2,  3 },
    {  7400, 1,  20, 0x3777,  8,  0,  2,  3 },
    {  9600, 0,  26, 0x2D6B, 14,  0,  4,  4 },
    {  9800, 1,  27, 0x0081, 15,  0,  4,  5 },
    { 12000, 0,  32, 0x7FFF, 20,  0,  6,  7 },
    { 12200, 1,  33, 0x2AAB, 21,  0,  7,  8 },
    { 14400, 0,  39, 0x14A5, 27,  0, 11, 13 },
    { 14600, 1,  39, 0x3FFF, 27,  0, 11, 13 },   /* SWP: see file comment */
    { 16800, 0,  45, 0x3DEF, 25,  1,  9, 11 },
    { 17000, 1,  46, 0x1249, 26,  1, 10, 12 },
    { 19200, 0,  52, 0x0421, 24,  2,  8, 10 },
    { 19400, 1,  52, 0x3777, 24,  2,  8, 10 },
    { 21600, 0,  58, 0x2D6B, 30,  2, 14, 17 },
    { 21800, 1,  59, 0x0081, 31,  2, 15, 18 },
    { 24000, 0,  64, 0x7FFF, 28,  3, 12, 14 },
    { 24200, 1,  65, 0x2AAB, 29,  3, 13, 15 },
    { 26400, 0,  71, 0x14A5, 27,  4, 11, 13 },
    { 26600, 1,  71, 0x3FFF, 27,  4, 11, 13 },
    { 28800, 0,  77, 0x3DEF, 25,  5,  9, 11 },
    { 29000, 1,  78, 0x1249, 26,  5, 10, 12 },
};
static const nf_v34_rateparam_t v34_rates_3200[] = {
    {  4800, 0,  12, 0xFFFF,  0,  0,  1,  1 },
    {  5000, 1,  13, 0x5555,  1,  0,  2,  2 },
    {  7200, 0,  18, 0xFFFF,  6,  0,  2,  2 },
    {  7400, 1,  19, 0x5555,  7,  0,  2,  2 },
    {  9600, 0,  24, 0xFFFF, 12,  0,  3,  4 },
    {  9800, 1,  25, 0x5555, 13,  0,  4,  4 },
    { 12000, 0,  30, 0xFFFF, 18,  0,  5,  6 },
    { 12200, 1,  31, 0x5555, 19,  0,  6,  6 },
    { 14400, 0,  36, 0xFFFF, 24,  0,  8, 10 },
    { 14600, 1,  37, 0x5555, 25,  0,  9, 11 },
    { 16800, 0,  42, 0xFFFF, 30,  0, 14, 17 },
    { 17000, 1,  43, 0x5555, 31,  0, 15, 18 },
    { 19200, 0,  48, 0xFFFF, 28,  1, 12, 14 },
    { 19400, 1,  49, 0x5555, 29,  1, 13, 15 },
    { 21600, 0,  54, 0xFFFF, 26,  2, 10, 12 },
    { 21800, 1,  55, 0x5555, 27,  2, 11, 13 },
    { 24000, 0,  60, 0xFFFF, 24,  3,  8, 10 },
    { 24200, 1,  61, 0x5555, 25,  3,  9, 11 },
    { 26400, 0,  66, 0xFFFF, 30,  3, 14, 17 },
    { 26600, 1,  67, 0x5555, 31,  3, 15, 18 },
    { 28800, 0,  72, 0xFFFF, 28,  4, 12, 14 },
    { 29000, 1,  73, 0x5555, 29,  4, 13, 15 },
    { 31200, 0,  78, 0xFFFF, 26,  5, 10, 12 },
    { 31400, 1,  79, 0x5555, 27,  5, 11, 13 },
};
static const nf_v34_rateparam_t v34_rates_3429[] = {
    {  4800, 0,  12, 0x0421,  0,  0,  1,  1 },
    {  5000, 1,  12, 0x36DB,  0,  0,  1,  1 },
    {  7200, 0,  17, 0x3DEF,  5,  0,  2,  2 },
    {  7400, 1,  18, 0x0889,  6,  0,  2,  2 },
    {  9600, 0,  23, 0x14A5, 11,  0,  3,  3 },
    {  9800, 1,  23, 0x3F7F, 11,  0,  3,  3 },
    { 12000, 0,  28, 0x7FFF, 16,  0,  4,  5 },
    { 12200, 1,  29, 0x1555, 17,  0,  5,  5 },
    { 14400, 0,  34, 0x2D6B, 22,  0,  7,  8 },
    { 14600, 1,  35, 0x0001, 23,  0,  8,  9 },
    { 16800, 0,  40, 0x0421, 28,  0, 12, 14 },
    { 17000, 1,  40, 0x36DB, 28,  0, 12, 14 },
    { 19200, 0,  45, 0x3DEF, 25,  1,  9, 11 },
    { 19400, 1,  46, 0x0889, 26,  1, 10, 12 },
    { 21600, 0,  51, 0x14A5, 31,  1, 15, 18 },
    { 21800, 1,  51, 0x3F7F, 31,  1, 15, 18 },
    { 24000, 0,  56, 0x7FFF, 28,  2, 12, 14 },
    { 24200, 1,  57, 0x1555, 29,  2, 13, 15 },
    { 26400, 0,  62, 0x2D6B, 26,  3, 10, 12 },
    { 26600, 1,  63, 0x0001, 27,  3, 11, 13 },
    { 28800, 0,  68, 0x0421, 24,  4,  8, 10 },
    { 29000, 1,  68, 0x36DB, 24,  4,  8, 10 },
    { 31200, 0,  73, 0x3DEF, 29,  4, 13, 15 },
    { 31400, 1,  74, 0x0889, 30,  4, 14, 17 },
    { 33600, 0,  79, 0x14A5, 27,  5, 11, 13 },
    { 33800, 1,  79, 0x3F7F, 27,  5, 11, 13 },
};

static const struct {
    const nf_v34_rateparam_t *rows;
    int n;
} v34_ratetabs[6] = {
    { v34_rates_2400, (int) (sizeof v34_rates_2400 / sizeof v34_rates_2400[0]) },
    { v34_rates_2743, (int) (sizeof v34_rates_2743 / sizeof v34_rates_2743[0]) },
    { v34_rates_2800, (int) (sizeof v34_rates_2800 / sizeof v34_rates_2800[0]) },
    { v34_rates_3000, (int) (sizeof v34_rates_3000 / sizeof v34_rates_3000[0]) },
    { v34_rates_3200, (int) (sizeof v34_rates_3200 / sizeof v34_rates_3200[0]) },
    { v34_rates_3429, (int) (sizeof v34_rates_3429 / sizeof v34_rates_3429[0]) },
};

const nf_v34_rateparam_t *nf_v34_ratetab(int sr_idx, int *count)
{
    if (sr_idx < 0 || sr_idx >= 6) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = v34_ratetabs[sr_idx].n;
    return v34_ratetabs[sr_idx].rows;
}

const nf_v34_rateparam_t *nf_v34_rateparam(int sr_idx, int rate)
{
    int i, n;
    const nf_v34_rateparam_t *t = nf_v34_ratetab(sr_idx, &n);

    for (i = 0; i < n; i++)
        if (t[i].rate == rate)
            return &t[i];
    return NULL;
}

int nf_v34_rate_mask(int sr_idx)
{
    int i, n, mask = 0;
    const nf_v34_rateparam_t *t = nf_v34_ratetab(sr_idx, &n);

    for (i = 0; i < n; i++)
        if (!t[i].aux && t[i].rate % 2400 == 0)
            mask |= 1 << (t[i].rate / 2400 - 1);
    return mask;
}

int nf_v34_modeparams_check(void)
{
    int si, ri, bad = 0;

    for (si = 0; si < 6; si++) {
        const nf_v34_srate_t *sr = &nf_v34_srates[si];
        int n;
        const nf_v34_rateparam_t *t = nf_v34_ratetab(si, &n);

        /* symbol-rate row internal consistency */
        /* Table 1 name = (a/c)*2400 rounded to nearest integer; samples
         * per symbol and TRN unit are exact rationals of a/c. */
        if ((2 * sr->a * 2400 + sr->c) / (2 * sr->c) != sr->baud_name ||
            sr->sps_num * sr->a * 2400 != sr->sps_den * 8000 * sr->c ||
            sr->trn_unit_sym * sr->c != 84 * sr->a) {
            fprintf(stderr, "modetab: srate row %d inconsistent\n", si);
            bad++;
        }
        {
            int o;
            for (o = 0; o < 2; o++) {
                /* carrier = (d/e)*S; cycles/sample = d*a*2400/(e*c*8000) */
                long lhs = (long) sr->car[o].cnum * sr->car[o].e * sr->c * 8000;
                long rhs = (long) sr->car[o].cden * sr->car[o].d * sr->a * 2400;
                if (lhs != rhs) {
                    fprintf(stderr, "modetab: srate %d carrier %d rational"
                            " mismatch\n", si, o);
                    bad++;
                }
            }
        }

        for (ri = 0; ri < n; ri++) {
            const nf_v34_rateparam_t *r = &t[ri];
            long N28 = (long) r->rate * 28;         /* R * 0.28 = R*28/100 */
            long N, b, rr, k;
            uint16_t swp = 0;
            int q, K, m_min, m_exp, cnt;
            double kk8;

            if (N28 % (100L * sr->J) != 0) {
                fprintf(stderr, "modetab: S=%d R=%d: N not integer\n",
                        sr->baud_name, r->rate);
                bad++;
                continue;
            }
            N = N28 / (100L * sr->J);
            b = (N + sr->P - 1) / sr->P;
            rr = N - (b - 1) * sr->P;
            if (b != r->b || rr < 1 || rr > sr->P) {
                fprintf(stderr, "modetab: S=%d R=%d: b=%ld (table %d) r=%ld\n",
                        sr->baud_name, r->rate, b, r->b, rr);
                bad++;
            }
            /* SWP per the 8.2 counter algorithm */
            cnt = 0;
            for (k = 0; k < sr->P; k++) {
                cnt += (int) rr;
                if (cnt < sr->P) {
                    /* low frame: bit stays 0 */
                } else {
                    swp |= (uint16_t) (1 << (sr->P - 1 - k));
                    cnt -= sr->P;
                }
            }
            if (swp != r->swp) {
                fprintf(stderr, "modetab: S=%d R=%d: SWP derived %04X table"
                        " %04X\n", sr->baud_name, r->rate, swp, r->swp);
                bad++;
            }
            /* K and q per eq 9-1 */
            if (b <= 12) {
                K = 0;
                q = 0;
            } else {
                q = 0;
                while (b - 12 - 8 * q >= 32)
                    q++;
                K = (int) b - 12 - 8 * q;
            }
            if (K != r->K || q != r->q) {
                fprintf(stderr, "modetab: S=%d R=%d: K/q derived %d/%d table"
                        " %d/%d\n", sr->baud_name, r->rate, K, q, r->K, r->q);
                bad++;
            }
            /* M_min/M_exp per 9.2; L = 4M*2^q (9-2) must stay <= 1664 */
            kk8 = pow(2.0, (double) K / 8.0);
            m_min = (int) ceil(kk8 - 1e-9);
            m_exp = (int) floor(1.25 * kk8 + 0.5);
            if (m_exp < m_min)
                m_exp = m_min;
            if (m_min != r->m_min || m_exp != r->m_exp ||
                4L * r->m_exp * (1L << q) > 1664) {
                fprintf(stderr, "modetab: S=%d R=%d: M derived %d/%d table"
                        " %d/%d (L_exp %ld)\n", sr->baud_name, r->rate,
                        m_min, m_exp, r->m_min, r->m_exp,
                        4L * r->m_exp * (1L << q));
                bad++;
            }
        }
    }
    return bad ? -1 : 0;
}

int nf_v34_pcparams_init(nf_v34_pcparams_t *pp, int sr_idx, int rate,
                         int high_carrier, int expanded, int nonlinear,
                         int trn_16pt)
{
    const nf_v34_srate_t *sr;
    const nf_v34_rateparam_t *r;
    int i;

    memset(pp, 0, sizeof(*pp));
    if (sr_idx < 0 || sr_idx >= 6)
        return -1;
    sr = &nf_v34_srates[sr_idx];
    r = nf_v34_rateparam(sr_idx, rate);
    if (!r || r->aux)                     /* aux rows need the aux channel */
        return -1;
    pp->sr_idx = sr_idx;
    pp->rate = rate;
    pp->high_carrier = high_carrier ? 1 : 0;
    pp->expanded = expanded ? 1 : 0;
    pp->trn_16pt = trn_16pt ? 1 : 0;
    pp->M = expanded ? r->m_exp : r->m_min;
    pp->q = r->q;
    pp->K = r->K;
    pp->b = r->b;
    pp->P = sr->P;
    pp->J = sr->J;
    pp->swp = r->swp;
    pp->theta = nonlinear ? 0.3125 : 0.0;
    pp->baud_nominal = (double) sr->a * 2400.0 / (double) sr->c;
    pp->carrier_hz = 8000.0 * (double) sr->car[pp->high_carrier].cnum /
                     (double) sr->car[pp->high_carrier].cden;
    pp->sps_num = sr->sps_num;
    pp->sps_den = sr->sps_den;
    pp->cnum = sr->car[pp->high_carrier].cnum;
    pp->cden = sr->car[pp->high_carrier].cden;
    pp->b1_sym = 8 * sr->P;
    pp->b1_bits = 0;
    for (i = 0; i < sr->P; i++) {
        int high = (r->swp >> (sr->P - 1 - i)) & 1;
        pp->b1_bits += high ? r->b : r->b - 1;
    }
    pp->trn_unit_sym = sr->trn_unit_sym;
    return 0;
}

/* Rolloff placeholder - V.34's actual per-rate excess bandwidth is not yet
 * confirmed against spec text (same caveat as the carrier table above). */
#define NF_V34_ROLLOFF 0.25

void nf_v34_init(nf_v34_t *s)
{
    int i;

    memset(s, 0, sizeof(*s));
    for (i = 0; i < NF_V34_NUM_RATES; i++) {
        double baud = nf_v34_rates[i].baud;
        double carrier = nf_v34_rates[i].carrier_low_hz;
        nf_v34_shaper_t *sh = &s->shaper[i];

        nf_rrc_design(1.0 / NF_V34_RRC_SETS, NF_V34_ROLLOFF,
                     NF_V34_RRC_SETS, NF_V34_RRC_TAPS, 0.0, sh->tx, NULL);
        nf_rrc_design(baud / (NF_V34_RRC_SETS * NF_SAMPLE_RATE), NF_V34_ROLLOFF,
                     NF_V34_RRC_SETS, NF_V34_RRC_TAPS, carrier, sh->rx_re, sh->rx_im);
    }
}

/* ── control-channel QPSK demodulator (real, validated - see nf_v34.h) ── */

/* Rolloff validated empirically against the real capture (0.3 gave a clean,
 * tightly-locked constellation; not from a spec citation). */
#define NF_V34_CTRL_ROLLOFF 0.3

void nf_v34_ctrl_rx_init(nf_v34_ctrl_rx_t *s, double carrier_hz)
{
    memset(s, 0, sizeof(*s));
    s->lo_rate = nf_dds_phase_rate(carrier_hz);
    nf_rrc_design((double) NF_V34_CTRL_BAUD / NF_SAMPLE_RATE, NF_V34_CTRL_ROLLOFF,
                 1, NF_V34_CTRL_TAPS, 0.0, s->rrc, NULL);
    s->baud_period = (double) NF_SAMPLE_RATE / NF_V34_CTRL_BAUD;
    s->idx = s->baud_period;      /* first "full" event; the mid event is idx-half */
    s->gardner_gain = 0.02;
    s->agc_scale = 1.0f;
    /* Costas loop gains: modest - this signal locks even without much pull;
     * validated empirically (see tests/nf_v34test.c ctrl mode). */
    s->track_p = 0.015f;
    s->track_i = 0.0003f;
}

/* Linear-interpolate the filtered-baseband history at fractional absolute
 * sample position `pos` (pos must be within ~1 sample of the newest history
 * entry - true here because the caller checks the crossing condition on
 * every incoming sample, so we never fall behind by more than one sample). */
static nf_cpx_t ctrl_interp(const nf_v34_ctrl_rx_t *s, double pos)
{
    double rel = (double) (s->hist_n - 1) - pos;
    int k0;
    double frac;

    if (rel < 0) rel = 0;
    k0 = (int) rel;
    frac = rel - k0;
    if (k0 + 1 >= NF_V34_CTRL_HIST) { k0 = NF_V34_CTRL_HIST - 2; frac = 1.0; }
    {
        nf_cpx_t a = s->hist[k0], b = s->hist[k0 + 1];
        return nf_cpx((float) (a.re * (1 - frac) + b.re * frac),
                      (float) (a.im * (1 - frac) + b.im * frac));
    }
}

void nf_v34_ctrl_rx(nf_v34_ctrl_rx_t *s, const int16_t *amp, int len,
                    void (*on_symbol)(void *user, const nf_cpx_t *z, int dibit),
                    void *user)
{
    int i, k;
    double half = s->baud_period / 2.0;

    for (i = 0; i < len; i++) {
        nf_cpx_t lo, bb, filt;

        lo = nf_dds_cpx(s->lo_phase);
        s->lo_phase += (uint32_t) s->lo_rate;
        /* downconvert: multiply by e^{-j*theta} = conj(lo) */
        bb = nf_cpx((float) amp[i] * lo.re, -(float) amp[i] * lo.im);

        s->firbuf[s->firptr] = bb;
        filt = nf_cpx(0.0f, 0.0f);
        for (k = 0; k < NF_V34_CTRL_TAPS; k++) {
            int p = s->firptr - k;
            if (p < 0) p += NF_V34_CTRL_TAPS;
            filt.re += s->rrc[k] * s->firbuf[p].re;
            filt.im += s->rrc[k] * s->firbuf[p].im;
        }
        s->firptr = (s->firptr + 1) % NF_V34_CTRL_TAPS;

        /* AGC: normalise the filtered sample to ~unit RMS before it ever
         * reaches the Gardner loop (see the struct comment - the loop's
         * error term is amplitude-squared, so real int16-scale audio
         * without this makes it free-run instead of tracking baud). */
        s->agc_power += ((filt.re * filt.re + filt.im * filt.im) - s->agc_power) * 0.01f;
        if (s->agc_power > 1e-6f)
            s->agc_scale = 1.0f / sqrtf(s->agc_power);
        filt.re *= s->agc_scale;
        filt.im *= s->agc_scale;

        for (k = NF_V34_CTRL_HIST - 1; k > 0; k--)
            s->hist[k] = s->hist[k - 1];
        s->hist[0] = filt;
        s->hist_n++;

        s->got_symbol = 0;

        if (!s->have_mid && (double) (s->hist_n - 1) >= s->idx - half) {
            s->mid_sample = ctrl_interp(s, s->idx - half);
            s->have_mid = 1;
        }
        if (s->have_mid && (double) (s->hist_n - 1) >= s->idx) {
            nf_cpx_t cur, e, z, rot;
            float err, cerr, mag;
            int dibit;

            cur = ctrl_interp(s, s->idx);
            e = nf_cpx_sub(cur, s->prev_symbol);
            err = e.re * s->mid_sample.re + e.im * s->mid_sample.im;
            s->idx += s->baud_period - s->gardner_gain * err;
            s->prev_symbol = cur;
            s->have_mid = 0;

            /* carrier correction: rotate by the tracked phase */
            rot = nf_dds_cpx(s->carrier_phase);
            z = nf_cpx_mul(cur, rot);

            /* QPSK slicer (4 quadrants) + standard QPSK Costas phase error */
            dibit = (z.im >= 0.0f ? 2 : 0) | ((z.re >= 0.0f) == (z.im >= 0.0f) ? 1 : 0);
            mag = (float) sqrt(z.re * z.re + z.im * z.im) + 1e-9f;
            cerr = ((z.re >= 0.0f ? 1.0f : -1.0f) * z.im -
                    (z.im >= 0.0f ? 1.0f : -1.0f) * z.re) / mag;
            {
                /* map a radian-scale phase error to 32-bit phase-word units:
                 * 2^32 corresponds to a full turn (2*pi radians). */
                const float RAD_TO_PHASE = 683565275.6f;
                s->carrier_rate += (int32_t) (s->track_i * cerr * RAD_TO_PHASE);
                s->carrier_phase = (uint32_t) ((int64_t) s->carrier_phase +
                                               (int32_t) (s->track_p * cerr * RAD_TO_PHASE) +
                                               (int32_t) s->carrier_rate);
            }

            s->got_symbol = 1;
            s->symbol = z;
            s->dibit = dibit;
            if (on_symbol)
                on_symbol(user, &z, dibit);
        }
    }
}

/* ── control-channel batch demodulator (static AGC + 4th-power Costas) ──
 * See nf_v34.h for why this exists separately from nf_v34_ctrl_rx_t. */

static nf_cpx_t v34_cc_interp(const nf_cpx_t *filt, int n, double pos)
{
    int p0 = (int) floor(pos);
    double frac = pos - p0;

    if (p0 < 0) { p0 = 0; frac = 0.0; }
    if (p0 + 1 >= n) { p0 = n - 2; frac = 1.0; }
    if (p0 < 0) p0 = 0;
    {
        nf_cpx_t a = filt[p0], b = filt[p0 + 1];
        return nf_cpx((float) (a.re * (1 - frac) + b.re * frac),
                      (float) (a.im * (1 - frac) + b.im * frac));
    }
}

/* One Gardner + 4th-power-Costas tracking pass over the filtered baseband,
 * starting at initial timing phase tau0 (samples). Symbols are written to
 * out[] (caller-sized n / baud_period + 2); returns the symbol count and
 * (in *qual) the pass's lock quality = mean |sin(4*phase_err)| over the
 * post-settling symbols (small = locked). Extracted from the old inline
 * loop so the acquisition can try several starting phases - the Gardner
 * recursion has an unstable equilibrium exactly between symbols, and a
 * burst whose symbol grid lands there (which the deterministic sample
 * alignment of a real half-duplex exchange makes SYSTEMATIC, not rare -
 * found via the impairment sweep, where cells failed or passed based on a
 * 100-sample shift in gate-open time) can leave the single-start loop
 * unlocked for hundreds of symbols. */
/* train_sym > 0 freezes the 4th-power carrier loop after that many symbols:
 * the PPh/ALT/MPh/E training is 4-point QPSK the loop locks on cleanly, but a
 * 2400-mode burst then switches to 16-point user data whose 4th power is NOT a
 * clean line - continuing to drive the loop from it corrupts the carrier phase
 * and the 16-point slice fails (and the pass-quality metric, if measured over
 * the data, picks the wrong timing phase). So after train_sym symbols we coast
 * at the locked frequency (carrier_rate held, no phase correction) and stop
 * accumulating quality. train_sym == 0 keeps the original continuous behaviour
 * (the 1200-mode capture regression relies on it being bit-identical). */
static int v34_cc_track_pass(const nf_cpx_t *filt, int n, double tau0,
                             nf_cpx_t *out, double *qual, int train_sym)
{
    double baud_period = (double) NF_SAMPLE_RATE / NF_V34_CTRL_BAUD;
    double half = baud_period / 2.0;
    double idx = baud_period + tau0;
    nf_cpx_t prev_symbol = { 0.0f, 0.0f };
    const double gardner_gain = 0.02;
    uint32_t carrier_phase = 0;
    int32_t carrier_rate = 0;
    const float track_p = 0.02f, track_i = 0.0005f;
    const float RAD_TO_PHASE = 683565275.6f;
    const int settle = 40;
    double qsum = 0.0, am = 0.0;
    long qn = 0;
    int nsym = 0;

    while (idx + 1 < n) {
        nf_cpx_t mid, cur, e, z, rot;
        float err, cerr, mag;
        double curmag;
        int active;

        mid = v34_cc_interp(filt, n, idx - half);
        cur = v34_cc_interp(filt, n, idx);

        /* amplitude gate: the burst gate prepends silence and the RRC ramp,
         * and noise there WALKS the Gardner index and WINDS the Costas
         * integrator - by the time real symbols arrive the loops carry a
         * bogus frequency and never fully recover (seen against a real SG3:
         * the output constellation span rotating at ~2 Hz while the input
         * was static). Both loops only update on significant amplitude. */
        curmag = sqrt((double) cur.re * cur.re + (double) cur.im * cur.im);
        am += (curmag - am) / 64.0;
        active = curmag > 0.5 * am && am > 1e-4;

        e = nf_cpx_sub(cur, prev_symbol);
        err = e.re * mid.re + e.im * mid.im;
        if (active)
            idx += baud_period - gardner_gain * err;
        else
            idx += baud_period;
        prev_symbol = cur;

        rot = nf_dds_cpx(carrier_phase);
        z = nf_cpx_mul(cur, rot);

        {
            nf_cpx_t z2 = nf_cpx_mul(z, z);
            nf_cpx_t z4 = nf_cpx_mul(z2, z2);
            mag = (float) sqrt(z4.re * z4.re + z4.im * z4.im) + 1e-9f;
            cerr = z4.im / mag;
        }
        if (!active || (train_sym > 0 && nsym >= train_sym)) {
            /* coast: hold the locked frequency (noise, or the 16-point
             * data's meaningless 4th-power error) */
            carrier_phase = (uint32_t) ((int64_t) carrier_phase +
                                        (int32_t) carrier_rate);
        } else {
            carrier_rate += (int32_t) (track_i * cerr * RAD_TO_PHASE);
            carrier_phase = (uint32_t) ((int64_t) carrier_phase +
                                         (int32_t) (track_p * cerr * RAD_TO_PHASE) +
                                         (int32_t) carrier_rate);
        }

        out[nsym++] = z;
        if (active && nsym > settle && (train_sym <= 0 || nsym < train_sym)) {
            qsum += fabs((double) cerr);
            qn++;
        }
    }
    *qual = qn ? qsum / (double) qn : 1e9;
    return nsym;
}

void nf_v34_cc_rx_batch(const int16_t *amp, int n, double carrier_hz,
                         void (*on_symbol)(void *user, const nf_cpx_t *z),
                         void *user, int cfo_max_sym)
{
    int i, k;
    uint32_t lo_phase = 0;
    int32_t lo_rate = nf_dds_phase_rate(carrier_hz);
    float rrc[NF_V34_CC_BATCH_TAPS];
    nf_cpx_t *bb, *filt;
    nf_cpx_t *best_sym = NULL, *cur_sym = NULL;
    double *pw;
    double sumsq = 0.0, pmax = 0.0, act_thresh;
    float scale;
    double baud_period = (double) NF_SAMPLE_RATE / NF_V34_CTRL_BAUD;
    int a0, a1;
    long nact;
    int best_n = 0, ph;
    double best_q = 1e18;
    double df;

    if (n <= NF_V34_CC_BATCH_TAPS * 2)
        return;

    nf_rrc_design((double) NF_V34_CTRL_BAUD / NF_SAMPLE_RATE, 0.3, 1, NF_V34_CC_BATCH_TAPS, 0.0, rrc, NULL);

    bb = malloc(sizeof(nf_cpx_t) * (size_t) n);
    filt = malloc(sizeof(nf_cpx_t) * (size_t) n);
    pw = malloc(sizeof(double) * (size_t) n);
    if (!bb || !filt || !pw) { free(bb); free(filt); free(pw); return; }

    for (i = 0; i < n; i++) {
        nf_cpx_t lo = nf_dds_cpx(lo_phase);
        lo_phase += (uint32_t) lo_rate;
        bb[i] = nf_cpx((float) amp[i] * lo.re, -(float) amp[i] * lo.im);
    }
    for (i = 0; i < n; i++) {
        nf_cpx_t acc = nf_cpx(0.0f, 0.0f);
        for (k = 0; k < NF_V34_CC_BATCH_TAPS; k++) {
            int p = i - k;
            if (p < 0) continue;
            acc.re += rrc[k] * bb[p].re;
            acc.im += rrc[k] * bb[p].im;
        }
        filt[i] = acc;
    }
    free(bb);

    /* Active-region detection (smoothed power vs -12 dB of peak): the
     * energy-gated burst buffers carry silence/noise head and tail room,
     * and normalizing over THOSE made the effective Gardner loop gain (its
     * error term is amplitude-squared) depend on how much padding the gate
     * happened to include. Static RMS normalization stays (see nf_v34.h),
     * but computed over the signal, not the padding. */
    {
        double p = 0.0;
        for (i = 0; i < n; i++) {
            double x = (double) filt[i].re * filt[i].re +
                       (double) filt[i].im * filt[i].im;
            p += (x - p) / 32.0;
            pw[i] = p;
            if (p > pmax)
                pmax = p;
        }
    }
    act_thresh = pmax / 16.0;
    a0 = 0;
    while (a0 < n && pw[a0] < act_thresh)
        a0++;
    a1 = n - 1;
    while (a1 > a0 && pw[a1] < act_thresh)
        a1--;
    a1++;
    nact = a1 - a0;
    if (nact < (long) (baud_period * 40.0)) {   /* too short for a burst */
        free(filt); free(pw);
        return;
    }
    sumsq = 0.0;
    for (i = a0; i < a1; i++)
        sumsq += (double) filt[i].re * filt[i].re + (double) filt[i].im * filt[i].im;
    scale = (sumsq > 0.0) ? (float) (1.0 / sqrt(sumsq / (double) nact)) : 1.0f;
    for (i = 0; i < n; i++) {
        filt[i].re *= scale;
        filt[i].im *= scale;
    }
    free(pw);

    /* Carrier-frequency offset estimate: the 4th power of a QPSK baseband
     * collapses the modulation into a spectral line at 4x the residual
     * carrier offset. Coarse/fine peak search over +-15 Hz of offset -
     * V.34 requires tolerating +-7 Hz of line frequency shift, far beyond
     * the 4th-power Costas loop's own pull-in at these loop gains. */
    {
        double best_p = -1.0, best_f4 = 0.0, f4, span = 60.0;
        int pass;
        /* CFO estimate window: the whole active region by default, or just the
         * training prefix for a 2400-mode burst (see the header note). */
        long a1c = a1;
        if (cfo_max_sym > 0) {
            long w = a0 + (long) ((double) cfo_max_sym * baud_period);
            if (w < a1c)
                a1c = w;
        }

        for (pass = 0; pass < 2; pass++) {
            double f_lo = pass ? best_f4 - 2.0 : -span;
            double f_hi = pass ? best_f4 + 2.0 : span;
            double f_step = pass ? 0.2 : 1.5;

            for (f4 = f_lo; f4 <= f_hi + 1e-9; f4 += f_step) {
                double cr = 0.0, ci = 0.0, p;
                for (i = a0; i < a1c; i++) {
                    nf_cpx_t z2 = nf_cpx_mul(filt[i], filt[i]);
                    nf_cpx_t z4 = nf_cpx_mul(z2, z2);
                    double phr = -2.0 * M_PI * f4 * (double) i / NF_SAMPLE_RATE;
                    cr += z4.re * cos(phr) - z4.im * sin(phr);
                    ci += z4.re * sin(phr) + z4.im * cos(phr);
                }
                p = cr * cr + ci * ci;
                if (p > best_p) { best_p = p; best_f4 = f4; }
            }
        }
        df = best_f4 / 4.0;
    }
    if (getenv("NFV34CCDBG"))
        fprintf(stderr, "[ccrx] n=%d act=[%d,%d) df=%.2f\n", n, a0, a1, df);
    if (fabs(df) > 0.05) {
        for (i = 0; i < n; i++) {
            double phr = -2.0 * M_PI * df * (double) i / NF_SAMPLE_RATE;
            float c = (float) cos(phr), s = (float) sin(phr);
            float re = filt[i].re, im = filt[i].im;
            filt[i].re = re * c - im * s;
            filt[i].im = re * s + im * c;
        }
    }

    /* Timing acquisition: run the tracking pass from four starting phases
     * a quarter symbol apart and keep the best-locked one (see
     * v34_cc_track_pass's comment for why one start is not enough). */
    best_sym = malloc(sizeof(nf_cpx_t) * ((size_t) (n / baud_period) + 4));
    cur_sym = malloc(sizeof(nf_cpx_t) * ((size_t) (n / baud_period) + 4));
    if (!best_sym || !cur_sym) {
        free(best_sym); free(cur_sym); free(filt);
        return;
    }
    /* track_pass counts symbols from filt[0], which precedes the active region
     * by ~a0/baud_period symbols of gated-in silence; offset the training-only
     * freeze point (cfo_max_sym counts from a0) by that lead. */
    {
    int freeze_sym = cfo_max_sym > 0 ?
        (int) ((double) a0 / baud_period) + cfo_max_sym : 0;
    for (ph = 0; ph < 4; ph++) {
        double q = 1e18;
        int ns = v34_cc_track_pass(filt, n, (double) ph * baud_period / 4.0,
                                   cur_sym, &q, freeze_sym);
        if (getenv("NFV34CCDBG"))
            fprintf(stderr, "[ccrx] phase %d: q=%.4f ns=%d\n", ph, q, ns);
        if (q < best_q) {
            nf_cpx_t *t = best_sym;
            best_q = q;
            best_n = ns;
            best_sym = cur_sym;
            cur_sym = t;
        }
    }
    }

    if (on_symbol) {
        for (i = 0; i < best_n; i++)
            on_symbol(user, &best_sym[i]);
    }

    free(best_sym);
    free(cur_sym);
    free(filt);
}

/* ── shell mapper (ITU-T V.34 (02/98) 9.4, equations 9-3 to 9-24) ──────── */

/* g2/g4/g8/z8 are recursively defined convolutions (g4 is g2 self-convolved,
 * g8 is g4 self-convolved, z8 is g8's running sum) - computing them by naive
 * recursion re-derives the whole chain from scratch on every call, which is
 * exponential-ish and hangs for anything but the smallest M (found by the
 * `shellmap` self-test itself timing out). Precompute each table bottom-up,
 * once per M, and cache the single most-recently-used M (a real decoder
 * only ever uses one M per connection, so this is effectively O(1) amortized
 * per shell_map/unmap call). */
#define SHELL_MAXM 64
typedef struct {
    int  M;
    int  g2[2*SHELL_MAXM];
    long g4[4*SHELL_MAXM];
    long g8[8*SHELL_MAXM];
    long z8[8*SHELL_MAXM + 1];
} shell_tables_t;

static shell_tables_t shell_cache;

static const shell_tables_t *shell_get_tables(int M)
{
    int p, k, n2, n4, n8;

    if (shell_cache.M == M)
        return &shell_cache;

    n2 = 2*(M-1); n4 = 4*(M-1); n8 = 8*(M-1);
    for (p = 0; p <= n2 && p < 2*SHELL_MAXM; p++)
        shell_cache.g2[p] = M - abs(p - M + 1);
    for (p = n2+1; p < 2*SHELL_MAXM; p++)
        shell_cache.g2[p] = 0;

    for (p = 0; p < 4*SHELL_MAXM; p++) {
        long s = 0;
        if (p <= n4)
            for (k = 0; k <= p; k++)
                s += (long) shell_cache.g2[k] * shell_cache.g2[p-k];
        shell_cache.g4[p] = s;
    }

    for (p = 0; p < 8*SHELL_MAXM; p++) {
        long s = 0;
        if (p <= n8)
            for (k = 0; k <= p; k++)
                s += shell_cache.g4[k] * shell_cache.g4[p-k];
        shell_cache.g8[p] = s;
    }

    shell_cache.z8[0] = 0;
    for (p = 1; p <= 8*SHELL_MAXM; p++)
        shell_cache.z8[p] = shell_cache.z8[p-1] + shell_cache.g8[p-1];

    shell_cache.M = M;
    return &shell_cache;
}

static int shell_g2(const shell_tables_t *t, int p, int M)
{
    if (p < 0 || p > 2*(M-1) || p >= 2*SHELL_MAXM) return 0;
    return t->g2[p];
}
static long shell_g4(const shell_tables_t *t, int p, int M)
{
    if (p < 0 || p > 4*(M-1) || p >= 4*SHELL_MAXM) return 0;
    return t->g4[p];
}
static long shell_z8(const shell_tables_t *t, int p, int M)
{
    (void) M;
    if (p < 0) return 0;
    if (p > 8*SHELL_MAXM) p = 8*SHELL_MAXM;
    return t->z8[p];
}

void nf_v34_shell_map(int M, uint32_t R0_in, int rings[4][2])
{
    const shell_tables_t *t = shell_get_tables(M);
    long R0 = (long) R0_in;
    int A, B, C, D;
    long R1, R2, R3, R4, R5, base, cand, ssum;
    int g4B, g2C, g2D, E, F, G, H, BC, ABD;

    A = 0;
    /* A can never legitimately exceed 8*(M-1) (z8 is flat beyond that) - a
     * caller passing an R0 outside the valid [0, 2^K) range for this M would
     * otherwise spin this loop forever, since z8() plateaus. */
    while (A < 8*(M-1) && shell_z8(t, A+1, M) <= R0)
        A++;

    base = R0 - shell_z8(t, A, M);
    B = 0;
    R1 = base;
    for (;;) {
        if (B+1 > A)
            break;
        ssum = 0;
        { int p; for (p = 0; p <= B; p++) ssum += shell_g4(t, p, M) * shell_g4(t, A-p, M); }
        cand = base - ssum;
        if (cand >= 0) { B++; R1 = cand; } else break;
    }

    g4B = (int) shell_g4(t, B, M);
    R2 = R1 % g4B;
    R3 = (R1 - R2) / g4B;

    C = 0; R4 = R2;
    for (;;) {
        if (C+1 > B) break;
        ssum = 0;
        { int p; for (p = 0; p <= C; p++) ssum += (long) shell_g2(t, p, M) * shell_g2(t, B-p, M); }
        cand = R2 - ssum;
        if (cand >= 0) { C++; R4 = cand; } else break;
    }

    D = 0; R5 = R3;
    for (;;) {
        if (D+1 > (A-B)) break;
        ssum = 0;
        { int p; for (p = 0; p <= D; p++) ssum += (long) shell_g2(t, p, M) * shell_g2(t, A-B-p, M); }
        cand = R3 - ssum;
        if (cand >= 0) { D++; R5 = cand; } else break;
    }

    g2C = shell_g2(t, C, M);
    g2D = shell_g2(t, D, M);
    E = (int) (R4 % g2C);
    F = (int) ((R4 - E) / g2C);
    G = (int) (R5 % g2D);
    H = (int) ((R5 - G) / g2D);

    if (C < M) {
        rings[0][0] = E; rings[0][1] = C - E;
    } else {
        rings[0][1] = M - 1 - E; rings[0][0] = C - rings[0][1];
    }
    BC = B - C;
    if (BC < M) {
        rings[1][0] = F; rings[1][1] = BC - F;
    } else {
        rings[1][1] = M - 1 - F; rings[1][0] = BC - rings[1][1];
    }
    if (D < M) {
        rings[2][0] = G; rings[2][1] = D - G;
    } else {
        rings[2][1] = M - 1 - G; rings[2][0] = D - rings[2][1];
    }
    ABD = A - B - D;
    if (ABD < M) {
        rings[3][0] = H; rings[3][1] = ABD - H;
    } else {
        rings[3][1] = M - 1 - H; rings[3][0] = ABD - rings[3][1];
    }
}

uint32_t nf_v34_shell_unmap(int M, const int rings[4][2])
{
    const shell_tables_t *t = shell_get_tables(M);
    int C, BC, D, ABD, E, F, G, H, B, A, g2C, g2D, g4B;
    long R4, R5, R2, R3, R1, R0, ssum;
    int p;

    C = rings[0][0] + rings[0][1];
    E = (C < M) ? rings[0][0] : (M - 1 - rings[0][1]);
    BC = rings[1][0] + rings[1][1];
    F = (BC < M) ? rings[1][0] : (M - 1 - rings[1][1]);
    D = rings[2][0] + rings[2][1];
    G = (D < M) ? rings[2][0] : (M - 1 - rings[2][1]);
    ABD = rings[3][0] + rings[3][1];
    H = (ABD < M) ? rings[3][0] : (M - 1 - rings[3][1]);

    B = BC + C;
    A = ABD + B + D;

    g2C = shell_g2(t, C, M);
    g2D = shell_g2(t, D, M);
    R4 = (long) F * g2C + E;
    R5 = (long) H * g2D + G;

    ssum = 0;
    for (p = 0; p < C; p++) ssum += (long) shell_g2(t, p, M) * shell_g2(t, B-p, M);
    R2 = R4 + ssum;

    ssum = 0;
    for (p = 0; p < D; p++) ssum += (long) shell_g2(t, p, M) * shell_g2(t, A-B-p, M);
    R3 = R5 + ssum;

    g4B = (int) shell_g4(t, B, M);
    R1 = R3 * g4B + R2;

    ssum = 0;
    for (p = 0; p < B; p++) ssum += shell_g4(t, p, M) * shell_g4(t, A-p, M);
    R0 = R1 + shell_z8(t, A, M) + ssum;

    return (uint32_t) R0;
}

/* ── trellis encoder building blocks (9.6.3) - see nf_v34.h for status ── */

/* Table 13/V.34, transcribed directly from the recommendation and
 * self-consistency-checked (rows 100/101/110/111 must equal rows
 * 000/001/010/011 with their first-4/last-4 columns swapped, matching the
 * Y4 = XOR-of-MSBs structure visible in the table - verified before use). */
const uint8_t nf_v34_table13[8][8] = {
    { 0x0, 0x0, 0x1, 0x1, 0x8, 0x8, 0x9, 0x9 },
    { 0x3, 0x2, 0x2, 0x3, 0xB, 0xA, 0xA, 0xB },
    { 0x5, 0x5, 0x4, 0x4, 0xD, 0xD, 0xC, 0xC },
    { 0x6, 0x7, 0x7, 0x6, 0xE, 0xF, 0xF, 0xE },
    { 0x8, 0x8, 0x9, 0x9, 0x0, 0x0, 0x1, 0x1 },
    { 0xB, 0xA, 0xA, 0xB, 0x3, 0x2, 0x2, 0x3 },
    { 0xD, 0xD, 0xC, 0xC, 0x5, 0x5, 0x4, 0x4 },
    { 0xE, 0xF, 0xF, 0xE, 0x6, 0x7, 0x7, 0x6 },
};

int nf_v34_trellis_step(int state, int y2, int y1, int *y0)
{
    int t1 = state & 1;
    int t2 = (state >> 1) & 1;
    int t3 = (state >> 2) & 1;
    int t4 = (state >> 3) & 1;
    int nt1 = t2 ^ y1;
    int nt2 = t3 ^ y2;
    int nt3 = t4 ^ t1 ^ y2;
    int nt4 = t1;

    if (y0)
        *y0 = t1;
    return nt1 | (nt2 << 1) | (nt3 << 2) | (nt4 << 3);
}

/* ── Viterbi decoder over the trellis (corrected U0/parity model) ──────
 * See nf_v34.h for the full story of why this is built around rotation
 * parity rather than (Y2,Y1) value equality. */

/* Full ITU-T V.34 Figure 5 quarter-superconstellation, labels 0..415 -
 * mechanically verified against the recommendation's own magnitude-ordering
 * rule (9.1) with zero violations; see nf_v34_superconstellation.h. Derived
 * once from that header's int8_t table rather than duplicated by hand. */
nf_v34_ipoint_t nf_v34_quarter_table[NF_V34_QUARTER_MAX];
static int v34_quarter_table_ready = 0;

static void v34_quarter_table_ensure(void)
{
    int i;
    if (v34_quarter_table_ready)
        return;
    for (i = 0; i < NF_V34_QUARTER_MAX; i++) {
        nf_v34_quarter_table[i].re = nf_v34_quarter_const[i][0];
        nf_v34_quarter_table[i].im = nf_v34_quarter_const[i][1];
    }
    v34_quarter_table_ready = 1;
}

static void v34_rotate_cw(int *re, int *im, int steps)
{
    int i, r = *re, m = *im;
    steps = ((steps % 4) + 4) % 4;
    for (i = 0; i < steps; i++) {
        int nr = m, nm = -r;
        r = nr; m = nm;
    }
    *re = r; *im = m;
}

void nf_v34_constellation_init(nf_v34_constellation_t *c, const nf_v34_ipoint_t *quarter, int M)
{
    int k, rot;

    v34_quarter_table_ensure();
    if (M > NF_V34_QUARTER_MAX)
        M = NF_V34_QUARTER_MAX;
    c->M = M;
    c->nalpha = M * 4;
    for (k = 0; k < M; k++) {
        for (rot = 0; rot < 4; rot++) {
            int re = quarter[k].re, im = quarter[k].im;
            v34_rotate_cw(&re, &im, rot);
            c->alphabet[k*4 + rot].re = re;
            c->alphabet[k*4 + rot].im = im;
            c->alpha_rot[k*4 + rot] = rot;
        }
    }
}

static float v34_dist2(nf_cpx_t p, nf_v34_ipoint_t q)
{
    float dr = p.re - (float) q.re;
    float di = p.im - (float) q.im;
    return dr*dr + di*di;
}

/* top-k nearest alphabet points to `target`, by insertion into a small
 * fixed-size sorted list (k is always small - a handful - so this is
 * cheaper and simpler than a real sort for the sizes involved here). */
#define V34_VIT_MAXKEEP 8

typedef struct {
    int idx[V34_VIT_MAXKEEP];
    float d[V34_VIT_MAXKEEP];
    int n;
} v34_topk_t;

static void v34_topk_reset(v34_topk_t *t) { t->n = 0; }

static void v34_topk_insert(v34_topk_t *t, int idx, float d, int maxk)
{
    int i, j;
    if (t->n < maxk) {
        i = t->n++;
    } else if (d < t->d[t->n-1]) {
        i = t->n - 1;
    } else {
        return;
    }
    for (j = i; j > 0 && t->d[j-1] > d; j--) {
        t->d[j] = t->d[j-1];
        t->idx[j] = t->idx[j-1];
    }
    t->d[j] = d;
    t->idx[j] = idx;
}

int nf_v34_viterbi_decode(const nf_v34_constellation_t *c,
                           const nf_cpx_t *noisy0, const nf_cpx_t *noisy1, int n,
                           nf_cpx_t *decoded0, nf_cpx_t *decoded1,
                           int keep, int keep0)
{
    const float INF = 1e18f;
    float *path_metric;
    int *rec_prev;             /* [n][NSTATES] */
    nf_v34_ipoint_t *rec_p0, *rec_p1;  /* [n][NSTATES] */
    int m, i, st;

    if (n <= 0 || keep <= 0 || keep0 <= 0 || keep > V34_VIT_MAXKEEP || keep0 > V34_VIT_MAXKEEP)
        return -1;

    path_metric = malloc(sizeof(float) * NF_V34_NSTATES);
    rec_prev = malloc(sizeof(int) * (size_t) n * NF_V34_NSTATES);
    rec_p0 = malloc(sizeof(nf_v34_ipoint_t) * (size_t) n * NF_V34_NSTATES);
    rec_p1 = malloc(sizeof(nf_v34_ipoint_t) * (size_t) n * NF_V34_NSTATES);
    if (!path_metric || !rec_prev || !rec_p0 || !rec_p1) {
        free(path_metric); free(rec_prev); free(rec_p0); free(rec_p1);
        return -1;
    }
    for (st = 0; st < NF_V34_NSTATES; st++)
        path_metric[st] = 0.0f;

    for (m = 0; m < n; m++) {
        v34_topk_t sym1;
        float new_path[NF_V34_NSTATES];
        int new_prev[NF_V34_NSTATES];
        nf_v34_ipoint_t new_p0[NF_V34_NSTATES], new_p1[NF_V34_NSTATES];
        float mn;

        /* top-k symbol-1 hypotheses (index into c->alphabet) */
        v34_topk_reset(&sym1);
        for (i = 0; i < c->nalpha; i++)
            v34_topk_insert(&sym1, i, v34_dist2(noisy0[m], c->alphabet[i]), keep0);

        for (st = 0; st < NF_V34_NSTATES; st++) {
            new_path[st] = INF;
            new_prev[st] = -1;
        }

        for (st = 0; st < NF_V34_NSTATES; st++) {
            int U0;
            int h;

            if (path_metric[st] >= INF)
                continue;
            U0 = st & 1;

            for (h = 0; h < sym1.n; h++) {
                int a0 = sym1.idx[h];
                float d0 = sym1.d[h];
                int Zhat = c->alpha_rot[a0];
                int s0 = nf_v34_subset_label(c->alphabet[a0].re, c->alphabet[a0].im);
                v34_topk_t sym2;
                int j;

                /* symbol-2 candidates restricted to the rotation parity
                 * consistent with U0 (see nf_v34.h for why this, not the
                 * naive Y2/Y1-value model, is the real constraint). */
                v34_topk_reset(&sym2);
                for (i = 0; i < c->nalpha; i++) {
                    int parity = ((c->alpha_rot[i] - Zhat) % 4 + 4) % 4 % 2;
                    if (parity == U0)
                        v34_topk_insert(&sym2, i, v34_dist2(noisy1[m], c->alphabet[i]), keep);
                }

                for (j = 0; j < sym2.n; j++) {
                    int a1 = sym2.idx[j];
                    float d1 = sym2.d[j];
                    int s1 = nf_v34_subset_label(c->alphabet[a1].re, c->alphabet[a1].im);
                    int y4321 = nf_v34_table13[s0][s1];
                    int y1b = y4321 & 1;
                    int y2b = (y4321 >> 1) & 1;
                    int nxt = nf_v34_trellis_step(st, y2b, y1b, NULL);
                    float cost = path_metric[st] + d0 + d1;

                    if (cost < new_path[nxt]) {
                        new_path[nxt] = cost;
                        new_prev[nxt] = st;
                        new_p0[nxt] = c->alphabet[a0];
                        new_p1[nxt] = c->alphabet[a1];
                    }
                }
            }
        }

        mn = INF;
        for (st = 0; st < NF_V34_NSTATES; st++)
            if (new_path[st] < mn) mn = new_path[st];
        for (st = 0; st < NF_V34_NSTATES; st++) {
            path_metric[st] = (new_path[st] < INF) ? new_path[st] - mn : INF;
            rec_prev[m*NF_V34_NSTATES + st] = new_prev[st];
            rec_p0[m*NF_V34_NSTATES + st] = new_p0[st];
            rec_p1[m*NF_V34_NSTATES + st] = new_p1[st];
        }
    }

    {
        int final_state = 0;
        float best = path_metric[0];
        int *states = malloc(sizeof(int) * (size_t) n);

        if (!states) {
            free(path_metric); free(rec_prev); free(rec_p0); free(rec_p1);
            return -1;
        }
        for (st = 1; st < NF_V34_NSTATES; st++) {
            if (path_metric[st] < best) { best = path_metric[st]; final_state = st; }
        }
        states[n-1] = final_state;
        for (m = n-1; m > 0; m--) {
            int prev = rec_prev[m*NF_V34_NSTATES + states[m]];
            states[m-1] = (prev >= 0) ? prev : 0;
        }
        for (m = 0; m < n; m++) {
            nf_v34_ipoint_t p0 = rec_p0[m*NF_V34_NSTATES + states[m]];
            nf_v34_ipoint_t p1 = rec_p1[m*NF_V34_NSTATES + states[m]];
            decoded0[m] = nf_cpx((float) p0.re, (float) p0.im);
            decoded1[m] = nf_cpx((float) p1.re, (float) p1.im);
        }
        free(states);
    }

    free(path_metric); free(rec_prev); free(rec_p0); free(rec_p1);
    return 0;
}

/* ── scrambler / descrambler (clause 7) ─────────────────────────────── */

void nf_v34_scrambler_init(nf_v34_scrambler_t *s, int is_call_modem)
{
    memset(s->hist, 0, sizeof(s->hist));
    s->pos = 0;
    if (is_call_modem) { s->tap_a = 18; s->tap_b = 23; }
    else               { s->tap_a = 5;  s->tap_b = 23; }
}

static int v34_hist_get(const nf_v34_scrambler_t *s, int delay)
{
    return s->hist[(s->pos - delay + 32*4) % 32];
}

int nf_v34_scramble_bit(nf_v34_scrambler_t *s, int bit)
{
    int v = (bit ^ v34_hist_get(s, s->tap_a) ^ v34_hist_get(s, s->tap_b)) & 1;
    s->hist[s->pos % 32] = (uint8_t) v;
    s->pos++;
    return v;
}

int nf_v34_descramble_bit(nf_v34_scrambler_t *s, int scrambled_bit)
{
    int v = (scrambled_bit ^ v34_hist_get(s, s->tap_a) ^ v34_hist_get(s, s->tap_b)) & 1;
    s->hist[s->pos % 32] = (uint8_t) (scrambled_bit & 1);
    s->pos++;
    return v;
}

/* ── mapping-frame receiver (9.5 differential decode + 9.4 shell-unmap) ── */

static int v34_alphabet_lookup(const nf_v34_constellation_t *c, int re, int im, int *label, int *rot)
{
    int i;
    for (i = 0; i < c->nalpha; i++) {
        if (c->alphabet[i].re == re && c->alphabet[i].im == im) {
            *label = i / 4;
            *rot = c->alpha_rot[i];
            return 0;
        }
    }
    return -1;
}

int nf_v34_rx_frame(nf_v34_rx_frame_state_t *s, const nf_v34_constellation_t *c,
                     int M, int K, int q,
                     const nf_cpx_t point0[4], const nf_cpx_t point1[4],
                     uint32_t *R0_out, int *aux)
{
    int rings[4][2];
    int per_group = 3 + 2*q;
    int j;

    (void) K;
    for (j = 0; j < 4; j++) {
        int re0 = (int) point0[j].re, im0 = (int) point0[j].im;
        int re1 = (int) point1[j].re, im1 = (int) point1[j].im;
        int label0, rot0, label1, rot1;
        int Z, U0, delta, i1, ival, i2, i3;
        int s0, s1, y4321, y1b, y2b, qi, base;

        if (v34_alphabet_lookup(c, re0, im0, &label0, &rot0) < 0) return -1;
        if (v34_alphabet_lookup(c, re1, im1, &label1, &rot1) < 0) return -1;

        Z = rot0;
        U0 = s->state & 1;
        delta = ((rot1 - Z - U0) % 4 + 4) % 4;
        i1 = (delta / 2) % 2;
        ival = ((Z - s->zprev) % 4 + 4) % 4;
        i2 = ival & 1;
        i3 = (ival >> 1) & 1;

        /* label0/label1 pack Q(n) = raw-Q-bits + 2^q * ring (9-26); split
         * them back into the shell mapper's ring index and the raw bits
         * carried alongside it (Q_1 = LSB first). */
        rings[j][0] = label0 >> q;
        rings[j][1] = label1 >> q;

        base = j * per_group;
        aux[base+0] = i1; aux[base+1] = i2; aux[base+2] = i3;
        for (qi = 0; qi < q; qi++)
            aux[base+3+qi] = (label0 >> qi) & 1;
        for (qi = 0; qi < q; qi++)
            aux[base+3+q+qi] = (label1 >> qi) & 1;

        s0 = nf_v34_subset_label(re0, im0);
        s1 = nf_v34_subset_label(re1, im1);
        y4321 = nf_v34_table13[s0][s1];
        y1b = y4321 & 1; y2b = (y4321 >> 1) & 1;
        s->state = nf_v34_trellis_step(s->state, y2b, y1b, NULL);
        s->zprev = Z;
    }

    *R0_out = nf_v34_shell_unmap(M, rings);
    return 0;
}

/* ── control-channel MP frame decoder (Annex A / 10.2.4) - see nf_v34.h ── */

static int v34_mp_quadrant(const nf_cpx_t *z)
{
    if (z->re >= 0.0f && z->im >= 0.0f) return 0;
    if (z->re <  0.0f && z->im >= 0.0f) return 1;
    if (z->re <  0.0f && z->im <  0.0f) return 2;
    return 3;
}

static uint16_t v34_mp_crc_update(uint16_t crc, int bit)
{
    int lsb = crc & 1;
    crc >>= 1;
    if (lsb ^ (bit & 1))
        crc ^= 0x8408;
    return crc;
}

void nf_v34_mp_rx_init(nf_v34_mp_rx_t *s, int is_call_modem)
{
    memset(s, 0, sizeof(*s));
    nf_v34_scrambler_init(&s->descr, is_call_modem);
    s->pos = -1;
}

/* Header/mask fields shared by Type 0 (Table 20/23) and Type 1 (Table
 * 21/24) - identical buf positions in both. Bits 24:27 (buf[6..9]) are
 * parsed BOTH ways: as the duplex tables' max_rate_a2c and as the
 * half-duplex tables' reserved+cc_rate (see nf_v34.h). */
static void v34_mp_parse_common(nf_v34_mp_rx_t *s)
{
    int i;

    s->type          = s->buf[0];
    s->max_rate_c2a   = (s->buf[2]) | (s->buf[3]<<1) | (s->buf[4]<<2) | (s->buf[5]<<3);
    s->max_rate_a2c   = (s->buf[6]) | (s->buf[7]<<1) | (s->buf[8]<<2) | (s->buf[9]<<3);
    s->cc_rate       = s->buf[9];
    s->aux_channel   = s->buf[10];
    s->trellis_size  = (s->buf[11]) | (s->buf[12]<<1);
    s->nonlinear     = s->buf[13];
    s->shaping       = s->buf[14];
    s->ack           = s->buf[15];
    s->rate_mask = 0;
    for (i = 0; i < 15; i++) s->rate_mask |= s->buf[17+i] << i;
    s->asym_enable   = s->buf[32];
}

/* 16-bit LSB-first two's-complement field (the Type-1 Q14 precoder taps) */
static int16_t v34_mp_q14(const uint8_t *b)
{
    uint16_t v = 0;
    int i;

    for (i = 0; i < 16; i++)
        v |= (uint16_t) (b[i] & 1) << i;
    return (int16_t) v;
}

static int v34_mp_feed_bit(nf_v34_mp_rx_t *s, int raw_bit)
{
    int bit = nf_v34_descramble_bit(&s->descr, raw_bit);

    s->have_frame = 0;
    if (s->pos < 0) {
        if (bit) {
            s->ones_run++;
        } else {
            if (s->ones_run >= 17)
                s->pos = 0;
            s->ones_run = 0;
        }
        return 0;
    }

    s->buf[s->pos] = (uint8_t) bit;
    s->pos++;

    if (s->pos == 70 && s->buf[0] == 0) {
        /* Type 0: data groups buf[0:15], buf[17:32], buf[34:49] (start bits
         * 16/33/50 skipped), CRC field buf[51:66]. */
        int i;
        uint16_t crc = 0xFFFF;
        int crc_val;

        for (i = 0;  i <= 15; i++) crc = v34_mp_crc_update(crc, s->buf[i]);
        for (i = 17; i <= 32; i++) crc = v34_mp_crc_update(crc, s->buf[i]);
        for (i = 34; i <= 49; i++) crc = v34_mp_crc_update(crc, s->buf[i]);

        if (s->buf[16] == 0 && s->buf[33] == 0 && s->buf[50] == 0) {
            crc_val = 0;
            for (i = 0; i < 16; i++) crc_val |= s->buf[51+i] << i;
            if (crc_val == crc) {
                v34_mp_parse_common(s);
                s->have_frame = 1;
            }
        }
        /* whether or not this was a real frame, resume searching for sync -
         * a real MP/MP' repeat will find the next 17-ones+0 right away. */
        s->pos = -1;
        s->ones_run = 0;
    } else if (s->pos >= 170) {
        /* Type 1 (buf[0] == 1): nine 16-bit data groups every 17 bits
         * (start bits 16/33/.../152 skipped from the CRC, same pattern as
         * Type 0 continued), CRC field buf[153:168], fill bit buf[169].
         * Groups buf[34..151] carry the precoder taps h1..h3 re/im. */
        static const int t1_starts[9] = { 16, 33, 50, 67, 84, 101, 118, 135, 152 };
        int i, g, starts_ok = 1;
        uint16_t crc = 0xFFFF;
        int crc_val;

        for (g = 0; g < 9; g++) {
            if (s->buf[t1_starts[g]] != 0)
                starts_ok = 0;
            for (i = g*17; i <= g*17 + 15; i++)
                crc = v34_mp_crc_update(crc, s->buf[i]);
        }

        if (s->buf[0] == 1 && starts_ok && s->buf[169] == 0) {
            crc_val = 0;
            for (i = 0; i < 16; i++) crc_val |= s->buf[153+i] << i;
            if (crc_val == crc) {
                v34_mp_parse_common(s);
                s->h_re[0] = v34_mp_q14(&s->buf[34]);
                s->h_im[0] = v34_mp_q14(&s->buf[51]);
                s->h_re[1] = v34_mp_q14(&s->buf[68]);
                s->h_im[1] = v34_mp_q14(&s->buf[85]);
                s->h_re[2] = v34_mp_q14(&s->buf[102]);
                s->h_im[2] = v34_mp_q14(&s->buf[119]);
                s->have_frame = 1;
            }
        }
        s->pos = -1;
        s->ones_run = 0;
    }
    return s->have_frame;
}

int nf_v34_mp_feed_symbol(nf_v34_mp_rx_t *s, const nf_cpx_t *z)
{
    int quad = v34_mp_quadrant(z);
    int delta, b0, b1, r0, r1;

    if (!s->have_prev_quad) {
        s->prev_quad = quad;
        s->have_prev_quad = 1;
        return 0;
    }
    delta = ((-(quad - s->prev_quad)) % 4 + 4) % 4;
    s->prev_quad = quad;

    b0 = delta & 1;
    b1 = (delta >> 1) & 1;
    r0 = v34_mp_feed_bit(s, b0);
    r1 = v34_mp_feed_bit(s, b1);
    return r0 || r1;
}

/* ── Phase-2 INFO-sequence decoder (INFO0/INFOh) - see nf_v34.h ────────── */

#define V34_INFO_LP_TAPS  161      /* Hamming-windowed sinc, ~250 Hz cutoff */
#define V34_INFO_LP_CUT   250.0
#define V34_INFO_SYNC_LEN 12       /* 1111 fill + 01110010 sync */

/* fill + sync, first bit in time first */
static const uint8_t v34_info_sync[V34_INFO_SYNC_LEN] =
    { 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0 };

static uint16_t v34_info_crc_bits(const uint8_t *bits, int n)
{
    uint16_t crc = 0xFFFF;
    int i;

    for (i = 0; i < n; i++)
        crc = v34_mp_crc_update(crc, bits[i]);
    return crc;
}

/* b = frame start (first fill bit), nbits = bits available from b onward.
 * Returns 1 and fills *f (except f->t) on a CRC-valid frame. The trailing
 * fill bits are NOT required - see nf_v34.h. */
static int v34_info_parse(const uint8_t *b, int nbits, int is_infoh,
                           nf_v34_info_frame_t *f)
{
    int info_len = is_infoh ? 19 : 17;       /* INFO0 bits 12:28, INFOh 12:30 */
    int crc_at = 12 + info_len;
    uint16_t crc_rx = 0;
    int i;

    if (nbits < crc_at + 16)
        return 0;
    for (i = 0; i < V34_INFO_SYNC_LEN; i++)
        if (b[i] != v34_info_sync[i])
            return 0;
    for (i = 0; i < 16; i++)
        crc_rx |= (uint16_t) (b[crc_at + i] & 1) << i;
    if (v34_info_crc_bits(b + 12, info_len) != crc_rx)
        return 0;

    memset(f, 0, sizeof(*f));
    f->is_infoh = is_infoh;
    if (is_infoh) {
        f->power_reduction = b[12] | (b[13] << 1) | (b[14] << 2);
        f->trn_len = b[15] | (b[16] << 1) | (b[17] << 2) | (b[18] << 3)
                   | (b[19] << 4) | (b[20] << 5) | (b[21] << 6);
        f->high_carrier = b[22];
        f->preemph_idx = b[23] | (b[24] << 1) | (b[25] << 2) | (b[26] << 3);
        f->symrate_idx = b[27] | (b[28] << 1) | (b[29] << 2);
        f->trn_16pt = b[30];
    } else {
        f->sr2743 = b[12];
        f->sr2800 = b[13];
        f->sr3429 = b[14];
        f->low3000 = b[15];
        f->high3000 = b[16];
        f->low3200 = b[17];
        f->high3200 = b[18];
        f->allow_3429 = b[19];
        f->can_reduce_power = b[20];
        f->max_sr_diff = b[21] | (b[22] << 1) | (b[23] << 2);
        f->from_cme = b[24];
        f->support_1664pt = b[25];
        f->clock_source = b[26] | (b[27] << 1);
        f->info0_ack = b[28];
    }
    return 1;
}

int nf_v34_info_rx_batch(const int16_t *amp, int n, double carrier_hz,
                          nf_v34_info_frame_t *out, int max_frames)
{
    float lp[V34_INFO_LP_TAPS];
    nf_cpx_t *bb, *filt;
    uint8_t *bits;
    double *tpos;
    uint32_t lo_phase = 0;
    int32_t lo_rate = nf_dds_phase_rate(carrier_hz);
    double sps = (double) NF_SAMPLE_RATE / NF_V34_CTRL_BAUD;
    int i, k, pol, off_i, nfound = 0;
    double sum = 0.0;

    if (n <= V34_INFO_LP_TAPS || max_frames <= 0)
        return 0;

    /* windowed-sinc lowpass, unit DC gain (a plain lowpass, not a matched
     * filter - the INFO waveform is binary DPSK, all we need is to reject
     * the answer channel's 1800 Hz guard tone after downconversion) */
    for (i = 0; i < V34_INFO_LP_TAPS; i++) {
        double x = 2.0 * V34_INFO_LP_CUT / NF_SAMPLE_RATE * (i - (V34_INFO_LP_TAPS - 1) / 2.0);
        double sinc = (fabs(x) < 1e-12) ? 1.0 : sin(M_PI * x) / (M_PI * x);
        lp[i] = (float) (sinc * (0.54 - 0.46 * cos(2.0 * M_PI * i / (V34_INFO_LP_TAPS - 1))));
        sum += lp[i];
    }
    for (i = 0; i < V34_INFO_LP_TAPS; i++)
        lp[i] /= (float) sum;

    bb = malloc(sizeof(nf_cpx_t) * (size_t) n);
    filt = malloc(sizeof(nf_cpx_t) * (size_t) n);
    bits = malloc((size_t) n);
    tpos = malloc(sizeof(double) * (size_t) n);
    if (!bb || !filt || !bits || !tpos) {
        free(bb); free(filt); free(bits); free(tpos);
        return 0;
    }

    for (i = 0; i < n; i++) {
        nf_cpx_t lo = nf_dds_cpx(lo_phase);
        lo_phase += (uint32_t) lo_rate;
        bb[i] = nf_cpx((float) amp[i] * lo.re, -(float) amp[i] * lo.im);
    }
    /* centred ("same") convolution - keeps sample positions aligned with
     * the input so reported frame times are directly meaningful */
    for (i = 0; i < n; i++) {
        nf_cpx_t acc = nf_cpx(0.0f, 0.0f);
        int c = (V34_INFO_LP_TAPS - 1) / 2;
        for (k = 0; k < V34_INFO_LP_TAPS; k++) {
            int p = i + k - c;
            if (p < 0 || p >= n) continue;
            acc.re += lp[k] * bb[p].re;
            acc.im += lp[k] * bb[p].im;
        }
        filt[i] = acc;
    }
    free(bb);
    bb = NULL;

    /* no timing recovery: a fixed 600-baud grid, scanned over one symbol
     * period of timing offsets (and both polarities); differential DPSK
     * demod bit = sign flip between consecutive grid samples */
    for (pol = 0; pol < 2 && nfound < max_frames; pol++) {
        for (off_i = 0; off_i < 27 && nfound < max_frames; off_i++) {
            double off = off_i * sps / 27.0;
            double p;
            nf_cpx_t zprev;
            int nb = 0, st;

            zprev = v34_cc_interp(filt, n, off);
            for (p = off + sps; p < (double) (n - 1); p += sps) {
                nf_cpx_t z = v34_cc_interp(filt, n, p);
                float d_re = z.re * zprev.re + z.im * zprev.im;
                bits[nb] = (uint8_t) (((d_re < 0.0f) ? 1 : 0) ^ pol);
                tpos[nb] = p;
                nb++;
                zprev = z;
            }

            for (st = 0; st + V34_INFO_SYNC_LEN <= nb && nfound < max_frames; st++) {
                int kind;

                for (kind = 0; kind < 2; kind++) {
                    nf_v34_info_frame_t f;
                    int j, dup = 0;

                    if (!v34_info_parse(bits + st, nb - st, kind, &f))
                        continue;
                    f.t = tpos[st] / NF_SAMPLE_RATE;
                    /* the same frame is found at many timing offsets -
                     * collapse anything of the same kind within ~2 bits */
                    for (j = 0; j < nfound; j++)
                        if (out[j].is_infoh == kind && fabs(out[j].t - f.t) < 0.003)
                            dup = 1;
                    if (!dup && nfound < max_frames)
                        out[nfound++] = f;
                }
            }
        }
    }

    free(filt); free(bits); free(tpos);
    return nfound;
}

/* ── control-channel HDLC user-data decoder - see nf_v34.h ─────────────── */

void nf_v34_ccdata_rx_init(nf_v34_ccdata_rx_t *s, int is_call_modem,
                            nf_hdlc_frame_fn handler, void *user)
{
    memset(s, 0, sizeof(*s));
    nf_v34_scrambler_init(&s->descr, is_call_modem);
    nf_hdlc_rx_init(&s->hdlc, 1, handler, user);
}

void nf_v34_ccdata_rx_set_rate(nf_v34_ccdata_rx_t *s, int rate_2400)
{
    s->cc_rate = rate_2400 ? 1 : 0;
}

/* The 16-point 2400 bit/s control-channel constellation (10.2.4): the four
 * quarter-superconstellation base labels 0..3 = (1,1),(-3,1),(1,-3),(-3,-3),
 * each rotated clockwise by 0/90/180/270 deg. All 16 points are distinct
 * (radii sqrt2, sqrt10, sqrt18), so a nearest-point slice recovers a unique
 * (base label 2*Q2+Q1, rotation Zn) pair - the uncoded RX inverse of the TX
 * mapping. Built once on first use. */
static struct { int re, im, base, rot; } v34_cc16[16];
static int v34_cc16_ready = 0;

/* Symbols buffered for the 2400-mode gain seed. The burst opens with >=142
 * inner-ring training symbols (Sh resync; PPh restart has more), preceded by
 * ~20 near-silent RRC-ramp symbols. The MEDIAN of the first 64 magnitudes
 * rejects both the low ramp outliers and noise, landing on the steady-state
 * training level (all data starts well past symbol 64). */
#define V34_CC2400_GAIN_SEED 64

static void v34_cc16_ensure(void)
{
    int b, r, n = 0;
    if (v34_cc16_ready)
        return;
    v34_quarter_table_ensure();
    for (b = 0; b < 4; b++)
        for (r = 0; r < 4; r++) {
            int re = nf_v34_quarter_table[b].re, im = nf_v34_quarter_table[b].im;
            v34_rotate_cw(&re, &im, r);
            v34_cc16[n].re = re;
            v34_cc16[n].im = im;
            v34_cc16[n].base = b;
            v34_cc16[n].rot = r;
            n++;
        }
    v34_cc16_ready = 1;
}

/* Nearest of the 16 points to z/gain (z in receiver units, gain the current
 * |rx|/|constellation| estimate). Returns the base label (0..3) and absolute
 * rotation (0..3); *pmag2 (may be NULL) receives that point's squared radius
 * for the decision-directed gain update. */
static void v34_cc16_nearest(const nf_cpx_t *z, double gain,
                             int *base, int *rot, double *pmag2,
                             double *pang)
{
    double zr = z->re / gain, zi = z->im / gain;
    double bestd = 1e30;
    int besti = 0, i;

    v34_cc16_ensure();
    for (i = 0; i < 16; i++) {
        double dr = zr - v34_cc16[i].re, di = zi - v34_cc16[i].im;
        double d = dr * dr + di * di;
        if (d < bestd) { bestd = d; besti = i; }
    }
    *base = v34_cc16[besti].base;
    *rot  = v34_cc16[besti].rot;
    if (pmag2)
        *pmag2 = (double) v34_cc16[besti].re * v34_cc16[besti].re +
                 (double) v34_cc16[besti].im * v34_cc16[besti].im;
    if (pang)
        *pang = atan2((double) v34_cc16[besti].im,
                      (double) v34_cc16[besti].re);
}

void nf_v34_ccdata_feed_symbol(nf_v34_ccdata_rx_t *s, const nf_cpx_t *z)
{
    if (s->cc_rate == 0 || !s->mode16) {
        int quad = v34_mp_quadrant(z);
        int b0, b1, delta;

        if (s->cc_rate) {
            /* 2400 negotiated, but still inside the 1200-mode training
             * (PPh/ALT/MPh.../E - arbitrarily long, since MPh loops until
             * the peer's MPh arrives): demap at 1200 while watching for the
             * E sequence (20 scrambled ones) that ends the training, and
             * keep a rolling window of inner-ring magnitudes to seed the
             * 16-point gain at the switch. */
            double mag = sqrt((double) z->re * z->re + (double) z->im * z->im);
            if (mag > 0.1) {
                s->mag_ring[s->mag_n % 32] = mag;
                s->mag_n++;
            }
        }
        if (!s->have_prev_quad) {
            s->prev_quad = quad;
            s->have_prev_quad = 1;
            return;
        }
        /* same validated differential demap as the MP decoder: quadrants CCW,
         * Zn = (-quadrant) mod 4, bit pair = (Zn - Zn-1) mod 4, I1 = LSB first
         * in time; descramble, then straight into the HDLC deframer */
        delta = ((-(quad - s->prev_quad)) % 4 + 4) % 4;
        s->prev_quad = quad;

        b0 = nf_v34_descramble_bit(&s->descr, delta & 1);
        b1 = nf_v34_descramble_bit(&s->descr, (delta >> 1) & 1);
        nf_hdlc_rx_put_bit(&s->hdlc, b0);
        nf_hdlc_rx_put_bit(&s->hdlc, b1);
        /* ALT-hold detection (see the alt_* field comments) */
        s->alt_run = (b0 != s->alt_last) ? s->alt_run + 1 : 0;
        s->alt_run = (b1 != b0) ? s->alt_run + 1 : 0;
        s->alt_last = b1;
        if (s->alt_run >= 600)
            s->alt_hold = 1;
        if (s->cc_rate) {
            s->ones_run = b0 ? s->ones_run + 1 : 0;
            s->ones_run = b1 ? s->ones_run + 1 : 0;
            if (s->ones_run >= 20 && s->mag_n > 0) {
                /* E complete: user data follows at 2400 bit/s (16-point).
                 * Seed the ring-radius gain from the median of the recent
                 * inner-ring (|.| = sqrt2) training magnitudes. */
                double tmp[32], med;
                int a, c, n = s->mag_n < 32 ? s->mag_n : 32;
                memcpy(tmp, s->mag_ring, sizeof(double) * (size_t) n);
                for (a = 1; a < n; a++) {                  /* insertion sort */
                    double v = tmp[a];
                    for (c = a; c > 0 && tmp[c-1] > v; c--)
                        tmp[c] = tmp[c-1];
                    tmp[c] = v;
                }
                med = tmp[n / 2];
                s->gain = (med > 1e-9) ? med / sqrt(2.0) : 1.0;
                s->mode16 = 1;
                if (getenv("NFV34CCDBG"))
                    fprintf(stderr, "[ccdata] E seen: 16-pt, gain=%.3f\n",
                            s->gain);
                /* seed the residual-phase tracker from this (inner-ring
                 * training) symbol: its true angle is 45 + k*90 deg */
                s->ph = remainder(atan2((double) z->im, (double) z->re)
                                  - M_PI / 4.0, M_PI / 2.0);
                /* rotation reference continues from THIS (last training)
                 * symbol, re-sliced in de-rotated 16-point terms */
                {
                    nf_cpx_t zr;
                    int sbase, srot;
                    double c = cos(-s->ph), sn = sin(-s->ph);
                    zr.re = (float) (z->re * c - z->im * sn);
                    zr.im = (float) (z->re * sn + z->im * c);
                    v34_cc16_nearest(&zr, s->gain, &sbase, &srot, NULL, NULL);
                    s->prev_quad = srot;
                }
            }
        }
        return;
    }

    /* 2400 bit/s (10.2.4): 4 bits/symbol on the 16-point constellation. Slice
     * to the nearest of the 16 points -> (base label, rotation Zn); I1,I2 come
     * from the differential rotation delta exactly as at 1200, Q1,Q2 from the
     * base label (base = 2*Q2+Q1). All four scrambled bits are descrambled and
     * fed to HDLC in time order I1,I2,Q1,Q2. */
    {
        nf_cpx_t zr;
        double c = cos(-s->ph), sn = sin(-s->ph);
        double mag, pang, zang, err;
        int base, rot, i1, i2, q1, q2, delta;
        double pm2 = 2.0;

        /* de-rotate by the tracked residual phase, slice, then update the
         * tracker from the sliced reference point's exact angle */
        zr.re = (float) (z->re * c - z->im * sn);
        zr.im = (float) (z->re * sn + z->im * c);
        mag = sqrt((double) zr.re * zr.re + (double) zr.im * zr.im);
        v34_cc16_nearest(&zr, s->gain, &base, &rot, &pm2, &pang);
        if (pm2 > 1e-9)                          /* gentle DD gain tracking */
            s->gain += 0.01 * (mag / sqrt(pm2) - s->gain);
        if (mag > 1e-9) {
            zang = atan2((double) zr.im, (double) zr.re);
            err = remainder(zang - pang, 2.0 * M_PI);
            s->ph += 0.08 * err;
        }

        delta = ((rot - s->prev_quad) % 4 + 4) % 4;
        s->prev_quad = rot;
        i1 = delta & 1;
        i2 = (delta >> 1) & 1;
        q1 = base & 1;
        q2 = (base >> 1) & 1;

        nf_hdlc_rx_put_bit(&s->hdlc, nf_v34_descramble_bit(&s->descr, i1));
        nf_hdlc_rx_put_bit(&s->hdlc, nf_v34_descramble_bit(&s->descr, i2));
        nf_hdlc_rx_put_bit(&s->hdlc, nf_v34_descramble_bit(&s->descr, q1));
        nf_hdlc_rx_put_bit(&s->hdlc, nf_v34_descramble_bit(&s->descr, q2));
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * primary-channel page decoder (batch, capture-anchored) - see nf_v34.h
 * for the full story. Ported from the validated Python pipeline
 * (diag51..diag71); every constant below was measured/validated against
 * references/v.34_modem_test.wav, not taken on faith from the spec.
 * ═══════════════════════════════════════════════════════════════════════ */

#define V34_PAGE_SR      8000.0
/* The symbol clock is now a caller parameter (`baud`) so the same decoder
 * serves both the real capture (NF_V34_PAGE_BAUD_CAPTURE = 3428.6385, the
 * measured +19.6 ppm clock) and the local transmitter below (exactly
 * 24000/7). The capture regression passes the capture value, keeping its
 * behaviour bit-identical to the pre-parameter code. */
#define V34_PAGE_FC      1959.0        /* Table 2 carrier, capture-verified   */
#define V34_PAGE_L       NF_V34_PAGE_TAPS
#define V34_PAGE_FUT     16            /* cursor offset from tap-window head  */
#define V34_PAGE_M       14            /* shell-mapper rings (R=24000, S=3429)*/
#define V34_PAGE_Q       2             /* raw Q bits per 2D symbol            */
#define V34_PAGE_K       28            /* shell-mapper bits per mapping frame */
#define V34_PAGE_LQ      (V34_PAGE_M << V34_PAGE_Q)   /* 56 quarter labels   */
#define V34_PAGE_NPTS    (V34_PAGE_LQ * 4)            /* 224-point alphabet  */
#define V34_PAGE_THETA   0.3125        /* 9.7 non-linear encoder parameter    */
#define V34_PAGE_NKNOWN  432           /* S(128) + Sbar(16) + PP(288)         */
#define V34_PAGE_PP0     144           /* first PP symbol within the knowns   */
#define V34_PAGE_B1SYM   120           /* B1 = one data frame = 120 symbols   */
#define V34_PAGE_B1BITS  840           /* 15 mapping frames x 56 bits         */
#define V34_PAGE_CHUNK   512           /* DD tracking chunk, symbols          */

/* Upper bounds over ALL Table 8 modes (largest b1 data frame: P=16 at b=79) */
#define V34_PAGE_B1SYM_MAX  128        /* 8P, P <= 16                         */
#define V34_PAGE_B1BITS_MAX 1600       /* P*b <= 16*79 (with slack)           */

/* One resolved page-decoder operating point. The NULL-pp default is the
 * capture's set - INCLUDING the 1959.0 Hz downconvert frequency the whole
 * capture regression was validated with (the exact Table 2 value is
 * 96000/49 = 1959.18...; the 0.18 Hz difference lands in the residual-
 * carrier estimate either way, but the default is kept bit-identical). */
typedef struct {
    double fc;           /* downconvert carrier, Hz                        */
    int M, q, K, b, P, J;
    uint16_t swp;
    double theta;
    int lq;              /* M << q quarter labels                          */
    int npts;            /* slicing alphabet size = 4*lq                   */
    int b1_sym;          /* 8P                                             */
    int b1_bits;         /* raw bits in one data frame (SWP-summed)        */
    int trn_16pt;
} v34_page_mode_t;

static void v34_page_mode_resolve(const nf_v34_pcparams_t *pp, v34_page_mode_t *m)
{
    if (pp) {
        m->fc = pp->carrier_hz;
        m->M = pp->M;
        m->q = pp->q;
        m->K = pp->K;
        m->b = pp->b;
        m->P = pp->P;
        m->J = pp->J;
        m->swp = pp->swp;
        m->theta = pp->theta;
        m->b1_sym = pp->b1_sym;
        m->b1_bits = pp->b1_bits;
        m->trn_16pt = pp->trn_16pt;
    } else {
        m->fc = V34_PAGE_FC;
        m->M = V34_PAGE_M;
        m->q = V34_PAGE_Q;
        m->K = V34_PAGE_K;
        m->b = 56;
        m->P = 15;
        m->J = 8;
        m->swp = 0x7FFF;
        m->theta = V34_PAGE_THETA;
        m->b1_sym = V34_PAGE_B1SYM;
        m->b1_bits = V34_PAGE_B1BITS;
        m->trn_16pt = 1;
    }
    m->lq = m->M << m->q;
    m->npts = m->lq * 4;
}

/* double-precision complex - the float nf_cpx_t is too coarse for the LS
 * normal equations and the long phase accumulations here */
typedef struct { double re, im; } v34_cd_t;

static v34_cd_t v34_cd(double re, double im) { v34_cd_t z; z.re = re; z.im = im; return z; }
static v34_cd_t v34_cd_add(v34_cd_t a, v34_cd_t b) { return v34_cd(a.re + b.re, a.im + b.im); }
static v34_cd_t v34_cd_sub(v34_cd_t a, v34_cd_t b) { return v34_cd(a.re - b.re, a.im - b.im); }
static v34_cd_t v34_cd_mul(v34_cd_t a, v34_cd_t b)
{
    return v34_cd(a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re);
}
/* a * conj(b) */
static v34_cd_t v34_cd_mulc(v34_cd_t a, v34_cd_t b)
{
    return v34_cd(a.re*b.re + a.im*b.im, a.im*b.re - a.re*b.im);
}
static v34_cd_t v34_cd_smul(v34_cd_t a, double s) { return v34_cd(a.re*s, a.im*s); }
static double   v34_cd_abs2(v34_cd_t a) { return a.re*a.re + a.im*a.im; }
static v34_cd_t v34_cd_expj(double phi) { return v34_cd(cos(phi), sin(phi)); }

/* ── windowed-sinc fractional interpolator ──────────────────────────────
 * Stands in for the Python prototype's FFT 8x oversampling + linear
 * interpolation: 32-tap Blackman-Harris-windowed sinc, 128 tabulated
 * phases with linear interpolation between them. The post-RRC baseband
 * only has content to ~0.56 Nyquist, where this design's error is far
 * below the equalizer's own residual (verified by the achieved TRN
 * holdout residual matching the Python pipeline's). */
#define V34_INTERP_TAPS   32
#define V34_INTERP_HALF   15           /* taps cover sample offsets -15..+16 */
#define V34_INTERP_PHASES 128

static double v34_interp_tab[(V34_INTERP_PHASES + 1) * V34_INTERP_TAPS];
static int v34_interp_ready = 0;

static void v34_interp_ensure(void)
{
    int p, j;

    if (v34_interp_ready)
        return;
    for (p = 0; p <= V34_INTERP_PHASES; p++) {
        double f = (double) p / V34_INTERP_PHASES;
        for (j = 0; j < V34_INTERP_TAPS; j++) {
            double u = (double) (j - V34_INTERP_HALF) - f;   /* -16..+16 */
            double snc = (fabs(u) < 1e-12) ? 1.0 : sin(M_PI * u) / (M_PI * u);
            double x = (u + 16.0) / 32.0;                    /* 0..1 across span */
            double win = 0.35875
                       - 0.48829 * cos(2.0 * M_PI * x)
                       + 0.14128 * cos(4.0 * M_PI * x)
                       - 0.01168 * cos(6.0 * M_PI * x);
            v34_interp_tab[p * V34_INTERP_TAPS + j] = snc * win;
        }
    }
    v34_interp_ready = 1;
}

/* ── front end: one downconverted/filtered/normalized time window ─────── */
typedef struct {
    double t0;            /* absolute capture time of filt[0], seconds */
    double baud;          /* the received symbol clock (caller-supplied) */
    long   n;
    v34_cd_t *filt;
} v34_page_fe_t;

/* RRC designed as a lowpass (t_step = baud/sr symbol periods per tap),
 * beta 0.3, ~8-symbol span, unit DC gain - the exact filter the Python
 * front end used (data_pump1.rrc_lowpass), reimplemented here rather than
 * via nf_rrc_design because the polyphase designer's conventions differ. */
#define V34_PAGE_RRC_MAX 33
static int v34_page_rrc_lowpass(double baud, double beta, double *h)
{
    int ntaps = (int) (8.0 * V34_PAGE_SR / baud);
    int i;
    double sum = 0.0;

    if (!(ntaps & 1))
        ntaps++;
    if (ntaps > V34_PAGE_RRC_MAX)
        ntaps = V34_PAGE_RRC_MAX;
    for (i = 0; i < ntaps; i++) {
        double t = (double) (i - ntaps/2) / V34_PAGE_SR * baud;
        double v;
        if (fabs(t) < 1e-8) {
            v = 1.0 - beta + 4.0*beta/M_PI;
        } else if (fabs(fabs(4.0*beta*t) - 1.0) < 1e-8) {
            v = (beta/sqrt(2.0)) * ((1.0 + 2.0/M_PI)*sin(M_PI/(4.0*beta)) +
                                     (1.0 - 2.0/M_PI)*cos(M_PI/(4.0*beta)));
        } else {
            double num = sin(M_PI*t*(1.0 - beta)) + 4.0*beta*t*cos(M_PI*t*(1.0 + beta));
            double den = M_PI*t*(1.0 - (4.0*beta*t)*(4.0*beta*t));
            v = num/den;
        }
        h[i] = v;
        sum += v;
    }
    for (i = 0; i < ntaps; i++)
        h[i] /= sum;
    return ntaps;
}

static int v34_page_fe_init(v34_page_fe_t *fe, const int16_t *amp, long n, double buf_t0,
                            double baud, double fc,
                            double w_from, double w_to, double act_from, double act_to)
{
    long i0 = (long) ((w_from - buf_t0) * V34_PAGE_SR);
    long i1 = (long) ((w_to - buf_t0) * V34_PAGE_SR);
    long N, i, a0, a1;
    int k, ntaps, c;
    double h[V34_PAGE_RRC_MAX];
    double sumsq = 0.0, norm;
    v34_cd_t *bb;

    v34_interp_ensure();
    if (i0 < 0) i0 = 0;
    if (i1 > n) i1 = n;
    N = i1 - i0;
    if (N < 1000)
        return -1;

    bb = malloc(sizeof(*bb) * (size_t) N);
    fe->filt = malloc(sizeof(*fe->filt) * (size_t) N);
    if (!bb || !fe->filt) {
        free(bb); free(fe->filt);
        fe->filt = NULL;
        return -1;
    }
    fe->t0 = buf_t0 + (double) i0 / V34_PAGE_SR;
    fe->baud = baud;
    fe->n = N;

    for (i = 0; i < N; i++) {
        double ph = -2.0 * M_PI * fc * (double) i / V34_PAGE_SR;
        double s = (double) amp[i0 + i];
        bb[i] = v34_cd(s * cos(ph), s * sin(ph));
    }

    ntaps = v34_page_rrc_lowpass(baud, 0.3, h);
    c = (ntaps - 1) / 2;
    for (i = 0; i < N; i++) {
        v34_cd_t acc = v34_cd(0.0, 0.0);
        for (k = 0; k < ntaps; k++) {
            long p = i + k - c;
            if (p < 0 || p >= N)
                continue;
            acc.re += h[k] * bb[p].re;
            acc.im += h[k] * bb[p].im;
        }
        fe->filt[i] = acc;
    }
    free(bb);

    a0 = (long) ((act_from - fe->t0) * V34_PAGE_SR);
    a1 = (long) ((act_to - fe->t0) * V34_PAGE_SR);
    if (a0 < 0) a0 = 0;
    if (a1 > N) a1 = N;
    if (a1 <= a0) { a0 = 0; a1 = N; }
    for (i = a0; i < a1; i++)
        sumsq += v34_cd_abs2(fe->filt[i]);
    norm = sqrt(sumsq / (double) (a1 - a0));
    if (norm <= 0.0)
        norm = 1.0;
    for (i = 0; i < N; i++)
        fe->filt[i] = v34_cd_smul(fe->filt[i], 1.0 / norm);
    return 0;
}

static void v34_page_fe_free(v34_page_fe_t *fe)
{
    free(fe->filt);
    fe->filt = NULL;
}

/* interpolated sample at fractional index pos (units: 8 kHz samples from
 * fe->filt[0]) */
static v34_cd_t v34_page_fe_sample(const v34_page_fe_t *fe, double pos)
{
    long m = (long) floor(pos);
    double f = pos - (double) m;
    double pp = f * V34_INTERP_PHASES;
    int pi = (int) pp;
    double pf = pp - pi;
    const double *ta, *tb;
    v34_cd_t acc = v34_cd(0.0, 0.0);
    int j;

    if (pi >= V34_INTERP_PHASES) { pi = V34_INTERP_PHASES - 1; pf = 1.0; }
    ta = v34_interp_tab + (size_t) pi * V34_INTERP_TAPS;
    tb = ta + V34_INTERP_TAPS;
    for (j = 0; j < V34_INTERP_TAPS; j++) {
        long k = m + j - V34_INTERP_HALF;
        double w;
        if (k < 0 || k >= fe->n)
            continue;
        w = ta[j] + (tb[j] - ta[j]) * pf;
        acc.re += w * fe->filt[k].re;
        acc.im += w * fe->filt[k].im;
    }
    return acc;
}

/* ── FSE application over one chunk of symbols ──────────────────────────
 * v34_page_fse_stream builds the SL-derotated T/2 stream for symbols
 * [s0-hist, s1+future) anchored at t_s (+tau samples): the residual-carrier
 * slope sl (rad/sym) is removed with phase referenced to the global T/2
 * index from t_s. *off_out = symbol offset of s0 within the stream.
 * v34_page_fse_apply then convolves with the taps:
 * out[i] = sum_k w[k] * stream[2(off+i)+1+FUT-k]. */
/* rotation multiples (x img_hz) of the image branches - decode must match
 * training (see nf_v34_page_eq_t.wimg) */
static const int v34_page_img_rot[6] = { 1, -1, 2, -2, 3, -3 };

static v34_cd_t *v34_page_fse_stream(const v34_page_fe_t *fe,
                                     const nf_v34_page_eq_t *eq,
                                     double t_s, double tau,
                                     long s0, long s1, long *off_out,
                                     long *nel_out)
{
    long marg_b = V34_PAGE_L/2 + 4;    /* symbols of tap history */
    long marg_f = V34_PAGE_FUT/2 + 4;  /* symbols of tap future  */
    long a = s0 - marg_b, b = s1 + marg_f;
    double period = V34_PAGE_SR / fe->baud;
    double base = (t_s - fe->t0) * V34_PAGE_SR + tau;
    v34_cd_t *st;
    long m;

    st = malloc(sizeof(*st) * (size_t) (2 * (b - a)));
    if (!st)
        return NULL;
    for (m = 0; m < b - a; m++) {
        double pos = base + period * (double) (a + m);
        v34_cd_t sym = v34_page_fe_sample(fe, pos);
        v34_cd_t hlf = v34_page_fe_sample(fe, pos - period / 2.0);
        /* global T/2 index of the symbol sample is 2(a+m)+1, of the half
         * sample 2(a+m); derotation phase = sl*(idx-1)/2 */
        st[2*m+1] = v34_cd_mul(sym, v34_cd_expj(eq->sl * (double) (2*(a + m)) / 2.0));
        st[2*m]   = v34_cd_mul(hlf, v34_cd_expj(eq->sl * (double) (2*(a + m) - 1) / 2.0));
    }
    *off_out = s0 - a;
    if (nel_out)
        *nel_out = 2 * (b - a);
    return st;
}

/* Rotate a chunk's T/2 stream into image-branch `b` (see nf_v34_page_eq_t):
 * element j of st has global T/2 index q = 2*(s0-off) + j; the branch is the
 * stream counter-rotated by rot_mult[b]*img_hz with the same (q-1)/2-symbol
 * phase reference the training-side branches used. */
static void v34_page_img_rotate(const v34_cd_t *st, long nel, long off, long s0,
                                double img_hz, double baud, int b, v34_cd_t *dst)
{
    double ph = -M_PI * img_hz / baud * (double) v34_page_img_rot[b];
    long q0 = 2 * (s0 - off);
    long j;

    for (j = 0; j < nel; j++)
        dst[j] = v34_cd_mul(st[j], v34_cd_expj(ph * (double) (q0 + j - 1)));
}

static void v34_page_fse_apply(const v34_cd_t *st, long off, long n,
                               const v34_cd_t *w, v34_cd_t *out)
{
    long i;
    int k;

    for (i = 0; i < n; i++) {
        long head = 2 * (i + off) + 1 + V34_PAGE_FUT;
        v34_cd_t acc = v34_cd(0.0, 0.0);
        for (k = 0; k < V34_PAGE_L; k++)
            acc = v34_cd_add(acc, v34_cd_mul(w[k], st[head - k]));
        out[i] = acc;
    }
}

static int v34_page_fse_chunk(const v34_page_fe_t *fe,
                              const nf_v34_page_eq_t *eq,
                              double t_s, double tau,
                              long s0, long s1, const v34_cd_t *w, v34_cd_t *out)
{
    long off;
    v34_cd_t *st = v34_page_fse_stream(fe, eq, t_s, tau, s0, s1, &off, NULL);

    if (!st)
        return -1;
    v34_page_fse_apply(st, off, s1 - s0, w, out);
    free(st);
    return 0;
}

/* ── known reference sequences ────────────────────────────────────────── */

/* TRN, 10.1.3.8: scrambler (GPC, zero state) fed binary ones; 16-point
 * variant takes four scrambled bits I1,I2,Q1,Q2 per symbol, point =
 * quarter_table[2*Q2+Q1] rotated CW by (2*I2+I1)*90deg; the 4-point
 * variant (INFOh bit 30 = 0) takes two bits and always uses label 0. NO
 * non-linear encoding (validated: the plain reference trains ~1.4x better
 * than a Phi-scaled one on this capture). Output normalized to unit RMS. */
static void v34_page_build_trn(v34_cd_t *out, int nsym, int sixteen_point)
{
    nf_v34_scrambler_t scr;
    int i;
    double sumsq = 0.0, s;

    v34_quarter_table_ensure();
    nf_v34_scrambler_init(&scr, 1);
    for (i = 0; i < nsym; i++) {
        int i1 = nf_v34_scramble_bit(&scr, 1);
        int i2 = nf_v34_scramble_bit(&scr, 1);
        int lab = 0;
        int re, im;
        if (sixteen_point) {
            int q1 = nf_v34_scramble_bit(&scr, 1);
            int q2 = nf_v34_scramble_bit(&scr, 1);
            lab = 2*q2 + q1;
        }
        re = nf_v34_quarter_table[lab].re;
        im = nf_v34_quarter_table[lab].im;
        v34_rotate_cw(&re, &im, 2*i2 + i1);
        out[i] = v34_cd((double) re, (double) im);
        sumsq += v34_cd_abs2(out[i]);
    }
    s = 1.0 / sqrt(sumsq / nsym);
    for (i = 0; i < nsym; i++)
        out[i] = v34_cd_smul(out[i], s);
}

/* S(128) + Sbar(16) + PP(288) per 10.1.3.7 / eq. 10-1, UNnormalized:
 * S/Sbar alternate point-0 rotations at |.|=sqrt(2), PP is unit-magnitude.
 * Gain fits downstream use the PP portion only. */
static void v34_page_build_sspp(v34_cd_t *out)
{
    int k, i;

    for (k = 0; k < 128; k++)
        out[k] = (k & 1) ? v34_cd(-1.0, 1.0) : v34_cd(1.0, 1.0);
    for (k = 0; k < 16; k++)
        out[128 + k] = (k & 1) ? v34_cd(1.0, -1.0) : v34_cd(-1.0, -1.0);
    for (i = 0; i < 288; i++) {
        int kk = i / 4, I = i % 4;
        double ang = M_PI * (double) ((kk % 3 == 1) ? (kk*I + 4) : (kk*I)) / 6.0;
        out[V34_PAGE_PP0 + i] = v34_cd_expj(ang);
    }
}

/* mean |p|^2 over the lq quarter labels actually used (for the capture's
 * 56-label set: 1000/7 ~ 142.857) - the 9.7 average-energy convention the
 * whole validated pipeline uses */
static double v34_page_avg_energy_lq(int lq)
{
    double s = 0.0;
    int l;

    v34_quarter_table_ensure();
    for (l = 0; l < lq; l++)
        s += (double) nf_v34_quarter_table[l].re * nf_v34_quarter_table[l].re +
             (double) nf_v34_quarter_table[l].im * nf_v34_quarter_table[l].im;
    return s / lq;
}

/* B1's known content: one data frame of scrambled ones (GPC, zero state) -
 * m->b1_bits raw bits, laid out per the mode's SWP/9.3 parse. Returns the
 * RMS of B1's 8P transmitted points (labels via the parse + shell map,
 * Phi-scaled; the tiny precoder contribution is ignored - 0.06% on this
 * norm on the reference capture, and the per-chunk DD gain absorbs it
 * immediately). */
static double v34_page_b1_known(const v34_page_mode_t *m,
                                uint8_t bits[V34_PAGE_B1BITS_MAX])
{
    nf_v34_scrambler_t scr;
    double avg_e = v34_page_avg_energy_lq(m->lq);
    double sum = 0.0;
    long bpos = 0;
    int i, f, j;

    nf_v34_scrambler_init(&scr, 1);
    for (i = 0; i < m->b1_bits; i++)
        bits[i] = (uint8_t) nf_v34_scramble_bit(&scr, 1);

    for (f = 0; f < m->P; f++) {
        int is_high = (m->swp >> (m->P - 1 - f)) & 1;
        int nbf = is_high ? m->b : m->b - 1;
        uint32_t R0 = 0;
        int rings[4][2];

        if (m->K > 0) {
            int nb = is_high ? m->K : m->K - 1;
            for (i = 0; i < nb; i++)
                R0 |= (uint32_t) bits[bpos + i] << i;
            bpos += nb;
            nf_v34_shell_map(m->M, R0, rings);
        } else {
            memset(rings, 0, sizeof(rings));
        }
        for (j = 0; j < 4; j++) {
            int q0 = 0, q1 = 0;
            int labs[2];
            bpos += 2;                               /* I1, I2 */
            if (m->K > 0 || j < nbf - 8)
                bpos++;                              /* I3 (when present) */
            for (i = 0; i < m->q; i++)
                q0 |= bits[bpos++] << i;
            for (i = 0; i < m->q; i++)
                q1 |= bits[bpos++] << i;
            labs[0] = q0 | (rings[j][0] << m->q);
            labs[1] = q1 | (rings[j][1] << m->q);
            for (i = 0; i < 2; i++) {
                double p2 = (double) nf_v34_quarter_table[labs[i]].re * nf_v34_quarter_table[labs[i]].re +
                            (double) nf_v34_quarter_table[labs[i]].im * nf_v34_quarter_table[labs[i]].im;
                double zeta = m->theta * p2 / avg_e;
                double phi = 1.0 + zeta/6.0 + zeta*zeta/120.0;
                sum += phi * phi * p2;
            }
        }
    }
    return sqrt(sum / m->b1_sym);
}

/* ── nonlinear-scaled slicing alphabet (npts = 4 * M * 2^q points) ──────
 * index = label*4 + rotation; point = Phi(p) * rotate_cw(quarter[label],
 * rotation), Phi per 9.7 with the avg-energy convention above. Built per
 * decode call from the mode (cheap: <= 1664 points). */
typedef struct {
    int npts;
    double avg_e2;       /* mean |p|^2 over the alphabet (PLL weighting) */
    v34_cd_t pts[NF_V34_QUARTER_MAX * 4];
} v34_page_alpha_t;

static void v34_page_alpha_build(const v34_page_mode_t *m, v34_page_alpha_t *a)
{
    double avg_e;
    int l, r;

    v34_quarter_table_ensure();
    avg_e = v34_page_avg_energy_lq(m->lq);
    a->npts = m->npts;
    for (l = 0; l < m->lq; l++) {
        for (r = 0; r < 4; r++) {
            int re = nf_v34_quarter_table[l].re;
            int im = nf_v34_quarter_table[l].im;
            double p2, zeta, phi;
            v34_rotate_cw(&re, &im, r);
            p2 = (double) re*re + (double) im*im;
            zeta = m->theta * p2 / avg_e;
            phi = 1.0 + zeta/6.0 + zeta*zeta/120.0;
            a->pts[l*4 + r] = v34_cd(phi * re, phi * im);
        }
    }
    a->avg_e2 = 0.0;
    for (l = 0; l < a->npts; l++)
        a->avg_e2 += v34_cd_abs2(a->pts[l]);
    a->avg_e2 /= (double) a->npts;
}

/* ── per-symbol decision-directed phase PLL ─────────────────────────────
 * The chunk-level tracker (one complex gain + linear phase per 512-symbol
 * chunk, ~150 ms) is far too slow for phase jitter at telephone power-line
 * rates: 10 degrees peak at 60 Hz sweeps +-0.175 rad through every chunk
 * and cost the sweep's jitter cell ~220/840 B1 symbol errors. This
 * proportional per-symbol loop (bandwidth ~150 Hz; the frequency
 * integrator is deliberately OFF - decision-gated sampling of a jitter
 * sinusoid biases it into wind-up, and true frequency residue is owned
 * by the chunk-level tracker) runs inside each chunk's first slicing
 * pass, with a non-causal smoothing pass after it; the per-symbol
 * error is the ML phase estimate Im(y*conj(p))/|p|^2, variance-weighted
 * by |p|^2 so the outer constellation points (which carry the reliable
 * phase information) dominate. */
#define V34_PLL_ALPHA 0.28
#define V34_PLL_BETA  0.0

typedef struct {
    double th;     /* phase correction, rad (applied as *e^{j th}) */
    double fr;     /* per-symbol frequency term                     */
} v34_page_pll_t;

static void v34_page_pll_step(v34_page_pll_t *p, v34_cd_t y,
                              v34_cd_t pt, double avg_e2)
{
    double p2 = v34_cd_abs2(pt);
    double eps = (y.im * pt.re - y.re * pt.im) / (p2 > 0.0 ? p2 : 1.0);
    double wgt = p2 / avg_e2;

    if (eps > 0.5) eps = 0.5;
    if (eps < -0.5) eps = -0.5;
    if (wgt > 1.5) wgt = 1.5;
    if (wgt < 0.2) wgt = 0.2;
    p->fr -= V34_PLL_BETA * wgt * eps;
    if (p->fr > 0.004) p->fr = 0.004;
    if (p->fr < -0.004) p->fr = -0.004;
    p->th -= V34_PLL_ALPHA * wgt * eps;
    p->th += p->fr;
}

/* nearest alphabet index; *d_out = Euclidean distance to it; *marg_out
 * (may be NULL) = distance to the SECOND-nearest point minus *d_out - the
 * decision margin used to gate the DD tap adaptation */
static int v34_page_slice1(const v34_page_alpha_t *a, v34_cd_t o,
                           double *d_out, double *marg_out)
{
    int i, best = 0;
    double bd = 1e300, bd2 = 1e300;

    for (i = 0; i < a->npts; i++) {
        double dr = o.re - a->pts[i].re;
        double di = o.im - a->pts[i].im;
        double d2 = dr*dr + di*di;
        if (d2 < bd) { bd2 = bd; bd = d2; best = i; }
        else if (d2 < bd2) { bd2 = d2; }
    }
    *d_out = sqrt(bd);
    if (marg_out)
        *marg_out = sqrt(bd2) - sqrt(bd);
    return best;
}

static int v34_page_dcmp(const void *a, const void *b)
{
    double da = *(const double *) a, db = *(const double *) b;
    return (da > db) - (da < db);
}

static double v34_page_median(double *d, long n)   /* reorders d */
{
    qsort(d, (size_t) n, sizeof(double), v34_page_dcmp);
    return (n & 1) ? d[n/2] : 0.5 * (d[n/2 - 1] + d[n/2]);
}

/* ── per-block phase (slope+intercept) fit ──────────────────────────────
 * Per B-symbol block: complex correlation angle of out vs ref; unwrap;
 * linear fit of angle vs block centre (units: rad/symbol, rad). */
static void v34_page_phase_fit(const v34_cd_t *out, const v34_cd_t *ref, long n, int B,
                               double *sl_out, double *ic_out)
{
    double ang_prev = 0.0;
    double Sx = 0.0, Sy = 0.0, Sxx = 0.0, Sxy = 0.0;
    long b0, i, nb = 0;

    for (b0 = 0; b0 < n - B; b0 += B) {
        v34_cd_t g = v34_cd(0.0, 0.0);
        double ang, cent;
        for (i = b0; i < b0 + B; i++)
            g = v34_cd_add(g, v34_cd_mulc(ref[i], out[i]));   /* conj(out)*ref */
        ang = atan2(g.im, g.re);
        if (nb > 0)
            ang -= 2.0 * M_PI * floor((ang - ang_prev) / (2.0 * M_PI) + 0.5);
        ang_prev = ang;
        cent = (double) b0 + B / 2.0;
        Sx += cent; Sy += ang; Sxx += cent * cent; Sxy += cent * ang;
        nb++;
    }
    if (nb >= 2) {
        double den = (double) nb * Sxx - Sx * Sx;
        *sl_out = ((double) nb * Sxy - Sx * Sy) / den;
        *ic_out = (Sy - *sl_out * Sx) / nb;
    } else {
        *sl_out = 0.0;
        *ic_out = (nb == 1) ? Sy : 0.0;
    }
}

/* ── complex Hermitian Cholesky solve (A w = b, A lower triangle) ─────── */
static int v34_page_chol_solve_n(v34_cd_t *A, const v34_cd_t *bvec, v34_cd_t *w,
                                 int L)
{
    int i, j, k;

    for (j = 0; j < L; j++) {
        double d = A[j*L + j].re;
        for (k = 0; k < j; k++)
            d -= v34_cd_abs2(A[j*L + k]);
        if (d <= 0.0)
            return -1;
        d = sqrt(d);
        A[j*L + j] = v34_cd(d, 0.0);
        for (i = j + 1; i < L; i++) {
            v34_cd_t s = A[i*L + j];
            for (k = 0; k < j; k++)
                s = v34_cd_sub(s, v34_cd_mulc(A[i*L + k], A[j*L + k]));
            A[i*L + j] = v34_cd_smul(s, 1.0 / d);
        }
    }
    for (i = 0; i < L; i++) {              /* L y = b (y into w) */
        v34_cd_t s = bvec[i];
        for (k = 0; k < i; k++)
            s = v34_cd_sub(s, v34_cd_mul(A[i*L + k], w[k]));
        w[i] = v34_cd_smul(s, 1.0 / A[i*L + i].re);
    }
    for (i = L - 1; i >= 0; i--) {          /* L^H w = y */
        v34_cd_t s = w[i];
        for (k = i + 1; k < L; k++)
            s = v34_cd_sub(s, v34_cd_mulc(w[k], A[k*L + i]));
        w[i] = v34_cd_smul(s, 1.0 / A[i*L + i].re);
    }
    return 0;
}

/* LS solve of min ||X w - ref|| over rows i_lo..i_lo+ntr-1 of the design
 * matrix X (row i, col k = st[2i+1+FUT-k]) via normal equations. A tiny
 * relative diagonal loading stands in for lstsq's rcond: the T/2 stream's
 * out-of-band subspace is nearly empty, so X^H X is ill-conditioned by
 * construction. */
static int v34_page_ls_solve(const v34_cd_t *st, const v34_cd_t *ref,
                             long i_lo, long ntr, v34_cd_t *w)
{
    const int L = V34_PAGE_L;
    v34_cd_t *A = calloc((size_t) L * L, sizeof(v34_cd_t));
    v34_cd_t *bvec = calloc((size_t) L, sizeof(v34_cd_t));
    long row;
    int i, j, rc;
    double tr = 0.0;

    if (!A || !bvec) {
        free(A); free(bvec);
        return -1;
    }
    for (row = 0; row < ntr; row++) {
        long h = 2 * (i_lo + row) + 1 + V34_PAGE_FUT;
        v34_cd_t r = ref[i_lo + row];
        for (i = 0; i < L; i++) {
            v34_cd_t xi = st[h - i];
            bvec[i] = v34_cd_add(bvec[i], v34_cd_mulc(r, xi));      /* conj(xi)*r */
            for (j = 0; j <= i; j++)
                A[i*L + j] = v34_cd_add(A[i*L + j], v34_cd_mulc(st[h - j], xi));
        }
    }
    for (i = 0; i < L; i++)
        tr += A[i*L + i].re;
    for (i = 0; i < L; i++)
        A[i*L + i].re += 1e-8 * tr / L;
    rc = v34_page_chol_solve_n(A, bvec, w, L);
    free(A);
    free(bvec);
    return rc;
}

static void v34_page_ls_out(const v34_cd_t *st, const v34_cd_t *w,
                            long i_lo, long rows, v34_cd_t *out)
{
    long row;
    int k;

    for (row = 0; row < rows; row++) {
        long h = 2 * (i_lo + row) + 1 + V34_PAGE_FUT;
        v34_cd_t acc = v34_cd(0.0, 0.0);
        for (k = 0; k < V34_PAGE_L; k++)
            acc = v34_cd_add(acc, v34_cd_mul(w[k], st[h - k]));
        out[row] = acc;
    }
}

/* ── joint multi-branch LS (main stream + model branches) ───────────────
 * Same normal-equations solve as v34_page_ls_solve, over the concatenated
 * regressor set of nb streams (nb*L taps): the counter-rotated copies that
 * make a frequency-shifter's image components stationary and/or the cubic
 * distortion stream (see nf_v34_page_eq_t's branch comments). */
#define V34_PAGE_NBRANCH 8
static int v34_page_ls_solve_nb(const v34_cd_t *const *sts, int nb,
                                const v34_cd_t *ref,
                                long i_lo, long ntr, v34_cd_t **ws)
{
    const int L = V34_PAGE_L;
    const int LN = nb * V34_PAGE_L;
    v34_cd_t *A = calloc((size_t) LN * LN, sizeof(v34_cd_t));
    v34_cd_t *bvec = calloc((size_t) LN, sizeof(v34_cd_t));
    v34_cd_t *ww = malloc(sizeof(v34_cd_t) * (size_t) LN);
    v34_cd_t x[V34_PAGE_NBRANCH * V34_PAGE_L];
    long row;
    int i, j, b, rc;
    double tr = 0.0;

    if (!A || !bvec || !ww || nb > V34_PAGE_NBRANCH) {
        free(A); free(bvec); free(ww);
        return -1;
    }
    for (row = 0; row < ntr; row++) {
        long h = 2 * (i_lo + row) + 1 + V34_PAGE_FUT;
        v34_cd_t r = ref[i_lo + row];
        for (b = 0; b < nb; b++)
            for (i = 0; i < L; i++)
                x[b*L + i] = sts[b][h - i];
        for (i = 0; i < LN; i++) {
            bvec[i] = v34_cd_add(bvec[i], v34_cd_mulc(r, x[i]));
            for (j = 0; j <= i; j++)
                A[i*LN + j] = v34_cd_add(A[i*LN + j], v34_cd_mulc(x[j], x[i]));
        }
    }
    for (i = 0; i < LN; i++)
        tr += A[i*LN + i].re;
    for (i = 0; i < LN; i++)
        A[i*LN + i].re += 1e-7 * tr / LN;
    rc = v34_page_chol_solve_n(A, bvec, ww, LN);
    if (rc == 0) {
        for (b = 0; b < nb; b++)
            memcpy(ws[b], ww + b*L, sizeof(v34_cd_t) * (size_t) L);
    }
    free(A);
    free(bvec);
    free(ww);
    return rc;
}

static void v34_page_ls_out_nb(const v34_cd_t *const *sts, int nb,
                               v34_cd_t *const *ws,
                               long i_lo, long rows, v34_cd_t *out)
{
    long row;
    int k, b;

    for (row = 0; row < rows; row++) {
        long h = 2 * (i_lo + row) + 1 + V34_PAGE_FUT;
        v34_cd_t acc = v34_cd(0.0, 0.0);
        for (b = 0; b < nb; b++)
            for (k = 0; k < V34_PAGE_L; k++)
                acc = v34_cd_add(acc, v34_cd_mul(ws[b][k], sts[b][h - k]));
        out[row] = acc;
    }
}

static double v34_page_resid(const v34_cd_t *out, const v34_cd_t *ref, long n)
{
    double num = 0.0, den = 0.0;
    long i;

    for (i = 0; i < n; i++) {
        num += v34_cd_abs2(v34_cd_sub(out[i], ref[i]));
        den += v34_cd_abs2(ref[i]);
    }
    return (den > 0.0) ? num / den : 1e300;
}

int nf_v34_page_train(const int16_t *amp, long n, double buf_t0,
                      double t_trn, int n_trn, double baud,
                      const nf_v34_pcparams_t *pp,
                      nf_v34_page_eq_t *eq)
{
    v34_page_mode_t md;
    v34_page_fe_t fe;
    double period = V34_PAGE_SR / baud;
    double w_from = t_trn - 0.056;
    double w_to = t_trn + n_trn / baud + 0.063;
    long nst = 2L * n_trn;
    v34_cd_t *ref = NULL, *st = NULL, *tmp = NULL, *out = NULL;
    v34_cd_t w[V34_PAGE_L];
    double base, sl, ic, sl_tot;
    long i, i_lo, i_hi, rows, ntr;
    const long holdout = 1000;
    int it, rc = -1;

    v34_page_mode_resolve(pp, &md);
    memset(eq, 0, sizeof(*eq));
    if (v34_page_fe_init(&fe, amp, n, buf_t0, baud, md.fc, w_from, w_to, w_from, w_to) < 0)
        return -1;

    ref = malloc(sizeof(*ref) * (size_t) n_trn);
    st = malloc(sizeof(*st) * (size_t) nst);
    tmp = malloc(sizeof(*tmp) * (size_t) n_trn);
    if (!ref || !st || !tmp)
        goto done;
    v34_page_build_trn(ref, n_trn, md.trn_16pt);

    base = (t_trn - fe.t0) * V34_PAGE_SR;
    for (i = 0; i < n_trn; i++) {
        double pos = base + period * (double) i;
        st[2*i + 1] = v34_page_fe_sample(&fe, pos);
        st[2*i]     = v34_page_fe_sample(&fe, pos - period / 2.0);
    }

    /* bootstrap residual-carrier fit on the RAW symbol-aligned samples -
     * works block-locally even before equalization (diag55 recipe). Two
     * passes: a 64-symbol-block pass first (unwrap pull-in ~+-26 Hz - the
     * original single 250-block pass aliases beyond ~+-7 Hz because the
     * block-to-block phase step exceeds pi, which is exactly the offset
     * range V.34 requires), then the finer 250-block pass on the result. */
    sl_tot = 0.0;
    {
        static const int boot_B[2] = { 64, 250 };
        int bp;

        for (bp = 0; bp < 2; bp++) {
            for (i = 0; i < n_trn; i++)
                tmp[i] = st[2*i + 1];
            v34_page_phase_fit(tmp, ref, n_trn, boot_B[bp], &sl, &ic);
            for (i = 0; i < nst; i++)
                st[i] = v34_cd_mul(st[i], v34_cd_expj(sl * (double) (i - 1) / 2.0 + ic));
            sl_tot += sl;
            if (getenv("NFV34DBG"))
                fprintf(stderr, "    [page_train boot B=%d] sl=%.6f ic=%.3f\n",
                        boot_B[bp], sl, ic);
        }
    }

    i_lo = 0;
    while (2*i_lo + 1 + V34_PAGE_FUT - (V34_PAGE_L - 1) < 0)
        i_lo++;
    i_hi = n_trn;
    while (2*(i_hi - 1) + 1 + V34_PAGE_FUT >= nst)
        i_hi--;
    rows = i_hi - i_lo;
    ntr = rows - holdout;
    if (ntr < 2000)
        goto done;
    out = malloc(sizeof(*out) * (size_t) rows);
    if (!out)
        goto done;

    /* iterated: LS solve -> refit residual carrier on the equalized output
     * -> derotate the T/2 stream -> re-solve (diag66's ls_train) */
    for (it = 0; it < 3; it++) {
        long h0 = 2*i_lo + 1 + V34_PAGE_FUT;
        if (v34_page_ls_solve(st, ref, i_lo, ntr, w) < 0)
            goto done;
        v34_page_ls_out(st, w, i_lo, rows, out);
        v34_page_phase_fit(out, ref + i_lo, rows, 250, &sl, &ic);
        for (i = 0; i < nst; i++)
            st[i] = v34_cd_mul(st[i], v34_cd_expj(sl * (double) (i - h0) / 2.0 + ic));
        sl_tot += sl;
        if (getenv("NFV34DBG"))
            fprintf(stderr, "    [page_train it%d] sl=%.7f ic=%.4f res=%.5f\n",
                    it, sl, ic, v34_page_resid(out, ref + i_lo, ntr));
        if (fabs(sl) < 1e-7 && fabs(ic) < 1e-4)
            break;
    }
    if (v34_page_ls_solve(st, ref, i_lo, ntr, w) < 0)
        goto done;
    v34_page_ls_out(st, w, i_lo, rows, out);

    for (i = 0; i < V34_PAGE_L; i++) {
        eq->w_re[i] = w[i].re;
        eq->w_im[i] = w[i].im;
    }
    eq->sl = sl_tot;
    eq->res_train = v34_page_resid(out, ref + i_lo, ntr);
    eq->res_holdout = v34_page_resid(out + ntr, ref + i_lo + ntr, rows - ntr);

    /* ── optional model branches (see nf_v34_page_eq_t) ─────────────────
     * 1. Frequency-shifter image: the removed carrier (sl_tot) minus the
     *    modulator's exact nominal carrier-vs-1959.0 excess (96000/49 -
     *    1959) measures the LINE's own frequency shift; a genuine shift
     *    means the line's SSB shifter also delivered image components
     *    rotating at multiples of 2x that shift relative to the derotated
     *    signal. (A no-shift line makes these branches collinear with the
     *    main stream - hence the gate.)
     * 2. Cubic distortion: st*|st|^2 (Hammerstein), for lines with
     *    harmonic distortion.
     * Each set is solved jointly with the main stream and kept only when
     * the HOLDOUT residual genuinely improves. */
    {
        /* nominal TX-carrier excess over the downconvert frequency: with a
         * mode struct the downconvert IS the exact carrier (excess 0); the
         * capture default downconverts at 1959.0 vs the exact 96000/49 */
        double f_nom = pp ? 0.0 : 96000.0 / 49.0 - 1959.0;
        double f_rem = sl_tot * baud / (2.0 * M_PI);
        double img_hz = 2.0 * (f_nom + f_rem);
        int use_img;
        v34_cd_t *stb[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        v34_cd_t wb[7][V34_PAGE_L];
        v34_cd_t wmain[V34_PAGE_L];
        const v34_cd_t *sts[V34_PAGE_NBRANCH + 1];
        v34_cd_t *ws[V34_PAGE_NBRANCH + 1];
        int b, nb, bad = 0, img_won = 0;

        use_img = fabs(img_hz) >= 3.0;

        for (b = 0; b < 7; b++) {
            stb[b] = malloc(sizeof(v34_cd_t) * (size_t) nst);
            if (!stb[b])
                bad = 1;
        }
        if (!bad) {
            double e2 = 0.0;

            for (i = 0; i < nst; i++)
                e2 += v34_cd_abs2(st[i]);
            e2 /= (double) nst;
            for (i = 0; i < nst; i++) {
                for (b = 0; b < 6; b++) {
                    double ph = -M_PI * img_hz / baud *
                                (double) v34_page_img_rot[b] * (double) (i - 1);
                    stb[b][i] = v34_cd_mul(st[i], v34_cd_expj(ph));
                }
                stb[6][i] = v34_cd_smul(st[i], v34_cd_abs2(st[i]) / e2);
            }

            /* pass 1: main + 6 image branches */
            if (use_img) {
                sts[0] = st;
                ws[0] = wmain;
                for (b = 0; b < 6; b++) {
                    sts[1 + b] = stb[b];
                    ws[1 + b] = wb[b];
                }
                if (v34_page_ls_solve_nb(sts, 7, ref, i_lo, ntr, ws) == 0) {
                    double r_tr, r_ho;
                    v34_page_ls_out_nb(sts, 7, ws, i_lo, rows, out);
                    r_tr = v34_page_resid(out, ref + i_lo, ntr);
                    r_ho = v34_page_resid(out + ntr, ref + i_lo + ntr, rows - ntr);
                    if (getenv("NFV34DBG"))
                        fprintf(stderr, "    [page_train img %.2f Hz] res %.5f/%.5f"
                                " -> %.5f/%.5f\n", img_hz, eq->res_train,
                                eq->res_holdout, r_tr, r_ho);
                    if (r_ho < eq->res_holdout) {
                        for (i = 0; i < V34_PAGE_L; i++) {
                            eq->w_re[i] = wmain[i].re;
                            eq->w_im[i] = wmain[i].im;
                            for (b = 0; b < 6; b++) {
                                eq->wimg_re[b][i] = wb[b][i].re;
                                eq->wimg_im[b][i] = wb[b][i].im;
                            }
                        }
                        eq->img_active = 1;
                        eq->img_hz = img_hz;
                        eq->res_train = r_tr;
                        eq->res_holdout = r_ho;
                        img_won = 1;
                    }
                }
            }

            /* pass 2: winner + the cubic branch. Kept only on a CLEAR
             * holdout win (5%) - on a clean line the extra freedom can
             * only overfit, and the holdout shows it. */
            sts[0] = st;
            ws[0] = wmain;
            nb = 1;
            if (img_won) {
                for (b = 0; b < 6; b++) {
                    sts[nb] = stb[b];
                    ws[nb] = wb[b];
                    nb++;
                }
            }
            sts[nb] = stb[6];
            ws[nb] = wb[6];
            nb++;
            if (v34_page_ls_solve_nb(sts, nb, ref, i_lo, ntr, ws) == 0) {
                double r_tr, r_ho;
                v34_page_ls_out_nb(sts, nb, ws, i_lo, rows, out);
                r_tr = v34_page_resid(out, ref + i_lo, ntr);
                r_ho = v34_page_resid(out + ntr, ref + i_lo + ntr, rows - ntr);
                if (getenv("NFV34DBG"))
                    fprintf(stderr, "    [page_train nl] res %.5f/%.5f"
                            " -> %.5f/%.5f\n", eq->res_train,
                            eq->res_holdout, r_tr, r_ho);
                if (r_ho < eq->res_holdout * 0.95) {
                    for (i = 0; i < V34_PAGE_L; i++) {
                        eq->w_re[i] = wmain[i].re;
                        eq->w_im[i] = wmain[i].im;
                        if (img_won) {
                            for (b = 0; b < 6; b++) {
                                eq->wimg_re[b][i] = wb[b][i].re;
                                eq->wimg_im[b][i] = wb[b][i].im;
                            }
                        }
                        eq->wnl_re[i] = wb[6][i].re;
                        eq->wnl_im[i] = wb[6][i].im;
                    }
                    eq->nl_active = 1;
                    eq->res_train = r_tr;
                    eq->res_holdout = r_ho;
                }
            }
        }
        for (b = 0; b < 7; b++)
            free(stb[b]);
    }

    /* ── aided per-symbol phase tracking (line-rate phase jitter) ────────
     * A residual this large with the model branches already tried is the
     * signature of phase jitter: the static LS fits THROUGH it, leaving
     * ~1% residual and jitter-contaminated taps. The TRN is known, so the
     * trajectory can be estimated data-aided at per-symbol resolution
     * (centred +-4-symbol smoothing, same estimator the page decoder's
     * jitter path uses), the T/2 stream derotated at the input, and the
     * taps re-solved on the cleaned stream. Two passes; kept only if the
     * holdout genuinely improves. */
    if (eq->res_holdout > 0.004) {
        double *epsv = malloc(sizeof(double) * (size_t) rows);
        double *wgtv = malloc(sizeof(double) * (size_t) rows);
        double *smv = malloc(sizeof(double) * (size_t) rows);
        int pass2;

        if (epsv && wgtv && smv) {
            for (pass2 = 0; pass2 < 4; pass2++) {
                static const int Ks2[4] = { 7, 4, 2, 2 };
                const int K = Ks2[pass2];
                long r2;

                if (v34_page_ls_solve(st, ref, i_lo, ntr, w) < 0)
                    break;
                v34_page_ls_out(st, w, i_lo, rows, out);
                for (r2 = 0; r2 < rows; r2++) {
                    v34_cd_t y = out[r2], p = ref[i_lo + r2];
                    double p2 = v34_cd_abs2(p);
                    double e = (y.im * p.re - y.re * p.im) / (p2 > 0.0 ? p2 : 1.0);
                    if (e > 0.5) e = 0.5;
                    if (e < -0.5) e = -0.5;
                    epsv[r2] = e;
                    wgtv[r2] = p2;
                }
                for (r2 = 0; r2 < rows; r2++) {
                    double se = 0.0, sw = 0.0;
                    long j0 = (r2 - K < 0) ? 0 : r2 - K;
                    long j1 = (r2 + K >= rows) ? rows - 1 : r2 + K;
                    long j;
                    for (j = j0; j <= j1; j++) {
                        se += epsv[j] * wgtv[j];
                        sw += wgtv[j];
                    }
                    smv[r2] = (sw > 0.0) ? se / sw : 0.0;
                }
                for (i = 0; i < nst; i++) {
                    double si2 = ((double) (i - 1)) / 2.0 - (double) i_lo;
                    double a2;
                    if (si2 <= 0.0) {
                        a2 = smv[0];
                    } else if (si2 >= (double) (rows - 1)) {
                        a2 = smv[rows - 1];
                    } else {
                        long i0b = (long) si2;
                        a2 = smv[i0b] + (smv[i0b + 1] - smv[i0b]) *
                             (si2 - (double) i0b);
                    }
                    st[i] = v34_cd_mul(st[i], v34_cd_expj(-a2));
                }
            }
            if (v34_page_ls_solve(st, ref, i_lo, ntr, w) == 0) {
                double r_tr, r_ho;
                v34_page_ls_out(st, w, i_lo, rows, out);
                r_tr = v34_page_resid(out, ref + i_lo, ntr);
                r_ho = v34_page_resid(out + ntr, ref + i_lo + ntr, rows - ntr);
                if (getenv("NFV34DBG"))
                    fprintf(stderr, "    [page_train jitter-track] res %.5f/%.5f"
                            " -> %.5f/%.5f\n", eq->res_train, eq->res_holdout,
                            r_tr, r_ho);
                if (r_ho < eq->res_holdout) {
                    for (i = 0; i < V34_PAGE_L; i++) {
                        eq->w_re[i] = w[i].re;
                        eq->w_im[i] = w[i].im;
                    }
                    eq->res_train = r_tr;
                    eq->res_holdout = r_ho;
                }
            }
        }
        free(epsv);
        free(wgtv);
        free(smv);
    }
    rc = 0;
done:
    free(ref); free(st); free(tmp); free(out);
    v34_page_fe_free(&fe);
    return rc;
}

/* ── S/Sbar/PP locator (normalized cross-correlation, diag51/56 method) ── */

static double v34_page_corr_at(const v34_page_fe_t *fe, const v34_cd_t *templ,
                               double e_t, double start, double df)
{
    double period = V34_PAGE_SR / fe->baud;
    v34_cd_t acc = v34_cd(0.0, 0.0);
    v34_cd_t ph = v34_cd(1.0, 0.0);
    v34_cd_t rot = v34_cd_expj(-2.0 * M_PI * df / fe->baud);
    double e_rx = 0.0;
    int k;

    for (k = 0; k < V34_PAGE_NKNOWN; k++) {
        v34_cd_t rx = v34_page_fe_sample(fe, start + period * k);
        e_rx += v34_cd_abs2(rx);
        acc = v34_cd_add(acc, v34_cd_mul(v34_cd_mulc(rx, templ[k]), ph));
        ph = v34_cd_mul(ph, rot);
    }
    return sqrt(v34_cd_abs2(acc)) / sqrt(e_rx * e_t + 1e-30);
}

/* Frequency-offset-tolerant variant: the 432-symbol template is split into
 * NSEG segments whose complex correlations are summed by MAGNITUDE, so a
 * residual carrier offset (which rotates the segment phasors against each
 * other and collapses the coherent sum - the old +-1.5 Hz coarse scan was
 * blind beyond that, failing outright at the +-7 Hz the spec requires) only
 * costs the small within-segment rotation (~4% at 10 Hz). segs_out (may be
 * NULL) receives the per-segment phasors: the phase progression between
 * consecutive segments measures the offset directly (one 54-symbol segment
 * apart = 2*pi*df*54/baud radians), which seeds the coherent fine scan. */
#define V34_LS_NSEG 8
static double v34_page_corr_incoh(const v34_page_fe_t *fe, const v34_cd_t *templ,
                                  double e_t, double start, v34_cd_t *segs_out)
{
    double period = V34_PAGE_SR / fe->baud;
    const int seg_len = V34_PAGE_NKNOWN / V34_LS_NSEG;
    double e_rx = 0.0, mag = 0.0;
    int g, k;

    for (g = 0; g < V34_LS_NSEG; g++) {
        v34_cd_t acc = v34_cd(0.0, 0.0);
        for (k = g * seg_len; k < (g + 1) * seg_len; k++) {
            v34_cd_t rx = v34_page_fe_sample(fe, start + period * k);
            e_rx += v34_cd_abs2(rx);
            acc = v34_cd_add(acc, v34_cd_mulc(rx, templ[k]));
        }
        mag += sqrt(v34_cd_abs2(acc));
        if (segs_out)
            segs_out[g] = acc;
    }
    return mag / sqrt(e_rx * e_t + 1e-30);
}

/* weighted mean segment-to-segment phase step -> frequency offset, Hz */
static double v34_page_seg_df(const v34_cd_t *segs, double baud)
{
    const int seg_len = V34_PAGE_NKNOWN / V34_LS_NSEG;
    v34_cd_t acc = v34_cd(0.0, 0.0);
    int g;

    for (g = 0; g + 1 < V34_LS_NSEG; g++)
        acc = v34_cd_add(acc, v34_cd_mulc(segs[g + 1], segs[g]));
    if (v34_cd_abs2(acc) <= 0.0)
        return 0.0;
    return atan2(acc.im, acc.re) / (2.0 * M_PI) * baud / (double) seg_len;
}

double nf_v34_page_locate_s(const int16_t *amp, long n, double buf_t0,
                            double t_lo, double t_hi, double baud,
                            const nf_v34_pcparams_t *pp,
                            double *corr_out)
{
    v34_page_mode_t md;
    v34_page_fe_t fe;
    v34_cd_t templ[V34_PAGE_NKNOWN];
    double w_from = t_lo - 0.05;
    double w_to = t_hi + V34_PAGE_NKNOWN / baud + 0.05;
    double e_t = 0.0, sumsq = 0.0;
    double best_c = -1.0, best_s = 0.0, best_df = 0.0;
    double s0, s1, start, df;
    int k;

    v34_page_mode_resolve(pp, &md);
    if (corr_out)
        *corr_out = -1.0;
    if (v34_page_fe_init(&fe, amp, n, buf_t0, baud, md.fc, w_from, w_to, w_from, w_to) < 0)
        return t_lo;

    v34_page_build_sspp(templ);
    for (k = 0; k < V34_PAGE_NKNOWN; k++)
        sumsq += v34_cd_abs2(templ[k]);
    sumsq = sqrt(sumsq / V34_PAGE_NKNOWN);
    for (k = 0; k < V34_PAGE_NKNOWN; k++)
        templ[k] = v34_cd_smul(templ[k], 1.0 / sumsq);
    e_t = V34_PAGE_NKNOWN;

    s0 = (t_lo - fe.t0) * V34_PAGE_SR;
    s1 = (t_hi - fe.t0) * V34_PAGE_SR;

    /* coarse: 1-sample start grid with the frequency-offset-tolerant
     * incoherent metric (works unchanged out to ~+-10 Hz - see
     * v34_page_corr_incoh), then a direct offset estimate from the
     * segment-phasor progression at the winning start */
    for (start = s0; start <= s1; start += 1.0) {
        double c = v34_page_corr_incoh(&fe, templ, e_t, start, NULL);
        if (c > best_c) { best_c = c; best_s = start; }
    }
    {
        v34_cd_t segs[V34_LS_NSEG];
        (void) v34_page_corr_incoh(&fe, templ, e_t, best_s, segs);
        best_df = v34_page_seg_df(segs, baud);
    }
    if (getenv("NFV34DBG"))
        fprintf(stderr, "    [locate_s] incoh best=%.3f at %.1f, df_est=%.2f Hz\n",
                best_c, best_s, best_df);
    /* fine: 0.1-sample / 0.05 Hz coherent scan around the coarse peak (the
     * returned corr is the coherent one, as before) */
    best_c = -1.0;
    s0 = best_s - 1.5;
    s1 = best_s + 1.5;
    for (start = s0; start <= s1; start += 0.1) {
        for (df = best_df - 0.4; df <= best_df + 0.4 + 1e-9; df += 0.05) {
            double c = v34_page_corr_at(&fe, templ, e_t, start, df);
            if (c > best_c) { best_c = c; best_s = start; }
        }
    }
    if (corr_out)
        *corr_out = best_c;
    s0 = fe.t0 + best_s / V34_PAGE_SR;
    v34_page_fe_free(&fe);
    return s0;
}

/* ── symbol -> bit extraction (spec 9.3 inverse, diag70) ────────────────
 * lab/rot per 2D symbol (n_sym a multiple of 8); z_prev = Z(-1) entering
 * the first interval; the first mapping frame sits at SWP position 0
 * (true for B1 and, because B1 is exactly P frames, for the data frames
 * too). Emits per HIGH mapping frame b bits: R0 (K, LSB-first from the
 * shell-unmapped rings), then per group I1,I2,I3,Q bits; low frames emit
 * b-1 bits (S_i,K - the inserted literal zero - is discarded, 9.3.1). The
 * b <= 12 case (9.3.2) has K = 0 and per-pattern I3 presence. *nbits_out
 * receives the emitted bit count. Returns the count of invalid ring
 * 8-tuples (their R0 clamped - such frames are sliced garbage and will
 * fail FCS anyway). */
static long v34_page_extract_bits(const v34_page_mode_t *m,
                                  const uint16_t *lab, const uint8_t *rot, long n_sym,
                                  int z_prev, uint8_t *bits, long *nbits_out,
                                  int *z_last)
{
    long n_frames = n_sym / 8;
    long f, fb = 0, n_bad = 0;
    int zp = z_prev, j, k;

    for (f = 0; f < n_frames; f++) {
        int is_high = (m->swp >> (m->P - 1 - (int) (f % m->P))) & 1;
        int nbf = is_high ? m->b : m->b - 1;
        int nbK = (m->K > 0) ? (is_high ? m->K : m->K - 1) : 0;
        int rings[4][2];
        long gb = fb + nbK;
        uint32_t R0;

        for (j = 0; j < 4; j++) {
            long mm = 4*f + j;
            int l0 = lab[2*mm],  r0 = rot[2*mm];
            int l1 = lab[2*mm+1], r1 = rot[2*mm+1];
            int Z = r0;
            int Ival = ((Z - zp) % 4 + 4) % 4;
            int delta = ((r1 - Z) % 4 + 4) % 4;
            zp = Z;
            bits[gb++] = (uint8_t) (delta >> 1);          /* I1 (U0 = delta&1 unused) */
            bits[gb++] = (uint8_t) (Ival & 1);            /* I2 */
            if (m->K > 0 || j < nbf - 8)
                bits[gb++] = (uint8_t) ((Ival >> 1) & 1); /* I3 (when present) */
            for (k = 0; k < m->q; k++)
                bits[gb++] = (uint8_t) ((l0 >> k) & 1);
            for (k = 0; k < m->q; k++)
                bits[gb++] = (uint8_t) ((l1 >> k) & 1);
            rings[j][0] = l0 >> m->q;
            rings[j][1] = l1 >> m->q;
        }
        if (m->K > 0) {
            R0 = nf_v34_shell_unmap(m->M, rings);
            if (R0 >= (1u << nbK)) {
                n_bad++;
                R0 &= (1u << nbK) - 1;
            }
            for (k = 0; k < nbK; k++)
                bits[fb + k] = (uint8_t) ((R0 >> k) & 1);
        }
        fb += nbf;
        if (gb != fb) {
            /* layout accounting must be exact - never ships wrong */
            fprintf(stderr, "v34 extract_bits: frame layout bug (%ld != %ld)\n",
                    gb, fb);
        }
    }
    if (z_last)
        *z_last = zp;
    if (nbits_out)
        *nbits_out = fb;
    return n_bad;
}

/* ── decision-directed tap adaptation (one chunk) ───────────────────────
 * The diag72 forensics on this capture showed the frozen-tap decode's
 * median slice distance degrading 0.23 -> 0.33 across the 23 s of burst 0
 * (slow channel drift after the 39.3 s TRN training; no audio events, no
 * tracker glitches) - and every one of the 6..8 lost ECM frames was killed
 * by 1-2 near-boundary symbols riding that raised noise floor. Fix: after
 * each chunk is sliced, refit the taps by regularized LS toward the sliced
 * decisions (block RLS with an exponential prior pulling toward the
 * current taps; lam = 8 x the average per-tap signal energy - validated
 * flat from 3x to 100x in Python, 256/256 + 149/149 across the range).
 * Only symbols whose decision margin exceeds 0.25 train the taps, so a
 * rare wrong decision cannot steer them. Targets are mapped back to the
 * tap domain by dividing out the tracker state c_i = e^{j(acc+fr*i)}*G*gc.
 * Returns 0 and updates w in place; on any failure leaves w unchanged. */
static int v34_page_adapt_taps(const v34_page_alpha_t *alpha,
                               const v34_cd_t *st, long off, long nl,
                               const int *ni, const double *marg,
                               double acc, double fr, v34_cd_t G, v34_cd_t gc,
                               const double *phi,
                               const v34_cd_t *imgsum, v34_cd_t *w)
{
    const int L = V34_PAGE_L;
    const double lam_mult = 8.0, marg_gate = 0.25;
    v34_cd_t *A = calloc((size_t) L * L, sizeof(v34_cd_t));
    v34_cd_t *bvec = calloc((size_t) L, sizeof(v34_cd_t));
    v34_cd_t wnew[V34_PAGE_L];
    double tr = 0.0, lam;
    long i, used = 0;
    int a, b, rc = -1;

    if (!A || !bvec)
        goto done;
    for (i = 0; i < nl; i++) {
        v34_cd_t c, tgt;
        long h;
        if (!(marg[i] > marg_gate))
            continue;
        c = v34_cd_mul(v34_cd_mul(v34_cd_expj(acc + fr * (double) i + phi[i]), G), gc);
        tgt = v34_cd_smul(v34_cd_mulc(alpha->pts[ni[i]], c), 1.0 / v34_cd_abs2(c));
        /* with the image branches active, the MAIN taps' desired output is
         * the total desired output minus the (frozen) image contribution */
        if (imgsum)
            tgt = v34_cd_sub(tgt, imgsum[i]);
        h = 2 * (i + off) + 1 + V34_PAGE_FUT;
        for (a = 0; a < L; a++) {
            v34_cd_t xa = st[h - a];
            bvec[a] = v34_cd_add(bvec[a], v34_cd_mulc(tgt, xa));    /* conj(xa)*tgt */
            for (b = 0; b <= a; b++)
                A[a*L + b] = v34_cd_add(A[a*L + b], v34_cd_mulc(st[h - b], xa));
        }
        used++;
    }
    if (used < 2 * L)          /* too few confident symbols - keep taps */
        goto done;
    for (a = 0; a < L; a++)
        tr += A[a*L + a].re;
    lam = lam_mult * tr / L;
    for (a = 0; a < L; a++) {
        A[a*L + a].re += lam;
        bvec[a] = v34_cd_add(bvec[a], v34_cd_smul(w[a], lam));
    }
    if (v34_page_chol_solve_n(A, bvec, wnew, L) < 0)
        goto done;
    memcpy(w, wnew, sizeof(wnew));
    rc = 0;
done:
    free(A);
    free(bvec);
    return rc;
}

/* FSE + PP complex-gain fit over the 432 knowns at timing offset tau;
 * returns the normalized residual on the PP portion, *g_out the gain. */
static double v34_page_knowns_resid(const v34_page_fe_t *fe, const nf_v34_page_eq_t *eq,
                                    double t_s, double tau, const v34_cd_t *w,
                                    const v34_cd_t *templ, v34_cd_t *g_out)
{
    v34_cd_t out[V34_PAGE_NKNOWN];
    v34_cd_t num = v34_cd(0.0, 0.0), g;
    double den = 0.0, rnum = 0.0, rden = 0.0;
    int i;

    if (v34_page_fse_chunk(fe, eq, t_s, tau, 0, V34_PAGE_NKNOWN, w, out) < 0)
        return 1e300;
    for (i = V34_PAGE_PP0; i < V34_PAGE_NKNOWN; i++) {
        num = v34_cd_add(num, v34_cd_mulc(templ[i], out[i]));   /* conj(out)*ref */
        den += v34_cd_abs2(out[i]);
    }
    g = v34_cd_smul(num, 1.0 / den);
    for (i = V34_PAGE_PP0; i < V34_PAGE_NKNOWN; i++) {
        rnum += v34_cd_abs2(v34_cd_sub(v34_cd_mul(out[i], g), templ[i]));
        rden += v34_cd_abs2(templ[i]);
    }
    if (g_out)
        *g_out = g;
    return rnum / rden;
}

/* One chunk stream's branch transform: kind 0..5 = image branch (see
 * v34_page_img_rotate), kind 6 = the cubic st*|st|^2 branch. e2 = the mean
 * |st|^2 normalization the cubic branch uses (matches training's). */
static void v34_page_branch_stream(const v34_cd_t *st, long nel, long off,
                                   long s0, const nf_v34_page_eq_t *eq,
                                   double baud, int kind, double e2,
                                   v34_cd_t *dst)
{
    long j;

    if (kind < 6) {
        v34_page_img_rotate(st, nel, off, s0, eq->img_hz, baud, kind, dst);
    } else {
        for (j = 0; j < nel; j++)
            dst[j] = v34_cd_smul(st[j], v34_cd_abs2(st[j]) / e2);
    }
}

/* Multi-branch variant of the PP fit (used when any model branch is
 * active): joint LS over the main FSE output and the nb branch outputs on
 * the 288 PP knowns. Each branch needs its own complex weight per burst -
 * the image rotations are only phase-defined up to the burst's time
 * origin, and the training-time img_hz estimate drifts a little between
 * bursts. *g_out = the main gain (the g_pp of the plain fit); gam[nb] =
 * branch weights RELATIVE to the main gain, which is exactly the
 * combination the chunk decoder applies to its per-chunk branch outputs. */
static double v34_page_knowns_fit_multi(const v34_page_fe_t *fe,
                                        const nf_v34_page_eq_t *eq,
                                        double t_s, double tau,
                                        const v34_cd_t *w,
                                        const v34_cd_t wbr[7][V34_PAGE_L],
                                        const int *kinds, int nb,
                                        const v34_cd_t *templ,
                                        v34_cd_t *g_out, v34_cd_t *gam)
{
    v34_cd_t *st, *str = NULL;
    v34_cd_t (*out)[V34_PAGE_NKNOWN] = NULL;
    v34_cd_t A[8 * 8], bv[8], z[8];
    double rnum = 0.0, rden = 0.0, tr = 0.0, e2 = 0.0;
    long off, nel, j;
    int i, b, k, l, nd = 1 + nb;
    double resid = 1e300;

    st = v34_page_fse_stream(fe, eq, t_s, tau, 0, V34_PAGE_NKNOWN, &off, &nel);
    if (!st)
        return 1e300;
    str = malloc(sizeof(*str) * (size_t) nel);
    out = malloc(sizeof(*out) * (size_t) nd);
    if (!str || !out)
        goto done;
    for (j = 0; j < nel; j++)
        e2 += v34_cd_abs2(st[j]);
    e2 = (e2 > 0.0) ? e2 / (double) nel : 1.0;
    v34_page_fse_apply(st, off, V34_PAGE_NKNOWN, w, out[0]);
    for (b = 0; b < nb; b++) {
        v34_page_branch_stream(st, nel, off, 0, eq, fe->baud, kinds[b], e2, str);
        v34_page_fse_apply(str, off, V34_PAGE_NKNOWN, wbr[b], out[1 + b]);
    }

    memset(A, 0, sizeof(A));
    memset(bv, 0, sizeof(bv));
    for (i = V34_PAGE_PP0; i < V34_PAGE_NKNOWN; i++) {
        for (k = 0; k < nd; k++) {
            bv[k] = v34_cd_add(bv[k], v34_cd_mulc(templ[i], out[k][i]));
            for (l = 0; l <= k; l++)
                A[k*8 + l] = v34_cd_add(A[k*8 + l],
                                        v34_cd_mulc(out[l][i], out[k][i]));
        }
    }
    for (k = 0; k < nd; k++)
        tr += A[k*8 + k].re;
    for (k = 0; k < nd; k++)
        A[k*8 + k].re += 1e-9 * tr;
    /* chol expects a compact nd x nd matrix */
    {
        v34_cd_t Ac[8 * 8];
        for (k = 0; k < nd; k++)
            for (l = 0; l < nd; l++)
                Ac[k*nd + l] = A[k*8 + l];
        if (v34_page_chol_solve_n(Ac, bv, z, nd) < 0 || v34_cd_abs2(z[0]) <= 0.0)
            goto done;
    }

    rnum = 0.0;
    rden = 0.0;
    for (i = V34_PAGE_PP0; i < V34_PAGE_NKNOWN; i++) {
        v34_cd_t acc = v34_cd(0.0, 0.0);
        for (k = 0; k < nd; k++)
            acc = v34_cd_add(acc, v34_cd_mul(z[k], out[k][i]));
        rnum += v34_cd_abs2(v34_cd_sub(acc, templ[i]));
        rden += v34_cd_abs2(templ[i]);
    }
    resid = rnum / rden;
    if (g_out)
        *g_out = z[0];
    if (gam) {
        double d = v34_cd_abs2(z[0]);
        for (b = 0; b < nb; b++)
            gam[b] = v34_cd_smul(v34_cd_mulc(z[1 + b], z[0]), 1.0 / d);
    }
done:
    free(st);
    free(str);
    free(out);
    return resid;
}

/* internal HDLC frame sink: count + record, then forward */
struct v34_page_frame_ctx {
    nf_v34_page_burst_t *res;
    nf_hdlc_frame_fn fn;
    void *user;
};

static void v34_page_on_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct v34_page_frame_ctx *c = user;
    nf_v34_page_burst_t *r = c->res;

    if (msg && len > 0) {
        if (!ok) {
            r->hdlc_bad++;
        } else {
            r->hdlc_ok++;
            if (len >= 4 && msg[2] == 0x06) {          /* FCD (T.4/A.3.6.1) */
                if (r->fcd_ok == 0)
                    r->fcd_first = msg[3];
                r->fcd_last = msg[3];
                r->fcd_ok++;
            } else if (len >= 3 && msg[2] == 0x86) {   /* RCP */
                r->rcp_ok++;
            }
            if (r->n_first_hdr < 2) {
                int c2 = len < 8 ? len : 8;
                memcpy(r->first_hdr[r->n_first_hdr], msg, (size_t) c2);
                r->first_hdr_len[r->n_first_hdr] = c2;
                r->n_first_hdr++;
            }
        }
    }
    if (c->fn)
        c->fn(c->user, msg, len, ok);
}

int nf_v34_page_decode_burst(const int16_t *amp, long n, double buf_t0,
                             const nf_v34_page_eq_t *eq,
                             double t_s, double t_end_max, double baud,
                             const nf_v34_pcparams_t *pp,
                             nf_hdlc_frame_fn on_frame, void *user,
                             nf_v34_page_burst_t *res)
{
    v34_page_mode_t md;
    v34_page_alpha_t *alpha = NULL;
    v34_page_fe_t fe;
    v34_cd_t templ[V34_PAGE_NKNOWN];
    v34_cd_t wt[V34_PAGE_L];
    v34_cd_t wbr[7][V34_PAGE_L];
    v34_cd_t gam[7];
    int kinds[7];
    uint8_t known840[V34_PAGE_B1BITS_MAX], b1bits[V34_PAGE_B1BITS_MAX];
    double b1_norm, tau, best_r, r;
    v34_cd_t g_pp, G;
    double fr = 0.0, acc = 0.0;
    long n_total, n_dec_max, s, end_sym, n_valid, n_df, i;
    uint16_t *lab = NULL;
    uint8_t *rot = NULL, *data_bits = NULL;
    v34_cd_t *o = NULL, *o3 = NULL, *om = NULL, *imgsum = NULL, *str = NULL;
    v34_cd_t *ob[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    int *ni = NULL;
    double *dbuf = NULL, *dsort = NULL, *mbuf = NULL, *phi = NULL;
    double *epsb = NULL, *wgtb = NULL, *smb = NULL;
    double *chunk_med = NULL;
    long n_chunk_med = 0, max_chunk_med;
    double phi_loop_tail[8];
    v34_page_pll_t pll = { 0.0, 0.0 };
    v34_cd_t b1pts[4][V34_PAGE_B1SYM_MAX];
    double avg_e2 = 0.0;
    int jitter_mode = 0;    /* latched when a chunk shows a real per-symbol
                             * phase trajectory; keeps the PLL/smoother
                             * fully inert on quiet lines otherwise */
    int nbr = 0;
    int ci, zi, b, rc = -1;

    v34_page_mode_resolve(pp, &md);
    memset(res, 0, sizeof(*res));
    memset(gam, 0, sizeof(gam));
    alpha = malloc(sizeof(*alpha));
    if (!alpha)
        return -1;
    v34_page_alpha_build(&md, alpha);
    avg_e2 = alpha->avg_e2;

    /* B1's exact 8P transmitted points for each possible differential-
     * encoder Z(-1) - the per-symbol PLL trains DATA-AIDED across B1 (a
     * decision-directed loop cannot acquire through line-rate phase jitter
     * on a dense constellation: at 10 degrees the outer points slice
     * wrong before the loop ever sees a usable error). */
    {
        int zi2;
        for (zi2 = 0; zi2 < 4; zi2++) {
            nf_v34_pc_tx_t txh;
            double bre[V34_PAGE_B1SYM_MAX], bim[V34_PAGE_B1SYM_MAX];
            if (pp)
                nf_v34_pc_tx_init_mode(&txh, pp);
            else
                nf_v34_pc_tx_init(&txh);
            txh.zprev = zi2;
            nf_v34_pc_encode(&txh, NULL, md.P, bre, bim);
            for (i = 0; i < md.b1_sym; i++)
                b1pts[zi2][i] = v34_cd(bre[i], bim[i]);
        }
    }
    v34_page_build_sspp(templ);
    b1_norm = v34_page_b1_known(&md, known840);
    for (i = 0; i < V34_PAGE_L; i++)
        wt[i] = v34_cd(eq->w_re[i], eq->w_im[i]);
    /* assemble the active model-branch set (see nf_v34_page_eq_t) */
    if (eq->img_active) {
        for (b = 0; b < 6; b++) {
            kinds[nbr] = b;
            for (i = 0; i < V34_PAGE_L; i++)
                wbr[nbr][i] = v34_cd(eq->wimg_re[b][i], eq->wimg_im[b][i]);
            nbr++;
        }
    }
    if (eq->nl_active) {
        kinds[nbr] = 6;
        for (i = 0; i < V34_PAGE_L; i++)
            wbr[nbr][i] = v34_cd(eq->wnl_re[i], eq->wnl_im[i]);
        nbr++;
    }

    if (v34_page_fe_init(&fe, amp, n, buf_t0, baud, md.fc,
                         t_s - 0.09, t_end_max + 0.10,
                         t_s, t_end_max - 0.25) < 0) {
        free(alpha);
        return -1;
    }

    /* timing-offset scan + complex gain, both fit on the 288 PP knowns
     * ONLY (diag67/68: real S/Sbar is 1.5x hotter than the template).
     * The scan itself uses the main branch only (an image at -16 dB does
     * not move the timing minimum); the final fit is joint when the image
     * branches are active. */
    best_r = 1e300;
    tau = 0.0;
    for (r = -1.2; r <= 1.2 + 1e-9; r += 0.1) {
        double rr = v34_page_knowns_resid(&fe, eq, t_s, r, wt, templ, NULL);
        if (rr < best_r) { best_r = rr; tau = r; }
    }
    {
        double t_c = tau;
        for (r = t_c - 0.12; r <= t_c + 0.12 + 1e-9; r += 0.02) {
            double rr = v34_page_knowns_resid(&fe, eq, t_s, r, wt, templ, NULL);
            if (rr < best_r) { best_r = rr; tau = r; }
        }
    }
    if (nbr > 0) {
        res->pp_resid = v34_page_knowns_fit_multi(&fe, eq, t_s, tau, wt,
                                                  (const v34_cd_t (*)[V34_PAGE_L]) wbr,
                                                  kinds, nbr, templ, &g_pp, gam);
        if (res->pp_resid >= 1e300) {           /* joint fit failed - fall back */
            nbr = 0;
            res->pp_resid = v34_page_knowns_resid(&fe, eq, t_s, tau, wt, templ, &g_pp);
        }
    } else {
        res->pp_resid = v34_page_knowns_resid(&fe, eq, t_s, tau, wt, templ, &g_pp);
    }
    res->tau_init = tau;

    /* decision-directed chunked decode of B1 + data (diag69/71): 2nd-order
     * phase/gain state (G, fr, acc) + slow tri-tau timing tracking */
    G = v34_cd_smul(g_pp, b1_norm);
    n_total = (long) ((t_end_max - t_s) * baud);
    n_dec_max = n_total - V34_PAGE_NKNOWN;
    if (n_dec_max < md.b1_sym + 8)
        goto done;
    max_chunk_med = n_dec_max / V34_PAGE_CHUNK + 2;
    chunk_med = malloc(sizeof(*chunk_med) * (size_t) max_chunk_med);
    lab = malloc(sizeof(*lab) * (size_t) n_dec_max);
    rot = malloc((size_t) n_dec_max);
    o = malloc(sizeof(*o) * V34_PAGE_CHUNK);
    o3 = malloc(sizeof(*o3) * V34_PAGE_CHUNK);
    ni = malloc(sizeof(*ni) * V34_PAGE_CHUNK);
    dbuf = malloc(sizeof(*dbuf) * V34_PAGE_CHUNK);
    dsort = malloc(sizeof(*dsort) * V34_PAGE_CHUNK);
    mbuf = malloc(sizeof(*mbuf) * V34_PAGE_CHUNK);
    phi = malloc(sizeof(*phi) * V34_PAGE_CHUNK);
    epsb = malloc(sizeof(*epsb) * V34_PAGE_CHUNK);
    wgtb = malloc(sizeof(*wgtb) * V34_PAGE_CHUNK);
    smb = malloc(sizeof(*smb) * V34_PAGE_CHUNK);
    if (!lab || !rot || !o || !o3 || !ni || !dbuf || !dsort || !mbuf || !phi ||
        !epsb || !wgtb || !smb || !chunk_med)
        goto done;
    if (nbr > 0) {
        /* chunk stream length is bounded by the full-chunk case */
        long nel_max = 2 * (V34_PAGE_CHUNK + (V34_PAGE_L/2 + 4) + (V34_PAGE_FUT/2 + 4));
        om = malloc(sizeof(*om) * V34_PAGE_CHUNK);
        imgsum = malloc(sizeof(*imgsum) * V34_PAGE_CHUNK);
        str = malloc(sizeof(*str) * (size_t) nel_max);
        if (!om || !imgsum || !str)
            goto done;
        for (b = 0; b < nbr; b++) {
            ob[b] = malloc(sizeof(*ob[b]) * V34_PAGE_CHUNK);
            if (!ob[b])
                goto done;
        }
    }

    s = V34_PAGE_NKNOWN;
    end_sym = n_total;
    ci = 0;
    while (s < n_total) {
        long s1 = s + V34_PAGE_CHUNK;
        long nl, off, nel;
        v34_cd_t num, gc;
        v34_cd_t *st;
        double den, med, th;

        if (s1 > n_total)
            s1 = n_total;
        nl = s1 - s;
        st = v34_page_fse_stream(&fe, eq, t_s, tau, s, s1, &off, &nel);
        if (!st)
            goto done;
        /* The whole per-chunk demod runs up to twice: if the first pass had
         * to apply a significant per-symbol phase trajectory (line-rate
         * phase jitter), the trajectory is applied to the T/2 stream ITSELF
         * and the chunk is re-equalized - the jitter varies WITHIN the
         * FSE's ~9 ms tap span, so correcting phase only after the FSE
         * leaves an irreducible mixed radial/tangential error (measured:
         * ~0.45 units at 10 degrees/60 Hz, far above the slicing margin).
         * On a jitter-free line the first pass's trajectory is ~0 and no
         * second pass runs. */
        {
            int rp, reproc = 0;
            double phiA_tail = 0.0;

            for (rp = 0; rp < 3; rp++) {
            v34_page_fse_apply(st, off, nl, wt, o);
            if (nbr > 0) {
                double e2 = 0.0;
                long j;
                for (j = 0; j < nel; j++)
                    e2 += v34_cd_abs2(st[j]);
                e2 = (e2 > 0.0) ? e2 / (double) nel : 1.0;
                for (b = 0; b < nbr; b++) {
                    v34_page_branch_stream(st, nel, off, s, eq, baud, kinds[b], e2, str);
                    v34_page_fse_apply(str, off, nl, wbr[b], ob[b]);
                }
                /* om = tracker-domain main output; o = combined at the current
                 * relative branch weights */
                for (i = 0; i < nl; i++) {
                    v34_cd_t d = v34_cd_mul(v34_cd_expj(acc + fr * (double) i), G);
                    v34_cd_t sum = o[i];
                    om[i] = v34_cd_mul(o[i], d);
                    for (b = 0; b < nbr; b++)
                        sum = v34_cd_add(sum, v34_cd_mul(gam[b], ob[b][i]));
                    o[i] = v34_cd_mul(sum, d);
                }
            } else {
                for (i = 0; i < nl; i++)
                    o[i] = v34_cd_mul(v34_cd_mul(o[i], v34_cd_expj(acc + fr * (double) i)), G);
            }

            /* First slicing pass, with the per-symbol phase PLL running (see
             * v34_page_pll_step - the chunk-level tracker alone cannot follow
             * power-line-rate phase jitter). Across B1 (the first 120 symbols)
             * the loop trains data-aided on B1's known content: the Z(-1)
             * hypothesis is picked by total distance over the four candidate
             * sequences. The applied phase is recorded per symbol and folded
             * into everything downstream. */
            if (s == V34_PAGE_NKNOWN && nl >= md.b1_sym) {
                double best_e = 1e300, best_thvar = 0.0;
                int best_zi = 0, zi2;
                for (zi2 = 0; zi2 < 4; zi2++) {
                    v34_page_pll_t ph2 = { 0.0, 0.0 };
                    double e = 0.0, ths = 0.0, ths2 = 0.0;
                    for (i = 0; i < md.b1_sym; i++) {
                        v34_cd_t y = v34_cd_mul(o[i], v34_cd_expj(ph2.th));
                        v34_cd_t p = b1pts[zi2][i];
                        double d2 = v34_cd_abs2(v34_cd_sub(y, p));
                        double dot = y.re * p.re + y.im * p.im;
                        double crs = y.im * p.re - y.re * p.im;
                        e += (d2 < 2.25) ? d2 : 2.25;    /* bound outliers */
                        /* aid only within 45 degrees of the prediction: real
                         * phase excursions are far smaller, while a mispredicted
                         * point (rotated 90/180 - the capture's TX does not zero
                         * its encoder state at B1) is far outside */
                        if (dot > fabs(crs))
                            v34_page_pll_step(&ph2, y, p, avg_e2);
                        ths += ph2.th;
                        ths2 += ph2.th * ph2.th;
                    }
                    if (e < best_e) {
                        best_e = e;
                        best_zi = zi2;
                        best_thvar = ths2 / md.b1_sym -
                                     (ths / md.b1_sym) * (ths / md.b1_sym);
                    }
                }
                /* jitter detection belongs HERE: the aided loop is the only
                 * phase detector that survives the dense constellation's
                 * decision wrapping (a 10-degree excursion re-slices to a
                 * NEARER point, so decision-directed estimates alias to
                 * almost nothing). The tracked loop phase's variance over
                 * B1 measures the real trajectory. */
                if (!jitter_mode && sqrt(best_thvar) > 0.04) {
                    jitter_mode = 1;
                    if (getenv("NFV34DBG"))
                        fprintf(stderr, "    [jitter detected: aided-B1 phase"
                                " rms %.3f rad]\n", sqrt(best_thvar));
                }
                if (!jitter_mode) {
                    i = 0;
                    goto plainpass;
                }
                for (i = 0; i < md.b1_sym; i++) {
                    double d;
                    phi[i] = pll.th;
                    o[i] = v34_cd_mul(o[i], v34_cd_expj(pll.th));
                    ni[i] = v34_page_slice1(alpha, o[i], &d, NULL);
                    /* same 45-degree aiding gate as the hypothesis scan */
                    {
                        v34_cd_t p = b1pts[best_zi][i];
                        double dot = o[i].re * p.re + o[i].im * p.im;
                        double crs = o[i].im * p.re - o[i].re * p.im;
                        if (dot > fabs(crs))
                            v34_page_pll_step(&pll, o[i], p, avg_e2);
                        else
                            v34_page_pll_step(&pll, o[i], alpha->pts[ni[i]], avg_e2);
                    }
                }
                i = md.b1_sym;
            } else {
                i = 0;
            }
plainpass:
            for (; i < nl; i++) {
                double d, mg;
                phi[i] = pll.th;
                o[i] = v34_cd_mul(o[i], v34_cd_expj(pll.th));
                ni[i] = v34_page_slice1(alpha, o[i], &d, &mg);
                if (!jitter_mode)
                    continue;
                /* decision-directed updates only from confident decisions - a
                 * wrong outer-point decision injects a heavily-weighted false
                 * error and can avalanche the loop */
                if (mg > 0.3)
                    v34_page_pll_step(&pll, o[i], alpha->pts[ni[i]], avg_e2);
                else
                    pll.th += pll.fr;
            }

            /* DD complex-gain fit against the sliced points, then re-slice.
             * With model branches active this generalizes to a joint (main
             * gain + branch weight) LS - the branch weights drift slowly as
             * the training-time img_hz estimate's small error accumulates,
             * and the per-chunk refit tracks that exactly. */
            gc = v34_cd(1.0, 0.0);
            if (nbr > 0) {
                v34_cd_t A2[8 * 8], bv[8], z[8];
                double tr2 = 0.0;
                int k, l, nd = 1 + nbr, ok2 = 0;

                memset(A2, 0, sizeof(A2));
                memset(bv, 0, sizeof(bv));
                for (i = 0; i < nl; i++) {
                    v34_cd_t d = v34_cd_mul(v34_cd_expj(acc + fr * (double) i + phi[i]), G);
                    v34_cd_t u[8];
                    v34_cd_t p = alpha->pts[ni[i]];
                    u[0] = v34_cd_mul(om[i], v34_cd_expj(phi[i]));
                    for (b = 0; b < nbr; b++)
                        u[1 + b] = v34_cd_mul(ob[b][i], d);
                    for (k = 0; k < nd; k++) {
                        bv[k] = v34_cd_add(bv[k], v34_cd_mulc(p, u[k]));
                        for (l = 0; l <= k; l++)
                            A2[k*8 + l] = v34_cd_add(A2[k*8 + l], v34_cd_mulc(u[l], u[k]));
                    }
                }
                for (k = 0; k < nd; k++)
                    tr2 += A2[k*8 + k].re;
                for (k = 0; k < nd; k++)
                    A2[k*8 + k].re += 1e-9 * tr2;
                {
                    v34_cd_t Ac[8 * 8];
                    for (k = 0; k < nd; k++)
                        for (l = 0; l < nd; l++)
                            Ac[k*nd + l] = A2[k*8 + l];
                    if (v34_page_chol_solve_n(Ac, bv, z, nd) == 0 &&
                        v34_cd_abs2(z[0]) > 0.0) {
                        double dd = v34_cd_abs2(z[0]);
                        gc = z[0];
                        for (b = 0; b < nbr; b++)
                            gam[b] = v34_cd_smul(v34_cd_mulc(z[1 + b], z[0]), 1.0 / dd);
                        ok2 = 1;
                    }
                }
                /* refined combined output at the updated weights, and the
                 * pre-tracker branch contribution for adaptation/timing */
                for (i = 0; i < nl; i++) {
                    v34_cd_t d = v34_cd_mul(v34_cd_expj(acc + fr * (double) i), G);
                    v34_cd_t isum = v34_cd(0.0, 0.0);
                    for (b = 0; b < nbr; b++)
                        isum = v34_cd_add(isum, v34_cd_mul(gam[b], ob[b][i]));
                    imgsum[i] = isum;
                    o[i] = v34_cd_mul(v34_cd_mul(v34_cd_add(om[i], v34_cd_mul(isum, d)),
                                                 ok2 ? gc : v34_cd(1.0, 0.0)),
                                      v34_cd_expj(phi[i]));
                }
                if (!ok2) {
                    /* joint refit failed - scalar fallback on the combined o */
                    num = v34_cd(0.0, 0.0);
                    den = 0.0;
                    for (i = 0; i < nl; i++) {
                        num = v34_cd_add(num, v34_cd_mulc(alpha->pts[ni[i]], o[i]));
                        den += v34_cd_abs2(o[i]);
                    }
                    gc = v34_cd_smul(num, 1.0 / den);
                    for (i = 0; i < nl; i++)
                        o[i] = v34_cd_mul(o[i], gc);
                }
                for (i = 0; i < nl; i++)
                    ni[i] = v34_page_slice1(alpha, o[i], &dbuf[i], &mbuf[i]);
            } else {
                num = v34_cd(0.0, 0.0);
                den = 0.0;
                for (i = 0; i < nl; i++) {
                    num = v34_cd_add(num, v34_cd_mulc(alpha->pts[ni[i]], o[i]));
                    den += v34_cd_abs2(o[i]);
                }
                gc = v34_cd_smul(num, 1.0 / den);
                for (i = 0; i < nl; i++) {
                    o[i] = v34_cd_mul(o[i], gc);
                    ni[i] = v34_page_slice1(alpha, o[i], &dbuf[i], &mbuf[i]);
                }
            }

            if (nl >= 8) {
                for (i = nl - 8; i < nl; i++)
                    phi_loop_tail[i - (nl - 8)] = phi[i];
            }

            /* Non-causal phase smoothing: even a wide causal PLL leaves
             * ~(f/fn)^2 of line-rate phase jitter; a centred, |p|^2-weighted
             * average of the per-symbol decision-directed phase errors
             * estimates the residual trajectory with no loop lag. Two
             * smooth-and-reslice passes; on a jitter-free line the smoothed
             * correction is noise-averaged to ~0 and nothing changes. */
            {
                int it2;
                static const int Ks[4] = { 7, 4, 2, 2 };
                for (it2 = 0; it2 < 4; it2++) {
                    const int K = Ks[it2];
                    for (i = 0; i < nl; i++) {
                        v34_cd_t p = alpha->pts[ni[i]];
                        double p2 = v34_cd_abs2(p);
                        double e = (o[i].im * p.re - o[i].re * p.im) /
                                   (p2 > 0.0 ? p2 : 1.0);
                        if (e > 0.5) e = 0.5;
                        if (e < -0.5) e = -0.5;
                        epsb[i] = e;
                        /* confident decisions dominate the phase estimate */
                        wgtb[i] = p2 * (mbuf[i] > 0.3 ? 1.0 : 0.02);
                    }
                    for (i = 0; i < nl; i++) {
                        double se = 0.0, sw = 0.0;
                        long j0 = (i - K < 0) ? 0 : i - K;
                        long j1 = (i + K >= nl) ? nl - 1 : i + K;
                        long j;
                        for (j = j0; j <= j1; j++) {
                            se += epsb[j] * wgtb[j];
                            sw += wgtb[j];
                        }
                        smb[i] = (sw > 0.0) ? se / sw : 0.0;
                    }
                    /* engage the jitter path only on a REAL trajectory: on
                     * a quiet line the smoothed correction is estimation
                     * noise, and blindly applying it costs the most
                     * marginal symbols (found by the capture regression
                     * dropping one frame). Discriminate by BOTH size and
                     * lag-8 autocorrelation: a line-rate jitter trajectory
                     * stays correlated across 8 symbols (rho ~ 0.6 at
                     * 60 Hz) while the K=4 smoother's noise decorrelates
                     * (triangular overlap rho ~ 0.1). */
                    if (!jitter_mode) {
                        double v0 = 0.0, v8 = 0.0;
                        for (i = 0; i < nl; i++)
                            v0 += smb[i] * smb[i];
                        for (i = 0; i + 8 < nl; i++)
                            v8 += smb[i] * smb[i + 8];
                        v0 /= (double) nl;
                        v8 /= (double) (nl - 8);
                        if (sqrt(v0) < 0.02 || v8 < 0.4 * v0)
                            break;
                        jitter_mode = 1;
                    }
                    for (i = 0; i < nl; i++) {
                        o[i] = v34_cd_mul(o[i], v34_cd_expj(-smb[i]));
                        phi[i] -= smb[i];
                    }
                    for (i = 0; i < nl; i++)
                        ni[i] = v34_page_slice1(alpha, o[i], &dbuf[i], &mbuf[i]);
                }
                /* the smoother corrected phase the causal loop had not seen
                 * yet - hand the end-of-chunk correction to the next chunk's
                 * loop state so it does not start behind */
                if (nl >= 8) {
                    double dtail = 0.0;
                    for (i = nl - 8; i < nl; i++)
                        dtail += phi[i] - phi_loop_tail[i - (nl - 8)];
                    pll.th += dtail / 8.0;
                }
            }

                if (rp == 2)
                    break;
                {
                    double prms = 0.0;
                    for (i = 0; i < nl; i++)
                        prms += phi[i] * phi[i];
                    prms = sqrt(prms / (double) nl);
                    if (prms < 0.02)
                        break;
                }
                {
                    long j;
                    for (j = 0; j < nel; j++) {
                        /* element j's own T/2 position corresponds to
                         * symbol (j-1)/2 - off (the trained cursor sits
                         * FUT taps into the window, i.e. on the symbol's
                         * own sample) */
                        double si2 = ((double) (j - 1)) / 2.0 - (double) off;
                        double a2;
                        long i0;
                        if (si2 <= 0.0) {
                            a2 = phi[0];
                        } else if (si2 >= (double) (nl - 1)) {
                            a2 = phi[nl - 1];
                        } else {
                            i0 = (long) si2;
                            a2 = phi[i0] + (phi[i0 + 1] - phi[i0]) *
                                 (si2 - (double) i0);
                        }
                        st[j] = v34_cd_mul(st[j], v34_cd_expj(a2));
                    }
                }
                phiA_tail = 0.0;
                for (i = nl - 8 > 0 ? nl - 8 : 0; i < nl; i++)
                    phiA_tail += phi[i];
                phiA_tail /= 8.0;
                reproc = 1;
                pll.th = 0.0;
                pll.fr = 0.0;
            }
            if (reproc)
                pll.th += phiA_tail;
        }
        memcpy(dsort, dbuf, sizeof(*dbuf) * (size_t) nl);
        med = v34_page_median(dsort, nl);
        if (getenv("NFV34DBG")) {
            long nerr = 0;
            double st2r = 0.0, st2t = 0.0;
            for (i = 0; i < nl; i++) {
                v34_cd_t p = alpha->pts[ni[i]];
                double pr = sqrt(v34_cd_abs2(p)) + 1e-12;
                double rad = sqrt(v34_cd_abs2(o[i])) - pr;
                double tan2 = v34_cd_abs2(v34_cd_sub(o[i], p)) - rad * rad;
                st2r += rad * rad;
                st2t += (tan2 > 0.0) ? tan2 : 0.0;
                if (mbuf[i] < 0.1)
                    nerr++;
            }
            fprintf(stderr, "    [chunk s=%ld nl=%ld] med=%.3f lowmarg=%ld"
                    " gc=(%.3f,%.3f) pll.th=%.3f fr=%.5f rad=%.3f tan=%.3f jm=%d\n",
                    s, nl, med, nerr, gc.re, gc.im, pll.th, pll.fr,
                    sqrt(st2r / (double) nl), sqrt(st2t / (double) nl), jitter_mode);
        }
        if (med > 0.75) {                   /* carrier drop = burst end */
            free(st);
            end_sym = s;
            break;
        }
        /* record decode quality (the carrier-drop chunk above is
         * deliberately excluded - it measures silence, not the line) */
        if (n_chunk_med < max_chunk_med)
            chunk_med[n_chunk_med++] = med;
        for (i = 0; i < nl; i++) {
            lab[s - V34_PAGE_NKNOWN + i] = (uint16_t) (ni[i] >> 2);
            rot[s - V34_PAGE_NKNOWN + i] = (uint8_t) (ni[i] & 3);
        }
        th = atan2(gc.im, gc.re);

        /* slow DD timing: every 2nd chunk, evaluate at tau-δ/tau/tau+δ on
         * the decimated chunk, parabolic step toward the minimum */
        if (ci & 1) {
            double mets[3];
            int t;
            for (t = 0; t < 3; t++) {
                double dt = (t - 1) * 0.15;
                double sum = 0.0, d;
                long cnt = 0;
                if (v34_page_fse_chunk(&fe, eq, t_s, tau + dt, s, s1, wt, o3) < 0) {
                    free(st);
                    goto done;
                }
                for (i = 0; i < nl; i += 2) {
                    v34_cd_t oi = o3[i];
                    v34_cd_t v;
                    if (nbr > 0)         /* branch part barely moves with dt */
                        oi = v34_cd_add(oi, imgsum[i]);
                    v = v34_cd_mul(v34_cd_mul(v34_cd_mul(oi,
                                    v34_cd_expj(acc + fr * (double) i + phi[i])), G), gc);
                    v34_page_slice1(alpha, v, &d, NULL);
                    sum += d * d;
                    cnt++;
                }
                mets[t] = sum / (double) cnt;
            }
            den = mets[2] - 2.0 * mets[1] + mets[0];
            if (den > 1e-12) {
                double step = 0.4 * (-0.15 * (mets[2] - mets[0]) / (2.0 * den));
                if (step > 0.05) step = 0.05;
                if (step < -0.05) step = -0.05;
                tau += step;
            }
        }
        /* slow DD tap adaptation (see v34_page_adapt_taps): uses this
         * chunk's stream/decisions at the pre-update tracker state */
        (void) v34_page_adapt_taps(alpha, st, off, nl, ni, mbuf, acc, fr, G, gc, phi,
                                   nbr > 0 ? imgsum : NULL, wt);
        free(st);
        G = v34_cd_mul(G, gc);
        if (jitter_mode)
            pll.th -= th;  /* gc's phase is folded into G now - the PLL
                            * measured it too, so back it out */
        acc += fr * (double) nl;
        fr += 0.5 * th / (double) nl;
        s = s1;
        ci++;
    }
    res->tau_final = tau;
    n_valid = end_sym - V34_PAGE_NKNOWN;
    res->nsym = n_valid;
    res->t_end = t_s + (double) end_sym / baud;
    if (n_chunk_med > 0)
        res->med_dist = v34_page_median(chunk_med, n_chunk_med);
    res->b1_bits = md.b1_bits;
    if (n_valid < md.b1_sym + 8)
        goto done;

    /* B1: brute-force the differential encoder's Z(-1) against the known
     * scrambled-ones content (a full-length match is unmistakable) */
    {
        int best_z = 0, best_score = -1, z_after = 0;
        long dbits = 0;
        for (zi = 0; zi < 4; zi++) {
            int zl, score = 0;
            v34_page_extract_bits(&md, lab, rot, md.b1_sym, zi, b1bits, NULL, &zl);
            for (i = 0; i < md.b1_bits; i++)
                if (b1bits[i] == known840[i])
                    score++;
            if (score > best_score) {
                best_score = score;
                best_z = zi;
                z_after = zl;
            }
        }
        res->zm1 = best_z;
        res->b1_match = best_score;

        n_df = (n_valid - md.b1_sym) / 8;
        data_bits = malloc((size_t) (n_df * md.b) + 1);
        if (!data_bits)
            goto done;
        res->bad_r0 = v34_page_extract_bits(&md, lab + md.b1_sym, rot + md.b1_sym,
                                            n_df * 8, z_after, data_bits, &dbits,
                                            NULL);

        /* continuous descramble (GPC) seeded by B1's known scrambled stream
         * (descramble-and-discard, exactly as the TX side ran it), then HDLC */
        {
            nf_v34_scrambler_t descr;
            nf_hdlc_rx_t hdlc;
            struct v34_page_frame_ctx fctx;

            fctx.res = res;
            fctx.fn = on_frame;
            fctx.user = user;
            nf_v34_scrambler_init(&descr, 1);
            nf_hdlc_rx_init(&hdlc, 1, v34_page_on_frame, &fctx);
            for (i = 0; i < md.b1_bits; i++)
                (void) nf_v34_descramble_bit(&descr, known840[i]);
            for (i = 0; i < dbits; i++)
                nf_hdlc_rx_put_bit(&hdlc, nf_v34_descramble_bit(&descr, data_bits[i]));
        }
    }
    rc = 0;
done:
    free(lab); free(rot); free(data_bits);
    free(o); free(o3); free(ni); free(dbuf); free(dsort); free(mbuf); free(phi);
    free(epsb); free(wgtb); free(smb);
    free(om); free(imgsum); free(str); free(chunk_med);
    for (b = 0; b < 6; b++)
        free(ob[b]);
    free(alpha);
    v34_page_fe_free(&fe);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════
 * transmitter (phase A: batch TX signal primitives) - see nf_v34.h.
 * Loopback-validated against the capture-validated receivers above
 * (check-v34: txinfo/txcc/txsig/txpage). Symbol-sequence conventions
 * mirror the Python forward encoders that matched the real capture at
 * 0.92-0.96 correlation (diag21 S/Sbar/PP, diag51 TRN, diag56 B1/data
 * with the V0-only-at-m==0-mod-2P fix, diag40 control channel).
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── exact-rational passband QAM modulator ──────────────────────────────
 * Symbol period = num/den samples; for symbol k centred at sample
 * c = at + k*num/den, the pulse argument at integer sample s is
 * (s-c)*den/num symbol periods = ((s-at)*den - k*num)/num - an integer
 * multiple of 1/num, so an RRC table at 1/num-symbol resolution is EXACT
 * (no fractional interpolation, no accumulated drift). The carrier is
 * cnum/cden cycles per sample, phase = 2*pi*((s mod cden)*cnum mod cden)/
 * cden - exact for all s. */

static double v34_tx_rrc(double t, double beta)   /* t in symbol periods */
{
    if (fabs(t) < 1e-9)
        return 1.0 - beta + 4.0 * beta / M_PI;
    if (fabs(fabs(4.0 * beta * t) - 1.0) < 1e-9)
        return (beta / sqrt(2.0)) * ((1.0 + 2.0/M_PI) * sin(M_PI/(4.0*beta)) +
                                     (1.0 - 2.0/M_PI) * cos(M_PI/(4.0*beta)));
    return (sin(M_PI*t*(1.0 - beta)) + 4.0*beta*t*cos(M_PI*t*(1.0 + beta))) /
           (M_PI*t*(1.0 - 16.0*beta*beta*t*t));
}

static void v34_tx_add_sample(int16_t *amp, long n, long s, double v)
{
    long q;

    if (s < 0 || s >= n)
        return;
    v += (double) amp[s];
    q = (long) (v >= 0.0 ? v + 0.5 : v - 0.5);
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    amp[s] = (int16_t) q;
}

static void v34_tx_qam(int16_t *amp, long n, long at,
                       const double *re, const double *im, long nsym,
                       int num, int den, double beta, int span,
                       int cnum, int cden, double gain)
{
    int half = span * num;
    long L, base, k, s;
    double *g, *bbre, *bbim;
    double g0;
    int j;

    if (nsym <= 0)
        return;
    /* segment sample range covered by any pulse tail */
    base = at - (long) ((double) half / den) - 2;
    L = (long) (((double) (nsym - 1) * num) / den) + 2 * ((long) ((double) half / den) + 2) + 3;

    g = malloc(sizeof(double) * (size_t) (2 * half + 1));
    bbre = calloc((size_t) L, sizeof(double));
    bbim = calloc((size_t) L, sizeof(double));
    if (!g || !bbre || !bbim) {
        free(g); free(bbre); free(bbim);
        return;
    }
    for (j = 0; j <= 2 * half; j++)
        g[j] = v34_tx_rrc((double) (j - half) / num, beta);
    g0 = g[half];
    for (j = 0; j <= 2 * half; j++)
        g[j] /= g0;

    for (k = 0; k < nsym; k++) {
        /* samples s with |(s-at)*den - k*num| <= half */
        long lo = at + (long) floor(((double) k * num - half) / den) + 1;
        long hi = at + (long) floor(((double) k * num + half) / den);
        for (s = lo; s <= hi; s++) {
            long idx = (s - at) * den - k * num;   /* |idx| <= half */
            double w = g[half + idx];
            bbre[s - base] += re[k] * w;
            bbim[s - base] += im[k] * w;
        }
    }

    for (s = 0; s < L; s++) {
        long abs_s = base + s;
        long ph;
        double c, sn;
        if (abs_s < 0)
            continue;
        ph = ((abs_s % cden) * cnum) % cden;
        c = cos(2.0 * M_PI * (double) ph / cden);
        sn = sin(2.0 * M_PI * (double) ph / cden);
        v34_tx_add_sample(amp, n, abs_s, gain * (bbre[s] * c - bbim[s] * sn));
    }
    free(g); free(bbre); free(bbim);
}

/* RMS the modulator above produces per unit symbol RMS and unit gain:
 * E|passband|^2 = E|sym|^2/2 * (den/num) * sum_j g(j/num)^2 / num. Used to
 * self-calibrate the answer role's 1800 Hz guard tone level. */
static double v34_tx_qam_rms_gain(int num, int den, double beta, int span)
{
    int half = span * num, j;
    double g0 = v34_tx_rrc(0.0, beta);
    double s2 = 0.0;

    for (j = -half; j <= half; j++) {
        double g = v34_tx_rrc((double) j / num, beta) / g0;
        s2 += g * g;
    }
    return sqrt(0.5 * s2 * den / ((double) num * num));
}

/* ── Phase 2 tones and line probing (10.1.2.1/.2/.4, Table 17) ────────── */

void nf_v34_tone_tx(int16_t *amp, long n, long at, long dur,
                    int is_answer, long reversal_at, double level)
{
    long s;
    int cnum = is_answer ? 6 : 3;      /* 2400 or 1200 Hz = cnum/20 cycles */
    double guard = is_answer ? level * pow(10.0, -6.0 / 20.0) : 0.0;

    for (s = at; s < at + dur && s < n; s++) {
        long ph = ((s % 20) * cnum) % 20;
        double v = level * cos(2.0 * M_PI * (double) ph / 20.0);
        if (reversal_at >= 0 && s >= reversal_at)
            v = -v;
        if (is_answer) {
            long gph = ((s % 40) * 9) % 40;          /* 1800 Hz = 9/40 */
            v += guard * cos(2.0 * M_PI * (double) gph / 40.0);
        }
        v34_tx_add_sample(amp, n, s, v);
    }
}

void nf_v34_probe_tx(int16_t *amp, long n, long at, long dur,
                     int is_l1, double level_rms)
{
    /* Table 17/V.34: tone index = multiple of 150 Hz; 900(6), 1200(8),
     * 1800(12), 2400(16) omitted; phi = 180 deg where marked. */
    static const int mult[21] =
        { 1, 2, 3, 4, 5, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
    static const int phi180[21] =
        { 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0 };
    double a = level_rms / sqrt(21.0 / 2.0);   /* per-cosine amplitude */
    long s;
    int k;

    if (is_l1)
        a *= pow(10.0, 6.0 / 20.0);            /* L1 = L2 + 6 dB */
    for (s = at; s < at + dur && s < n; s++) {
        double v = 0.0;
        for (k = 0; k < 21; k++) {
            /* 150*mult Hz = 3*mult/160 cycles/sample, exact */
            long ph = ((s % 160) * 3 * mult[k]) % 160;
            double c = cos(2.0 * M_PI * (double) ph / 160.0);
            v += phi180[k] ? -a * c : a * c;
        }
        v34_tx_add_sample(amp, n, s, v);
    }
}

/* ── INFO sequence TX (10.1.2.3, Tables 14/22) ────────────────────────── */

/* Mirror of v34_info_parse: fields -> frame bits (fill+sync+info+CRC+fill).
 * Returns the bit count (65 for INFO0, 67 for INFOh). */
static int v34_info_build_bits(const nf_v34_info_frame_t *f, uint8_t *b)
{
    int info_len = f->is_infoh ? 19 : 17;
    int crc_at = 12 + info_len;
    uint16_t crc;
    int i;

    memset(b, 0, (size_t) (crc_at + 20));
    for (i = 0; i < V34_INFO_SYNC_LEN; i++)
        b[i] = v34_info_sync[i];
    if (f->is_infoh) {
        for (i = 0; i < 3; i++) b[12 + i] = (uint8_t) ((f->power_reduction >> i) & 1);
        for (i = 0; i < 7; i++) b[15 + i] = (uint8_t) ((f->trn_len >> i) & 1);
        b[22] = (uint8_t) (f->high_carrier & 1);
        for (i = 0; i < 4; i++) b[23 + i] = (uint8_t) ((f->preemph_idx >> i) & 1);
        for (i = 0; i < 3; i++) b[27 + i] = (uint8_t) ((f->symrate_idx >> i) & 1);
        b[30] = (uint8_t) (f->trn_16pt & 1);
    } else {
        b[12] = (uint8_t) (f->sr2743 & 1);
        b[13] = (uint8_t) (f->sr2800 & 1);
        b[14] = (uint8_t) (f->sr3429 & 1);
        b[15] = (uint8_t) (f->low3000 & 1);
        b[16] = (uint8_t) (f->high3000 & 1);
        b[17] = (uint8_t) (f->low3200 & 1);
        b[18] = (uint8_t) (f->high3200 & 1);
        b[19] = (uint8_t) (f->allow_3429 & 1);
        b[20] = (uint8_t) (f->can_reduce_power & 1);
        for (i = 0; i < 3; i++) b[21 + i] = (uint8_t) ((f->max_sr_diff >> i) & 1);
        b[24] = (uint8_t) (f->from_cme & 1);
        b[25] = (uint8_t) (f->support_1664pt & 1);
        for (i = 0; i < 2; i++) b[26 + i] = (uint8_t) ((f->clock_source >> i) & 1);
        b[28] = (uint8_t) (f->info0_ack & 1);
    }
    crc = v34_info_crc_bits(b + 12, info_len);
    for (i = 0; i < 16; i++)
        b[crc_at + i] = (uint8_t) ((crc >> i) & 1);
    for (i = 0; i < 4; i++)
        b[crc_at + 16 + i] = 1;                     /* trailing fill 1111 */
    return crc_at + 20;
}

#define V34_CC_NUM 40      /* 600 baud: symbol period 40/3 samples */
#define V34_CC_DEN 3
#define V34_CC_BETA 0.3
#define V34_CC_SPAN 8

/* modulate 600-baud symbols on the role's carrier; answer role adds the
 * 1800 Hz guard tone 6 dB below the modulated signal's RMS (spec: carrier
 * -1 dB, guard -7 dB relative nominal). */
static void v34_tx_cc_qam(int16_t *amp, long n, long at,
                          const double *re, const double *im, long nsym,
                          int is_answer, double gain)
{
    int cnum = is_answer ? 6 : 3;      /* 2400 / 1200 Hz over cden 20 */

    v34_tx_qam(amp, n, at, re, im, nsym,
               V34_CC_NUM, V34_CC_DEN, V34_CC_BETA, V34_CC_SPAN,
               cnum, 20, gain);
    if (is_answer) {
        double srms = 0.0;
        long k;
        for (k = 0; k < nsym; k++)
            srms += re[k]*re[k] + im[k]*im[k];
        srms = sqrt(srms / (double) nsym) * gain *
               v34_tx_qam_rms_gain(V34_CC_NUM, V34_CC_DEN, V34_CC_BETA, V34_CC_SPAN);
        {
            double ga = srms * sqrt(2.0) * pow(10.0, -6.0 / 20.0);
            long dur = (long) (((double) nsym * V34_CC_NUM) / V34_CC_DEN) + 2;
            long s;
            for (s = at; s < at + dur && s < n; s++) {
                long ph = ((s % 40) * 9) % 40;
                v34_tx_add_sample(amp, n, s, ga * cos(2.0 * M_PI * (double) ph / 40.0));
            }
        }
    }
}

long nf_v34_info_tx(const nf_v34_info_frame_t *f, int is_answer,
                    int16_t *amp, long n, long at, double gain)
{
    uint8_t bits[80];
    double re[80], im[80];
    int nbits = v34_info_build_bits(f, bits);
    double cr = 1.0, ci = 0.0;
    long nsym = 0;
    int i;

    /* one point at arbitrary carrier phase, then DPSK: bit 1 = 180 deg */
    re[nsym] = cr; im[nsym] = ci; nsym++;
    for (i = 0; i < nbits; i++) {
        if (bits[i]) { cr = -cr; ci = -ci; }
        re[nsym] = cr; im[nsym] = ci; nsym++;
    }
    v34_tx_cc_qam(amp, n, at, re, im, nsym, is_answer, gain);
    return nsym;
}

/* ── control-channel TX (10.2.4, Tables 23/24) ────────────────────────── */

void nf_v34_cc_tx_init(nf_v34_cc_tx_t *s, int is_call_modem)
{
    memset(s, 0, sizeof(*s));
    nf_v34_scrambler_init(&s->scr, is_call_modem);
    s->is_call = is_call_modem;
    v34_quarter_table_ensure();          /* 2400-mode base-point selection */
}

void nf_v34_cc_tx_set_rate(nf_v34_cc_tx_t *s, int rate_2400)
{
    s->cc_rate = rate_2400 ? 1 : 0;
}

void nf_v34_cc_tx_free(nf_v34_cc_tx_t *s)
{
    free(s->re);
    free(s->im);
    s->re = s->im = NULL;
    s->nsym = s->cap = 0;
}

static int v34_cc_tx_push(nf_v34_cc_tx_t *s, double re, double im)
{
    if (s->nsym >= s->cap) {
        long nc = s->cap ? s->cap * 2 : 256;
        double *nr = realloc(s->re, sizeof(double) * (size_t) nc);
        double *ni = realloc(s->im, sizeof(double) * (size_t) nc);
        if (nr) s->re = nr;
        if (ni) s->im = ni;
        if (!nr || !ni)
            return -1;
        s->cap = nc;
    }
    s->re[s->nsym] = re;
    s->im[s->nsym] = im;
    s->nsym++;
    return 0;
}

/* point 0 of the quarter-superconstellation rotated CLOCKWISE by zn*90 deg */
static void v34_cc_point(int zn, double *re, double *im)
{
    int r = 1, m = 1;
    v34_rotate_cw(&r, &m, zn);
    *re = r;
    *im = m;
}

/* one scrambled dibit -> one differentially-encoded 4-point symbol */
static int v34_cc_tx_dibit(nf_v34_cc_tx_t *s, int i1_raw, int i2_raw)
{
    int i1 = nf_v34_scramble_bit(&s->scr, i1_raw & 1);
    int i2 = nf_v34_scramble_bit(&s->scr, i2_raw & 1);
    double re, im;

    s->zn = (s->zn + i1 + 2 * i2) & 3;
    v34_cc_point(s->zn, &re, &im);
    return v34_cc_tx_push(s, re, im);
}

int nf_v34_cc_tx_pph(nf_v34_cc_tx_t *s)
{
    /* eq 10-2 with the misprint corrected as in spandsp's
     * make_v34_probe_signals.c: PPh(2k+I) = e^{j*pi*(2k(k-I)+1)/4}
     * (the printed "2k(k-1)+1" would make PPh independent of I).
     * Scaled by sqrt(2) to the 4-point constellation's energy. */
    int i;

    for (i = 0; i < 32; i++) {
        int k = i / 2, I = i % 2;
        double th = M_PI * (2.0 * k * (k - I) + 1.0) / 4.0;
        if (v34_cc_tx_push(s, sqrt(2.0) * cos(th), sqrt(2.0) * sin(th)) < 0)
            return -1;
    }
    return 0;
}

int nf_v34_cc_tx_alt(nf_v34_cc_tx_t *s, int nsym)
{
    int i;

    for (i = 0; i < nsym; i++)
        if (v34_cc_tx_dibit(s, 0, 1) < 0)   /* alternating 0/1, I1 first */
            return -1;
    return 0;
}

int nf_v34_cc_tx_e(nf_v34_cc_tx_t *s)
{
    int i;

    for (i = 0; i < 10; i++)                /* 20 scrambled ones */
        if (v34_cc_tx_dibit(s, 1, 1) < 0)
            return -1;
    return 0;
}

int nf_v34_cc_tx_sh(nf_v34_cc_tx_t *s)
{
    /* 10.2.4: "Signal Sh is transmitted by alternating between point 0 of the
     * quarter-superconstellation and the same point rotated counterclockwise
     * by 90 degrees. Signal S̄h ... between point 0 rotated by 180 degrees and
     * point 0 rotated counterclockwise by 270 degrees. Sh shall end with ...
     * CCW 90 [= (-1,1)]; S̄h shall begin with ... 180 [= (-1,-1)]." Point 0 =
     * (1,1). These are the exact same point patterns as the primary channel's
     * S/S̄ (see v34_page_build_sspp), transmitted as direct points (no
     * differential encoder / scrambler, like PPh). 24T Sh + 8T S̄h. */
    int k;

    for (k = 0; k < 24; k++)                 /* Sh: (1,1) even, (-1,1) odd */
        if (v34_cc_tx_push(s, (k & 1) ? -1.0 : 1.0, 1.0) < 0)
            return -1;
    for (k = 0; k < 8; k++)                  /* S̄h: (-1,-1) even, (1,-1) odd */
        if (v34_cc_tx_push(s, (k & 1) ? 1.0 : -1.0, -1.0) < 0)
            return -1;
    return 0;
}

int nf_v34_cc_tx_ac(nf_v34_cc_tx_t *s, int nsym)
{
    /* 10.2.4.1: AC alternates point 0 = (1,1) and point 0 rotated 180 deg =
     * (-1,-1). Direct points. */
    int k;

    for (k = 0; k < nsym; k++)
        if (v34_cc_tx_push(s, (k & 1) ? -1.0 : 1.0, (k & 1) ? -1.0 : 1.0) < 0)
            return -1;
    return 0;
}

/* one 2400-bit/s user-data quad -> one 16-point symbol (10.2.4): the two
 * differential I bits rotate Zn exactly as at 1200; the two Q bits select the
 * quarter-superconstellation base label 2*Q2+Q1, which is then rotated
 * clockwise by Zn*90. All four bits pass through the scrambler in time order
 * I1,I2,Q1,Q2. */
static int v34_cc_tx_quad(nf_v34_cc_tx_t *s, int b_i1, int b_i2,
                          int b_q1, int b_q2)
{
    int i1 = nf_v34_scramble_bit(&s->scr, b_i1 & 1);
    int i2 = nf_v34_scramble_bit(&s->scr, b_i2 & 1);
    int q1 = nf_v34_scramble_bit(&s->scr, b_q1 & 1);
    int q2 = nf_v34_scramble_bit(&s->scr, b_q2 & 1);
    int base = 2 * q2 + q1;
    int re = nf_v34_quarter_table[base].re, im = nf_v34_quarter_table[base].im;

    s->zn = (s->zn + i1 + 2 * i2) & 3;
    v34_rotate_cw(&re, &im, s->zn);
    return v34_cc_tx_push(s, re, im);
}

int nf_v34_cc_tx_bits(nf_v34_cc_tx_t *s, const uint8_t *bits, long nbits)
{
    long i;

    if (s->cc_rate) {                    /* 2400 bit/s: 4 bits/symbol */
        if (nbits & 3)
            return -1;
        for (i = 0; i < nbits; i += 4)
            if (v34_cc_tx_quad(s, bits[i], bits[i+1], bits[i+2], bits[i+3]) < 0)
                return -1;
        return 0;
    }
    if (nbits & 1)                       /* 1200 bit/s: 2 bits/symbol */
        return -1;
    for (i = 0; i < nbits; i += 2)
        if (v34_cc_tx_dibit(s, bits[i], bits[i + 1]) < 0)
            return -1;
    return 0;
}

/* fields -> MPh frame bits per Table 23 (88 bits) / Table 24 (188 bits);
 * exact mirror of the validated nf_v34_mp_rx_t parser (17-ones sync, start
 * bits every 17 data bits skipped from the CRC, CRC-16 no complement). */
static int v34_mph_build_bits(const nf_v34_mph_fields_t *f, uint8_t *b)
{
    int n = f->type ? 188 : 88;
    uint16_t crc = 0xFFFF;
    int i, g, ngroups;

    memset(b, 0, (size_t) n);
    for (i = 0; i < 17; i++)
        b[i] = 1;                                    /* frame sync */
    b[18] = (uint8_t) (f->type & 1);
    for (i = 0; i < 4; i++) b[20 + i] = (uint8_t) ((f->max_rate >> i) & 1);
    b[27] = (uint8_t) (f->cc_rate & 1);
    b[29] = (uint8_t) (f->trellis_size & 1);
    b[30] = (uint8_t) ((f->trellis_size >> 1) & 1);
    b[31] = (uint8_t) (f->nonlinear & 1);
    b[32] = (uint8_t) (f->shaping & 1);
    b[33] = (uint8_t) (f->ack & 1);
    for (i = 0; i < 15; i++) b[35 + i] = (uint8_t) ((f->rate_mask >> i) & 1);
    b[50] = (uint8_t) (f->asym_enable & 1);
    if (f->type) {
        static const int h_at[6] = { 52, 69, 86, 103, 120, 137 };
        for (g = 0; g < 3; g++) {
            for (i = 0; i < 16; i++) {
                b[h_at[2*g] + i]   = (uint8_t) (((uint16_t) f->h_re[g] >> i) & 1);
                b[h_at[2*g+1] + i] = (uint8_t) (((uint16_t) f->h_im[g] >> i) & 1);
            }
        }
    }
    ngroups = f->type ? 9 : 3;
    for (g = 0; g < ngroups; g++)
        for (i = 0; i < 16; i++)
            crc = v34_mp_crc_update(crc, b[18 + 17*g + i]);
    for (i = 0; i < 16; i++)
        b[18 + 17*ngroups + i] = (uint8_t) ((crc >> i) & 1);
    /* start bits and trailing fill are already 0 from the memset */
    return n;
}

int nf_v34_cc_tx_mph(nf_v34_cc_tx_t *s, const nf_v34_mph_fields_t *f)
{
    uint8_t bits[188];
    int n = v34_mph_build_bits(f, bits);

    return nf_v34_cc_tx_bits(s, bits, n);
}

long nf_v34_cc_tx_modulate(const nf_v34_cc_tx_t *s, int16_t *amp, long n,
                           long at, double gain)
{
    /* Peak-normalize the burst: a 2400-mode burst mixes the 1200 training
     * (inner ring, |.| = sqrt2) with 16-point user data reaching |.| = sqrt18
     * (~3x hotter). Scaling the WHOLE burst by sqrt2/peak keeps its peak
     * amplitude identical to a pure 1200 burst (no clipping) while preserving
     * the constellation geometry the decision-directed RX gain relies on. A
     * pure 1200 burst has peak exactly sqrt2, so this leaves it bit-identical. */
    double g = gain, pk = 0.0;
    long i;

    for (i = 0; i < s->nsym; i++) {
        double m = s->re[i] * s->re[i] + s->im[i] * s->im[i];
        if (m > pk) pk = m;
    }
    pk = sqrt(pk);
    if (pk > sqrt(2.0) + 1e-9)
        g = gain * sqrt(2.0) / pk;
    v34_tx_cc_qam(amp, n, at, s->re, s->im, s->nsym, !s->is_call, g);
    return s->nsym;
}

/* ── primary-channel TX (clauses 7/8/9, 10.1.3, 12.5) ─────────────────── */

/* Table 12/V.34: the superframe bit-inversion patterns, one slot per half
 * data frame (2J slots per superframe). J=8: "01 11 01 11 11 11 10 10";
 * J=7: "01 11 01 11 11 11 10" - both transcribed from the PDF's Table 12
 * (left-most bit = first half data frame of a superframe). */
static const uint8_t v34_table12_j8[16] =
    { 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0 };
static const uint8_t v34_table12_j7[14] =
    { 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0 };

/* common tail of the two inits: scrambler, superframe slot, avg energy */
static void v34_pc_tx_init_tail(nf_v34_pc_tx_t *s)
{
    int l;

    nf_v34_scrambler_init(&s->scr, 1);       /* GPC - call modem sends pages */
    s->sf_slot = 2 * s->J - 2;               /* B1 = "last frame of a superframe" */
    v34_quarter_table_ensure();
    s->avg_energy = 0.0;
    for (l = 0; l < (s->M << s->q); l++)
        s->avg_energy += (double) nf_v34_quarter_table[l].re * nf_v34_quarter_table[l].re +
                         (double) nf_v34_quarter_table[l].im * nf_v34_quarter_table[l].im;
    s->avg_energy /= (double) (s->M << s->q);
}

void nf_v34_pc_tx_init(nf_v34_pc_tx_t *s)
{
    memset(s, 0, sizeof(*s));
    /* S=3429 R=24000 (Tables 7/8/10): the capture's negotiated set */
    s->M = 14;
    s->q = 2;
    s->K = 28;
    s->b = 56;
    s->P = 15;
    s->J = 8;
    s->swp = 0x7FFF;
    s->theta = 0.3125;
    v34_pc_tx_init_tail(s);
}

void nf_v34_pc_tx_init_mode(nf_v34_pc_tx_t *s, const nf_v34_pcparams_t *pp)
{
    memset(s, 0, sizeof(*s));
    s->M = pp->M;
    s->q = pp->q;
    s->K = pp->K;
    s->b = pp->b;
    s->P = pp->P;
    s->J = pp->J;
    s->swp = pp->swp;
    s->theta = pp->theta;
    v34_pc_tx_init_tail(s);
}

/* 9.6.2 rounding: nearest multiple of `step`, ties toward smaller magnitude */
static double v34_pc_round_grid(double v, double step)
{
    double q = v / step;
    double fl = floor(q);
    double frac = q - fl;

    if (fabs(frac - 0.5) < 1e-9) {
        double lo = fl * step, hi = (fl + 1.0) * step;
        return (fabs(lo) <= fabs(hi)) ? lo : hi;
    }
    return floor(q + 0.5) * step;
}

static void v34_pc_precoder(const nf_v34_pc_tx_t *s, double *p_re, double *p_im,
                            double *c_re, double *c_im)
{
    double qr = 0.0, qi = 0.0;
    double w = (s->b >= 56) ? 4.0 : 2.0;     /* 2^w grid, w = 2 or 1 */
    int k;

    for (k = 0; k < 3; k++) {
        qr += s->xh_re[k] * s->h_re[k] - s->xh_im[k] * s->h_im[k];
        qi += s->xh_re[k] * s->h_im[k] + s->xh_im[k] * s->h_re[k];
    }
    *p_re = v34_pc_round_grid(qr, 1.0 / 128.0);
    *p_im = v34_pc_round_grid(qi, 1.0 / 128.0);
    *c_re = v34_pc_round_grid(*p_re, w);
    *c_im = v34_pc_round_grid(*p_im, w);
}

long nf_v34_pc_encode(nf_v34_pc_tx_t *s, const uint8_t *bits, int nframes,
                      double *re, double *im)
{
    long bpos = 0;
    long out = 0;
    int fi, j;

    for (fi = 0; fi < nframes; fi++) {
        int is_high = (s->swp >> (s->P - 1 - (s->mf_idx % s->P))) & 1;
        int nb_frame = is_high ? s->b : s->b - 1;
        uint32_t R0 = 0;
        int rings[4][2];
        int i;

        if (s->K > 0) {
            int nb = is_high ? s->K : s->K - 1;
            for (i = 0; i < nb; i++) {
                int raw = bits ? (bits[bpos++] & 1) : 1;
                R0 |= (uint32_t) nf_v34_scramble_bit(&s->scr, raw) << i;
            }
            /* low mapping frames force S(i,K) = 0 (9.3.1) - nb < K leaves
             * the top bit 0 (a literal, unscrambled zero) */
            nf_v34_shell_map(s->M, R0, rings);
        } else {
            /* b <= 12 (9.3.2): no shell mapper, ring indices always 0 */
            memset(rings, 0, sizeof(rings));
        }
        s->mf_idx++;

        for (j = 0; j < 4; j++) {
            int i1, i2, i3, q0, q1, l0, l1;
            int Ival, Z, V0, Y0, C0, U0;
            int u0re, u0im, u1re, u1im;
            double p0r, p0i, c0r, c0i, p1r, p1i, c1r, c1i;
            double y0r, y0i, y1r, y1i, x0r, x0i, x1r, x1i;
            int s0, s1, y4321;
            int qi;

            i1 = nf_v34_scramble_bit(&s->scr, bits ? (bits[bpos++] & 1) : 1);
            i2 = nf_v34_scramble_bit(&s->scr, bits ? (bits[bpos++] & 1) : 1);
            /* 9.3.2's 8/9/11/12-bit patterns: with K = 0 only the first
             * nb_frame-8 groups carry an I3 bit; the rest force I3 = 0 (a
             * literal zero - no data bit consumed, nothing scrambled). With
             * K > 0 (9.3.1) every group carries I3. */
            if (s->K > 0 || j < nb_frame - 8)
                i3 = nf_v34_scramble_bit(&s->scr, bits ? (bits[bpos++] & 1) : 1);
            else
                i3 = 0;
            q0 = 0;
            q1 = 0;
            for (qi = 0; qi < s->q; qi++)
                q0 |= nf_v34_scramble_bit(&s->scr, bits ? (bits[bpos++] & 1) : 1) << qi;
            for (qi = 0; qi < s->q; qi++)
                q1 |= nf_v34_scramble_bit(&s->scr, bits ? (bits[bpos++] & 1) : 1) << qi;

            l0 = q0 | (rings[j][0] << s->q);         /* Q(n), eq 9-26 */
            l1 = q1 | (rings[j][1] << s->q);

            Ival = i2 + 2 * i3;
            Z = (s->zprev + Ival) & 3;

            /* V0 (9.6.3/Table 12): ONLY at 4D intervals m == 0 mod 2P (the
             * start of each half data frame) - diag56's validated V0 fix */
            V0 = 0;
            if (s->m4d % (2L * s->P) == 0) {
                if (s->J == 7)
                    V0 = v34_table12_j7[s->sf_slot % 14];
                else
                    V0 = v34_table12_j8[s->sf_slot % 16];
                s->sf_slot++;
            }
            Y0 = s->state & 1;

            u0re = nf_v34_quarter_table[l0].re;
            u0im = nf_v34_quarter_table[l0].im;
            v34_rotate_cw(&u0re, &u0im, Z);
            v34_pc_precoder(s, &p0r, &p0i, &c0r, &c0i);
            y0r = u0re + c0r; y0i = u0im + c0i;
            x0r = y0r - p0r;  x0i = y0i - p0i;
            s->xh_re[2] = s->xh_re[1]; s->xh_im[2] = s->xh_im[1];
            s->xh_re[1] = s->xh_re[0]; s->xh_im[1] = s->xh_im[0];
            s->xh_re[0] = x0r;         s->xh_im[0] = x0i;

            v34_pc_precoder(s, &p1r, &p1i, &c1r, &c1i);
            /* modulo encoder C0 (9.6.3.3): parity of (c.re + c.im)/2 across
             * the pair; with h = 0 both are 0 -> C0 = 0 */
            {
                int pe0 = ((long) floor((c0r + c0i) / 2.0 + 0.5)) & 1;
                int pe1 = ((long) floor((c1r + c1i) / 2.0 + 0.5)) & 1;
                C0 = pe0 ^ pe1;
            }
            U0 = Y0 ^ C0 ^ V0;

            u1re = nf_v34_quarter_table[l1].re;
            u1im = nf_v34_quarter_table[l1].im;
            v34_rotate_cw(&u1re, &u1im, (Z + 2 * i1 + U0) & 3);
            y1r = u1re + c1r; y1i = u1im + c1i;
            x1r = y1r - p1r;  x1i = y1i - p1i;
            s->xh_re[2] = s->xh_re[1]; s->xh_im[2] = s->xh_im[1];
            s->xh_re[1] = s->xh_re[0]; s->xh_im[1] = s->xh_im[0];
            s->xh_re[0] = x1r;         s->xh_im[0] = x1i;

            s0 = nf_v34_subset_label((int) floor(y0r + 0.5), (int) floor(y0i + 0.5));
            s1 = nf_v34_subset_label((int) floor(y1r + 0.5), (int) floor(y1i + 0.5));
            y4321 = nf_v34_table13[s0][s1];
            s->state = nf_v34_trellis_step(s->state, (y4321 >> 1) & 1, y4321 & 1, NULL);
            s->zprev = Z;
            s->m4d++;

            /* non-linear encoder (9.7): x' = Phi(x) x */
            {
                double e0 = x0r * x0r + x0i * x0i;
                double e1 = x1r * x1r + x1i * x1i;
                double z0 = s->theta * e0 / s->avg_energy;
                double z1 = s->theta * e1 / s->avg_energy;
                double f0 = 1.0 + z0 / 6.0 + z0 * z0 / 120.0;
                double f1 = 1.0 + z1 / 6.0 + z1 * z1 / 120.0;
                re[out] = f0 * x0r; im[out] = f0 * x0i; out++;
                re[out] = f1 * x1r; im[out] = f1 * x1i; out++;
            }
        }
    }
    return bpos;
}

long nf_v34_pc_frames_bits(const nf_v34_pc_tx_t *s, int nframes)
{
    long bits = 0;
    int fi;

    for (fi = 0; fi < nframes; fi++) {
        int is_high = (s->swp >> (s->P - 1 - ((s->mf_idx + fi) % s->P))) & 1;
        bits += is_high ? s->b : s->b - 1;
    }
    return bits;
}

void nf_v34_pc_sspp(double *re, double *im)
{
    v34_cd_t t[V34_PAGE_NKNOWN];
    int i;

    v34_page_build_sspp(t);
    for (i = 0; i < V34_PAGE_NKNOWN; i++) {
        re[i] = t[i].re;
        im[i] = t[i].im;
    }
}

void nf_v34_pc_trn(int nsym, int sixteen_point, double *re, double *im)
{
    nf_v34_scrambler_t scr;
    int i;

    v34_quarter_table_ensure();
    nf_v34_scrambler_init(&scr, 1);
    for (i = 0; i < nsym; i++) {
        int i1 = nf_v34_scramble_bit(&scr, 1);
        int i2 = nf_v34_scramble_bit(&scr, 1);
        int lab = 0;
        int r, m;
        if (sixteen_point) {
            int q1 = nf_v34_scramble_bit(&scr, 1);
            int q2 = nf_v34_scramble_bit(&scr, 1);
            lab = 2 * q2 + q1;
        }
        r = nf_v34_quarter_table[lab].re;
        m = nf_v34_quarter_table[lab].im;
        v34_rotate_cw(&r, &m, 2 * i2 + i1);
        re[i] = r;
        im[i] = m;
    }
}

long nf_v34_pc_burst_build(const nf_v34_pcparams_t *pp,
                           const uint8_t *data_bits, long nbits,
                           double **re_out, double **im_out)
{
    nf_v34_pc_tx_t enc;
    long nframes, frame_bits, b1sym, nsym;
    double *re, *im;
    uint8_t *pad = NULL;
    double b1_norm = 0.0;
    long i, o;

    *re_out = *im_out = NULL;
    if (pp)
        nf_v34_pc_tx_init_mode(&enc, pp);
    else
        nf_v34_pc_tx_init(&enc);
    /* data frames start right after B1 (= P mapping frames, keeping the
     * SWP position aligned); walk the per-frame bit counts (b high / b-1
     * low) until nbits fit */
    nframes = 0;
    frame_bits = 0;
    while (frame_bits < nbits) {
        int is_high = (enc.swp >> (enc.P - 1 - ((int) nframes % enc.P))) & 1;
        frame_bits += is_high ? enc.b : enc.b - 1;
        nframes++;
    }
    b1sym = 8L * enc.P;
    nsym = V34_PAGE_NKNOWN + b1sym + 8 * (nframes + 1);
    re = malloc(sizeof(double) * (size_t) nsym);
    im = malloc(sizeof(double) * (size_t) nsym);
    if (!re || !im) {
        free(re); free(im);
        return -1;
    }
    if (nbits < frame_bits) {
        pad = malloc((size_t) frame_bits);
        if (!pad) {
            free(re); free(im);
            return -1;
        }
        memcpy(pad, data_bits, (size_t) nbits);
        memset(pad + nbits, 1, (size_t) (frame_bits - nbits));   /* HDLC idle */
        data_bits = pad;
    }

    nf_v34_pc_sspp(re, im);
    o = V34_PAGE_NKNOWN;
    nf_v34_pc_encode(&enc, NULL, enc.P, re + o, im + o);          /* B1 */
    for (i = 0; i < b1sym; i++)
        b1_norm += re[o + i] * re[o + i] + im[o + i] * im[o + i];
    b1_norm = sqrt(b1_norm / (double) b1sym);
    nf_v34_pc_encode(&enc, data_bits, (int) nframes,
                     re + o + b1sym, im + o + b1sym);
    nf_v34_pc_encode(&enc, NULL, 1,                               /* 12.5.3 turn-off */
                     re + nsym - 8, im + nsym - 8);
    /* power compensation (10.1.3 NOTE): B1/data at the same line power as
     * the unit-RMS PP - the relation the page decoder's G = g_pp * b1_norm
     * transfer was validated against on the real capture */
    for (i = o; i < nsym; i++) {
        re[i] /= b1_norm;
        im[i] /= b1_norm;
    }
    free(pad);
    *re_out = re;
    *im_out = im;
    return nsym;
}

void nf_v34_pc_modulate(const nf_v34_pcparams_t *pp,
                        int16_t *amp, long n, long at,
                        const double *re, const double *im, long nsym,
                        double gain)
{
    /* Exact rational baud and carrier per mode (default: exactly 24000/7
     * baud = 7/3 samples/symbol, carrier exactly 96000/49 Hz = 12/49
     * cycles/sample). RRC beta 0.12 keeps the band edges strictly inside
     * 0..4000 Hz for every Table 1 x Table 2 combination (tightest cases:
     * S=3429 at 1959.18 Hz -> 39..3879 Hz, and S=3200's low carrier at
     * 1828.57 Hz -> 37..3621 Hz). */
    v34_tx_qam(amp, n, at, re, im, nsym,
               pp ? pp->sps_num : 7, pp ? pp->sps_den : 3, 0.12, 12,
               pp ? pp->cnum : 12, pp ? pp->cden : 49, gain);
}

/* ═══════════════════════════════════════════════════════════════════════
 * half-duplex session driver (ITU-T V.34 clause 12 + T.30 Annex F) - the
 * phase-B state machine that runs the batch primitives above as a live
 * modem below nf_fax/nf_t30. See nf_v34.h for the interface contract.
 *
 * Streaming facade, batch under the hood: receive audio is energy-gated
 * into per-burst buffers (a short pre-roll ring keeps the burst onset),
 * and each completed burst is decoded with the capture-validated batch
 * receivers (nf_v34_info_rx_batch / nf_v34_cc_rx_batch + MP/ccdata /
 * nf_v34_page_locate_s + train + decode_burst).
 *
 * Documented simplifications vs the letter of clause 12 (loopback-grade;
 * hardening TODOs for a real far-end machine):
 *   - Phase 2 round-trip measurement is FAKED: tone B/A phase reversals
 *     are transmitted at fixed offsets and never measured; the choreography
 *     advances on decoded INFO frames and energy-gated burst boundaries,
 *     not on the reversal handshake. TODO: real quadrature reversal
 *     detectors + the MD timing rules of 11.2/12.2.
 *   - Line probing L1/L2 is transmitted (real Table 17 signal) but the
 *     recipient does NOT analyse it yet: it selects S=3429, low carrier,
 *     16-point TRN, 40x35 ms (NFV34SRATE/NFV34HIGHC override for
 *     experiments). The DATA RATE, however, is fully negotiated: an
 *     initial cap from the TRN-training SNR estimate, MPh masks/maxima
 *     per 12.4.1.3, and automatic per-burst fallback renegotiation. Every
 *     Table 8 (S,R) point is txrates-validated. TODO (stage 2): probing
 *     analysis driving the symbol-rate/carrier choice too.
 *   - Stage 3 recovery mechanisms are IMPLEMENTED: a stable control-channel
 *     turnaround (parameters unchanged) takes the short Sh/S̄h/ALT/E resync
 *     of 12.6 (v34_sess_build_cc's use_sh path); a rate change forces the
 *     full PPh/ALT/MPh/MPh/E restart of 12.4 carrying the new MPh caps; the
 *     receiver discriminates the two by MPh-presence, corroborated by a
 *     rotation-invariant Sh correlator (v34_cc_sh_score). An unrecoverable
 *     burst (hard failure already at the floor rate) escalates to a full
 *     control-channel retrain (12.7/12.8) that re-runs Phase 2 probing ->
 *     Phase 3 training (nf_v34_sess_retrain). Simplifications: the retrain's
 *     tone/AC duplex handshake and the clause-12 three-second recovery timers
 *     are collapsed to the loopback's energy-gated burst boundaries, and the
 *     mid-call retrain is exercised at the session level (check-v34's
 *     `recover`) rather than wired through T.30's steady-state loop - a live
 *     fax's mid-call degradation is handled by the 12.4 rate renegotiation
 *     (check-v34fax's NFV34HIT run) plus T.30's own ECM/PPR recovery.
 *   - Phase 3's final J/J' sequences are omitted; the recipient trains on
 *     the TRN located via the S/Sbar/PP correlator and confirms success
 *     through the control-channel handshake (MPh exchange) instead.
 *   - Inter-burst gaps are nominal 75 ms (150 ms during startup where V.8
 *     tails / decoder hangover need the margin), not the adaptive spec
 *     timings; burst boundaries are energy-detected with ~15 ms open /
 *     ~60 ms close hangover, with both thresholds riding an adaptive
 *     noise-floor estimate (see the V34S_OPEN_P block) so noisy lines
 *     cannot hold the gate open and quiet lines keep full sensitivity.
 *     TODO for real lines: proper tone/carrier detectors instead of
 *     broadband energy, echo protection, clause 12's T-timer supervision.
 * ═══════════════════════════════════════════════════════════════════════ */

#include <stdio.h>

#define V34S_LEAD          600     /* 75 ms burst lead-in silence          */
#define V34S_LEAD_STARTUP 1200     /* 150 ms during startup (V.8 tails)    */
#define V34S_TAIL          500
#define V34S_CC_GAIN      6000.0
#define V34S_PRI_GAIN     5000.0
#define V34S_CC2400_MIN_SNR_DB 13.0  /* min line SNR to advertise 2400 cc (10.2.4) */
#define V34S_INFO_GAIN    8000.0
#define V34S_TONE_LEVEL   8000.0
#define V34S_PROBE_RMS    2000.0
#define V34S_TRN_UNITS      40     /* TRN length in 35 ms units (=4800 sym) */
/* Energy-gate thresholds: the MINIMUM open/close power levels. The gate
 * also tracks the line's noise floor (slow EMA while closed) and raises
 * both thresholds above it (open = 9 dB, close = 6 dB over the floor), so
 * noisy lines can't hold the gate open forever (at 25 dB SNR the noise
 * floor alone sits above the old fixed close threshold) while quiet lines
 * keep the original sensitivity. */
#define V34S_OPEN_P   (200.0 * 200.0)   /* gate open power floor            */
#define V34S_CLOSE_P  (100.0 * 100.0)   /* gate close power floor           */
#define V34S_OPEN_SNR   8.0        /* open threshold = noise * this (~9 dB) */
#define V34S_CLOSE_SNR  4.0        /* close threshold = noise * this (~6 dB)*/
#define V34S_OPEN_RUN   192        /* samples above threshold to open       */
#define V34S_CLOSE_RUN  480        /* samples below threshold to close      */
#define V34S_PRE        480        /* pre-roll kept ahead of the gate       */
#define V34S_BURST_MAX  (30L * 8000)    /* force-process guard              */
#define V34S_DEADLINE   (25L * 8000)    /* startup supervision              */
#define V34S_NFRAMES      8        /* control frames per burst (T.30 uses 1) */
#define V34S_FRAME_MAX   64
#define V34S_RXFRAME_MAX 96        /* stashed rx cc frames (NSF can be long) */
#define V34S_SCAN_WIN  32000       /* incremental cc scan window, 4 s        */

/* held cc tx stream states */
enum {
    CCS_OFF = 0,
    CCS_RUN,                   /* preamble/flags/frames, extending with flags */
    CCS_ONES,                  /* turnaround: >= 40 ones, await peer silence  */
    CCS_DRAIN,                 /* stop extending, play the rendered tail out  */
    CCS_GAP                    /* 70 +- 5 ms silence, then CCS_OFF            */
};
#define CCS_LEAD        560    /* render origin: 70 ms lead-in silence        */
#define CCS_MARGIN     2000    /* keep >= this many rendered samples ahead    */
#define CCS_ONES_WAIT (5L * 8000)  /* cap the wait for the peer to fall silent */

/* startup choreography states */
enum {
    V34S_IDLE = 0,
    /* call modem (source) */
    V34S_C_START, V34S_C_INFO0_PLAY, V34S_C_INFO0_WAIT,
    V34S_C_TONES_Q, V34S_C_TONES_PLAY,
    V34S_C_INFOH_WAIT,
    V34S_C_PHASE3_Q, V34S_C_PHASE3_PLAY,
    /* 12.4.1: the SOURCE opens the control channel - 70 ms after TRN it
     * transmits PPh/ALT/MPh/E blind, THEN awaits the recipient's cc burst
     * (which, from a real fax, already carries NSF/CSI/DIS as user data) */
    V34S_C_CC_INIT_Q, V34S_C_CC_INIT_PLAY,
    V34S_C_CC1_WAIT, V34S_C_CC2_Q, V34S_C_CC2_PLAY,
    /* answer modem (recipient) */
    V34S_A_INFO0C_WAIT, V34S_A_INFO0A_Q, V34S_A_INFO0A_PLAY,
    V34S_A_PROBE_WAIT, V34S_A_INFOH_Q, V34S_A_INFOH_PLAY,
    V34S_A_TRN_WAIT, V34S_A_CC1_Q, V34S_A_CC1_PLAY, V34S_A_CC2_WAIT,
    V34S_DONE, V34S_FAILED
};

/* ── real-time Phase 2 (clause 12.2.1) states ─────────────────────────────
 * The energy-gated burst machinery cannot drive Phase 2: against a real
 * V.34 modem the answerer transmits a CONTINUOUS carrier (Tone A 2400 Hz +
 * 1800 Hz guard, interleaved with INFO0a) with no >60 ms gap, so the
 * caller's close-hangover gate never fires and the INFO0a is never decoded
 * (confirmed: farend2 capture has min RMS 2877 over 7.8-32.4 s, longest
 * sub-600-RMS run = 0 ms). Phase 2 is a simultaneous, bidirectional tone
 * handshake (Tone A at 2400, Tone B at 1200 - different frequencies, so both
 * modems transmit at once on the full-duplex sample pump). This sub-engine
 * runs sample-synchronously alongside the pump: it continuously generates the
 * role's tone/probe/INFO and runs real-time tone-presence + 180-degree-phase-
 * reversal detectors on the received audio, following clause 12.2.1 exactly
 * (Figure 23). When Phase 2 completes (INFOh exchanged) it hands the selected
 * S/carrier/TRN parameters to the existing burst FSM at Phase 3. */
enum {
    P2_OFF = 0,
    /* call modem = source (12.2.1.1) */
    P2C_SILENCE, P2C_INFO0, P2C_TONEB, P2C_TB_REV, P2C_L1, P2C_L2,
    P2C_TONEB2, P2C_DONE,
    /* answer modem = recipient (12.2.1.2) */
    P2A_SILENCE, P2A_INFO0, P2A_TONEA, P2A_TA_REV, P2A_SIL2, P2A_PROBE,
    P2A_TONEA2, P2A_INFOH, P2A_DONE
};

struct nf_v34_sess {
    int is_call;
    nf_v34_sess_status_fn status_fn;
    nf_hdlc_frame_fn frame_fn;
    void *user;

    long now;                   /* rx sample clock                          */

    /* ── tx ── */
    int tx_mode;
    int16_t *pcm;               /* burst being played                       */
    long pcm_len, pcm_pos;
    int pcm_active;
    int pcm_kind;               /* NF_V34_SESS_* the burst was built FOR: the
                                 * completion actions must match the burst,
                                 * not whatever tx_mode is by the time it
                                 * finishes (t30 can switch modes mid-play) */

    /* ── held control-channel tx stream (T.30 Annex F F.3.1.4/F.3.2.1) ──
     * A V.34 fax terminal does NOT send the control channel as bursts: after
     * cc start-up each side HOLDS its carrier, idling HDLC flags, inserting
     * T.30 frames inline, and keeps flagging while awaiting the peer's
     * response on the other cc carrier. The stream is a growing symbol
     * accumulator re-rendered ahead of the playout position (the modulator
     * is a pure function of the symbol array, so extension is prefix-exact).
     * The carrier drops only for the B/D-phase turnaround: >= 40 ones, wait
     * for the peer's cc carrier to fall, 70 ms silence, primary channel. */
    int ccs_state;              /* CCS_*                                     */
    nf_v34_cc_tx_t ccs_tx;      /* symbol history of this carrier-up period  */
    int ccs_on;                 /* ccs_tx holds an initialised accumulator   */
    long ccs_out;               /* stream samples already handed to the pump */
    int16_t *ccs_pcm;           /* rendered stream                           */
    long ccs_pcm_len, ccs_pcm_cap;
    long ccs_data_end_sym;      /* symbol where the queued frames end (-1)   */
    int ccs_data_pending;       /* NF_SIG_SEND_COMPLETE not yet reported     */
    int ccs_pre_mph;            /* startup interlock (12.4.1.3): still
                                 * looping MPh - E goes out only once the
                                 * peer's MPh has been decoded, so the flags
                                 * after E are at the NEGOTIATED cc rate     */
    int ccs_form_startup;       /* current stream uses the MPh-interlock
                                 * start-up form                             */
    /* copy of the last data section put into the stream, so a Figure-26
     * restart that aborts it mid-flight can re-queue it after E (the peer
     * renegotiated exactly BECAUSE it wants the command again, at the new
     * rate - waiting for nf_t30's retry timer loses seconds it may not
     * grant) */
    uint8_t ccs_saved[V34S_NFRAMES][V34S_FRAME_MAX];
    int ccs_saved_len[V34S_NFRAMES];
    int ccs_nsaved;
    int ccs_peer_mph;           /* a peer MPh decoded since this stream began */
    int cc_reneg_pending;       /* peer initiated a PPh/MPh cc start-up
                                 * mid-call (Figure 26, rate change): our
                                 * next/current cc stream must answer with
                                 * the start-up form, not an Sh resync       */
    long cc_reneg_holdoff;      /* suppress restart detection until this
                                 * s->now: right after an interlock both
                                 * sides' MPh linger in the held carriers
                                 * and the windowed scans keep re-seeing
                                 * them - without a hold-off the two ends
                                 * answer each other's echoes forever       */
    long ccs_ones_bits;         /* turnaround: ones appended so far          */
    long ccs_gap_left;          /* CCS_GAP: silence samples remaining        */
    uint8_t frames[V34S_NFRAMES][V34S_FRAME_MAX];   /* queued control frames */
    int frame_len[V34S_NFRAMES];
    /* rx frames stashed while decoding the recipient's startup cc burst, so
     * they can be handed up AFTER the establishment report (the report makes
     * nf_t30 switch into its awaiting-DIS state; a real fax piggybacks
     * NSF/CSI/DIS onto that very burst) */
    uint8_t rxq[V34S_NFRAMES][V34S_RXFRAME_MAX];
    int rxq_len[V34S_NFRAMES];
    int rxq_ok[V34S_NFRAMES];
    int nrxq;
    /* streaming control-channel decode: a real fax HOLDS its cc carrier
     * (idling HDLC flags) while it waits for the T.30 answer, repeating its
     * command inside ONE burst - waiting for the carrier to drop before
     * decoding would answer long past its T.30 timers. Incremental scans
     * over the growing burst deliver each frame as soon as it decodes. */
    long cc_scan_at;            /* burst_n of the last incremental scan     */
    /* per-burst CONTENT dedup of delivered cc frames: incremental scans
     * re-decode the growing burst from scratch, and the demod is not
     * perfectly prefix-stable (active-region normalisation and CFO window
     * shift as the burst grows), so frame INDEXES can wobble between scans.
     * Content identity is stable; a genuinely repeated frame inside ONE
     * held carrier (a re-sent DIS) is redundant to T.30 anyway. */
    uint8_t cc_seen[16][V34S_RXFRAME_MAX];
    int cc_seen_len[16];
    int cc_nseen;
    int pri_skip_burst;         /* rx switched to PRI mid-carrier: the next
                                 * burst may be (or contain) the peer's cc
                                 * tail                                      */
    int cc_burst_seen;          /* the PRI-mode scans found cc content (MPh,
                                 * frames, ALT hold) in the burst underway:
                                 * it is the peer's held control channel,
                                 * not a failed primary transmission        */
    int pri_maybe_cc_tail;      /* transient: tolerate S-locate failure on
                                 * the burst being processed (no fallback)   */
    int nframes;
    int (*get_frame)(void *user, uint8_t *buf, int maxlen);   /* pri stream  */
    void *get_frame_user;
    int pri_pending;

    /* ── rx gate ── */
    int rx_mode;
    double pwr;                 /* power EMA (~8 ms time constant)          */
    double noise;               /* noise-floor power EMA (tracked while the
                                 * gate is closed; raises both thresholds)  */
    int gate_open;
    long open_run, close_run;
    int16_t pre[V34S_PRE];      /* pre-roll ring                            */
    int pre_n, pre_pos;
    int16_t *burst;
    long burst_n, burst_cap;

    /* ── startup FSM ── */
    int st;
    int started, done;
    long st_timer;              /* INFO0 re-send schedule                   */
    long deadline;
    int infoh_trn_len;          /* x35 ms units, from the (our) INFOh       */
    int infoh_16pt;

    /* ── negotiated operating point + rate state (12.4.1.3) ── */
    int srate_idx;              /* INFOh symbol rate (recipient selects;
                                 * source adopts from the received INFOh)   */
    int high_carrier;           /* INFOh bit 22                             */
    int probe_override;         /* NFV34SRATE/NFV34HIGHC forced S/carrier   */
    int local_max_rate;         /* our advertised MPh cap, bit/s            */
    int remote_max_rate;        /* peer's MPh max, bit/s (0 = not yet seen) */
    int remote_mask;            /* peer's MPh rate capability mask          */
    int rate_cur;               /* selected primary rate (max enabled in
                                 * both masks <= both maxima), bit/s        */
    double snr_est_db;          /* recipient: SNR estimate from the TRN
                                 * training residual (~ -10log10(res))      */

    /* ── Stage 4: 2400 bit/s control-channel user-data mode (10.2.4) ──────
     * cc_rate_adv is our own MPh bit 27 (what we advertise); remote_cc_rate is
     * the peer's last-seen bit 27; cc_rate is the negotiated rate actually
     * used for BOTH tx and rx of the control-channel user data (1 = 2400 iff
     * both sides advertise it - the symmetric common case, capture stays
     * 1200). Training (PPh/ALT/MPh/E) is always 1200. */
    int cc_rate_adv;            /* our MPh bit 27 (1 = advertise 2400)         */
    int remote_cc_rate;         /* peer's MPh bit 27 (0 until first seen)      */
    int cc_rate;                /* negotiated: 0 = 1200, 1 = 2400              */

    /* ── recipient training result ── */
    int have_eq;
    nf_v34_page_eq_t eq;

    /* ── Stage 3: control-channel resync / renegotiation / retrain ──────
     * cc_established goes 1 at startup success (0 during startup / retrain).
     * A steady-state control-channel turnaround uses the short Sh/S̄h resync
     * (12.6) when the control channel is established AND the MPh caps we would
     * advertise (max_rate + rate_mask) match what we last signalled; a change
     * (rate fallback) forces the full PPh/ALT/MPh/MPh/E restart (12.4) that
     * carries the new caps. */
    int cc_established;
    int cc_last_adv_rate;       /* last MPh max_rate code we TX'd (-1 = none)  */
    int cc_last_adv_mask;       /* last MPh rate_mask we TX'd (-1 = none)      */
    int sh_resyncs;             /* Sh short resyncs transmitted               */
    int pph_renegs;             /* PPh/MPh renegotiations transmitted (post-
                                 * startup; startup handshakes not counted)    */
    int last_rx_cc_kind;        /* NF_V34_CC_* of the last received cc burst   */
    double last_rx_cc_sh_score; /* its Sh-correlator score                    */
    double last_rx_cc_pph_score;/* its PPh-correlator score (12.6 restarts)   */

    /* full retrain (12.7/12.8) */
    int retrains;               /* retrains initiated this call                */
    int in_retrain;             /* re-running Phase 2->3 (pumps act as STARTUP)*/
    int floor_hard_fails;       /* consecutive hard fails at the floor rate    */
    int saved_tx_mode;          /* tx mode to restore after a retrain          */

    /* ── real-time Phase 2 tone handshake (clause 12.2.1) ── */
    int p2_state;               /* P2_* (P2_OFF = not in Phase 2)              */
    long p2_out;                /* absolute Phase-2 TX sample index            */
    long p2_in;                 /* absolute Phase-2 RX sample index            */
    long p2_state_out;          /* p2_out when the current tx state was entered*/
    long p2_deadline;           /* p2_in supervision (2.5 s + RTDs, CME-gated) */
    int16_t *p2_seg;            /* pre-rendered INFO waveform being streamed    */
    long p2_seg_len, p2_seg_pos;
    long p2_rev_out;            /* scheduled own tone phase-reversal TX index   */
    long p2_l1_out, p2_l2_out;  /* L1 / L2 probe start TX index (source)        */
    long p2_evt_in;             /* RX index of a timed event (Tone A reversal)  */
    double rtd_ms;              /* measured round-trip delay (recipient, 12.2)  */
    /* tone-presence + reversal detector (on the peer's tone frequency) */
    double p2_det_th;           /* mixer LO phase, rad                          */
    double p2_acc_re, p2_acc_im;/* leaky-integrated tone phasor                 */
    double p2_ref_re, p2_ref_im;/* slow reference phasor (reversal reference)   */
    double p2_inpow;            /* input power EMA                              */
    int p2_tone_on;             /* tone currently present                       */
    long p2_tone_run, p2_off_run;/* consecutive on / off sample runs           */
    long p2_stable_run;         /* consecutive phase-stable (pure-tone) samples */
    int p2_notch;               /* inside a tone envelope notch (reversal cand) */
    long p2_notch_age;          /* samples since the notch began                */
    int p2_arm_rev;             /* reversal detector armed                      */
    int p2_seg_pure;            /* current tone segment is a pure tone (not INFO)*/
    int p2_saw_tone;            /* tone was seen on at least once (for onset)   */
    /* rolling INFO capture buffer + periodic decode */
    int16_t *p2_roll;
    long p2_roll_n, p2_roll_cap, p2_scan_at;
    int p2_info_seen;           /* peer INFO0 decoded (caller: 0a, ansr: 0c)    */
};

#define V34S_MAX_RETRAINS 3

static int v34_sess_dbg(void)
{
    static int d = -1;
    if (d < 0)
        d = getenv("NFV34DBG") ? 1 : 0;
    return d;
}

#define SDBG(s, ...) \
    do { \
        if (v34_sess_dbg()) { \
            fprintf(stderr, "  <v34-%c %7.3fs> ", (s)->is_call ? 'C' : 'A', \
                    (double) (s)->now / 8000.0); \
            fprintf(stderr, __VA_ARGS__); \
        } \
    } while (0)

/* highest / next-lower primary rate available at symbol rate sr */
static int v34_sess_top_rate(int sr)
{
    int mask = nf_v34_rate_mask(sr), i;

    for (i = 14; i >= 1; i--)
        if (mask & (1 << (i - 1)))
            return i * 2400;
    return 0;
}

static int v34_sess_floor_rate(int sr)
{
    int mask = nf_v34_rate_mask(sr), i;

    for (i = 1; i <= 14; i++)
        if (mask & (1 << (i - 1)))
            return i * 2400;
    return 0;
}

nf_v34_sess_t *nf_v34_sess_alloc(int is_call, nf_v34_sess_status_fn status_fn,
                                 nf_hdlc_frame_fn frame_fn, void *user)
{
    nf_v34_sess_t *s = calloc(1, sizeof(*s));
    const char *env;

    if (!s)
        return NULL;
    s->is_call = is_call;
    s->status_fn = status_fn;
    s->frame_fn = frame_fn;
    s->user = user;
    s->st = V34S_IDLE;
    /* default operating point until probing analysis (stage 2) exists:
     * S=3429, low carrier - overridable for experiments via NFV34SRATE */
    s->srate_idx = NF_V34_RATE_3429;
    env = getenv("NFV34SRATE");
    if (env) {
        int v = atoi(env);
        if (v >= 0 && v < NF_V34_NUM_RATES)
            s->srate_idx = v;
        s->probe_override = 1;
    }
    if (getenv("NFV34HIGHC")) {
        s->high_carrier = atoi(getenv("NFV34HIGHC")) != 0;
        s->probe_override = 1;
    }
    s->local_max_rate = v34_sess_top_rate(s->srate_idx);
    env = getenv("NFV34MAXRATE");
    if (env && atoi(env) > 0)
        nf_v34_sess_set_max_rate(s, atoi(env));
    s->cc_last_adv_rate = -1;
    s->cc_last_adv_mask = -1;
    /* Control-channel user-data rate advertisement (MPh bit 27). DEFAULT 1200
     * bit/s (4-point, 10.2.4): the T.30 handshake frames (DIS/DCS/CFR/PPS/MCF)
     * ride the control channel, so a lost cc frame fails the whole call - and
     * the uncoded 16-point 2400 mode, while fully implemented and unit-tested
     * (see txcc/ccimp), is NOT robust to a real line carrier-frequency offset
     * (its carrier is locked on the 1200 training prefix and coasted through
     * the data - fine on a clean/gain/tilt line, but a few Hz of line offset
     * corrupts the 16-point frames and drops the call; measured: foff+-3/-7
     * and the combo cell regress at the 2400 default while passing at 1200).
     * The ~2x cc speedup only affects the short handshake, not the bulk image
     * transfer, so robustness wins: keep 1200 in-session. Opt in to 2400 with
     * NFV34CC2400=1 (clean lines only). The answerer still withdraws the advert
     * below V34S_CC2400_MIN_SNR_DB. */
    s->cc_rate_adv = getenv("NFV34CC2400") ? 1 : 0;
    return s;
}

void nf_v34_sess_set_max_rate(nf_v34_sess_t *s, int rate)
{
    int top = v34_sess_top_rate(s->srate_idx);
    int fl = v34_sess_floor_rate(s->srate_idx);

    rate -= rate % 2400;
    if (rate > top)
        rate = top;
    if (rate < fl)
        rate = fl;
    s->local_max_rate = rate;
}

int nf_v34_sess_data_rate(const nf_v34_sess_t *s)
{
    return s->rate_cur;
}

int nf_v34_sess_cc_rate(const nf_v34_sess_t *s)
{
    return s->cc_rate ? 2400 : 1200;
}

/* Negotiate the control-channel user-data rate: 2400 only when BOTH sides
 * advertise it (symmetric common case per 10.2.4 / Annex F F.3.1.4). */
static void v34_sess_pick_cc_rate(nf_v34_sess_t *s)
{
    int nr = (s->cc_rate_adv && s->remote_cc_rate) ? 1 : 0;
    if (nr != s->cc_rate) {
        SDBG(s, "control-channel user-data rate -> %d bit/s "
             "(local adv %d, remote adv %d)\n", nr ? 2400 : 1200,
             s->cc_rate_adv, s->remote_cc_rate);
        s->cc_rate = nr;
    }
}

int nf_v34_sess_sh_resyncs(const nf_v34_sess_t *s) { return s->sh_resyncs; }
int nf_v34_sess_pph_renegs(const nf_v34_sess_t *s) { return s->pph_renegs; }
int nf_v34_sess_retrains(const nf_v34_sess_t *s)   { return s->retrains; }
int nf_v34_sess_established(const nf_v34_sess_t *s) { return s->cc_established; }
int nf_v34_sess_last_rx_cc_kind(const nf_v34_sess_t *s)
{
    return s->last_rx_cc_kind;
}
double nf_v34_sess_last_rx_cc_sh_score(const nf_v34_sess_t *s)
{
    return s->last_rx_cc_sh_score;
}

/* 12.4.1.3 / 12.4.2.4: the rate for the primary channel is the maximum
 * rate enabled (in both modems' capability masks) that is less than or
 * equal to the maxima specified in both modems' MPh sequences. */
static void v34_sess_pick_rate(nf_v34_sess_t *s)
{
    int mask = nf_v34_rate_mask(s->srate_idx);
    int cap = s->local_max_rate;
    int old = s->rate_cur;
    int i;

    if (s->remote_max_rate > 0 && s->remote_max_rate < cap)
        cap = s->remote_max_rate;
    if (s->remote_max_rate > 0)
        mask &= s->remote_mask;
    s->rate_cur = 0;
    for (i = cap / 2400; i >= 1; i--) {
        if (mask & (1 << (i - 1))) {
            s->rate_cur = i * 2400;
            break;
        }
    }
    if (s->rate_cur == 0)                    /* no overlap: take our floor */
        s->rate_cur = v34_sess_floor_rate(s->srate_idx);
    if (s->rate_cur != old)
        SDBG(s, "primary rate selected: %d bit/s (local max %d, remote max"
             " %d mask 0x%04x)\n", s->rate_cur, s->local_max_rate,
             s->remote_max_rate, s->remote_mask);
}

/* ── per-rate SNR requirement (slicer floor per constellation size) ─────
 *
 * The page decoder deliberately slices without the trellis (see nf_v34.h -
 * it lacks the ~4 dB TCM coding gain a full V.34 receiver would have), so
 * its requirement tracks the alphabet's nonlinear-scaled mean energy
 * directly: req_dB ~ C + 10*log10(avg |Phi(p)*p|^2), floored by the
 * training/acquisition floor. Measured with the `ratesnr` test mode
 * (AWGN-only decode threshold of a one-FCD-frame burst, expanded shaping,
 * training through the same noise; single-trial, +-1..2 dB) at S=3429:
 *   4800:15  7200:15  9600:18  12000:17  14400:21  16800:23  19200:25
 *  21600:26  24000:27  26400:31  28800:33  31200:37  33600:37 dB
 * at S=2400: 2400:13 4800:12 7200:14 9600:18 12000:20 14400:22 16800:26
 *  19200:30 21600:35; at S=3000: 4800:12 7200:15 9600:18 12000:19
 *  14400:20 16800:23 19200:25 21600:29 24000:30 26400:33 28800:35
 * (the same energy law fits all three symbol rates).
 * The energy model fits these with C between ~5 and ~8.5; C = 7 (upper
 * range) keeps the initial rate choice honest-conservative, and the
 * <= 15 dB floor reflects the measured training/locate floor. Stage 2's
 * probing analysis consumes this same curve. */
static double v34_rate_req_snr_db(int sr_idx, int rate)
{
    const nf_v34_rateparam_t *r = nf_v34_rateparam(sr_idx, rate);
    double avg_e, sum = 0.0, req;
    int lq, l;

    if (!r)
        return 100.0;
    lq = r->m_exp << r->q;
    avg_e = v34_page_avg_energy_lq(lq);
    for (l = 0; l < lq; l++) {
        double p2 = (double) nf_v34_quarter_table[l].re * nf_v34_quarter_table[l].re +
                    (double) nf_v34_quarter_table[l].im * nf_v34_quarter_table[l].im;
        double zeta = 0.3125 * p2 / avg_e;
        double phi = 1.0 + zeta/6.0 + zeta*zeta/120.0;
        sum += phi * phi * p2;
    }
    sum /= (double) lq;
    req = 7.0 + 10.0 * log10(sum);
    return req < 15.0 ? 15.0 : req;
}

/* ── Phase-2 line-probing analyzer + selector (see nf_v34.h) ────────────── */

/* single-frequency Goertzel: amplitude of a cosine at freq_hz over x[0..N-1]
 * (returns (2/N)*|X(freq)|, i.e. the peak amplitude of a pure tone). */
static double v34_goertzel_amp(const double *x, long N, double freq_hz)
{
    double w = 2.0 * M_PI * freq_hz / 8000.0;
    double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0, re, im;
    long i;

    for (i = 0; i < N; i++) {
        double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    re = s1 - s2 * cos(w);
    im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double) N;
}

int nf_v34_probe_analyze(const int16_t *amp, long n, nf_v34_probe_t *pr)
{
    /* Table 17 present tones (150 Hz * mult). The 4 omitted multiples
     * (900/1200/1800/2400 Hz) are silent probes, but the noise floor is
     * measured on the HALF-GRID (75 Hz off every tone): those points are an
     * integer number of DFT bins from every tone, so a rectangular-window
     * Goertzel sees exactly zero tone leakage there, giving ~26 clean noise
     * samples instead of just 4 (far lower-variance floor + per-tone SNR). */
    #define NF_V34_PROBE_NHG 26                 /* 75,225,...,3825 Hz */
    static const int mult[NF_V34_PROBE_NTONES] =
        { 1, 2, 3, 4, 5, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
    static const int strong[6]    = { 5, 7, 10, 13, 14, 17 };   /* 750..2550 Hz */
    const double SNR_USABLE_DB = 10.0;          /* usable-band threshold (tuned) */
    long N, i;
    double *x, mean = 0.0, off, nmed, cal;
    double hg_amp[NF_V34_PROBE_NHG];
    int k;

    if (!pr || n < 1600)
        return -1;
    N = n > 4800 ? 4800 : n;
    N -= N % 1600;                              /* exact-bin window (bin/150Hz) */
    if (N < 1600)
        N = 1600;
    x = malloc(sizeof(double) * (size_t) N);
    if (!x)
        return -1;
    for (i = 0; i < N; i++)
        mean += (double) amp[i];
    mean /= (double) N;
    for (i = 0; i < N; i++)
        x[i] = (double) amp[i] - mean;

    /* frequency offset: fine peak search around the strong mid-band tones,
     * amplitude-weighted average (2 Hz coarse grid + parabolic refine). */
    {
        double sum = 0.0, wsum = 0.0, d;
        int si;
        for (si = 0; si < 6; si++) {
            double f0 = 150.0 * strong[si];
            double best = -1.0, bestd = 0.0, am, a0, ap, den, frac;
            for (d = -40.0; d <= 40.0; d += 2.0) {
                double av = v34_goertzel_amp(x, N, f0 + d);
                if (av > best) { best = av; bestd = d; }
            }
            am = v34_goertzel_amp(x, N, f0 + bestd - 2.0);
            a0 = v34_goertzel_amp(x, N, f0 + bestd);
            ap = v34_goertzel_amp(x, N, f0 + bestd + 2.0);
            den = am - 2.0 * a0 + ap;
            frac = den != 0.0 ? 0.5 * (am - ap) / den : 0.0;
            if (frac > 1.0) frac = 1.0;
            if (frac < -1.0) frac = -1.0;
            d = bestd + 2.0 * frac;
            sum += d * best;
            wsum += best;
        }
        off = wsum > 0.0 ? sum / wsum : 0.0;
    }
    pr->freq_offset_hz = off;
    pr->n = NF_V34_PROBE_NTONES;

    /* per-tone amplitudes at nominal + measured offset */
    for (k = 0; k < NF_V34_PROBE_NTONES; k++) {
        pr->freq_hz[k] = 150.0 * mult[k];
        pr->tone_amp[k] = v34_goertzel_amp(x, N, pr->freq_hz[k] + off);
    }
    /* noise floor at the half-grid points (75 + 150*j Hz) */
    for (k = 0; k < NF_V34_PROBE_NHG; k++)
        hg_amp[k] = v34_goertzel_amp(x, N, (75.0 + 150.0 * k) + off);
    {
        double sp = 0.0;
        for (k = 0; k < NF_V34_PROBE_NHG; k++)
            sp += hg_amp[k] * hg_amp[k];
        nmed = sqrt(sp / (double) NF_V34_PROBE_NHG);   /* RMS floor amplitude */
    }
    if (nmed < 1e-6) nmed = 1e-6;
    pr->noise_floor = nmed;

    /* per-tone SNR vs the LOCAL floor (mean power of the two adjacent
     * half-grid points, tone k at 150*mult straddles hg[mult-1] and hg[mult]) */
    for (k = 0; k < NF_V34_PROBE_NTONES; k++) {
        int m = mult[k];
        double lo = (m - 1 < NF_V34_PROBE_NHG) ? hg_amp[m - 1] : nmed;
        double hi = (m < NF_V34_PROBE_NHG) ? hg_amp[m] : nmed;
        double fl = sqrt(0.5 * (lo * lo + hi * hi));
        if (fl < 1e-6) fl = 1e-6;
        pr->snr_db[k] = 20.0 * log10(pr->tone_amp[k] / fl);
    }

    /* usable band = longest contiguous run of tones that both clear the SNR
     * threshold AND sit within REL_DROP_DB of the strongest tone (the latter
     * catches a band edge on a quiet line, where an attenuated tone can still
     * tower over the near-silent noise floor). */
    {
        const double REL_DROP_DB = 12.0;
        double peak = 0.0;
        int best_lo = -1, best_hi = -1, run_lo = -1, blen = 0;
        for (k = 0; k < NF_V34_PROBE_NTONES; k++)
            if (pr->tone_amp[k] > peak) peak = pr->tone_amp[k];
        if (peak < 1e-9) peak = 1e-9;
        for (k = 0; k <= NF_V34_PROBE_NTONES; k++) {
            int ok = (k < NF_V34_PROBE_NTONES) &&
                     (pr->snr_db[k] >= SNR_USABLE_DB) &&
                     (20.0 * log10(pr->tone_amp[k] / peak) >= -REL_DROP_DB);
            if (ok) {
                if (run_lo < 0) run_lo = k;
            } else if (run_lo >= 0) {
                int len = k - run_lo;
                if (len > blen) { blen = len; best_lo = run_lo; best_hi = k - 1; }
                run_lo = -1;
            }
        }
        if (best_lo < 0) {
            pr->band_lo_hz = pr->band_hi_hz = 0.0;
        } else {
            pr->band_lo_hz = pr->freq_hz[best_lo];
            pr->band_hi_hz = pr->freq_hz[best_hi];
        }
    }

    /* spectral tilt: high-band minus low-band average SNR (split at 1950 Hz) */
    {
        double lo = 0.0, hi = 0.0;
        int nlo = 0, nhi = 0;
        for (k = 0; k < NF_V34_PROBE_NTONES; k++) {
            if (pr->freq_hz[k] < 1950.0) { lo += pr->snr_db[k]; nlo++; }
            else { hi += pr->snr_db[k]; nhi++; }
        }
        pr->tilt_db = (nhi ? hi / nhi : 0.0) - (nlo ? lo / nlo : 0.0);
    }

    /* broadband SNR estimate, calibrated onto the Es/N0 scale the slicer
     * requirement curve (v34_rate_req_snr_db) uses. Derivation: for 21 equal
     * cosines of amplitude a at total RMS = level and white noise sigma, the
     * mean per-bin Goertzel noise POWER is E[amp^2] = 4*sigma^2/N, and each
     * tone's amp^2 = a^2 = level^2 * 2/21, so
     *   10log10(mean(a^2)/mean(noise_amp^2)) = 10log10(N/42) + SNR_dB,
     * i.e. subtract (10log10(N) - 16.23) to recover the channel SNR in dB. */
    {
        double sig = 0.0, npw = 0.0, blo, bhi;
        int ns = 0, nn = 0;
        blo = pr->band_hi_hz > 0.0 ? pr->band_lo_hz : 0.0;
        bhi = pr->band_hi_hz > 0.0 ? pr->band_hi_hz : 4000.0;
        for (k = 0; k < NF_V34_PROBE_NTONES; k++) {
            if (pr->freq_hz[k] >= blo && pr->freq_hz[k] <= bhi) {
                sig += pr->tone_amp[k] * pr->tone_amp[k];
                ns++;
            }
        }
        for (k = 0; k < NF_V34_PROBE_NHG; k++) {
            double f = 75.0 + 150.0 * k;
            if (f >= blo && f <= bhi) { npw += hg_amp[k] * hg_amp[k]; nn++; }
        }
        if (ns == 0) {
            for (k = 0; k < NF_V34_PROBE_NTONES; k++) { sig += pr->tone_amp[k] * pr->tone_amp[k]; ns++; }
        }
        if (nn == 0) { npw = nmed * nmed; nn = 1; }
        sig /= (double) ns;
        npw /= (double) nn;
        if (npw < 1e-12) npw = 1e-12;
        cal = 10.0 * log10((double) N) - 16.23;
        pr->band_snr_db = 10.0 * log10(sig / npw) - cal;
        if (pr->band_snr_db < 0.0) pr->band_snr_db = 0.0;
        if (pr->band_snr_db > 60.0) pr->band_snr_db = 60.0;
    }
    #undef NF_V34_PROBE_NHG

    free(x);
    return 0;
}

/* highest data rate at symbol rate si whose slicer SNR requirement is met by
 * snr_db (with a small margin), capped at `cap` bit/s (<=0 = uncapped); at
 * least the symbol rate's floor rate. */
static int v34_probe_proj_rate(int si, double snr_db, int cap)
{
    int mask = nf_v34_rate_mask(si);
    const double margin = 1.0;
    int i, best = 0;

    for (i = 1; i <= 14; i++) {
        int r = i * 2400;
        if (!(mask & (1 << (i - 1))))
            continue;
        if (cap > 0 && r > cap)
            break;
        if (v34_rate_req_snr_db(si, r) <= snr_db - margin)
            best = r;
    }
    if (best == 0)
        best = v34_sess_floor_rate(si);
    return best;
}

int nf_v34_probe_select(const nf_v34_probe_t *pr, int local_max_cap,
                        nf_v34_probe_sel_t *sel)
{
    const double beta = 0.12;
    /* GUARD lets the widest signal band (S=3429 spans 39..3879 Hz) sit just
     * past the probe's outermost tones (150..3750 Hz) - the excess-bandwidth
     * edges carry little energy. Tuned so a clean line selects 3429. */
    const double GUARD = 200.0;
    double flo = pr->band_lo_hz, fhi = pr->band_hi_hz;
    int si, best_si = -1;

    memset(sel, 0, sizeof(*sel));
    sel->freq_offset_hz = pr->freq_offset_hz;
    if (fhi <= 0.0) {                       /* nothing usable: lowest S */
        sel->srate_idx = NF_V34_RATE_2400;
        sel->high_carrier = 0;
        sel->projected_max_rate = v34_sess_floor_rate(NF_V34_RATE_2400);
        return -1;
    }
    for (si = NF_V34_NUM_RATES - 1; si >= 0; si--) {
        const nf_v34_srate_t *sr = &nf_v34_srates[si];
        double S = (double) sr->a / (double) sr->c * 2400.0;
        double hw = (1.0 + beta) / 2.0 * S;
        int co, bestco = -1;
        double bestmargin = -1e9;

        for (co = 0; co < 2; co++) {
            double carrier = (double) sr->car[co].d / (double) sr->car[co].e * S;
            double lo = carrier - hw, hi = carrier + hw;
            double m, mlo, mhi;
            if (lo < flo - GUARD || hi > fhi + GUARD)
                continue;                   /* band does not fit */
            mlo = lo - (flo - GUARD);
            mhi = (fhi + GUARD) - hi;
            m = mlo < mhi ? mlo : mhi;      /* min edge margin */
            if (m > bestmargin) { bestmargin = m; bestco = co; }
        }
        if (bestco >= 0) {
            sel->per_srate_rate[si] =
                v34_probe_proj_rate(si, pr->band_snr_db, local_max_cap);
            if (best_si < 0) {
                best_si = si;
                sel->srate_idx = si;
                sel->high_carrier = bestco;
            }
        } else {
            sel->per_srate_rate[si] = 0;
        }
    }
    if (best_si < 0) {                      /* no S fit: fall back to lowest */
        sel->srate_idx = NF_V34_RATE_2400;
        sel->high_carrier = 0;
        sel->projected_max_rate =
            v34_probe_proj_rate(NF_V34_RATE_2400, pr->band_snr_db, local_max_cap);
        return -1;
    }
    sel->projected_max_rate = sel->per_srate_rate[best_si];
    return 0;
}

/* Locate a clean, uniform-level analysis window inside a received probe burst.
 * The burst is [tone B | L1 | L2] then a decaying gate-close tail: tone B is
 * the loudest, L1 is 6 dB above L2, and L2 is the last sustained plateau at
 * the nominal level. Picking a window that straddles any of those amplitude
 * steps (or the tail) would leak the strong tones into every DFT bin and wreck
 * the noise-floor estimate, so this finds the longest block-run at L2's level
 * and returns a centred sub-window. Returns 1 with ws,wl set, else 0. */
static int v34_probe_window(const int16_t *b, long n, long *ws, long *wl)
{
    const long B = 400;
    long nb = n / B, i, j, m = 0;
    double *rms, base = 0.0;
    long best_lo = -1, best_hi = -1, run_lo = -1, blen = 0;

    if (nb < 6)
        return 0;
    rms = malloc(sizeof(double) * (size_t) nb);
    if (!rms)
        return 0;
    for (i = 0; i < nb; i++) {
        double s = 0.0;
        for (j = 0; j < B; j++) { double v = (double) b[i * B + j]; s += v * v; }
        rms[i] = sqrt(s / (double) B);
    }
    /* L2 level = median of the last 40% of blocks (pure L2 + tail; median
     * rejects the tail) */
    {
        double tmp[4096];
        long lo = nb * 6 / 10;
        for (i = lo; i < nb && m < 4096; i++)
            if (rms[i] > 200.0) tmp[m++] = rms[i];
        if (m == 0) { free(rms); return 0; }
        for (i = 1; i < m; i++) {
            double key = tmp[i];
            j = i - 1;
            while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = key;
        }
        base = tmp[m / 2];
    }
    /* longest run of blocks within [0.75,1.4] * base (excludes tone B, L1 and
     * the decaying tail); ties resolve to the later run (= L2, not an earlier
     * plateau) */
    for (i = 0; i <= nb; i++) {
        int ok = (i < nb) && rms[i] >= 0.75 * base && rms[i] <= 1.4 * base;
        if (ok) {
            if (run_lo < 0) run_lo = i;
        } else if (run_lo >= 0) {
            long len = i - run_lo;
            if (len >= blen) { blen = len; best_lo = run_lo; best_hi = i - 1; }
            run_lo = -1;
        }
    }
    free(rms);
    if (best_lo < 0)
        return 0;
    {
        long rs = best_lo * B, re = (best_hi + 1) * B, rl = re - rs;
        long win = rl - 800;                    /* 400-sample margin each side */
        if (win > 4800) win = 4800;
        win -= win % 1600;                      /* exact-bin analysis window */
        if (win < 1600) win = 1600;
        if (win > rl) { win = rl - (rl % 1600); if (win < 1600) return 0; }
        *ws = rs + (rl - win) / 2;
        *wl = win;
    }
    return 1;
}

/* recipient: initial honest rate cap from the TRN-training SNR estimate
 * (stage 2's probing analysis will refine this; the TRN residual is
 * already a direct, validated measurement of the equalized channel) */
static void v34_sess_cap_from_snr(nf_v34_sess_t *s, double snr_db)
{
    const double margin_db = 1.0;
    int mask = nf_v34_rate_mask(s->srate_idx);
    int i, best = v34_sess_floor_rate(s->srate_idx);

    for (i = 1; i <= 14; i++) {
        if (!(mask & (1 << (i - 1))))
            continue;
        if (i * 2400 > s->local_max_rate)
            break;
        if (v34_rate_req_snr_db(s->srate_idx, i * 2400) <= snr_db - margin_db)
            best = i * 2400;
    }
    if (best < s->local_max_rate) {
        SDBG(s, "line SNR estimate %.1f dB caps the rate at %d bit/s\n",
             snr_db, best);
        s->local_max_rate = best;
    }
}

void nf_v34_sess_free(nf_v34_sess_t *s)
{
    if (!s)
        return;
    if (s->ccs_on)
        nf_v34_cc_tx_free(&s->ccs_tx);
    free(s->ccs_pcm);
    free(s->pcm);
    free(s->burst);
    free(s->p2_seg);
    free(s->p2_roll);
    free(s);
}

/* fresh tx burst buffer (replaces any previous one) */
static int16_t *v34_sess_pcm_new(nf_v34_sess_t *s, long n)
{
    free(s->pcm);
    s->pcm = calloc((size_t) n, sizeof(int16_t));
    if (!s->pcm) {
        s->pcm_len = s->pcm_pos = 0;
        s->pcm_active = 0;
        return NULL;
    }
    s->pcm_len = n;
    s->pcm_pos = 0;
    s->pcm_active = 1;
    s->pcm_kind = NF_V34_SESS_STARTUP;  /* default; the lazy data-burst
                                         * builders in nf_v34_sess_tx override */
    return s->pcm;
}

static void v34_sess_report(nf_v34_sess_t *s, int ok)
{
    if (s->done)
        return;
    s->done = 1;
    s->st = ok ? V34S_DONE : V34S_FAILED;
    if (ok) {
        s->cc_established = 1;
        s->floor_hard_fails = 0;
    }
    if (s->in_retrain) {
        /* re-establishment after a mid-call control-channel retrain (12.8):
         * resume the steady-state control/primary alternation where it was.
         * Do NOT re-fire the training-status callback (the upper T.30 layer
         * was never told the physical layer went down). */
        s->in_retrain = 0;
        s->tx_mode = s->saved_tx_mode;
        SDBG(s, "retrain %s: control channel %s\n",
             ok ? "complete" : "FAILED",
             ok ? "re-established, resuming" : "not re-established");
        return;
    }
    SDBG(s, "startup %s\n", ok ? "complete: control channel established"
                               : "FAILED");
    if (s->status_fn)
        s->status_fn(s->user, ok ? NF_SIG_TRAINING_SUCCEEDED
                                 : NF_SIG_TRAINING_FAILED);
}

/* ── INFO frame contents (the fixed parameter set - see file comment) ─── */

static void v34_sess_info0(nf_v34_info_frame_t *f, int ack)
{
    memset(f, 0, sizeof(*f));
    f->sr2743 = 1;
    f->sr2800 = 1;
    f->sr3429 = 1;
    f->low3000 = 1;
    f->high3000 = 1;
    f->low3200 = 1;
    f->high3200 = 1;
    f->allow_3429 = 1;
    f->support_1664pt = 1;     /* required for rates > 28800 (Table 23 n.1) */
    f->info0_ack = ack;
}

static void v34_sess_infoh(const nf_v34_sess_t *s, nf_v34_info_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    f->is_infoh = 1;
    f->trn_len = V34S_TRN_UNITS;
    f->trn_16pt = 1;
    f->symrate_idx = s->srate_idx;
    f->high_carrier = s->high_carrier;
    f->power_reduction = 0;
    f->preemph_idx = 0;
}

/* pcparams for the session's current operating point; `rate` <= 0 uses the
 * symbol rate's top rate (good enough for phase-3 signals, which only need
 * the baud/carrier - the constellation fields are unused there). */
static int v34_sess_pcparams(const nf_v34_sess_t *s, int rate,
                             nf_v34_pcparams_t *pp)
{
    if (rate <= 0)
        rate = v34_sess_top_rate(s->srate_idx);
    return nf_v34_pcparams_init(pp, s->srate_idx, rate, s->high_carrier,
                                1 /* expanded */, 1 /* nonlinear */,
                                s->infoh_16pt ? 1 : 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Real-time Phase 2 tone handshake (ITU-T V.34 clause 12.2.1, Figure 23)
 *
 * "Call modem as source" is our real-fax case (12.2.1). Both roles are
 * implemented so the loopback answerer follows the same clause. Unlike the
 * rest of the session engine (energy-gated bursts), this runs sample-
 * synchronously with the full-duplex pump: the role continuously emits its
 * tone/probe/INFO while real-time detectors watch the peer's tone frequency
 * for presence and 180-degree phase reversals. Tone/probe waveforms are pure
 * functions of the absolute output-sample index (matching nf_v34_tone_tx /
 * nf_v34_probe_tx), so they stay phase-continuous across state changes;
 * INFO frames are pre-rendered with nf_v34_info_tx and streamed.
 *
 * NOW REAL (were faked before): continuous Tone B after INFO0c (the missing
 * piece that deadlocked the real machine - it received our INFO0c, ACK'd it,
 * but never saw Tone B so never left 12.2.1.4.1's INFO0a loop); 2400 Hz
 * Tone A detection + phase-reversal detection; the 40 ms Tone-A-reversal ->
 * Tone-B-reversal ranging rule (12.2.1.1.3); INFOh reception. The recipient
 * measures RTD from its Tone-A-reversal-TX to Tone-B-reversal-RX (minus the
 * source's 40 ms), stored in s->rtd_ms.
 * ═══════════════════════════════════════════════════════════════════════ */

static int v34_sess_burst_push(nf_v34_sess_t *s, int16_t x);     /* fwd */

/* absolute-index tone/probe generators (mirror nf_v34_tone_tx/probe_tx) */
static double v34_p2_toneB(long idx, long rev, double level)     /* 1200 Hz */
{
    long ph = ((idx % 20) * 3) % 20;
    double v = level * cos(2.0 * M_PI * (double) ph / 20.0);
    return (rev >= 0 && idx >= rev) ? -v : v;
}

static double v34_p2_toneA(long idx, long rev, double level)  /* 2400 + 1800 */
{
    long ph = ((idx % 20) * 6) % 20;
    double v = level * cos(2.0 * M_PI * (double) ph / 20.0);
    long gph;
    if (rev >= 0 && idx >= rev)
        v = -v;
    gph = ((idx % 40) * 9) % 40;                     /* 1800 Hz guard, -6 dB */
    v += level * pow(10.0, -6.0 / 20.0) * cos(2.0 * M_PI * (double) gph / 40.0);
    return v;
}

static double v34_p2_probe(long idx, int is_l1, double rms)     /* Table 17 */
{
    static const int mult[21] =
        { 1, 2, 3, 4, 5, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
    static const int phi180[21] =
        { 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0 };
    double a = rms / sqrt(21.0 / 2.0), v = 0.0;
    int k;

    if (is_l1)
        a *= pow(10.0, 6.0 / 20.0);
    for (k = 0; k < 21; k++) {
        long ph = ((idx % 160) * 3 * mult[k]) % 160;
        double c = cos(2.0 * M_PI * (double) ph / 160.0);
        v += phi180[k] ? -a * c : a * c;
    }
    return v;
}

static int16_t v34_p2_clip(double v)
{
    if (v > 32767.0)  return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t) lrint(v);
}

/* real-time tone-presence + 180-degree-reversal detector on the peer's tone.
 * Returns 1 on the sample a phase reversal (envelope notch + antiphase
 * recovery) of the locked pure tone is confirmed. */
static int v34_p2_detect(nf_v34_sess_t *s, double x, double f)
{
    double w = 2.0 * M_PI * f / (double) NF_SAMPLE_RATE;
    double c = cos(s->p2_det_th), sn = sin(s->p2_det_th);
    double zr = x * c, zi = -x * sn;                 /* x * e^{-j th} */
    double a = 1.0 / 24.0;                           /* ~3 ms leaky LP (the
                                                      * reversed tone is only
                                                      * 10 ms, 12.2.1.2.3) */
    double mag, mag2, rmag, dot, inrms;
    int on, rev = 0;

    s->p2_det_th += w;
    if (s->p2_det_th > 2.0 * M_PI)
        s->p2_det_th -= 2.0 * M_PI;
    s->p2_acc_re += (zr - s->p2_acc_re) * a;
    s->p2_acc_im += (zi - s->p2_acc_im) * a;
    s->p2_inpow  += (x * x - s->p2_inpow) / 64.0;
    mag2  = s->p2_acc_re * s->p2_acc_re + s->p2_acc_im * s->p2_acc_im;
    mag   = sqrt(mag2);
    inrms = sqrt(s->p2_inpow);
    /* pure tone: |acc| ~ A/2, inrms ~ A/sqrt2  ->  |acc|/inrms ~ 0.35 */
    on = (mag > 0.18 * inrms) && (inrms > 40.0);
    if (on) { s->p2_tone_run++; s->p2_off_run = 0; s->p2_saw_tone = 1; }
    else    { s->p2_off_run++; s->p2_tone_run = 0; }

    rmag = sqrt(s->p2_ref_re * s->p2_ref_re + s->p2_ref_im * s->p2_ref_im);
    dot  = s->p2_acc_re * s->p2_ref_re + s->p2_acc_im * s->p2_ref_im;
    /* lock the reference on a pure, phase-stable tone (rejects INFO DPSK) */
    if (on && s->p2_tone_run > 96) {
        if (rmag < 0.3 * mag) {                       /* not locked: snap ref */
            s->p2_ref_re = s->p2_acc_re; s->p2_ref_im = s->p2_acc_im;
            s->p2_stable_run = 0;
        } else if (dot > 0.5 * mag * rmag) {          /* in phase: pure tone */
            s->p2_ref_re += (s->p2_acc_re - s->p2_ref_re) / 128.0;
            s->p2_ref_im += (s->p2_acc_im - s->p2_ref_im) / 128.0;
            if (++s->p2_stable_run >= 160)            /* 20 ms pure tone */
                s->p2_seg_pure = 1;
        } else {                                      /* unstable (e.g. DPSK) */
            s->p2_stable_run = 0;
        }
    } else {
        s->p2_stable_run = 0;
    }
    if (s->p2_off_run > 80) {                          /* >=10 ms silence: a new
                                                        * tone segment follows -
                                                        * drop the stale ref so
                                                        * it re-locks fresh */
        s->p2_seg_pure = 0;
        s->p2_ref_re = s->p2_ref_im = 0.0;
    }
    /* Reversal = a SINGLE envelope notch of the locked pure tone (its component
     * collapses as the phase flips through zero; the 1800 Hz guard keeps inrms
     * up, so the presence gate must not reset us) followed by SUSTAINED
     * antiphase. INFO DPSK also notches - but every symbol (~1.7 ms) - so a
     * second notch soon after, or antiphase that does not persist, marks the
     * segment as DPSK (seg_pure cleared) rather than a reversal. This both
     * catches the 10 ms reversed tone (12.2.1.2.3/.1.1.3) and rejects the
     * real machine's INFO0a that abuts Tone A with no silence gap. */
    if (s->p2_arm_rev && s->p2_seg_pure && rmag > 500.0) {
        s->p2_notch_age++;
        switch (s->p2_notch) {
        case 0:                                       /* watching pure tone */
            if (mag < 0.25 * rmag) { s->p2_notch = 1; s->p2_notch_age = 0; }
            break;
        case 1:                                       /* notched: await recovery */
            if (mag > 0.5 * rmag) {                   /* tone came back */
                if (dot < -0.4 * mag * rmag)          /* antiphase: candidate */
                    { s->p2_notch = 2; s->p2_notch_age = 0; }
                else                                  /* same phase: spurious */
                    s->p2_notch = 0;
            } else if (s->p2_notch_age > 200) {
                s->p2_notch = 0; s->p2_seg_pure = 0;  /* no recovery: tone end */
            }
            break;
        case 2:                                       /* confirm sustained antiph. */
            if (mag < 0.25 * rmag) {                  /* a 2nd notch: DPSK, not a
                                                       * reversal - reject */
                s->p2_notch = 0; s->p2_seg_pure = 0;
            } else if (dot > 0.0) {                    /* phase flipped back: DPSK */
                s->p2_notch = 0; s->p2_seg_pure = 0;
            } else if (s->p2_notch_age >= 24) {        /* 3 ms stable antiphase,
                                                        * past a DPSK symbol */
                rev = 1;
                s->p2_notch = 0;
                s->p2_ref_re = s->p2_acc_re;           /* re-lock reversed */
                s->p2_ref_im = s->p2_acc_im;
            }
            break;
        }
    } else {
        s->p2_notch = 0;
    }
    return rev;
}

/* rolling RX buffer for periodic INFO decode */
static void v34_p2_roll_push(nf_v34_sess_t *s, int16_t x)
{
    if (!s->p2_roll) {
        s->p2_roll_cap = 8000;
        s->p2_roll = malloc(sizeof(int16_t) * (size_t) s->p2_roll_cap);
        s->p2_roll_n = 0;
        if (!s->p2_roll) return;
    }
    if (s->p2_roll_n >= s->p2_roll_cap) {             /* keep the last 6400 */
        long keep = 6400;
        memmove(s->p2_roll, s->p2_roll + (s->p2_roll_n - keep),
                sizeof(int16_t) * (size_t) keep);
        s->p2_roll_n = keep;
        s->p2_scan_at -= (s->p2_roll_cap - keep);
        if (s->p2_scan_at < 0) s->p2_scan_at = 0;
    }
    s->p2_roll[s->p2_roll_n++] = x;
}

static int v34_p2_scan_info(nf_v34_sess_t *s, int want_infoh,
                            nf_v34_info_frame_t *out)
{
    double carrier = s->is_call ? 2400.0 : 1200.0;   /* peer's INFO channel */
    nf_v34_info_frame_t fr[8];
    int m, j;

    if (!s->p2_roll || s->p2_roll_n < 1500)
        return 0;
    if (s->p2_in - s->p2_scan_at < 320)              /* scan every ~40 ms */
        return 0;
    s->p2_scan_at = s->p2_in;
    m = nf_v34_info_rx_batch(s->p2_roll, (int) s->p2_roll_n, carrier, fr, 8);
    for (j = 0; j < m; j++)
        if (fr[j].is_infoh == want_infoh) { *out = fr[j]; return 1; }
    return 0;
}

/* render an INFO frame into the streaming segment buffer */
static void v34_p2_start_info0(nf_v34_sess_t *s, int ack)
{
    nf_v34_info_frame_t f;
    long nsym, need;

    v34_sess_info0(&f, ack);
    nsym = 66;                                        /* 65 bits + ref point */
    need = (nsym * V34_CC_NUM) / V34_CC_DEN + 512;
    free(s->p2_seg);
    s->p2_seg = calloc((size_t) need, sizeof(int16_t));
    if (!s->p2_seg) { s->p2_seg_len = 0; return; }
    nsym = nf_v34_info_tx(&f, s->is_call ? 0 : 1, s->p2_seg, need, 0,
                          V34S_INFO_GAIN);
    s->p2_seg_len = (nsym * V34_CC_NUM) / V34_CC_DEN + 40;
    if (s->p2_seg_len > need) s->p2_seg_len = need;
    s->p2_seg_pos = 0;
    s->p2_state = s->is_call ? P2C_INFO0 : P2A_INFO0;
    s->p2_state_out = s->p2_out;
}

static void v34_p2_start_infoh(nf_v34_sess_t *s)
{
    nf_v34_info_frame_t f;
    long nsym, need;

    v34_sess_infoh(s, &f);
    s->infoh_trn_len = f.trn_len;
    s->infoh_16pt = f.trn_16pt;
    nsym = 68;                                        /* 67 bits + ref point */
    need = (nsym * V34_CC_NUM) / V34_CC_DEN + 512;
    free(s->p2_seg);
    s->p2_seg = calloc((size_t) need, sizeof(int16_t));
    if (!s->p2_seg) { s->p2_seg_len = 0; return; }
    nsym = nf_v34_info_tx(&f, 1, s->p2_seg, need, 0, V34S_INFO_GAIN);
    s->p2_seg_len = (nsym * V34_CC_NUM) / V34_CC_DEN + 40;
    if (s->p2_seg_len > need) s->p2_seg_len = need;
    s->p2_seg_pos = 0;
    s->p2_state = P2A_INFOH;
    s->p2_state_out = s->p2_out;
    SDBG(s, "Phase 2: send INFOh S=%d %s carrier TRN %dx35ms %d-pt\n",
         nf_v34_srates[s->srate_idx].baud_name, s->high_carrier ? "high" : "low",
         f.trn_len, f.trn_16pt ? 16 : 4);
}

/* the recipient's probe (L1/L2) analysis, run on the captured probe samples */
static void v34_p2_analyze_probe(nf_v34_sess_t *s)
{
    nf_v34_probe_t pr;
    nf_v34_probe_sel_t psel;
    long start = 0, win = 0;

    if (s->probe_override) {
        SDBG(s, "Phase 2 probe: S/carrier forced -> S=%d %s carrier\n",
             nf_v34_srates[s->srate_idx].baud_name,
             s->high_carrier ? "high" : "low");
        return;
    }
    if (v34_probe_window(s->burst, s->burst_n, &start, &win) &&
        nf_v34_probe_analyze(s->burst + start, win, &pr) == 0 &&
        nf_v34_probe_select(&pr, s->local_max_rate, &psel) >= 0) {
        s->srate_idx = psel.srate_idx;
        s->high_carrier = psel.high_carrier;
        if (s->local_max_rate > v34_sess_top_rate(s->srate_idx))
            s->local_max_rate = v34_sess_top_rate(s->srate_idx);
        if (psel.projected_max_rate > 0 &&
            psel.projected_max_rate < s->local_max_rate)
            s->local_max_rate = psel.projected_max_rate;
        SDBG(s, "Phase 2 probe (%.2f s): band %.0f-%.0f Hz SNR %.1f dB tilt "
             "%+.1f dB foff %+.1f Hz -> S=%d %s carrier (cap %d)\n",
             (double) win / 8000.0, pr.band_lo_hz, pr.band_hi_hz, pr.band_snr_db,
             pr.tilt_db, pr.freq_offset_hz, nf_v34_srates[s->srate_idx].baud_name,
             s->high_carrier ? "high" : "low", s->local_max_rate);
    } else {
        SDBG(s, "Phase 2 probe: inconclusive - keeping S=%d %s carrier\n",
             nf_v34_srates[s->srate_idx].baud_name,
             s->high_carrier ? "high" : "low");
    }
}

static void v34_p2_init(nf_v34_sess_t *s)
{
    s->p2_state   = s->is_call ? P2C_SILENCE : P2A_SILENCE;
    s->p2_out = s->p2_in = 0;
    s->p2_state_out = 0;
    s->p2_seg_pos = s->p2_seg_len = 0;
    s->p2_rev_out = s->p2_l1_out = s->p2_l2_out = -1;
    s->p2_evt_in = -1;
    s->p2_det_th = 0.0;
    s->p2_acc_re = s->p2_acc_im = s->p2_ref_re = s->p2_ref_im = 0.0;
    s->p2_inpow = 0.0;
    s->p2_tone_on = 0;
    s->p2_tone_run = s->p2_off_run = s->p2_stable_run = 0;
    s->p2_notch = 0; s->p2_notch_age = 0;
    s->p2_arm_rev = s->p2_saw_tone = s->p2_info_seen = 0;
    s->p2_seg_pure = 0;
    s->p2_roll_n = s->p2_scan_at = 0;
    SDBG(s, "Phase 2 (real-time tone handshake) start: %s\n",
         s->is_call ? "call=source (12.2.1.1)" : "answer=recipient (12.2.1.2)");
}

static void v34_p2_finish(nf_v34_sess_t *s)
{
    s->p2_state = P2_OFF;
    free(s->p2_seg);  s->p2_seg = NULL;  s->p2_seg_len = s->p2_seg_pos = 0;
    free(s->p2_roll); s->p2_roll = NULL; s->p2_roll_n = s->p2_roll_cap = 0;
    /* hand the line back to the burst machinery that drives Phase 3+ */
    s->gate_open = 0; s->open_run = s->close_run = 0;
    s->pre_n = s->pre_pos = 0; s->burst_n = 0;
    s->st = s->is_call ? V34S_C_PHASE3_Q : V34S_A_TRN_WAIT;
    SDBG(s, "Phase 2 complete -> Phase 3 (%s), rtd=%.1f ms\n",
         s->is_call ? "send S/PP/TRN" : "await TRN", s->rtd_ms);
}

/* Phase-2 TX generator (called from nf_v34_sess_tx while p2 is active) */
static void v34_p2_tx(nf_v34_sess_t *s, int16_t *amp, int max_len)
{
    int i;

    for (i = 0; i < max_len; i++) {
        double v = 0.0;
        long idx = s->p2_out;

        switch (s->p2_state) {
        /* ── call modem = source (12.2.1.1) ── */
        case P2C_SILENCE:                            /* 75 ms silence */
            if (idx - s->p2_state_out >= 600)
                v34_p2_start_info0(s, 0);            /* INFO0c bit28=0 */
            break;
        case P2C_INFO0:
            if (s->p2_seg_pos < s->p2_seg_len)
                v = s->p2_seg[s->p2_seg_pos++];
            else { s->p2_state = P2C_TONEB; s->p2_state_out = idx; }
            break;
        case P2C_TONEB:                              /* Tone B, await ToneA rev */
            v = v34_p2_toneB(idx, -1, V34S_TONE_LEVEL);
            break;
        case P2C_TB_REV:                             /* Tone B with reversal */
            v = v34_p2_toneB(idx, s->p2_rev_out, V34S_TONE_LEVEL);
            if (idx >= s->p2_rev_out + 80) {         /* +10 ms then L1 */
                s->p2_state = P2C_L1; s->p2_l1_out = idx;
                SDBG(s, "Phase 2: Tone B reversal sent -> L1/L2 line probing "
                     "(12.2.1.1.3)\n");
            }
            break;
        case P2C_L1:
            v = v34_p2_probe(idx, 1, V34S_PROBE_RMS);
            if (idx - s->p2_l1_out >= 1280) {        /* L1 = 160 ms */
                s->p2_state = P2C_L2; s->p2_l2_out = idx;
            }
            break;
        case P2C_L2:
            v = v34_p2_probe(idx, 0, V34S_PROBE_RMS);
            if (idx - s->p2_l2_out >= 4400) {        /* cap L2 at 550 ms */
                s->p2_state = P2C_TONEB2; s->p2_state_out = idx;
                SDBG(s, "Phase 2: L2 timeout, Tone B awaiting INFOh\n");
            }
            break;
        case P2C_TONEB2:                             /* Tone B, await INFOh */
            v = v34_p2_toneB(idx, -1, V34S_TONE_LEVEL);
            break;
        /* ── answer modem = recipient (12.2.1.2) ── */
        case P2A_SILENCE:
            if (idx - s->p2_state_out >= 600)
                v34_p2_start_info0(s, s->p2_info_seen);  /* INFO0a, ack=have 0c */
            break;
        case P2A_INFO0:
            if (s->p2_seg_pos < s->p2_seg_len)
                v = s->p2_seg[s->p2_seg_pos++];
            else { s->p2_state = P2A_TONEA; s->p2_state_out = idx; }
            break;
        case P2A_TONEA:                              /* Tone A, await Tone B */
            v = v34_p2_toneA(idx, -1, V34S_TONE_LEVEL);
            if (idx - s->p2_state_out >= 5600)       /* resend INFO0a ~700 ms */
                v34_p2_start_info0(s, s->p2_info_seen);
            break;
        case P2A_TA_REV:                             /* Tone A with reversal */
            v = v34_p2_toneA(idx, s->p2_rev_out, V34S_TONE_LEVEL);
            if (idx >= s->p2_rev_out + 80) {         /* +10 ms then silence */
                s->p2_state = P2A_SIL2; s->p2_state_out = idx;
                s->burst_n = 0;                      /* capture probe next */
            }
            break;
        case P2A_SIL2:                               /* silence, await ToneB rev */
            /* 12.2.1.4.2: Tone B reversal not detected -> re-send INFO0a and
             * Tone A, then reverse again (also gives the caller, which may not
             * yet have decoded INFO0a to arm, another INFO0a + reversal) */
            if (idx - s->p2_state_out >= 6400) {     /* 800 ms */
                SDBG(s, "Phase 2: no Tone B reversal (12.2.1.4.2) - retrying "
                     "INFO0a/Tone A + reversal\n");
                v34_p2_start_info0(s, s->p2_info_seen);
            }
            break;
        case P2A_PROBE:                              /* silence, receive L1/L2 */
            if (idx - s->p2_state_out >= 2880) {     /* ~360 ms probe reception */
                v34_p2_analyze_probe(s);
                s->p2_state = P2A_TONEA2; s->p2_state_out = idx;
                s->p2_rev_out = -1;                  /* marker: Tone B not seen */
            }
            break;
        case P2A_TONEA2:                             /* Tone A, await Tone B */
            v = v34_p2_toneA(idx, -1, V34S_TONE_LEVEL);
            if (s->p2_rev_out >= 0 && idx >= s->p2_rev_out)
                v34_p2_start_infoh(s);               /* +25 ms Tone A elapsed */
            break;
        case P2A_INFOH:
            if (s->p2_seg_pos < s->p2_seg_len)
                v = s->p2_seg[s->p2_seg_pos++];
            else
                v34_p2_finish(s);                    /* -> Phase 3 (await TRN) */
            break;
        default:
            break;
        }
        amp[i] = v34_p2_clip(v);
        s->p2_out++;
    }
}

/* Phase-2 RX detector + state advance (called from nf_v34_sess_rx) */
static void v34_p2_rx(nf_v34_sess_t *s, const int16_t *amp, int len)
{
    double f = s->is_call ? 2400.0 : 1200.0;         /* the peer's tone */
    nf_v34_info_frame_t fr;
    int i;

    for (i = 0; i < len; i++) {
        int rev;

        /* arm the reversal detector only where a reversal is expected */
        s->p2_arm_rev = (s->is_call && s->p2_state == P2C_TONEB && s->p2_info_seen)
                     || (!s->is_call && s->p2_state == P2A_SIL2);
        rev = v34_p2_detect(s, (double) amp[i], f);
        v34_p2_roll_push(s, amp[i]);
        /* capture probe samples for the recipient's analysis */
        if (!s->is_call &&
            (s->p2_state == P2A_SIL2 || s->p2_state == P2A_PROBE))
            (void) v34_sess_burst_push(s, amp[i]);
        s->p2_in++;

        switch (s->p2_state) {
        /* caller */
        case P2C_TONEB:
            if (!s->p2_info_seen && v34_p2_scan_info(s, 0, &fr)) {
                s->p2_info_seen = 1;                 /* 12.2.1.1.2: arm rev det */
                SDBG(s, "Phase 2: rx INFO0a (ack=%d) - arming Tone A reversal "
                     "detector\n", fr.info0_ack);
            }
            if (s->p2_info_seen && rev) {            /* Tone A reversal seen */
                s->p2_rev_out = s->p2_in + 320;      /* Tone B rev +40 ms */
                s->p2_state = P2C_TB_REV;
                SDBG(s, "Phase 2: Tone A reversal detected -> Tone B "
                     "reversal in 40 ms (12.2.1.1.3)\n");
            }
            break;
        case P2C_L2:
            if (s->p2_stable_run >= 240) {             /* Tone A back on (30 ms) */
                s->p2_state = P2C_TONEB2; s->p2_state_out = s->p2_out;
                SDBG(s, "Phase 2: Tone A re-detected -> Tone B, await INFOh\n");
            }
            break;
        case P2C_TONEB2:
            if (v34_p2_scan_info(s, 1, &fr)) {       /* INFOh */
                if (fr.symrate_idx >= 0 && fr.symrate_idx < NF_V34_NUM_RATES)
                    s->srate_idx = fr.symrate_idx;
                s->high_carrier = fr.high_carrier ? 1 : 0;
                s->infoh_trn_len = fr.trn_len;
                s->infoh_16pt = fr.trn_16pt;
                if (s->local_max_rate > v34_sess_top_rate(s->srate_idx))
                    s->local_max_rate = v34_sess_top_rate(s->srate_idx);
                SDBG(s, "Phase 2: rx INFOh symrate=%d hc=%d TRN %dx35ms %d-pt\n",
                     fr.symrate_idx, fr.high_carrier, fr.trn_len,
                     fr.trn_16pt ? 16 : 4);
                v34_p2_finish(s);
            }
            break;
        /* answerer */
        case P2A_TONEA:
            if (!s->p2_info_seen && v34_p2_scan_info(s, 0, &fr)) {
                s->p2_info_seen = 1;                 /* got INFO0c: ACK next 0a */
                SDBG(s, "Phase 2: rx INFO0c - ACK set in INFO0a\n");
            }
            /* Tone B present >=50 ms and Tone A sent >=80 ms -> Tone A rev
             * (the extra Tone A lets the caller lock its 2400 reference and
             * mark the segment pure before the reversal) */
            if (s->p2_stable_run >= 400 &&
                s->p2_out - s->p2_state_out >= 640) {
                s->p2_rev_out = s->p2_out;           /* reverse now (12.2.1.2.3) */
                s->p2_evt_in = s->p2_in;             /* RTD reference */
                s->p2_state = P2A_TA_REV;
                SDBG(s, "Phase 2: Tone B detected -> Tone A reversal "
                     "(12.2.1.2.3)\n");
            }
            break;
        case P2A_SIL2:
            if (rev) {                               /* Tone B reversal seen */
                long interval = s->p2_in - s->p2_evt_in;
                s->rtd_ms = ((double) interval - 320.0) / 8.0;  /* -40 ms */
                if (s->rtd_ms < 0.0) s->rtd_ms = 0.0;
                s->p2_state = P2A_PROBE; s->p2_state_out = s->p2_out;
                SDBG(s, "Phase 2: Tone B reversal detected, RTD=%.1f ms - "
                     "receiving L1/L2 (12.2.1.2.4/.5)\n", s->rtd_ms);
            }
            break;
        case P2A_TONEA2:
            if (s->p2_rev_out < 0 && s->p2_stable_run >= 240) {
                /* Tone B back: continue Tone A 25 ms, then INFOh (12.2.1.2.6) */
                s->p2_rev_out = s->p2_out + 200;
                SDBG(s, "Phase 2: Tone B re-detected -> 25 ms Tone A + INFOh\n");
            }
            break;
        default:
            break;
        }
    }
}

/* ── startup tx burst builders ─────────────────────────────────────────── */

static void v34_sess_q_info0(nf_v34_sess_t *s)
{
    nf_v34_info_frame_t f;
    long lead = V34S_LEAD_STARTUP;
    int16_t *b;

    if (s->is_call) {
        long n = lead + 900 + V34S_TAIL;
        v34_sess_info0(&f, 0);
        b = v34_sess_pcm_new(s, n);
        if (!b)
            return;
        nf_v34_info_tx(&f, 0, b, n, lead, V34S_INFO_GAIN);
        SDBG(s, "tx INFO0c\n");
    } else {
        /* INFO0a, a 200 ms gap (so the far gate closes and decodes it),
         * then tone A with a (choreographed, unmeasured) phase reversal */
        long tone_at = lead + 900 + 1600;
        long tone_dur = 3200;
        long n = tone_at + tone_dur + V34S_TAIL;
        v34_sess_info0(&f, 1);
        b = v34_sess_pcm_new(s, n);
        if (!b)
            return;
        nf_v34_info_tx(&f, 1, b, n, lead, V34S_INFO_GAIN);
        nf_v34_tone_tx(b, n, tone_at, tone_dur, 1, tone_at + tone_dur / 2,
                       V34S_TONE_LEVEL);
        SDBG(s, "tx INFO0a + tone A (phase reversal)\n");
    }
}

static void v34_sess_q_tones_probe(nf_v34_sess_t *s)
{
    long lead = V34S_LEAD_STARTUP;
    long toneb = 3600, l1 = 1280, l2 = 5600;    /* 450/160/700 ms */
    long n = lead + toneb + l1 + l2 + V34S_TAIL;
    int16_t *b = v34_sess_pcm_new(s, n);

    if (!b)
        return;
    nf_v34_tone_tx(b, n, lead, toneb, 0, lead + 2000, V34S_TONE_LEVEL);
    nf_v34_probe_tx(b, n, lead + toneb, l1, 1, V34S_PROBE_RMS);
    nf_v34_probe_tx(b, n, lead + toneb + l1, l2, 0, V34S_PROBE_RMS);
    SDBG(s, "tx tone B (phase reversal) + L1/L2 line probing\n");
}

static void v34_sess_q_infoh(nf_v34_sess_t *s)
{
    nf_v34_info_frame_t f;
    long lead = V34S_LEAD_STARTUP;
    long n = lead + 950 + V34S_TAIL;
    int16_t *b = v34_sess_pcm_new(s, n);

    if (!b)
        return;
    v34_sess_infoh(s, &f);
    /* the recipient's own record of what it asked the source to send */
    s->infoh_trn_len = f.trn_len;
    s->infoh_16pt = f.trn_16pt;
    nf_v34_info_tx(&f, 1, b, n, lead, V34S_INFO_GAIN);
    SDBG(s, "tx INFOh: S=%d %s carrier, TRN %dx35ms %d-point\n",
         nf_v34_srates[s->srate_idx].baud_name,
         s->high_carrier ? "high" : "low", f.trn_len, f.trn_16pt ? 16 : 4);
}

static void v34_sess_q_phase3(nf_v34_sess_t *s)
{
    nf_v34_pcparams_t pcp;
    long lead = V34S_LEAD_STARTUP;
    int units = s->infoh_trn_len > 0 ? s->infoh_trn_len : V34S_TRN_UNITS;
    int trn;
    long nsym, n, i;
    double *re, *im;
    double rms = 0.0;
    int16_t *b;

    if (v34_sess_pcparams(s, 0, &pcp) < 0)
        return;
    trn = units * pcp.trn_unit_sym;
    nsym = 432 + trn;
    re = malloc(sizeof(double) * (size_t) nsym);
    im = malloc(sizeof(double) * (size_t) nsym);
    if (!re || !im) {
        free(re);
        free(im);
        return;
    }
    nf_v34_pc_sspp(re, im);
    nf_v34_pc_trn(trn, s->infoh_16pt, re + 432, im + 432);
    for (i = 432; i < nsym; i++)
        rms += re[i] * re[i] + im[i] * im[i];
    rms = sqrt(rms / (double) trn);
    for (i = 432; i < nsym; i++) {          /* TRN at unit RMS, like PP */
        re[i] /= rms;
        im[i] /= rms;
    }
    n = lead + (nsym * pcp.sps_num) / pcp.sps_den + V34S_TAIL;
    b = v34_sess_pcm_new(s, n);
    if (b)
        nf_v34_pc_modulate(&pcp, b, n, lead, re, im, nsym, V34S_PRI_GAIN);
    free(re);
    free(im);
    SDBG(s, "tx phase-3 S/Sbar/PP + TRN (%d symbols, %d-point)\n",
         trn, s->infoh_16pt ? 16 : 4);
}

/* ── control-channel burst builder (startup handshake + T.30 data) ─────── */

static void v34_sess_mph(const nf_v34_sess_t *s, nf_v34_mph_fields_t *f)
{
    int cap = s->local_max_rate / 2400;

    memset(f, 0, sizeof(*f));
    f->type = 0;
    f->max_rate = cap;         /* N x 2400 bit/s (Table 23 bits 20:23) */
    f->cc_rate = s->cc_rate_adv;  /* bit 27: advertise 2400 cc when set */
    f->trellis_size = 0;       /* 16-state */
    f->nonlinear = 1;
    f->shaping = 1;
    /* honest mask: the rates this implementation supports at the selected
     * symbol rate, clipped at our advertised cap */
    f->rate_mask = nf_v34_rate_mask(s->srate_idx) & ((1 << cap) - 1);
}

struct v34_cc_src {
    nf_v34_sess_t *s;
    nf_hdlc_tx_t tx;
    int next;
};

static void v34_cc_underflow(void *user)
{
    struct v34_cc_src *c = user;

    if (c->next < c->s->nframes)
        nf_hdlc_tx_frame(&c->tx, c->s->frames[c->next],
                         c->s->frame_len[c->next]);
    else if (c->next == c->s->nframes)
        nf_hdlc_tx_frame(&c->tx, NULL, 0);          /* end marker */
    c->next++;
}

/* drain an armed nf_hdlc_tx_t into a growable bit array (LSB-level bits) */
static long v34_sess_collect_bits(nf_hdlc_tx_t *tx, uint8_t **out)
{
    long cap = 8192, n = 0;
    uint8_t *bits = malloc((size_t) cap);
    int b;

    *out = NULL;
    if (!bits)
        return -1;
    for (;;) {
        b = nf_hdlc_tx_get_bit(tx);
        if (b == NF_SIG_END_OF_DATA)
            break;
        if (n + 4 > cap) {           /* keep >= 4 slack for the alignment pad */
            uint8_t *nb;
            cap *= 2;
            if (cap > 4000000L) {                    /* runaway guard */
                free(bits);
                return -1;
            }
            nb = realloc(bits, (size_t) cap);
            if (!nb) {
                free(bits);
                return -1;
            }
            bits = nb;
        }
        bits[n++] = (uint8_t) b;
    }
    while (n & 3)
        bits[n++] = 1;              /* align to 4 bits (1200 dibit AND 2400 quad) */
    *out = bits;
    return n;
}

/* ── held control-channel tx stream (T.30 Annex F F.3.1.4/F.3.2) ────────
 * See the ccs_* field comments: the cc is NOT sent as bursts. The stream is
 * a growing symbol accumulator; the rendered PCM is re-generated ahead of
 * the playout position (the renderer is a pure function of the symbol array
 * and the ABSOLUTE sample index, so extension is prefix-exact). */

static void v34_ccs_reset(nf_v34_sess_t *s)
{
    if (s->ccs_on) {
        nf_v34_cc_tx_free(&s->ccs_tx);
        s->ccs_on = 0;
    }
    free(s->ccs_pcm);
    s->ccs_pcm = NULL;
    s->ccs_pcm_len = s->ccs_pcm_cap = 0;
    s->ccs_out = 0;
    s->ccs_state = CCS_OFF;
    s->ccs_data_end_sym = -1;
    s->ccs_data_pending = 0;
    s->ccs_pre_mph = 0;
    s->ccs_form_startup = 0;
    s->ccs_peer_mph = 0;
    s->ccs_ones_bits = 0;
    s->ccs_gap_left = 0;
}

static void v34_ccs_render(nf_v34_sess_t *s)
{
    long n = CCS_LEAD + (s->ccs_tx.nsym * V34_CC_NUM) / V34_CC_DEN + 400;
    double gain = V34S_CC_GAIN;

    if (s->ccs_pcm_cap < n) {
        long nc = s->ccs_pcm_cap ? s->ccs_pcm_cap : 16384;
        int16_t *nb;
        while (nc < n)
            nc *= 2;
        nb = realloc(s->ccs_pcm, sizeof(int16_t) * (size_t) nc);
        if (!nb)
            return;
        s->ccs_pcm = nb;
        s->ccs_pcm_cap = nc;
    }
    memset(s->ccs_pcm, 0, sizeof(int16_t) * (size_t) n);
    /* Deterministic peak normalisation: nf_v34_cc_tx_modulate normalises
     * over whatever is accumulated SO FAR, which would retroactively change
     * already-played samples once 16-point (2400 bit/s) data is appended.
     * Decide once, from whether 2400-mode data can appear in this stream.
     * (The answer role's guard tone level also depends on mean symbol
     * energy - constant for 1200-only streams, the only ones we advertise
     * by default.) */
    if (s->cc_rate || s->cc_rate_adv)
        gain *= sqrt(2.0 / 18.0);
    v34_tx_cc_qam(s->ccs_pcm, n, CCS_LEAD, s->ccs_tx.re, s->ccs_tx.im,
                  s->ccs_tx.nsym, !s->is_call, gain);
    s->ccs_pcm_len = n;
}

/* samples of the render that are final (later appends only disturb the last
 * ~V34_CC_SPAN symbol tails; keep a 16-symbol guard) */
static long v34_ccs_usable(const nf_v34_sess_t *s)
{
    long nsym = s->ccs_tx.nsym - 16;

    if (nsym < 0)
        nsym = 0;
    return CCS_LEAD + (nsym * V34_CC_NUM) / V34_CC_DEN;
}

static void v34_ccs_flags(nf_v34_sess_t *s, int nflags)
{
    static const uint8_t flag[8] = { 0, 1, 1, 1, 1, 1, 1, 0 };  /* 0x7E LSB */
    int i;

    nf_v34_cc_tx_set_rate(&s->ccs_tx, s->cc_rate);
    for (i = 0; i < nflags; i++)
        (void) nf_v34_cc_tx_bits(&s->ccs_tx, flag, 8);
}

static void v34_ccs_ones(nf_v34_sess_t *s, int nbits)
{
    static const uint8_t ones[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    int i;

    nf_v34_cc_tx_set_rate(&s->ccs_tx, s->cc_rate);
    for (i = 0; i < nbits; i += 8)
        (void) nf_v34_cc_tx_bits(&s->ccs_tx, ones, 8);
    s->ccs_ones_bits += nbits;
}

/* start a held cc stream with the appropriate preamble: the long blind
 * startup form (12.4, MPh over-provisioned - the peer starts listening only
 * after answering our PPh), a PPh renegotiation carrying changed caps, or
 * the short Sh/S̄h resync when the caps are unchanged (12.6). */
static void v34_ccs_begin(nf_v34_sess_t *s, int startup_form)
{
    nf_v34_mph_fields_t f;
    int use_sh, mi, mph_reps;

    v34_ccs_reset(s);
    nf_v34_cc_tx_init(&s->ccs_tx, s->is_call);
    s->ccs_on = 1;
    v34_sess_mph(s, &f);
    use_sh = !startup_form && s->cc_established &&
             f.max_rate  == s->cc_last_adv_rate &&
             f.rate_mask == s->cc_last_adv_mask;
    if (use_sh) {
        nf_v34_cc_tx_sh(&s->ccs_tx);       /* Sh(24T) + S̄h(8T), no PPh/MPh */
        nf_v34_cc_tx_alt(&s->ccs_tx, 100);
        nf_v34_cc_tx_e(&s->ccs_tx);
        s->sh_resyncs++;
    } else {
        mph_reps = 2;
        nf_v34_cc_tx_pph(&s->ccs_tx);
        nf_v34_cc_tx_alt(&s->ccs_tx, 100);
        for (mi = 0; mi < mph_reps; mi++)
            nf_v34_cc_tx_mph(&s->ccs_tx, &f);
        if (s->cc_established && s->cc_last_adv_rate >= 0 &&
            (f.max_rate != s->cc_last_adv_rate ||
             f.rate_mask != s->cc_last_adv_mask))
            s->pph_renegs++;              /* a genuine mid-call renegotiation */
        s->cc_last_adv_rate = f.max_rate;
        s->cc_last_adv_mask = f.rate_mask;
    }
    if (startup_form && !use_sh) {
        /* 12.4.1.3/12.4.2.4: keep sending MPh sequences until the peer's MPh
         * has been decoded, THEN send E - so the flags following E are at
         * the negotiated cc user-data rate */
        s->ccs_pre_mph = 1;
        s->ccs_form_startup = 1;
        s->cc_reneg_pending = 0;       /* we are answering/initiating it now */
    } else {
        nf_v34_cc_tx_e(&s->ccs_tx);
        v34_ccs_flags(s, 4);   /* F.3.1.4: >= 2 flags before the first frame */
    }
    s->ccs_state = CCS_RUN;
    SDBG(s, "tx cc stream: %s, carrier held (F.3.1.4)\n",
         use_sh ? "Sh/S̄h/ALT/E short resync (12.6, params unchanged) + flags"
                : (startup_form ? "PPh/ALT + MPh loop until peer MPh (12.4)"
                                : "PPh/ALT/MPh/MPh/E restart (12.4, new caps) + flags"));
    v34_ccs_render(s);
}

/* insert the queued T.30 frames inline into the held stream */
static void v34_ccs_append_frames(nf_v34_sess_t *s)
{
    struct v34_cc_src src;
    uint8_t *bits = NULL;
    long nbits;
    int i;

    src.s = s;
    src.next = 0;
    nf_hdlc_tx_init(&src.tx, 2, v34_cc_underflow, &src);
    nf_hdlc_tx_flags(&src.tx, 8);
    nbits = v34_sess_collect_bits(&src.tx, &bits);
    /* user data runs at the negotiated cc rate (training stays 1200);
     * collect_bits pads to a multiple of 4 so both 1200 (2 b/sym) and 2400
     * (4 b/sym) symbol alignment is satisfied */
    nf_v34_cc_tx_set_rate(&s->ccs_tx, s->cc_rate);
    if (nbits > 0)
        (void) nf_v34_cc_tx_bits(&s->ccs_tx, bits, nbits);
    free(bits);
    for (i = 0; i < s->nframes; i++) {
        SDBG(s, "tx cc frame fcf=0x%02x len=%d (inline, carrier held)\n",
             s->frame_len[i] >= 3 ? s->frames[i][2] : 0, s->frame_len[i]);
        memcpy(s->ccs_saved[i], s->frames[i], (size_t) s->frame_len[i]);
        s->ccs_saved_len[i] = s->frame_len[i];
    }
    s->ccs_nsaved = s->nframes;
    s->nframes = 0;
    v34_ccs_flags(s, 2);
    s->ccs_data_end_sym = s->ccs_tx.nsym;
    s->ccs_data_pending = 1;
    v34_ccs_render(s);
}

/* the held-stream tx pump: fills amp[0..max_len) and returns 1, or returns 0
 * when the stream is off and the ordinary burst path should run instead */
static int v34_ccs_tx(nf_v34_sess_t *s, int tx_mode, int16_t *amp, int max_len)
{
    long avail;
    int len = 0;

    if (s->ccs_state == CCS_OFF)
        return 0;

    if (s->ccs_state == CCS_RUN) {
        if (s->cc_reneg_pending && !s->ccs_form_startup) {
            /* The peer initiated a PPh/MPh cc start-up mid-call (Figure 26):
             * abandon the current Sh-form stream and answer with the
             * start-up interlock form. A data section the old stream was
             * still carrying (the very command the peer wants re-issued at
             * the new rate) is re-queued so it goes out right after E -
             * the peer's give-up timer may not outlast nf_t30's retry. */
            /* re-queue even when the section already finished playing: the
             * peer restarted because it did NOT (usably) receive it - its
             * receiver was deaf during its own restart. A duplicated
             * command is protocol-safe (same command -> same response). */
            int requeue = s->nframes == 0 && s->ccs_nsaved > 0;
            SDBG(s, "tx cc stream: peer initiated PPh/MPh restart - "
                 "answering with start-up form (Fig. 26)%s\n",
                 requeue ? ", re-queueing aborted frames" : "");
            if (requeue) {
                int i;
                for (i = 0; i < s->ccs_nsaved; i++) {
                    memcpy(s->frames[i], s->ccs_saved[i],
                           (size_t) s->ccs_saved_len[i]);
                    s->frame_len[i] = s->ccs_saved_len[i];
                }
                s->nframes = s->ccs_nsaved;
            }
            v34_ccs_begin(s, 1);
        }
        if (tx_mode == NF_V34_SESS_PRI) {
            /* B/D-phase turnaround (F.3.2.3): >= 40 consecutive ones, then
             * hold ones until the peer's cc carrier falls silent */
            v34_ccs_ones(s, 48);
            s->ccs_gap_left = CCS_ONES_WAIT;        /* wait budget */
            s->ccs_state = CCS_ONES;
            v34_ccs_render(s);
            SDBG(s, "tx cc stream: turnaround - >=40 ones, awaiting peer "
                 "silence (F.3.2.3)\n");
        } else if (tx_mode != NF_V34_SESS_CC &&
                   tx_mode != NF_V34_SESS_STARTUP) {
            s->ccs_state = CCS_DRAIN;               /* e.g. DCN sent, line down */
        } else if (tx_mode == NF_V34_SESS_CC && s->nframes > 0 &&
                   !s->ccs_data_pending && !s->ccs_pre_mph) {
            v34_ccs_append_frames(s);
        }
    }

    /* startup MPh interlock (12.4.1.3/12.4.2.4): loop MPh until the peer's
     * MPh has been decoded (initial establishment or a Figure-26 restart),
     * then E + flags at the negotiated cc rate */
    if (s->ccs_state == CCS_RUN && s->ccs_pre_mph && s->ccs_peer_mph) {
        nf_v34_cc_tx_e(&s->ccs_tx);
        v34_ccs_flags(s, 4);
        s->ccs_pre_mph = 0;
        /* the interlock just completed: MPh from it lingers in the held
         * carriers and keeps showing up in windowed re-scans - not a new
         * restart (see cc_reneg_holdoff) */
        s->cc_reneg_holdoff = s->now + 4L * 8000;
        v34_ccs_render(s);
        SDBG(s, "tx cc stream: peer MPh decoded - E + flags at negotiated "
             "cc rate (12.4)\n");
    }
    /* keep the final (tail-stable) render comfortably ahead of playout */
    while (s->ccs_state == CCS_RUN &&
           v34_ccs_usable(s) - s->ccs_out < CCS_MARGIN) {
        if (s->ccs_pre_mph) {
            nf_v34_mph_fields_t f;
            v34_sess_mph(s, &f);
            nf_v34_cc_tx_mph(&s->ccs_tx, &f);
        } else {
            v34_ccs_flags(s, 30);                   /* ~0.2 s of idle flags */
        }
        v34_ccs_render(s);
    }
    while (s->ccs_state == CCS_ONES &&
           v34_ccs_usable(s) - s->ccs_out < CCS_MARGIN) {
        v34_ccs_ones(s, 48);
        v34_ccs_render(s);
    }

    avail = (s->ccs_state == CCS_DRAIN ? s->ccs_pcm_len : v34_ccs_usable(s))
            - s->ccs_out;
    if (avail > 0) {
        len = (avail < max_len) ? (int) avail : max_len;
        memcpy(amp, s->ccs_pcm + s->ccs_out, (size_t) len * sizeof(int16_t));
        s->ccs_out += len;
    }
    memset(amp + len, 0, (size_t) (max_len - len) * sizeof(int16_t));

    /* a queued data section has fully left the modulator: report completion
     * while the carrier stays up (there is no burst end to infer it from) */
    if (s->ccs_data_pending && s->ccs_data_end_sym >= 0 &&
        s->ccs_out >= CCS_LEAD +
            ((s->ccs_data_end_sym + V34_CC_SPAN) * V34_CC_NUM) / V34_CC_DEN) {
        s->ccs_data_pending = 0;
        if (s->status_fn)
            s->status_fn(s->user, NF_SIG_SEND_COMPLETE);
    }

    if (s->ccs_state == CCS_ONES) {
        s->ccs_gap_left -= max_len;
        if ((s->ccs_ones_bits >= 40 && !s->gate_open) || s->ccs_gap_left <= 0) {
            /* peer's cc carrier is down (or wait budget spent): play the
             * rendered tail out, then drop ours; the following primary
             * burst provides the 70 +- 5 ms silent gap as its lead-in */
            s->ccs_state = CCS_DRAIN;
            SDBG(s, "tx cc stream: peer silent - dropping cc carrier for "
                 "the primary channel\n");
        }
    }
    if (s->ccs_state == CCS_DRAIN && s->ccs_out >= s->ccs_pcm_len)
        v34_ccs_reset(s);                           /* carrier down */
    return 1;
}

/* ── primary-channel burst builder (ECM block, driven by nf_t30) ───────── */

/* NOTE: the session must NEVER truncate the frame stream itself - the
 * frame count is T.30 ECM accounting (the PPS frame total), and a
 * silently-shortened burst makes the receiver believe the block was
 * smaller, dropping the tail of the page. Burst duration is bounded where
 * the accounting lives: nf_t30's ecm_build_block sizes V.34 blocks by the
 * negotiated rate so a block stays under the rx gate's 30 s force-process
 * guard. (A rate-fallback retransmission of a full block can still exceed
 * it; the guard then chops the RX burst, the tail frames go missing, and
 * the ordinary PPR path recovers them - slow but correct.) */
struct v34_pri_src {
    nf_v34_sess_t *s;
    nf_hdlc_tx_t tx;
    int done;
    int nframes;
};

static void v34_pri_underflow(void *user)
{
    struct v34_pri_src *p = user;
    uint8_t buf[4 + 256 + 8];
    int n = 0;

    if (!p->done && p->s->get_frame)
        n = p->s->get_frame(p->s->get_frame_user, buf, (int) sizeof(buf));
    if (n > 0) {
        nf_hdlc_tx_frame(&p->tx, buf, n);
        p->nframes++;
    } else if (!p->done) {
        p->done = 1;
        nf_hdlc_tx_frame(&p->tx, NULL, 0);
    }
}

static void v34_sess_build_pri(nf_v34_sess_t *s)
{
    struct v34_pri_src src;
    nf_v34_pcparams_t pcp;
    uint8_t *bits = NULL;
    double *re = NULL, *im = NULL;
    long nbits, nsym, n;
    int16_t *b;

    if (v34_sess_pcparams(s, s->rate_cur, &pcp) < 0) {
        SDBG(s, "tx primary burst: no negotiated rate\n");
        return;
    }
    src.s = s;
    src.done = 0;
    src.nframes = 0;
    nf_hdlc_tx_init(&src.tx, 1, v34_pri_underflow, &src);
    nf_hdlc_tx_flags(&src.tx, 4);
    nbits = v34_sess_collect_bits(&src.tx, &bits);
    if (nbits <= 0) {
        free(bits);
        SDBG(s, "tx primary burst: no data\n");
        return;
    }
    /* Trailing flag pad (~100 ms at the negotiated rate): the burst tail is
     * decode-hostile at BOTH ends of the link - the rx page decoder detects
     * carrier end by slice-distance blow-up at chunk granularity and a real
     * frame ending flush with the carrier drop can die with the junk chunk.
     * Idle HDLC flags carry nothing, so push the real content clear of the
     * tail. (Bit count stays a multiple of 8, preserving the alignment
     * collect_bits established.) */
    {
        long pad = ((long) pcp.rate / 10 + 7) / 8 * 8, k;
        uint8_t *nb = realloc(bits, (size_t) (nbits + pad));
        if (nb) {
            static const uint8_t flag[8] = { 0, 1, 1, 1, 1, 1, 1, 0 };
            bits = nb;
            for (k = 0; k < pad; k++)
                bits[nbits + k] = flag[k & 7];
            nbits += pad;
        }
    }
    nsym = nf_v34_pc_burst_build(&pcp, bits, nbits, &re, &im);
    free(bits);
    if (nsym < 0)
        return;
    n = V34S_LEAD + (nsym * pcp.sps_num) / pcp.sps_den + V34S_TAIL;
    b = v34_sess_pcm_new(s, n);
    if (b)
        nf_v34_pc_modulate(&pcp, b, n, V34S_LEAD, re, im, nsym, V34S_PRI_GAIN);
    free(re);
    free(im);
    SDBG(s, "tx primary burst: %d HDLC frames, %ld bits at %d bit/s ->"
         " S/Sbar/PP+B1+data, %ld symbols (%.1f s)\n", src.nframes, nbits,
         pcp.rate, nsym, (double) n / 8000.0);
}

/* ── rx burst decoding ─────────────────────────────────────────────────── */

/* INFO scan over the finished burst; remote carrier is role-determined */
static int v34_sess_info_scan(nf_v34_sess_t *s, int want_infoh,
                              nf_v34_info_frame_t *out)
{
    nf_v34_info_frame_t fr[8];
    double carrier = s->is_call ? 2400.0 : 1200.0;
    int n, i;

    n = nf_v34_info_rx_batch(s->burst, (int) s->burst_n, carrier, fr, 8);
    for (i = 0; i < n; i++) {
        if (fr[i].is_infoh == want_infoh) {
            *out = fr[i];
            return 1;
        }
    }
    return 0;
}

#define V34S_CC_EARLY 2464      /* symbols captured for the Sh/PPh correlators:
                                 * the PPh of a mid-carrier restart can sit
                                 * anywhere in a scan window (4 s = 2400 sym) */

/* v34_sess_process_cc flags */
#define V34CC_DELIVER 1     /* hand frames straight up via frame_fn         */
#define V34CC_STASH   2     /* hold frames in s->rxq (flushed after report) */
#define V34CC_SCAN    4     /* incremental mid-burst scan: drop bad frames  */

struct v34_sess_ccrx {
    nf_v34_sess_t *s;
    nf_v34_mp_rx_t mp;
    nf_v34_ccdata_rx_t cc;
    int nmp, flags, nframes;
    int last_max_rate, last_mask;    /* fields of the last CRC-valid MPh */
    int last_cc_rate;                /* its bit 27 (peer's advertised cc rate) */
    nf_cpx_t early[V34S_CC_EARLY];   /* first symbols, for the Sh correlator */
    int nearly;
};

/* Sh/S̄h vs PPh discriminator (12.6.2.1: "condition its receiver to detect
 * signal PPh or signal Sh followed by S̄h"). Rotation-invariant correlator on
 * the demodulated symbol stream (carrier/timing recovered, pre-differential-
 * decode): Sh/S̄h is a pure two-point quarter-turn alternation - EVERY
 * inter-symbol step is +-90 deg - whereas PPh (eq 10-2) contains
 * consecutive-identical symbols (step ~ 0 deg) and ALT is random QPSK. Returns
 * the best fraction of clean quarter-turns over any 16-step window in the
 * first ~44 symbols; >= 0.75 indicates the Sh/S̄h preamble. */
static double v34_cc_sh_score(const nf_cpx_t *z, int nsym)
{
    /* Sh/S̄h occupies the first 32 symbols: keep the original 48-symbol
     * search window (V34S_CC_EARLY is larger for the PPh correlator, and
     * PPh itself contains quarter-turn runs that would fake an Sh hit) */
    int lim = nsym < 48 ? nsym : 48;
    double best = 0.0;
    int i;

    if (lim < 18)
        return 0.0;
    for (i = 1; i + 16 <= lim; i++) {
        int good = 0, k;
        for (k = 0; k < 16; k++) {
            nf_cpx_t a = z[i + k], b = z[i + k - 1];
            double re = (double) a.re * b.re + (double) a.im * b.im; /* Re(a·b*) */
            double im = (double) a.im * b.re - (double) a.re * b.im; /* Im(a·b*) */
            double mag = sqrt(re * re + im * im);
            /* quarter turn <=> |cos(step)| < 0.5 (step within +-30 deg of
             * +-90); repeats (~0) and reversals (~180) have |cos| ~ 1 */
            if (mag > 1e-6 && fabs(re) / mag < 0.5)
                good++;
        }
        if ((double) good / 16.0 > best)
            best = (double) good / 16.0;
    }
    return best;
}

/* PPh detector on the demodulated early symbols: normalized, rotation-
 * invariant correlation against the 32-symbol eq 10-2 reference, searched
 * over the alignment (the burst gate prepends a variable silence/RRC ramp).
 * A cc burst that OPENS with PPh is a control-channel (re)start (12.4 /
 * 12.6.1.3 / 12.6.2.3) - a peer changing modulation parameters answers an
 * Sh resync with PPh + ALT and then WAITS for our PPh, sending no MPh and
 * no HDLC at all until we reply, so PPh itself must be recognized. */
static double v34_cc_pph_score(const nf_cpx_t *z, int nsym)
{
    double best = 0.0;
    int a, i;

    for (a = 0; a + 32 <= nsym; a++) {
        double cr = 0.0, ci = 0.0, pz = 0.0;
        for (i = 0; i < 32; i++) {
            int k = i / 2, I = i % 2;
            double th = M_PI * (2.0 * k * (k - I) + 1.0) / 4.0;
            double pr = cos(th), pi = sin(th);
            cr += (double) z[a + i].re * pr + (double) z[a + i].im * pi;
            ci += (double) z[a + i].im * pr - (double) z[a + i].re * pi;
            pz += (double) z[a + i].re * z[a + i].re +
                  (double) z[a + i].im * z[a + i].im;
        }
        if (pz > 1e-9) {
            double c = sqrt((cr * cr + ci * ci) / (pz * 32.0));
            if (c > best)
                best = c;
        }
    }
    return best;
}

static void v34_sess_cc_frame(void *user, const uint8_t *msg, int len, int ok)
{
    struct v34_sess_ccrx *r = user;

    if (msg && len > 0 && ok) {
        r->nframes++;
    } else if (r->flags & V34CC_SCAN) {
        /* a bad/aborted frame in a mid-burst scan may just be the cut at
         * the buffer edge - it will decode cleanly next scan */
        return;
    } else if (len < 3) {
        /* HDLC abort/idle debris (a held stream ends mid-flag when the
         * carrier drops): not a frame, nothing for T.30 to act on */
        return;
    }
    if (r->flags & V34CC_STASH) {      /* hold until flushed (post-report,
                                        * content-deduplicated) */
        nf_v34_sess_t *s = r->s;
        if (msg && len > 0 && len <= V34S_RXFRAME_MAX &&
            s->nrxq < V34S_NFRAMES) {
            memcpy(s->rxq[s->nrxq], msg, (size_t) len);
            s->rxq_len[s->nrxq] = len;
            s->rxq_ok[s->nrxq] = ok;
            s->nrxq++;
        }
        return;
    }
    if ((r->flags & V34CC_DELIVER) && r->s->frame_fn)
        r->s->frame_fn(r->s->user, msg, len, ok);
}

static void v34_sess_cc_symbol(void *user, const nf_cpx_t *z)
{
    struct v34_sess_ccrx *r = user;

    if (r->nearly < V34S_CC_EARLY)
        r->early[r->nearly++] = *z;
    if (nf_v34_mp_feed_symbol(&r->mp, z)) {
        r->nmp++;
        r->last_max_rate = r->mp.max_rate_c2a * 2400;
        r->last_mask = r->mp.rate_mask;
        r->last_cc_rate = r->mp.cc_rate;
    }
    if (r->flags & (V34CC_DELIVER | V34CC_STASH))
        nf_v34_ccdata_feed_symbol(&r->cc, z);
}

/* Decode one finished control-channel burst. deliver = forward FCS-checked
 * HDLC frames up (T.30 data bursts); startup handshake bursts only count
 * MPh frames. Returns the count of CRC-valid MPh frames seen. */
static int v34_sess_process_cc(nf_v34_sess_t *s, int flags, long from)
{
    struct v34_sess_ccrx r;
    double carrier = s->is_call ? 2400.0 : 1200.0;
    int cfo_limit;

    if (from < 0 || from >= s->burst_n)
        from = 0;
    memset(&r, 0, sizeof(r));
    r.s = s;
    r.flags = flags;
    nf_v34_mp_rx_init(&r.mp, !s->is_call);
    nf_v34_ccdata_rx_init(&r.cc, !s->is_call, v34_sess_cc_frame, &r);
    /* decode this burst's user data at the rate negotiated by the PREVIOUS
     * exchange (this burst's own MPh, parsed below, applies to the NEXT one) */
    nf_v34_ccdata_rx_set_rate(&r.cc, s->cc_rate);
    /* At 2400 bit/s the 16-point user data biases the 4th-power CFO estimate;
     * confine it to the 1200-mode training prefix (Sh resync carries >=142
     * training symbols; 130 stays safely inside it). 1200 bursts pass 0 and
     * are bit-identical to before. Incremental (windowed) scans always
     * confine it - a window can begin mid-data. */
    cfo_limit = (flags & V34CC_SCAN) ? 130 : (s->cc_rate ? 130 : 0);
    nf_v34_cc_rx_batch(s->burst + from, (int) (s->burst_n - from), carrier,
                       v34_sess_cc_symbol, &r, cfo_limit);
    /* Discriminate the turnaround form (12.6.2.1): an MPh present means a full
     * PPh restart carrying (possibly new) caps - a renegotiation; no MPh means
     * the short Sh/S̄h resync (parameters unchanged). The Sh correlator
     * corroborates the MPh-presence decision on the raw symbol stream. */
    s->last_rx_cc_sh_score = v34_cc_sh_score(r.early, r.nearly);
    s->last_rx_cc_pph_score = v34_cc_pph_score(r.early, r.nearly);
    /* A burst from an established peer that either carries MPh, or OPENS
     * with PPh (a 12.6.2.3 answer: PPh + held ALT, no MPh until it hears
     * OUR PPh), is a cc start-up it initiated - answer with the start-up
     * interlock form unless we are already in it. */
    if (s->cc_established && s->now >= s->cc_reneg_holdoff &&
        !(s->ccs_state != CCS_OFF && s->ccs_form_startup) &&
        (r.nmp > 0 || r.cc.alt_hold ||
         (s->last_rx_cc_pph_score >= 0.75 && s->last_rx_cc_sh_score < 0.75))) {
        if (!s->cc_reneg_pending)
            SDBG(s, "rx cc burst: peer cc start-up detected (MPh %d, "
                 "ALT-hold %d, PPh-score %.2f, Sh-score %.2f) - reneg "
                 "pending\n", r.nmp, r.cc.alt_hold,
                 s->last_rx_cc_pph_score, s->last_rx_cc_sh_score);
        s->cc_reneg_pending = 1;
    }
    if (r.nmp > 0) {
        s->last_rx_cc_kind = NF_V34_CC_PPH_RENEG;
        s->ccs_peer_mph = 1;
        if (!(flags & V34CC_SCAN) || s->cc_nseen == 0)
            SDBG(s, "rx cc burst%s: PPh/MPh restart - %d MPh frame(s) (peer "
                 "max %d bit/s, mask 0x%04x, Sh-score %.2f)%s\n",
                 (flags & V34CC_SCAN) ? " (scan)" : "", r.nmp, r.last_max_rate,
                 r.last_mask, s->last_rx_cc_sh_score,
                 (flags & V34CC_DELIVER) ? "" : " [startup handshake]");
        /* 12.4: every MPh carries the peer's current max rate and rate
         * capability mask - a between-page renegotiation (the recipient
         * lowering its cap after a bad burst) rides these */
        s->remote_max_rate = r.last_max_rate;
        s->remote_mask = r.last_mask;
        s->remote_cc_rate = r.last_cc_rate;      /* bit 27: peer's cc-rate advert */
        v34_sess_pick_rate(s);
        v34_sess_pick_cc_rate(s);
    } else {
        s->last_rx_cc_kind = NF_V34_CC_SH_RESYNC;
        if (!(flags & V34CC_SCAN))
            SDBG(s, "rx cc burst: Sh short resync (no MPh, Sh-score %.2f) - "
                 "reusing negotiated params (%d frames)\n",
                 s->last_rx_cc_sh_score, r.nframes);
    }
    return r.nmp;
}

/* hand stashed rx cc frames (see v34_sess_cc_frame) up to nf_fax/nf_t30,
 * suppressing frames already delivered from this burst (content identity -
 * see the cc_seen field comment) */
static void v34_sess_flush_rxq(nf_v34_sess_t *s)
{
    int i, j, dup;

    for (i = 0; i < s->nrxq; i++) {
        dup = 0;
        for (j = 0; j < s->cc_nseen; j++)
            if (s->cc_seen_len[j] == s->rxq_len[i] &&
                memcmp(s->cc_seen[j], s->rxq[i],
                       (size_t) s->rxq_len[i]) == 0) {
                dup = 1;
                break;
            }
        if (dup)
            continue;
        if (s->cc_nseen < 16) {
            memcpy(s->cc_seen[s->cc_nseen], s->rxq[i],
                   (size_t) s->rxq_len[i]);
            s->cc_seen_len[s->cc_nseen] = s->rxq_len[i];
            s->cc_nseen++;
        }
        SDBG(s, "rx cc frame fcf=0x%02x len=%d\n",
             s->rxq_len[i] >= 3 ? s->rxq[i][2] : 0, s->rxq_len[i]);
        if (s->frame_fn)
            s->frame_fn(s->user, s->rxq[i], s->rxq_len[i], s->rxq_ok[i]);
    }
    s->nrxq = 0;
}

/* Incremental cc decode over the still-growing rx burst (see the cc_scan_at
 * field comment). Runs from the rx pump while the gate is open; establishes
 * the control channel as soon as the peer's MPh decodes, and streams user
 * frames (a real fax piggybacks NSF/CSI/DIS and repeats them inside the held
 * burst) up as they become complete. */
static void v34_sess_cc_scan(nf_v34_sess_t *s)
{
    /* window the scan: a held carrier can run for tens of seconds, and a
     * full-burst re-decode both costs O(n) and free-runs its (frozen)
     * timing across the whole span. A fresh acquisition over the recent
     * window keeps decode quality constant and spots a MID-carrier
     * PPh/MPh restart (the peer escalating) at the window head. */
    long interval = 800 + (s->burst_n > V34S_SCAN_WIN ? V34S_SCAN_WIN
                                                      : s->burst_n) / 8;
    long from;
    int can_establish, nmp;

    if (s->burst_n < 4800 || s->burst_n - s->cc_scan_at < interval)
        return;
    s->cc_scan_at = s->burst_n;
    can_establish = !s->done &&
        (s->st == V34S_C_CC_INIT_PLAY || s->st == V34S_C_CC1_WAIT ||
         s->st == V34S_A_CC1_PLAY || s->st == V34S_A_CC2_WAIT);
    s->nrxq = 0;
    from = s->burst_n > V34S_SCAN_WIN ? s->burst_n - V34S_SCAN_WIN : 0;
    nmp = v34_sess_process_cc(s, V34CC_STASH | V34CC_SCAN, from);
    if (nmp > 0 || s->nrxq > 0)
        s->cc_burst_seen = 1;           /* this burst carries control channel */
    if (can_establish && nmp > 0)
        v34_sess_report(s, 1);          /* nf_t30 arms its DIS wait / sends DIS */
    if (s->done && s->st == V34S_DONE) {
        v34_sess_flush_rxq(s);          /* frames go up NOW, mid-burst */
    } else {
        s->nrxq = 0;                    /* not established: retry next scan */
    }
}

/* recipient: locate S/Sbar/PP in the phase-3 burst and LS-train the FSE */
static int v34_sess_train(nf_v34_sess_t *s)
{
    nf_v34_pcparams_t pcp;
    double corr = 0.0, t_s, t_trn, baud;
    int units = s->infoh_trn_len > 0 ? s->infoh_trn_len : V34S_TRN_UNITS;
    int trn_sym;

    if (v34_sess_pcparams(s, 0, &pcp) < 0)
        return -1;
    baud = pcp.baud_nominal;
    trn_sym = units * pcp.trn_unit_sym;
    t_s = nf_v34_page_locate_s(s->burst, s->burst_n, 0.0, 0.0, 0.15,
                               baud, &pcp, &corr);
    if (corr < 0.5) {
        SDBG(s, "phase-3: S locator failed (corr=%.3f)\n", corr);
        return -1;
    }
    t_trn = t_s + 432.0 / baud;
    if (nf_v34_page_train(s->burst, s->burst_n, 0.0, t_trn, trn_sym - 200,
                          baud, &pcp, &s->eq) < 0 ||
        s->eq.res_holdout > 0.02) {
        SDBG(s, "phase-3: TRN training failed (res_holdout=%.5f)\n",
             s->eq.res_holdout);
        return -1;
    }
    s->have_eq = 1;
    /* the normalized training residual measures the equalized channel's
     * noise floor directly: SNR ~ -10log10(res). Cap the rate we will
     * advertise in MPh accordingly (honest initial rate; the per-burst
     * fallback below is the backstop). */
    s->snr_est_db = -10.0 * log10(s->eq.res_holdout > 1e-12 ?
                                  s->eq.res_holdout : 1e-12);
    SDBG(s, "phase-3 trained: S corr=%.3f, FSE res_train=%.5f"
         " res_holdout=%.5f (SNR est %.1f dB)\n", corr, s->eq.res_train,
         s->eq.res_holdout, s->snr_est_db);
    v34_sess_cap_from_snr(s, s->snr_est_db);
    /* Stage 4: gate the 2400 bit/s control channel on the measured line SNR.
     * The 600-baud 16-point cc mode is far more robust than the primary
     * channel (narrow band, 16 vs 224 points), so a modest threshold suffices;
     * below it, withdraw our bit-27 advertisement and fall back to 1200. Only
     * the recipient measures the line, so only it can lower the shared rate. */
    if (s->cc_rate_adv && s->snr_est_db < V34S_CC2400_MIN_SNR_DB) {
        SDBG(s, "line SNR %.1f dB below %.0f dB: withdrawing 2400 cc advert\n",
             s->snr_est_db, (double) V34S_CC2400_MIN_SNR_DB);
        s->cc_rate_adv = 0;
        v34_sess_pick_cc_rate(s);
    }
    return 0;
}

static void v34_sess_pri_frame(void *user, const uint8_t *msg, int len, int ok)
{
    nf_v34_sess_t *s = user;

    if (s->frame_fn)
        s->frame_fn(s->user, msg, len, ok);
}

/* Automatic rate fallback (the RECIPIENT side of 12.4's renegotiation):
 * judge the finished burst; on failure or degradation lower the local
 * advertised max rate - the next control-channel burst (the T.30 PPR/PPS
 * answer that triggers the block retransmission anyway) carries the lower
 * cap in its MPh, the source re-selects per 12.4.1.3, and the retransmit
 * runs at the lower rate. The step count comes from the measured median
 * slice distance when available (each 2400 bit/s step at fixed S shrinks
 * the alphabet by ~2^0.7, buying ~1.27x of noise margin), so a badly
 * mismatched rate drops several steps in one round instead of grinding
 * down one retransmission at a time. */
/* Returns 1 if the advertised max rate was actually lowered (a renegotiation
 * will follow on the next control-channel burst), 0 if already at the floor
 * (the caller then escalates to a full retrain). */
static int v34_sess_rate_fallback(nf_v34_sess_t *s, const nf_v34_page_burst_t *res,
                                  int hard_fail)
{
    const double med_ok = 0.28;      /* margin-gated slicing wants <= ~0.3 */
    int steps = 1;
    int mask = nf_v34_rate_mask(s->srate_idx);
    int cur = s->rate_cur > 0 ? s->rate_cur : s->local_max_rate;
    int i, nrate = cur;

    if (res && res->med_dist > med_ok) {
        steps = (int) ceil(log(res->med_dist / med_ok) / log(1.27));
        if (steps < 1)
            steps = 1;
        if (steps > 8)
            steps = 8;
    } else if (hard_fail) {
        steps = 3;                   /* no usable quality metric - big step */
    }
    for (i = 0; i < steps; i++) {
        int n = nrate / 2400 - 1;
        while (n >= 1 && !(mask & (1 << (n - 1))))
            n--;
        if (n < 1)
            break;                   /* already at the floor */
        nrate = n * 2400;
    }
    if (nrate < cur) {
        SDBG(s, "rate fallback: burst quality (med=%.3f%s) drops max rate"
             " %d -> %d bit/s (next cc burst renegotiates via PPh/MPh)\n",
             res ? res->med_dist : -1.0,
             hard_fail ? ", hard fail" : "", cur, nrate);
        s->local_max_rate = nrate;
        v34_sess_pick_rate(s);
        return 1;
    }
    return 0;                        /* already at the floor - unrecoverable */
}

/* ── full recovery: control-channel retrain (12.8, AC signal) ──────────────
 * Initiated when a primary-channel burst is unrecoverable even at the floor
 * rate. Resets the physical layer back to Phase 2 line probing -> Phase 3
 * equalizer training and re-establishes (the recipient re-trains its FSE, the
 * MPh caps are re-exchanged). The pumps run the existing startup choreography
 * while s->in_retrain is set (documented simplification: the tone/AC handshake
 * and the clause-12 three-second recovery timers are collapsed to the
 * loopback's energy-gated burst boundaries; 12.7's primary-channel/tone path
 * and 12.8's true duplex AC exchange share this single re-training back end).
 * Returns 0 if armed, -1 if the per-call retrain limit is reached. */
int nf_v34_sess_retrain(nf_v34_sess_t *s)
{
    if (s->retrains >= V34S_MAX_RETRAINS) {
        SDBG(s, "retrain: per-call limit (%d) reached - giving up\n",
             V34S_MAX_RETRAINS);
        return -1;
    }
    s->retrains++;
    /* Both retrain paths share this back end: 12.7 (primary retrain: tone
     * B/A -> Phase 2 -> Phase 3) and 12.8 (control-channel retrain: signal
     * AC -> PPh -> MPh). This simplified form re-runs the whole Phase 2
     * probing -> Phase 3 training choreography; the on-wire AC trigger (the
     * nf_v34_cc_tx_ac primitive) and the duplex tone handshake are collapsed
     * to the loopback's energy-gated burst boundaries. */
    SDBG(s, "full retrain #%d (12.7/12.8): re-running Phase 2 probing -> "
         "Phase 3 training\n", s->retrains);
    /* tear the physical layer down and re-arm the startup FSM */
    s->done = 0;
    s->have_eq = 0;
    s->cc_established = 0;
    s->cc_last_adv_rate = s->cc_last_adv_mask = -1;
    s->floor_hard_fails = 0;
    s->saved_tx_mode = s->tx_mode;
    v34_ccs_reset(s);             /* drop any held cc carrier immediately */
    s->in_retrain = 1;
    s->started = 1;
    s->deadline = s->now + V34S_DEADLINE;
    s->st = s->is_call ? V34S_C_START : V34S_A_INFO0C_WAIT;
    /* reset the rx gate and any half-built burst */
    s->gate_open = 0;
    s->open_run = s->close_run = 0;
    s->pre_n = s->pre_pos = 0;
    s->burst_n = 0;
    s->pcm_active = 0;
    free(s->pcm);
    s->pcm = NULL;
    s->pcm_len = s->pcm_pos = 0;
    v34_p2_init(s);                          /* re-run real-time Phase 2 */
    return 0;
}

/* decode one finished primary-channel burst and report CARRIER_DOWN */
static void v34_sess_process_pri(nf_v34_sess_t *s)
{
    nf_v34_pcparams_t pcp;
    nf_v34_page_burst_t res;
    double corr = 0.0, t_s, t_end_max, baud;
    const nf_v34_page_burst_t *rp = NULL;   /* quality metric (NULL = none)   */
    int have_res = 0, hard = 0, degraded = 0, no_eq = 0;

    if (!s->have_eq) {
        SDBG(s, "primary burst but no trained equalizer - dropped\n");
        no_eq = 1;
        goto out;
    }
    if (v34_sess_pcparams(s, s->rate_cur, &pcp) < 0) {
        no_eq = 1;
        goto out;
    }
    baud = pcp.baud_nominal;
    t_s = nf_v34_page_locate_s(s->burst, s->burst_n, 0.0, 0.0, 0.15,
                               baud, &pcp, &corr);
    if (corr < 0.5) {
        if (s->pri_maybe_cc_tail) {
            /* the receiver was pointed at the primary channel while the
             * peer's held cc carrier was still up: no S in the burst just
             * means it was that cc tail (flags + turnaround ones), not a
             * degraded line. No rate-fallback bookkeeping, and NO carrier-
             * down report: the real primary burst is still coming and
             * nf_t30 must stay armed for it. */
            SDBG(s, "rx burst: peer's cc tail after PRI switch (no S) - "
                 "ignored\n");
            return;
        }
        SDBG(s, "primary burst: S locator failed (corr=%.3f)\n", corr);
        hard = 1;
        goto out;
    }
    /* The gate's close hangover leaves ~60 ms of padding after the real
     * carrier drop, and the decoder detects the actual end itself (median
     * slice distance blow-up), so only a small guard is needed here. The
     * previous 0.11 s margin ate real data symbols whenever the trailing
     * hangover was short - at low line levels the power EMA crosses the
     * close threshold sooner after carrier drop, and short retransmission
     * bursts lost their closing HDLC flag + RCPs to it (found via the
     * gain -9 dB sweep cell, where every PPR retransmission decoded B1
     * perfectly and then zero frames). */
    t_end_max = (double) s->burst_n / 8000.0 - 0.02;
    if (nf_v34_page_decode_burst(s->burst, s->burst_n, 0.0, &s->eq, t_s,
                                 t_end_max, baud, &pcp,
                                 v34_sess_pri_frame, s, &res) < 0) {
        SDBG(s, "primary burst: decode failed\n");
        hard = 1;
        goto out;
    }
    have_res = 1;
    rp = &res;
    SDBG(s, "rx primary burst: rate %d, %ld symbols, B1 %d/%d, FCS-valid %d"
         " (FCD %d, RCP %d), bad %d, med %.3f\n", pcp.rate, res.nsym,
         res.b1_match, res.b1_bits, res.hdlc_ok, res.fcd_ok, res.rcp_ok,
         res.hdlc_bad, res.med_dist);
    hard = res.hdlc_ok == 0 || res.b1_match * 10 < res.b1_bits * 9;
    degraded = res.hdlc_bad > 0 && res.hdlc_bad * 4 >= res.hdlc_ok;
out:
    /* Unified recovery escalation (12.4 -> 12.7/12.8), reached from every
     * failure path (S-locate fail, decode fail, or a decoded-but-bad burst):
     * a 12.4 rate renegotiation first, and if we are already at the floor rate
     * and the burst is still a hard failure, a full retrain. A clean burst
     * clears the floor-fail counter. The "no equalizer / no rate yet" path is
     * not a channel-quality failure, so it neither falls back nor retrains. */
    if (!no_eq) {
        if (hard || degraded) {
            if (!v34_sess_rate_fallback(s, rp, hard) && hard) {
                if (++s->floor_hard_fails >= 2)
                    nf_v34_sess_retrain(s);
            } else {
                s->floor_hard_fails = 0;
            }
        } else if (have_res) {
            s->floor_hard_fails = 0;
        }
    }
    /* burst is over either way - hand the line back to the control channel.
     * (If a retrain was just armed, s->in_retrain now steers the pumps back
     * through the startup choreography - see nf_v34_sess_tx / v34_sess_tick.) */
    if (s->status_fn)
        s->status_fn(s->user, NF_SIG_CARRIER_DOWN);
}

/* ── startup FSM ───────────────────────────────────────────────────────── */

static void v34_sess_burst_played(nf_v34_sess_t *s)
{
    switch (s->st) {
    case V34S_C_INFO0_PLAY:
        s->st = V34S_C_INFO0_WAIT;
        s->st_timer = s->now + 4800;                 /* re-send in 600 ms */
        break;
    case V34S_C_TONES_PLAY:  s->st = V34S_C_INFOH_WAIT; break;
    case V34S_C_PHASE3_PLAY: s->st = V34S_C_CC_INIT_Q;  break;
    case V34S_C_CC_INIT_PLAY: s->st = V34S_C_CC1_WAIT;  break;
    case V34S_C_CC2_PLAY:    v34_sess_report(s, 1);     break;
    case V34S_A_INFO0A_PLAY: s->st = V34S_A_PROBE_WAIT; break;
    case V34S_A_INFOH_PLAY:  s->st = V34S_A_TRN_WAIT;   break;
    case V34S_A_CC1_PLAY:    s->st = V34S_A_CC2_WAIT;   break;
    default: break;
    }
}

/* time-driven startup actions; called from the rx pump (always running) */
static void v34_sess_tick(nf_v34_sess_t *s)
{
    if (!s->started || s->done)
        return;
    if (s->now > s->deadline) {
        v34_sess_report(s, 0);
        return;
    }
    if (s->p2_state != P2_OFF)
        return;                              /* real-time Phase 2 owns the line */
    if (s->pcm_active)
        return;                              /* wait for the burst to play */
    switch (s->st) {
    case V34S_C_START:
        v34_sess_q_info0(s);
        s->st = V34S_C_INFO0_PLAY;
        break;
    case V34S_C_INFO0_WAIT:
        if (s->now >= s->st_timer) {
            v34_sess_q_info0(s);
            s->st = V34S_C_INFO0_PLAY;
        }
        break;
    case V34S_C_TONES_Q:
        v34_sess_q_tones_probe(s);
        s->st = V34S_C_TONES_PLAY;
        break;
    case V34S_C_PHASE3_Q:
        v34_sess_q_phase3(s);
        s->st = V34S_C_PHASE3_PLAY;
        break;
    case V34S_C_CC_INIT_Q:
        /* 12.4.1.1: 70 +- 5 ms of silence after TRN, then the source opens
         * the control channel (PPh/ALT/MPh.../E) and HOLDS the carrier with
         * flags (F.3.1.4). Blind: the MPh train is long enough that the
         * recipient - which starts listening only after answering our PPh
         * with its own PPh + ALT - is guaranteed complete MPh sequences. */
        v34_ccs_begin(s, 1);
        s->st = V34S_C_CC_INIT_PLAY;
        break;
    case V34S_A_INFO0A_Q:
        v34_sess_q_info0(s);
        s->st = V34S_A_INFO0A_PLAY;
        break;
    case V34S_A_INFOH_Q:
        v34_sess_q_infoh(s);
        s->st = V34S_A_INFOH_PLAY;
        break;
    case V34S_A_CC1_Q:
        /* recipient's cc start-up response (12.4.2), carrier held (F.3.1.4) */
        v34_ccs_begin(s, 1);
        s->st = V34S_A_CC1_PLAY;
        break;
    default:
        break;
    }
}

/* a finished rx burst during startup */
static void v34_sess_startup_burst(nf_v34_sess_t *s)
{
    nf_v34_info_frame_t f;
    long sig = s->burst_n - V34S_PRE - V34S_CLOSE_RUN;

    switch (s->st) {
    case V34S_C_INFO0_PLAY:
    case V34S_C_INFO0_WAIT:
        if (v34_sess_info_scan(s, 0, &f)) {
            SDBG(s, "rx INFO0a (ack=%d) - phase 2 tones/probing next\n",
                 f.info0_ack);
            s->st = V34S_C_TONES_Q;
        }
        break;
    case V34S_C_INFOH_WAIT:
        if (v34_sess_info_scan(s, 1, &f)) {
            s->infoh_trn_len = f.trn_len;
            s->infoh_16pt = f.trn_16pt;
            /* the recipient's INFOh dictates our Phase 3 + primary channel
             * operating point (symbol rate, carrier option) */
            if (f.symrate_idx >= 0 && f.symrate_idx < NF_V34_NUM_RATES)
                s->srate_idx = f.symrate_idx;
            s->high_carrier = f.high_carrier ? 1 : 0;
            if (s->local_max_rate > v34_sess_top_rate(s->srate_idx))
                s->local_max_rate = v34_sess_top_rate(s->srate_idx);
            SDBG(s, "rx INFOh: symrate_idx=%d high_carrier=%d TRN %dx35ms"
                 " %d-point - phase 3 next\n", f.symrate_idx, f.high_carrier,
                 f.trn_len, f.trn_16pt ? 16 : 4);
            s->st = V34S_C_PHASE3_Q;
        }
        break;
    case V34S_C_CC_INIT_PLAY:          /* peer's cc can end while ours plays */
    case V34S_C_CC1_WAIT:
        /* the recipient's cc burst: PPh/ALT/MPh/E, and - from a real fax -
         * NSF/CSI/DIS user data in the same transmission. Decode with the
         * frames stashed, report establishment (nf_t30 arms its DIS wait),
         * then flush the stashed frames up. */
        if (v34_sess_process_cc(s, V34CC_STASH, 0) > 0) {
            v34_sess_report(s, 1);
            v34_sess_flush_rxq(s);
        } else {
            s->nrxq = 0;
        }
        break;
    case V34S_A_INFO0C_WAIT:
        if (v34_sess_info_scan(s, 0, &f)) {
            SDBG(s, "rx INFO0c (sr3429=%d allow_3429=%d)\n",
                 f.sr3429, f.allow_3429);
            s->st = V34S_A_INFO0A_Q;
        }
        break;
    case V34S_A_PROBE_WAIT:
        if (sig >= 7200) {                      /* tone B + L1/L2, >= 0.9 s */
            nf_v34_probe_t pr;
            nf_v34_probe_sel_t psel;
            long start = 0, win = 0;
            int have_win = v34_probe_window(s->burst, s->burst_n, &start, &win);
            if (s->probe_override) {
                SDBG(s, "rx tone B + L1/L2 probing (%.2f s) - S/carrier forced"
                     " (NFV34SRATE/NFV34HIGHC): S=%d %s carrier\n",
                     (double) sig / 8000.0,
                     nf_v34_srates[s->srate_idx].baud_name,
                     s->high_carrier ? "high" : "low");
            } else if (have_win &&
                       nf_v34_probe_analyze(s->burst + start, win, &pr) == 0 &&
                       nf_v34_probe_select(&pr, s->local_max_rate, &psel) >= 0) {
                s->srate_idx = psel.srate_idx;
                s->high_carrier = psel.high_carrier;
                if (s->local_max_rate > v34_sess_top_rate(s->srate_idx))
                    s->local_max_rate = v34_sess_top_rate(s->srate_idx);
                if (psel.projected_max_rate > 0 &&
                    psel.projected_max_rate < s->local_max_rate)
                    s->local_max_rate = psel.projected_max_rate;
                SDBG(s, "rx tone B + L1/L2 probing (%.2f s): band %.0f-%.0f Hz "
                     "SNR %.1f dB tilt %+.1f dB foff %+.1f Hz -> S=%d %s carrier"
                     ", projected %d bit/s (cap %d)\n",
                     (double) sig / 8000.0, pr.band_lo_hz, pr.band_hi_hz,
                     pr.band_snr_db, pr.tilt_db, pr.freq_offset_hz,
                     nf_v34_srates[s->srate_idx].baud_name,
                     s->high_carrier ? "high" : "low",
                     psel.projected_max_rate, s->local_max_rate);
            } else {
                SDBG(s, "rx tone B + L1/L2 probing (%.2f s): analysis "
                     "inconclusive - keeping S=%d %s carrier\n",
                     (double) sig / 8000.0,
                     nf_v34_srates[s->srate_idx].baud_name,
                     s->high_carrier ? "high" : "low");
            }
            s->st = V34S_A_INFOH_Q;
        }
        break;
    case V34S_A_TRN_WAIT:
        if (sig >= 6400) {                      /* S/Sbar/PP + TRN, >= 0.8 s */
            if (v34_sess_train(s) == 0)
                s->st = V34S_A_CC1_Q;
            else
                v34_sess_report(s, 0);
        }
        break;
    case V34S_A_CC1_PLAY:              /* source's cc can end while ours plays */
    case V34S_A_CC2_WAIT:
        if (v34_sess_process_cc(s, V34CC_STASH, 0) > 0) {
            v34_sess_report(s, 1);
            v34_sess_flush_rxq(s);
        } else {
            s->nrxq = 0;
        }
        break;
    default:
        break;
    }
}

/* ── mode control (driven by nf_fax on behalf of nf_t30) ───────────────── */

void nf_v34_sess_set_tx_mode(nf_v34_sess_t *s, int mode)
{
    s->tx_mode = mode;
    if (mode == NF_V34_SESS_STARTUP && !s->started) {
        s->started = 1;
        s->deadline = s->now + V34S_DEADLINE;
        s->st = s->is_call ? V34S_C_START : V34S_A_INFO0C_WAIT;
        SDBG(s, "session start (%s)\n", s->is_call ? "call/source"
                                                   : "answer/recipient");
        v34_p2_init(s);                    /* real-time Phase 2 (clause 12.2) */
    }
    if (mode == NF_V34_SESS_CC)
        s->nframes = 0;
    if (mode == NF_V34_SESS_PRI)
        s->pri_pending = 0;
}

void nf_v34_sess_set_rx_mode(nf_v34_sess_t *s, int mode)
{
    /* if the peer's cc carrier is STILL UP at this switch to the primary
     * channel, the tail of that held stream (flags, then the F.3.2.3
     * turnaround ones) would otherwise be captured and misprocessed as a
     * primary-channel burst - flag the carrier in progress for discard
     * (checked BEFORE the gate reset below clears gate_open) */
    if (mode == NF_V34_SESS_PRI && s->rx_mode != NF_V34_SESS_PRI &&
        s->gate_open)
        s->pri_skip_burst = 1;
    if (mode != NF_V34_SESS_PRI)
        s->pri_skip_burst = 0;
    if (s->rx_mode != mode) {
        /* STARTUP -> CC happens inside the establishment report, fired by a
         * mid-burst scan while the peer's cc carrier is STILL UP (it holds
         * the carrier and follows MPh/E with T.30 frames): keep the capture
         * running so the streaming decode continues seamlessly */
        if (!(s->rx_mode == NF_V34_SESS_STARTUP && mode == NF_V34_SESS_CC &&
              s->gate_open)) {
            s->gate_open = 0;
            s->open_run = s->close_run = 0;
            s->pre_n = s->pre_pos = 0;
            s->burst_n = 0;
        }
    }
    /* the receiver being pointed at the primary channel means the peer is
     * about to turn the line around: stop extending our held cc stream
     * (F.3.2.2 - the recipient's flags stop once the source's ones arrive;
     * playing out the rendered tail approximates that hand-off) */
    if (mode == NF_V34_SESS_PRI && s->ccs_state == CCS_RUN)
        s->ccs_state = CCS_DRAIN;
    s->rx_mode = mode;
    if (mode == NF_V34_SESS_STARTUP && !s->started) {
        s->started = 1;
        s->deadline = s->now + V34S_DEADLINE;
        s->st = s->is_call ? V34S_C_START : V34S_A_INFO0C_WAIT;
        SDBG(s, "session start (%s)\n", s->is_call ? "call/source"
                                                   : "answer/recipient");
        v34_p2_init(s);                    /* real-time Phase 2 (clause 12.2) */
    }
}

void nf_v34_sess_rx_prime(nf_v34_sess_t *s, const int16_t *amp, int len)
{
    /* Prime the just-started Phase-2 receiver with audio that arrived BEFORE
     * the session took over the line (i.e. during the V.8 tail). The answerer
     * transmits its single INFO0a within tens of ms of V.8 ending, so without
     * this history the caller can miss the head of the one frame that arms
     * its whole startup. Only meaningful once, right at session start. */
    if (!s || len <= 0 || !s->started || s->p2_state == P2_OFF || s->p2_in != 0)
        return;
    /* The primed samples lie in the past: shift the tx-side clocks (and the
     * supervision deadline) forward by the same amount so rx (p2_in) and tx
     * (p2_out) keep indexing the same wall-clock instants. */
    s->p2_out += len;
    s->p2_state_out += len;
    s->now += len;
    s->deadline += len;
    v34_p2_rx(s, amp, len);
}

void nf_v34_sess_queue_frame(nf_v34_sess_t *s, const uint8_t *msg, int len)
{
    if (len < 0) {
        s->nframes = 0;
        return;
    }
    if (len == 0 || len > V34S_FRAME_MAX || s->nframes >= V34S_NFRAMES)
        return;
    memcpy(s->frames[s->nframes], msg, (size_t) len);
    s->frame_len[s->nframes] = len;
    s->nframes++;
}

void nf_v34_sess_begin_stream(nf_v34_sess_t *s,
                              int (*get_frame)(void *user, uint8_t *buf, int maxlen),
                              void *user)
{
    s->get_frame = get_frame;
    s->get_frame_user = user;
    s->pri_pending = 1;
}

/* ── sample pumps ──────────────────────────────────────────────────────── */

int nf_v34_sess_tx(nf_v34_sess_t *s, int16_t *amp, int max_len)
{
    /* during a mid-call retrain the pumps run the startup choreography again
     * (the tick queues INFO0/tones/TRN/PPh bursts into s->pcm) regardless of
     * the mode nf_fax last set */
    int tx_mode = s->in_retrain ? NF_V34_SESS_STARTUP : s->tx_mode;
    int len = 0;
    int completed_kind = -1;

    /* real-time Phase 2 (clause 12.2): generate the tone/probe/INFO stream */
    if (s->p2_state != P2_OFF) {
        v34_p2_tx(s, amp, max_len);
        return max_len;
    }

    /* control-channel data with no carrier up: raise a fresh held stream
     * (Sh resync or PPh restart per 12.6; the full start-up interlock when
     * the peer has initiated a Figure-26 renegotiation) and put the frames
     * inline (after E, for the interlock form) */
    if (s->ccs_state == CCS_OFF && !s->pcm_active &&
        tx_mode == NF_V34_SESS_CC && s->nframes > 0) {
        v34_ccs_begin(s, s->cc_reneg_pending ? 1 : 0);
        if (!s->ccs_pre_mph)
            v34_ccs_append_frames(s);
    }
    /* held cc stream owns the transmitter while active */
    if (v34_ccs_tx(s, tx_mode, amp, max_len))
        return max_len;

    /* lazily build the armed data burst (frames were queued after the mode
     * switch, and nf_t30's substate must be in place before frames are
     * pulled - see ecm_start_burst's ordering) */
    if (!s->pcm_active) {
        if (tx_mode == NF_V34_SESS_PRI && s->pri_pending) {
            s->pri_pending = 0;
            v34_sess_build_pri(s);
            s->pcm_kind = NF_V34_SESS_PRI;
        }
    }
    if (s->pcm_active) {
        long avail = s->pcm_len - s->pcm_pos;
        len = (avail < max_len) ? (int) avail : max_len;
        memcpy(amp, s->pcm + s->pcm_pos, (size_t) len * sizeof(int16_t));
        s->pcm_pos += len;
        if (s->pcm_pos >= s->pcm_len) {
            completed_kind = s->pcm_kind;
            free(s->pcm);
            s->pcm = NULL;
            s->pcm_len = s->pcm_pos = 0;
            s->pcm_active = 0;
            if (completed_kind == NF_V34_SESS_CC)
                s->nframes = 0;                    /* burst sent */
            if (completed_kind == NF_V34_SESS_STARTUP)
                v34_sess_burst_played(s);
        }
    }
    if (tx_mode == NF_V34_SESS_STARTUP ||
        completed_kind == NF_V34_SESS_STARTUP) {
        /* startup never "completes a step": idle = silence (like nf_v8_tx).
         * That includes a startup burst still draining after nf_t30 already
         * switched tx_mode to CC (it queued DIS on the establishment report):
         * a short return here would fire SEND_STEP_COMPLETE for a data burst
         * that has not even been built yet, and the pump would stop. */
        memset(amp + len, 0, (size_t) (max_len - len) * sizeof(int16_t));
        len = max_len;
    }
    return len;
}

/* NFV34DUMP=<dir>: write every completed rx burst to <dir> as raw s16le -
 * the offline-replay debugging aid the impairment hardening was done with */
static void v34_sess_burst_dump(nf_v34_sess_t *s)
{
    const char *dir = getenv("NFV34DUMP");
    char path[512];
    FILE *fp;

    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s/v34_%c_%08ld_m%d.s16", dir,
             s->is_call ? 'C' : 'A', s->now, s->rx_mode);
    fp = fopen(path, "wb");
    if (fp) {
        fwrite(s->burst, sizeof(int16_t), (size_t) s->burst_n, fp);
        fclose(fp);
    }
}

static void v34_sess_burst_done(nf_v34_sess_t *s)
{
    /* a retrain in progress re-runs the startup burst handling */
    int rx_mode = s->in_retrain ? NF_V34_SESS_STARTUP : s->rx_mode;

    SDBG(s, "rx burst end (%.2f s)\n", (double) s->burst_n / 8000.0);
    v34_sess_burst_dump(s);
    switch (rx_mode) {
    case NF_V34_SESS_CC:
        /* final pass: stash + dedup-flush, so frames the scans already
         * delivered are not repeated up to T.30 */
        s->nrxq = 0;
        (void) v34_sess_process_cc(s, V34CC_STASH, 0);
        v34_sess_flush_rxq(s);
        break;
    case NF_V34_SESS_PRI:
        /* pri_maybe_cc_tail: the peer's held cc carrier was still up at the
         * rx switch, so this first burst may be its tail (or the tail MERGED
         * with the primary transmission when the gap fell inside the gate's
         * close hangover - the S locator sorts the two cases out) */
        s->pri_maybe_cc_tail = s->pri_skip_burst || s->cc_burst_seen;
        s->pri_skip_burst = 0;
        v34_sess_process_pri(s);
        s->pri_maybe_cc_tail = 0;
        break;
    case NF_V34_SESS_STARTUP:
        v34_sess_startup_burst(s);
        break;
    default:
        break;
    }
}

static int v34_sess_burst_push(nf_v34_sess_t *s, int16_t x)
{
    if (s->burst_n >= V34S_BURST_MAX)
        return -1;                     /* over-long burst: force-process */
    if (s->burst_n >= s->burst_cap) {
        long nc = s->burst_cap ? s->burst_cap * 2 : 16384;
        int16_t *nb;
        if (nc > V34S_BURST_MAX)
            nc = V34S_BURST_MAX;       /* clamp, don't fail: a full 256-frame
                                        * primary burst is ~23 s of carrier */
        nb = realloc(s->burst, sizeof(int16_t) * (size_t) nc);
        if (!nb)
            return -1;
        s->burst = nb;
        s->burst_cap = nc;
    }
    s->burst[s->burst_n++] = x;
    return 0;
}

int nf_v34_sess_rx(nf_v34_sess_t *s, const int16_t *amp, int len)
{
    int i;

    /* real-time Phase 2 (clause 12.2) owns the receiver during probing */
    if (s->p2_state != P2_OFF) {
        s->now += len;
        v34_p2_rx(s, amp, len);
        v34_sess_tick(s);                    /* deadline supervision only */
        return 0;
    }

    for (i = 0; i < len; i++) {
        int16_t x = amp[i];
        double xx = (double) x * (double) x;
        double open_p, close_p;

        s->pwr += (xx - s->pwr) / 64.0;
        open_p = V34S_OPEN_P;
        if (s->noise * V34S_OPEN_SNR > open_p)
            open_p = s->noise * V34S_OPEN_SNR;
        close_p = V34S_CLOSE_P;
        if (s->noise * V34S_CLOSE_SNR > close_p)
            close_p = s->noise * V34S_CLOSE_SNR;
        if (!s->gate_open) {
            /* noise-floor tracking: fast down, slow up, and never from
             * samples already loud enough to be opening the gate */
            if (s->pwr < open_p)
                s->noise += (s->pwr - s->noise) *
                            (s->pwr < s->noise ? 1.0 / 64.0 : 1.0 / 2048.0);
            /* pre-roll ring */
            s->pre[s->pre_pos] = x;
            s->pre_pos = (s->pre_pos + 1) % V34S_PRE;
            if (s->pre_n < V34S_PRE)
                s->pre_n++;
            if (s->pwr > open_p) {
                if (++s->open_run >= V34S_OPEN_RUN) {
                    int k, p;
                    s->gate_open = 1;
                    s->close_run = 0;
                    s->burst_n = 0;
                    s->cc_scan_at = 0;
                    s->cc_nseen = 0;
                    s->cc_burst_seen = 0;
                    p = (s->pre_pos - s->pre_n + V34S_PRE) % V34S_PRE;
                    for (k = 0; k < s->pre_n; k++) {
                        (void) v34_sess_burst_push(s, s->pre[p]);
                        p = (p + 1) % V34S_PRE;
                    }
                }
            } else {
                s->open_run = 0;
            }
        } else {
            if (v34_sess_burst_push(s, x) < 0) {
                /* over-long burst: force-process what we have */
                s->gate_open = 0;
                s->open_run = 0;
                s->pre_n = s->pre_pos = 0;
                v34_sess_burst_done(s);
                s->burst_n = 0;
            } else if (s->pwr < close_p) {
                if (++s->close_run >= V34S_CLOSE_RUN) {
                    s->gate_open = 0;
                    s->open_run = 0;
                    s->pre_n = s->pre_pos = 0;
                    v34_sess_burst_done(s);
                    s->burst_n = 0;
                }
            } else {
                s->close_run = 0;
            }
            /* streaming cc decode while the peer holds its carrier. Also in
             * PRI mode: the analogue of the classic path's parallel V.21
             * watch - a recipient waiting for a primary burst may instead
             * be sent control-channel frames (the source PPS-ing a block it
             * thinks was delivered), and the source HOLDS its cc carrier
             * while waiting, so there is no burst end to learn it from. A
             * true primary-channel burst just decodes to nothing here. */
            if (s->gate_open) {
                int rxm = s->in_retrain ? NF_V34_SESS_STARTUP : s->rx_mode;
                if (rxm == NF_V34_SESS_CC || rxm == NF_V34_SESS_PRI ||
                    (rxm == NF_V34_SESS_STARTUP &&
                     (s->st == V34S_C_CC_INIT_PLAY ||
                      s->st == V34S_C_CC1_WAIT ||
                      s->st == V34S_A_CC1_PLAY ||
                      s->st == V34S_A_CC2_WAIT)))
                    v34_sess_cc_scan(s);
            }
        }
        s->now++;
    }
    v34_sess_tick(s);
    return 0;
}
