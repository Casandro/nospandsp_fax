#include "nf_v8.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── modulation-mode octet codec (verified against a real captured CM/JM:
 * bytes 81 85 D4 90 decoded to call-function T.30 Tx FAX and modulations
 * V.21|V.27ter|V.29|V.17|V.34HDX) ─────────────────────────────────────── */

static int build_modulation_octets(uint32_t mod, uint8_t out[3])
{
    uint8_t o0 = 0x05;   /* modulation tag, bits 4:0 */
    uint8_t o1 = 0, o2 = 0;
    int need2, need1;

    if (mod & NF_V8_MOD_V34HDX) o0 |= 0x80;
    if (mod & NF_V8_MOD_V34)    o0 |= 0x40;
    if (mod & NF_V8_MOD_V90)    o0 |= 0x20;

    if (mod & NF_V8_MOD_V27TER) o1 |= 0x80;
    if (mod & NF_V8_MOD_V29)    o1 |= 0x40;
    if (mod & NF_V8_MOD_V17)    o1 |= 0x04;
    if (mod & NF_V8_MOD_V22)    o1 |= 0x02;
    if (mod & NF_V8_MOD_V32)    o1 |= 0x01;

    if (mod & NF_V8_MOD_V21)    o2 |= 0x80;
    if (mod & NF_V8_MOD_V23HDX) o2 |= 0x40;
    if (mod & NF_V8_MOD_V23)    o2 |= 0x04;
    if (mod & NF_V8_MOD_V26BIS) o2 |= 0x02;
    if (mod & NF_V8_MOD_V26TER) o2 |= 0x01;

    need2 = (mod & (NF_V8_MOD_V21 | NF_V8_MOD_V23HDX | NF_V8_MOD_V23 |
                    NF_V8_MOD_V26BIS | NF_V8_MOD_V26TER)) != 0;
    need1 = need2 || (mod & (NF_V8_MOD_V27TER | NF_V8_MOD_V29 | NF_V8_MOD_V17 |
                             NF_V8_MOD_V22 | NF_V8_MOD_V32)) != 0;

    out[0] = o0;
    if (need1) out[1] = o1 | 0x10;   /* extension marker: bits 5:3 == 0b010 */
    if (need2) out[2] = o2 | 0x10;
    return need2 ? 3 : (need1 ? 2 : 1);
}

static int parse_modulation(const uint8_t *d, int i, int len, uint32_t *mod)
{
    uint8_t p;

    if (i >= len) return i;
    p = d[i++];
    if (p & 0x80) *mod |= NF_V8_MOD_V34HDX;
    if (p & 0x40) *mod |= NF_V8_MOD_V34;
    if (p & 0x20) *mod |= NF_V8_MOD_V90;

    if (i < len && (d[i] & 0x38) == 0x10) {
        p = d[i++];
        if (p & 0x80) *mod |= NF_V8_MOD_V27TER;
        if (p & 0x40) *mod |= NF_V8_MOD_V29;
        if (p & 0x04) *mod |= NF_V8_MOD_V17;
        if (p & 0x02) *mod |= NF_V8_MOD_V22;
        if (p & 0x01) *mod |= NF_V8_MOD_V32;

        if (i < len && (d[i] & 0x38) == 0x10) {
            p = d[i++];
            if (p & 0x80) *mod |= NF_V8_MOD_V21;
            if (p & 0x40) *mod |= NF_V8_MOD_V23HDX;
            if (p & 0x04) *mod |= NF_V8_MOD_V23;
            if (p & 0x02) *mod |= NF_V8_MOD_V26BIS;
            if (p & 0x01) *mod |= NF_V8_MOD_V26TER;
        }
    }
    return i;
}

/* A literal 0x00 octet never occurs inside a real CM/JM (every tag/extension
 * octet carries a nonzero tag or marker bit), so it is a safe terminator -
 * it's how CJ (three zero octets) stays distinguishable from a parseable
 * message instead of being mistaken for one. */
static void parse_message(const uint8_t *d, int len, int32_t *call_function, uint32_t *modulations)
{
    int i = 0;

    *call_function = -1;
    *modulations = 0;
    while (i < len && d[i] != 0) {
        int tag = d[i] & 0x1F;
        if (tag == 0x01) {
            *call_function = (d[i] >> 5) & 0x07;
            i++;
        } else if (tag == 0x05) {
            i = parse_modulation(d, i, len, modulations);
        } else {
            i++;
        }
        while (i < len && (d[i] & 0x38) == 0x10)
            i++;
    }
}

