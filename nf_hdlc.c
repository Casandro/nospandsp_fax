#include "nf_hdlc.h"
#include "nf_dsp.h"
#include <string.h>

/* ── CRC-16/X.25 ───────────────────────────────────────────────────── */

static uint16_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t) i;
        for (int b = 0; b < 8; b++)
            c = (c & 1) ? (c >> 1) ^ 0x8408 : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint16_t nf_crc16(const uint8_t *buf, int len, uint16_t crc)
{
    if (!crc_ready) crc_init();
    for (int i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc_table[(crc ^ buf[i]) & 0xFF];
    return crc;
}

/* ── tx ────────────────────────────────────────────────────────────── */

void nf_hdlc_tx_init(nf_hdlc_tx_t *s, int inter_frame_flags,
                     nf_hdlc_underflow_fn underflow, void *user)
{
    memset(s, 0, sizeof(*s));
    s->underflow = underflow;
    s->user = user;
    s->inter_frame_flags = inter_frame_flags;
}

int nf_hdlc_tx_frame(nf_hdlc_tx_t *s, const uint8_t *buf, int len)
{
    if (len == 0) {
        s->tx_end = 1;
        return 0;
    }
    if (len > NF_HDLC_MAXFRAME || s->nbytes)
        return -1;                       /* lockout: one frame at a time */
    memcpy(s->buf, buf, (size_t) len);
    uint16_t crc = nf_crc16(buf, len, 0xFFFF) ^ 0xFFFF;
    s->buf[len]     = (uint8_t) crc;
    s->buf[len + 1] = (uint8_t) (crc >> 8);
    s->nbytes = len + 2;
    s->pos = 0;
    s->bytebit = 0;
    s->tx_end = 0;
    return 0;
}

void nf_hdlc_tx_flags(nf_hdlc_tx_t *s, int count)
{
    s->flag_octets = count;
    s->report_flag_underflow = 1;
    s->tx_end = 0;
}

void nf_hdlc_tx_restart(nf_hdlc_tx_t *s)
{
    s->nbytes = s->pos = s->bytebit = 0;
    s->ones = s->stuff = 0;
    s->sending_data = 0;
    s->flag_bitpos = 0;
    s->flag_octets = 0;
    s->report_flag_underflow = 0;
    s->tx_end = 0;
}

int nf_hdlc_tx_get_bit(nf_hdlc_tx_t *s)
{
    int bit;

    if (s->sending_data) {
        if (s->stuff) {                  /* insert a 0 after five ones */
            s->stuff = 0;
            s->ones = 0;
            return 0;
        }
        if (s->pos >= s->nbytes) {
            /* Frame (and any final stuffed bit) done. Report the underflow
             * now so a streaming source can queue the next frame to follow
             * after the inter-frame flags. */
            s->sending_data = 0;
            s->flag_bitpos = 0;
            s->nbytes = 0;
            s->report_flag_underflow = 0;
            if (s->underflow)
                s->underflow(s->user);
            s->flag_octets = s->nbytes ? s->inter_frame_flags : 3;
            /* fall through to flags */
        } else {
            bit = (s->buf[s->pos] >> s->bytebit) & 1;
            if (++s->bytebit == 8) { s->bytebit = 0; s->pos++; }
            if (bit) {
                if (++s->ones == 5) s->stuff = 1;
            } else {
                s->ones = 0;
            }
            return bit;
        }
    }

    /* flags / idle */
    if (s->flag_bitpos == 0 && s->flag_octets == 0) {
        /* At an octet boundary with no timed flags pending: start a queued
         * frame, end the stream, or keep idling on flags. */
        if (s->nbytes) {
            s->sending_data = 1;
            s->pos = 0; s->bytebit = 0;
            s->ones = 0; s->stuff = 0;
            return nf_hdlc_tx_get_bit(s);
        }
        if (s->tx_end) {
            s->tx_end = 0;
            return NF_SIG_END_OF_DATA;
        }
    }
    bit = (0x7E >> s->flag_bitpos) & 1;  /* LSB first: 0,1,1,1,1,1,1,0 */
    if (++s->flag_bitpos == 8) {
        s->flag_bitpos = 0;
        if (s->flag_octets > 0 && --s->flag_octets == 0
            && s->report_flag_underflow) {
            s->report_flag_underflow = 0;
            if (s->nbytes == 0 && s->underflow)
                s->underflow(s->user);
        }
    }
    return bit;
}

/* ── rx ────────────────────────────────────────────────────────────── */

void nf_hdlc_rx_init(nf_hdlc_rx_t *s, int framing_ok_threshold,
                     nf_hdlc_frame_fn handler, void *user)
{
    memset(s, 0, sizeof(*s));
    s->handler = handler;
    s->user = user;
    s->framing_ok_threshold = framing_ok_threshold < 1 ? 1 : framing_ok_threshold;
}

static void rx_status(nf_hdlc_rx_t *s, int status)
{
    if (s->handler)
        s->handler(s->user, NULL, status, 1);
}

static void rx_flag_or_abort(nf_hdlc_rx_t *s)
{
    if (s->raw & 0x0100) {
        /* abort: seven or more ones */
        rx_status(s, -8 /* abort, same value as spandsp's SIG_STATUS_ABORT */);
        if (s->flags_seen < s->framing_ok_threshold - 1)
            s->flags_seen = 0;
        else
            s->flags_seen = s->framing_ok_threshold - 1;
    } else {
        /* flag */
        if (s->flags_seen >= s->framing_ok_threshold) {
            if (s->len) {
                if (s->num_bits == 7 && s->len >= 2
                    && s->len <= NF_HDLC_MAXFRAME + 2) {
                    int ok = nf_crc16(s->buffer, s->len, 0xFFFF) == 0xF0B8;
                    if (s->handler)
                        s->handler(s->user, s->buffer, s->len - 2, ok);
                } else {
                    /* too short / misaligned: report as a bad frame, as
                     * spandsp does with report_bad_frames set */
                    int len = s->len >= 2 ? s->len - 2 : 0;
                    if (s->handler)
                        s->handler(s->user, s->buffer, len, 0);
                }
            }
        } else {
            /* Require back-to-back flags while validating the preamble
             * (except one short of the threshold, where an abort may have
             * broken octet alignment). */
            if (s->flags_seen != s->framing_ok_threshold - 1 && s->num_bits != 7) {
                if (s->flags_seen < s->framing_ok_threshold - 1)
                    s->flags_seen = 0;
                else
                    s->flags_seen = s->framing_ok_threshold - 1;
            }
            if (++s->flags_seen >= s->framing_ok_threshold
                && !s->framing_ok_announced) {
                rx_status(s, -6 /* framing OK, spandsp's SIG_STATUS_FRAMING_OK */);
                s->framing_ok_announced = 1;
            }
        }
    }
    s->len = 0;
    s->num_bits = 0;
}

void nf_hdlc_rx_put_bit(nf_hdlc_rx_t *s, int bit)
{
    if (bit < 0) {                       /* a status, not a bit */
        rx_status(s, bit);
        return;
    }
    s->raw = (uint16_t) ((s->raw << 1) | ((bit << 8) & 0x100));
    if ((s->raw & 0x3E00) == 0x3E00) {
        /* five ones in a row just passed: stuffing point, flag or abort */
        if ((s->raw & 0x4100) == 0)
            return;                      /* exactly five ones + 0: destuff */
        if ((s->raw & 0xFE00) == 0x7E00) {
            rx_flag_or_abort(s);
            return;
        }
    }
    s->num_bits++;
    if (s->flags_seen < s->framing_ok_threshold)
        return;                          /* not synchronized yet */
    s->byte_in_progress = (s->byte_in_progress | (s->raw & 0x100)) >> 1;
    if (s->num_bits == 8) {
        if (s->len < NF_HDLC_MAXFRAME + 2) {
            s->buffer[s->len++] = (uint8_t) s->byte_in_progress;
        } else {
            /* overlength: abandon and wait for the next flag */
            s->len = 0;
            s->flags_seen = s->framing_ok_threshold - 1;
        }
        s->num_bits = 0;
    }
}
