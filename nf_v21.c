#include "nf_v21.h"
#include <string.h>

#define FREQ_ZERO   1850.0          /* space */
#define FREQ_ONE    1650.0          /* mark  */
#define TX_LEVEL    (-14.0)         /* dBm0 */
#define MIN_LEVEL   (-30.0f)        /* rx cutoff, dBm0 */
#define BAUD_RATE   30000           /* 300 baud in 0.01-baud units */
#define RATE_100    (NF_SAMPLE_RATE * 100)

/* ── tx ────────────────────────────────────────────────────────────── */

void nf_v21_tx_init(nf_v21_tx_t *s, int (*get_bit)(void *), void *user)
{
    memset(s, 0, sizeof(*s));
    s->get_bit = get_bit;
    s->get_user = user;
    s->rates[0] = nf_dds_phase_rate(FREQ_ZERO);
    s->rates[1] = nf_dds_phase_rate(FREQ_ONE);
    s->cur_rate = s->rates[1];      /* idle on mark until the first bit */
    s->scaling = nf_dbm0_scaling(TX_LEVEL);
}

int nf_v21_tx(nf_v21_tx_t *s, int16_t *amp, int max_len)
{
    int sample;

    if (s->shutdown)
        return 0;
    for (sample = 0; sample < max_len; sample++) {
        if ((s->baud_frac += BAUD_RATE) >= RATE_100) {
            s->baud_frac -= RATE_100;
            int bit = s->get_bit(s->get_user);
            if (bit < 0) {          /* NF_SIG_END_OF_DATA */
                s->shutdown = 1;
                break;
            }
            s->cur_rate = s->rates[bit & 1];
        }
        amp[sample] = nf_dds_mod(&s->phase, s->cur_rate, s->scaling);
    }
    return sample;
}

/* ── rx ────────────────────────────────────────────────────────────── */

void nf_v21_rx_init(nf_v21_rx_t *s, void (*put_bit)(void *, int), void *user)
{
    memset(s, 0, sizeof(*s));
    s->put_bit = put_bit;
    s->put_user = user;
    s->rate[0] = nf_dds_phase_rate(FREQ_ZERO);
    s->rate[1] = nf_dds_phase_rate(FREQ_ONE);
    /* +-2.5 dB hysteresis; -5.3 dB allows for the elementary HPF's gain */
    s->on_power  = nf_power_level_dbm0(MIN_LEVEL + 2.5f - 5.3f);
    s->off_power = nf_power_level_dbm0(MIN_LEVEL - 2.5f - 5.3f);
    nf_power_init(&s->power, 4);
}

void nf_v21_rx_set_status_handler(nf_v21_rx_t *s,
                                  void (*status)(void *, int), void *user)
{
    s->status = status;
    s->status_user = user;
}

static void rx_report(nf_v21_rx_t *s, int status)
{
    if (s->status)
        s->status(s->status_user, status);
    else if (s->put_bit)
        s->put_bit(s->put_user, status);
}

int nf_v21_rx(nf_v21_rx_t *s, const int16_t *amp, int len)
{
    int buf_ptr = s->buf_ptr;

    for (int i = 0; i < len; i++) {
        float sum[2];
        for (int j = 0; j < 2; j++) {
            s->dot[j] = nf_cpx_sub(s->dot[j], s->window[j][buf_ptr]);
            nf_cpx_t ph = nf_dds_cpx_mod(&s->phase[j], s->rate[j]);
            s->window[j][buf_ptr] = nf_cpx(ph.re * amp[i], ph.im * amp[i]);
            s->dot[j] = nf_cpx_add(s->dot[j], s->window[j][buf_ptr]);
            sum[j] = nf_cpx_power(s->dot[j]);
        }
        /* power with DC blocked by the most elementary HPF */
        int16_t x = amp[i] >> 1;
        int32_t power = nf_power_update(&s->power, (int16_t) (x - s->last_sample));
        s->last_sample = x;

        if (s->signal_present) {
            if (power < s->off_power) {
                if (--s->signal_present <= 0) {
                    rx_report(s, NF_SIG_CARRIER_DOWN);
                    s->baud_phase = 0;
                    goto next;
                }
            }
        } else {
            if (power < s->on_power) {
                s->baud_phase = 0;
                goto next;
            }
            s->signal_present = 1;
            s->baud_phase = 0;
            s->last_bit = 0;
            rx_report(s, NF_SIG_CARRIER_UP);
        }

        {
            int baudstate = sum[0] < sum[1];
            if (s->last_bit != baudstate) {
                /* nudge the baud phase toward mid-bit on each transition */
                s->last_bit = baudstate;
                if (s->baud_phase < RATE_100 / 2)
                    s->baud_phase += BAUD_RATE >> 3;
                else
                    s->baud_phase -= BAUD_RATE >> 3;
            }
            if ((s->baud_phase += BAUD_RATE) >= RATE_100) {
                s->baud_phase -= RATE_100;
                s->put_bit(s->put_user, baudstate);
            }
        }
next:
        if (++buf_ptr >= NF_V21_SPAN)
            buf_ptr = 0;
    }
    s->buf_ptr = buf_ptr;
    return 0;
}