/* ── channel-parameterised V.21 async FSK tx ───────────────────────────── */

/* Arm the FSK tx with `repeat` back-to-back segments, each = 10 mark-bit
 * preamble + a framed sync octet + the framed message octets. `sync` is 0xE0
 * for CM/JM/CJ and 0x00 for CI. loop != 0 keeps replaying the whole armed
 * sequence forever (CM/JM/CJ); loop == 0 plays it once then falls silent (CI
 * bursts, so the caller can insert an inter-burst listening gap). */
static void fsk_tx_arm(nf_v8_fsk_tx_t *tx, double mark_hz, double space_hz,
                       uint8_t sync, const uint8_t *msg, int msglen,
                       int repeat, int loop)
{
    int n = 0, i, b, rep;

    for (rep = 0; rep < repeat; rep++) {
        for (i = 0; i < 10; i++)
            tx->bits[n++] = 1;                       /* preamble */
        tx->bits[n++] = 0;                           /* framed sync octet */
        for (b = 0; b < 8; b++)
            tx->bits[n++] = (sync >> b) & 1;
        tx->bits[n++] = 1;
        for (i = 0; i < msglen; i++) {
            tx->bits[n++] = 0;
            for (b = 0; b < 8; b++)
                tx->bits[n++] = (msg[i] >> b) & 1;
            tx->bits[n++] = 1;
        }
    }
    tx->nbits = n;
    tx->pos = 0;
    tx->baud_frac = 0;
    tx->loop = loop;
    tx->done = 0;
    tx->rates[0] = nf_dds_phase_rate(space_hz);
    tx->rates[1] = nf_dds_phase_rate(mark_hz);
    tx->cur_rate = tx->rates[1];
    tx->scaling = nf_dbm0_scaling(-14.0);
    tx->active = 1;
}

#define V8_BAUD_100 30000                   /* 300 baud, in 0.01-baud units */
#define V8_RATE_100 (NF_SAMPLE_RATE * 100)

static int fsk_tx_gen(nf_v8_fsk_tx_t *tx, int16_t *amp, int max_len)
{
    int n;

    if (!tx->active) {
        memset(amp, 0, (size_t) max_len * sizeof(int16_t));
        return max_len;
    }
    for (n = 0; n < max_len; n++) {
        if ((tx->baud_frac += V8_BAUD_100) >= V8_RATE_100) {
            int bit;

            tx->baud_frac -= V8_RATE_100;
            if (tx->done) {                           /* last bit's samples done */
                tx->active = 0;
                break;
            }
            bit = tx->bits[tx->pos++];
            if (tx->pos >= tx->nbits) {
                if (tx->loop)
                    tx->pos = 0;                      /* loop the message */
                else
                    tx->done = 1;                     /* oneshot: emit this bit, then stop */
            }
            tx->cur_rate = tx->rates[bit & 1];
        }
        amp[n] = nf_dds_mod(&tx->phase, tx->cur_rate, tx->scaling);
    }
    if (n < max_len)                                  /* oneshot ran out: pad silence */
        memset(amp + n, 0, (size_t) (max_len - n) * sizeof(int16_t));
    return max_len;
}

/* ── channel-parameterised V.21 async FSK rx ───────────────────────────── */

static void fsk_rx_init(nf_v8_fsk_rx_t *r, double mark_hz, double space_hz)
{
    memset(r, 0, sizeof(*r));
    r->rate[0] = nf_dds_phase_rate(space_hz);
    r->rate[1] = nf_dds_phase_rate(mark_hz);
    r->on_power  = nf_power_level_dbm0(-30.0f + 2.5f - 5.3f);
    r->off_power = nf_power_level_dbm0(-30.0f - 2.5f - 5.3f);
    nf_power_init(&r->power, 4);
}

/* spandsp-compatible sync/framing state machine: 20-bit shift register,
 * newest bit at bit 19. Preamble+sync patterns (10 ones then a framed sync
 * octet, LSB first): 0x803FF for CI (sync octet 0x00), 0xF03FF for CM/JM/CJ
 * (sync octet 0xE0). Octets are 1 start (0) + 8 data (LSB first) + 1 stop
 * (1); once framed, data = bits 18..11 of the register. */
static void fsk_rx_put_bit(nf_v8_fsk_rx_t *r, int bit)
{
    r->bit_stream = (r->bit_stream >> 1) | ((uint32_t) bit << 19);

    /* Classic V.21 HDLC flags (01111110 repeating) instead of a V.8 message:
     * an answerer that went straight to DIS. Detected as an 8-periodic bit
     * stream that is neither all-ones (V.8 preamble) nor all-zeros. */
    if ((((r->bit_stream ^ (r->bit_stream >> 8)) & 0xFFu) == 0) &&
        (r->bit_stream & 0xFFu) != 0 && (r->bit_stream & 0xFFu) != 0xFFu) {
        if (r->flags_run < 1000)
            r->flags_run++;
    } else {
        r->flags_run = 0;
    }

    if (r->bit_stream == 0xF03FFu) {
        if (!r->have_msg) {
            if (r->saved_len > 0 && r->saved_len == r->rx_len &&
                memcmp(r->saved_msg, r->rx_data, (size_t) r->rx_len) == 0) {
                memcpy(r->valid_msg, r->rx_data, (size_t) r->rx_len);
                r->valid_len = r->rx_len;
                r->have_msg = 1;
            } else {
                memcpy(r->saved_msg, r->rx_data, (size_t) r->rx_len);
                r->saved_len = r->rx_len;
            }
        }
        r->have_sync = 1;
        r->bit_cnt = 0;
        r->rx_len = 0;
    }

    if (r->have_sync) {
        r->bit_cnt++;
        if ((r->bit_stream & 0x80400u) == 0x80000u && r->bit_cnt >= 10) {
            uint8_t octet = (uint8_t) ((r->bit_stream >> 11) & 0xFF);

            if (r->rx_len < NF_V8_MSG_MAX)
                r->rx_data[r->rx_len++] = octet;
            if (octet == 0) {
                if (++r->zero_run >= 3)
                    r->have_cj = 1;
            } else {
                r->zero_run = 0;
            }
            r->bit_cnt = 0;
        }
    }
}

static void fsk_rx_process(nf_v8_fsk_rx_t *r, const int16_t *amp, int len)
{
    int buf_ptr = r->buf_ptr;
    int i;

    for (i = 0; i < len; i++) {
        float sum[2];
        int j, baudstate;
        int16_t x;
        int32_t power;

        for (j = 0; j < 2; j++) {
            nf_cpx_t ph;

            r->dot[j] = nf_cpx_sub(r->dot[j], r->window[j][buf_ptr]);
            ph = nf_dds_cpx_mod(&r->phase[j], r->rate[j]);
            r->window[j][buf_ptr] = nf_cpx(ph.re * amp[i], ph.im * amp[i]);
            r->dot[j] = nf_cpx_add(r->dot[j], r->window[j][buf_ptr]);
            sum[j] = nf_cpx_power(r->dot[j]);
        }
        x = (int16_t) (amp[i] >> 1);
        power = nf_power_update(&r->power, (int16_t) (x - r->last_sample));
        r->last_sample = x;

        if (r->signal_present) {
            if (power < r->off_power) {
                if (--r->signal_present <= 0) {
                    r->baud_phase = 0;
                    goto next;
                }
            }
        } else {
            if (power < r->on_power) {
                r->baud_phase = 0;
                goto next;
            }
            r->signal_present = 1;
            r->baud_phase = 0;
            r->last_bit = 0;
        }

        baudstate = sum[0] < sum[1];
        if (r->last_bit != baudstate) {
            r->last_bit = baudstate;
            if (r->baud_phase < V8_RATE_100 / 2)
                r->baud_phase += V8_BAUD_100 >> 3;
            else
                r->baud_phase -= V8_BAUD_100 >> 3;
        }
        if ((r->baud_phase += V8_BAUD_100) >= V8_RATE_100) {
            r->baud_phase -= V8_RATE_100;
            fsk_rx_put_bit(r, baudstate);
        }
next:
        if (++buf_ptr >= NF_V8_FSK_SPAN)
            buf_ptr = 0;
    }
    r->buf_ptr = buf_ptr;
}

/* ── ANSam tx (formula matches spandsp's modem_connect_tones.c, since real
 * Super-G3 gear and our oracle both key off it): 200 ms silence then a
 * 2100 Hz tone at -11 dBm0, amplitude-modulated by a 15 Hz tone at 1/5 the
 * carrier's peak level (modulation index 0.2). The 15 Hz AM *is* emitted -
 * this is genuine ANSam, not a plain 2100 Hz CED, so a caller distinguishes it
 * from a non-V.8 answerer. Only ANSam-PR's extra 180-degree carrier phase
 * reversal every 450 ms is deferred (optional; a V.8 peer accepts plain
 * ANSam), so we emit plain ANSam, not ANSam-PR. ── */

static void ansam_tx_arm(nf_v8_ansam_tx_t *a)
{
    memset(a, 0, sizeof(*a));
    a->tone_rate = nf_dds_phase_rate(2100.0);
    a->mod_rate  = nf_dds_phase_rate(15.0);
    a->level     = nf_dbm0_scaling(-11.0);
    a->mod_level = a->level / 5.0f;
    a->silence_left = NF_SAMPLE_RATE * 200 / 1000;
    a->tone_left    = NF_SAMPLE_RATE * 3300 / 1000;
}

static int ansam_tx_gen(nf_v8_ansam_tx_t *a, int16_t *amp, int max_len)
{
    int n;

    for (n = 0; n < max_len; n++) {
        if (a->silence_left > 0) {
            amp[n] = 0;
            a->silence_left--;
            continue;
        }
        if (a->tone_left > 0) {
            float modv = a->level + nf_dds_sin(a->mod_phase) * a->mod_level;

            a->mod_phase += (uint32_t) a->mod_rate;
            amp[n] = nf_dds_mod(&a->tone_phase, a->tone_rate, modv);
            a->tone_left--;
            continue;
        }
        amp[n] = 0;
    }
    return n;
}

/* ── ANSam / ANS rx: frequency-selective 2100 Hz detector (see nf_v8.h) ── */

/* ratio (tone energy / total energy) above which the band is "a 2100 Hz tone".
 * A perfectly on-frequency pure tone gives ~1.0; noise/other bands give ~0. */
#define ANSAM_TONE_RATIO 0.5f
/* time the tone must persist before we declare ANSam present */
#define ANSAM_PRESENT_MS 250
/* AM-classification window: a few 15 Hz cycles */
#define ANSAM_AM_WIN_MS  200
/* ignore the tone onset before classifying AM: the detector's own priming
 * ramp reads as an envelope swing (a real Panasonic's flat ANS measured 17%
 * power swing in the first window purely from the onset, vs 3% after) */
#define ANSAM_AM_SKIP_MS 100
/* presence needed before the caller acts on the tone: by then one clean
 * (post-onset) AM window has completed and am_class is authoritative */
#define ANSAM_DECIDE_MS  350
/* V.8 8.1.1/8.1.2: after detecting ANSam the caller stops its call signal and
 * transmits NO signal for a period Te before sending CM. V.8 sets the minimum Te
 * at 0.5 s, and Te >= 1 s only "if it is desired to allow for network echo
 * canceller disabling". We use the 0.5 s minimum: these GSTN/VoIP paths show a
 * plain echo of our own signal (i.e. no echo canceller is engaged), so the
 * longer EC-disable value would only add pointless latency. */
#define V8_TE_MS 500

static void ansam_rx_init(nf_v8_ansam_rx_t *r)
{
    memset(r, 0, sizeof(*r));
    r->rate = nf_dds_phase_rate(2100.0);
    r->floor_pwr = (float) nf_power_level_dbm0(-40.0f);
    nf_power_init(&r->env_pm, 6);          /* ~8 ms: smooths 2100 Hz, tracks 15 Hz */
    r->env_min = INT32_MAX;
    r->env_max = 0;
    r->am_class = -1;
}

static void ansam_rx_process(nf_v8_ansam_rx_t *r, const int16_t *amp, int len)
{
    const int W = NF_V8_ANSAM_SPAN;
    int i;

    for (i = 0; i < len; i++) {
        float s = (float) amp[i];
        nf_cpx_t ph;
        int32_t env;
        int present;

        /* slow envelope meter runs on EVERY sample so it is fully settled by
         * the time the correlator window primes (otherwise its startup ramp
         * would masquerade as amplitude modulation). Used only to classify
         * AM'd ANSam vs a flat tone. */
        env = nf_power_update(&r->env_pm, amp[i]);

        /* slide the 2100 Hz correlator and the matched energy sum */
        r->dot = nf_cpx_sub(r->dot, r->window[r->buf_ptr]);
        ph = nf_dds_cpx_mod(&r->phase, r->rate);
        r->window[r->buf_ptr] = nf_cpx(ph.re * s, ph.im * s);   /* |sum x e^{jwn}| == |sum x e^{-jwn}| for real x */
        r->dot = nf_cpx_add(r->dot, r->window[r->buf_ptr]);

        r->e_dot -= r->e_window[r->buf_ptr];
        r->e_window[r->buf_ptr] = s * s;
        r->e_dot += r->e_window[r->buf_ptr];

        if (++r->buf_ptr >= W)
            r->buf_ptr = 0;
        if (r->filled < W) {
            r->filled++;
            continue;                       /* window not primed yet */
        }

        {
            float mean_pwr = r->e_dot / (float) W;
            float tone_pwr = 2.0f * nf_cpx_power(r->dot) / ((float) W * (float) W);
            present = (mean_pwr > r->floor_pwr) &&
                      (tone_pwr > ANSAM_TONE_RATIO * mean_pwr);
        }

        if (present) {
            if (r->present_run < NF_SAMPLE_RATE * 60)   /* cap, never matters */
                r->present_run++;
            if (r->present_run < NF_SAMPLE_RATE * ANSAM_AM_SKIP_MS / 1000)
                continue;                   /* onset ramp: not yet classifiable */
            if (env > r->env_max) r->env_max = env;
            if (env < r->env_min) r->env_min = env;
            if (++r->am_win >= NF_SAMPLE_RATE * ANSAM_AM_WIN_MS / 1000) {
                /* >~15% peak-to-trough envelope swing == AM present (ANSam).
                 * index-0.2 ANSam swings the power meter ~55%; a flat ANS/CED
                 * only ripples a few %. */
                if (r->env_max > 0 &&
                    (int64_t) (r->env_max - r->env_min) * 100 > (int64_t) r->env_max * 15)
                    r->am_class = 1;
                else if (r->env_max > 0)
                    r->am_class = 0;
                r->am_win = 0;
                r->env_min = INT32_MAX;
                r->env_max = 0;
            }
        } else {
            r->present_run = 0;
            r->am_win = 0;
            r->env_min = INT32_MAX;
            r->env_max = 0;
        }
    }
}

/* ── the engine ─────────────────────────────────────────────────────── */

enum {
    ST_IDLE = 0,
    ST_ANS_ANSAM,     /* answerer: emitting ANSam, listening for CM (ch1) */
    ST_ANS_JM,        /* answerer: emitting JM, listening for CJ (ch1)   */
    ST_CALL_CI,       /* caller: emitting CI bursts, listening for ANSam */
    ST_CALL_TE,       /* caller: ANSam heard, silent for Te before CM (8.1.1) */
    ST_CALL_CM,       /* caller: emitting CM, listening for JM (ch2)     */
    ST_CALL_CJ,       /* caller: emitting CJ, timed hold then done       */
    ST_DONE
};

static void arm_ci(nf_v8_t *s);

void nf_v8_init(nf_v8_t *s, int calling_party, uint32_t our_modulations,
                int32_t our_call_function,
                nf_v8_result_handler_t handler, void *user)
{
    memset(s, 0, sizeof(*s));
    s->calling_party = calling_party;
    s->our_modulations = our_modulations;
    s->our_call_function = our_call_function;
    s->handler = handler;
    s->handler_user = user;

    s->result.ansam_am = -1;

    if (calling_party) {
        s->state = ST_CALL_CI;
        s->timer = NF_SAMPLE_RATE * 10;              /* 10s: send CI, hear ANSam */
        ansam_rx_init(&s->ansam_rx);
        fsk_rx_init(&s->fsk_rx, 1650.0, 1850.0);      /* JM: V.21 ch 2   */
        arm_ci(s);                                    /* start soliciting ANSam */
    } else {
        s->state = ST_ANS_ANSAM;
        s->timer = NF_SAMPLE_RATE * 6;                /* 6s to hear CM   */
        ansam_tx_arm(&s->ansam_tx);
        fsk_rx_init(&s->fsk_rx, 980.0, 1180.0);        /* CM/CJ: V.21 ch 1 */
    }
}

static void report(nf_v8_t *s, int status)
{
    if (s->reported)
        return;
    s->reported = 1;
    s->state = ST_DONE;
    if (s->handler)
        s->handler(s->handler_user, status, &s->result);
}

/* CI: sync octet 0x00 + just the call-function octet, on V.21 channel 1. A
 * burst of 4 segments (spandsp send_ci sends 4; the spec says >= 3), played
 * once (loop=0) so the caller can fall silent and listen for ANSam between
 * bursts. */
#define V8_CI_BURST 4

static void arm_ci(nf_v8_t *s)
{
    uint8_t msg[1];

    msg[0] = (uint8_t) (0x80 | ((s->our_call_function & 7) << 5) | 0x01);
    fsk_tx_arm(&s->fsk_tx, 980.0, 1180.0, 0x00, msg, 1, V8_CI_BURST, 0);
    s->ci_gap = 0;
}

static void arm_cm(nf_v8_t *s)
{
    uint8_t msg[4];
    uint8_t mo[3];
    int n = 0, mn;

    msg[n++] = (uint8_t) (0x80 | ((s->our_call_function & 7) << 5) | 0x01);
    mn = build_modulation_octets(s->our_modulations, mo);
    memcpy(&msg[n], mo, (size_t) mn);
    n += mn;
    fsk_tx_arm(&s->fsk_tx, 980.0, 1180.0, 0xE0, msg, n, 1, 1);
}

static void arm_jm(nf_v8_t *s, int32_t call_function, uint32_t modulations)
{
    uint8_t msg[4];
    uint8_t mo[3];
    int n = 0, mn;

    msg[n++] = (uint8_t) (0x80 | ((call_function & 7) << 5) | 0x01);
    mn = build_modulation_octets(modulations, mo);
    memcpy(&msg[n], mo, (size_t) mn);
    n += mn;
    fsk_tx_arm(&s->fsk_tx, 1650.0, 1850.0, 0xE0, msg, n, 1, 1);
}

static void arm_cj(nf_v8_t *s)
{
    static const uint8_t zeros[3] = { 0, 0, 0 };

    /* CJ is sent exactly ONCE (V.8: three zero octets), then the caller goes
     * silent for >= 75 ms and starts the negotiated modulation. One-shot, so
     * completion of the burst - not a fixed hold - ends the V.8 phase: a real
     * SG3 answerer sends its one INFO0a within ~50 ms of seeing CJ end, and
     * every extra CJ repetition here is startup audio the V.34 engine never
     * gets to hear. */
    fsk_tx_arm(&s->fsk_tx, 980.0, 1180.0, 0xE0, zeros, 3, 1, 0);
    /* a short mark (async idle) tail after the final stop bit, so receivers
     * that only deliver an octet once the line has moved past it (spandsp)
     * still flush the third zero octet before the carrier drops */
    {
        int i;
        for (i = 0; i < 10; i++)
            s->fsk_tx.bits[s->fsk_tx.nbits++] = 1;
    }
}

int nf_v8_tx(nf_v8_t *s, int16_t *amp, int max_len)
{
    switch (s->state) {
    case ST_CALL_CI:
    case ST_CALL_CM:
    case ST_CALL_CJ:
    case ST_ANS_JM:
        return fsk_tx_gen(&s->fsk_tx, amp, max_len);
    case ST_ANS_ANSAM:
        return ansam_tx_gen(&s->ansam_tx, amp, max_len);
    default:
        memset(amp, 0, (size_t) max_len * sizeof(int16_t));
        return max_len;
    }
}

int nf_v8_rx(nf_v8_t *s, const int16_t *amp, int len)
{
    if (s->reported)
        return 0;

    if (s->calling_party)
        ansam_rx_process(&s->ansam_rx, amp, len);
    fsk_rx_process(&s->fsk_rx, amp, len);

    if (s->timer > 0)
        s->timer -= len;

    switch (s->state) {
    case ST_CALL_CI:
        if (s->ansam_rx.present_run >=
                NF_SAMPLE_RATE * ANSAM_DECIDE_MS / 1000) {
            /* Answer tone held long enough for a clean AM classification.
             * AM'd (ANSam) == the peer offers V.8 -> proceed to CM. A flat
             * tone (plain ANS/CED) == the peer will not do V.8: report at
             * once so T.30 falls back and catches the peer's FIRST DIS
             * (a classic answerer starts phase B right after its CED). */
            s->result.ansam_am = s->ansam_rx.am_class;
            if (s->ansam_rx.am_class == 1) {
                /* V.8 8.1.1: stop the call signal (CI) and transmit nothing for
                 * Te before CM, so the ANSam's phase reversals disable any
                 * network echo canceller first (nf_v8_tx is silent in this
                 * state). CM is never sent before ANSam (V.8 7.2). */
                if (getenv("NFV34DBG"))
                    fprintf(stderr, "[v8] ANSam detected; silent Te (%d ms) before CM\n", V8_TE_MS);
                s->state = ST_CALL_TE;
                s->timer = NF_SAMPLE_RATE * V8_TE_MS / 1000;
            } else {
                report(s, NF_V8_STATUS_NON_V8_CALL);
            }
        } else if (s->fsk_rx.flags_run >= 32) {
            /* No ANSam, but a T.30-style V.21(H) carrier (HDLC flags) on the
             * answer channel: this answerer skipped V.8 and is already in the
             * V.21 message phase. Do NOT send CM (V.8 7.2 needs ANSam first) -
             * hand straight to T.30 so it catches the DIS. */
            if (getenv("NFV34DBG"))
                fprintf(stderr, "[v8] V.21 carrier (no ANSam) -> T.30, no CM\n");
            report(s, NF_V8_STATUS_NON_V8_CALL);
        } else if (!s->fsk_tx.active) {
            /* CI burst finished: fall silent for an inter-burst gap (still
             * listening for ANSam), then re-send the burst. */
            if (s->ci_gap <= 0) {
                s->ci_gap = NF_SAMPLE_RATE / 2;                  /* 500 ms gap */
            } else {
                s->ci_gap -= len;
                if (s->ci_gap <= 0)
                    arm_ci(s);                                   /* next burst */
            }
            if (s->timer <= 0)
                report(s, NF_V8_STATUS_NON_V8_CALL);
        } else if (s->timer <= 0) {
            report(s, NF_V8_STATUS_NON_V8_CALL);
        }
        break;

    case ST_CALL_TE:
        /* Silent gap Te after ANSam detection (nf_v8_tx emits nothing here).
         * When it elapses, transmit CM. */
        if (s->timer <= 0) {
            if (getenv("NFV34DBG"))
                fprintf(stderr, "[v8] Te elapsed -> sending CM\n");
            arm_cm(s);
            s->state = ST_CALL_CM;
            s->timer = NF_SAMPLE_RATE * 5;                       /* 5s for JM */
        }
        break;

    case ST_CALL_CM:
        if (s->fsk_rx.have_msg) {
            int32_t cf;
            uint32_t mod;

            parse_message(s->fsk_rx.valid_msg, s->fsk_rx.valid_len, &cf, &mod);
            s->result.call_function = s->our_call_function;
            s->result.modulations = mod & s->our_modulations;
            arm_cj(s);
            s->state = ST_CALL_CJ;
            s->timer = NF_SAMPLE_RATE * 3 / 10;                   /* hold 300ms */
        } else if (s->fsk_rx.flags_run >= 32) {
            /* Sustained HDLC flags where JM should be: the answerer skipped
             * V.8 and is already sending its DIS preamble - fall back now so
             * T.30 catches this DIS rather than a 3 s-later repetition. */
            report(s, NF_V8_STATUS_NON_V8_CALL);
        } else if (s->timer <= 0) {
            report(s, NF_V8_STATUS_FAILED);
        }
        break;

    case ST_CALL_CJ:
        /* report as soon as the one-shot CJ has fully played (the timer is
         * only a backstop): V.8 is then complete and the negotiated modem
         * must take over the line within its own 75 ms lead-in */
        if (!s->fsk_tx.active || s->timer <= 0)
            report(s, NF_V8_STATUS_V8_CALL);
        break;

    case ST_ANS_ANSAM:
        if (s->fsk_rx.have_msg) {
            int32_t cf;
            uint32_t mod;

            parse_message(s->fsk_rx.valid_msg, s->fsk_rx.valid_len, &cf, &mod);
            s->result.call_function = (cf >= 0) ? cf : s->our_call_function;
            s->result.modulations = mod & s->our_modulations;
            arm_jm(s, s->result.call_function, s->result.modulations);
            s->state = ST_ANS_JM;
            s->timer = NF_SAMPLE_RATE * 4;                        /* 4s for CJ */
        } else if (s->timer <= 0) {
            report(s, NF_V8_STATUS_NON_V8_CALL);
        }
        break;

    case ST_ANS_JM:
        if (s->fsk_rx.have_cj)
            report(s, NF_V8_STATUS_V8_CALL);
        else if (s->timer <= 0)
            report(s, NF_V8_STATUS_FAILED);
        break;

    default:
        break;
    }
    return 0;
}
