#include "nf_udptl.h"
#include <string.h>

/* ── T.38 Annex B length / open-type codec ───────────────────────────────── */

static int enc_length(uint8_t *buf, int *len, int value)
{
    if (value < 0x80) {                 /* 1 octet */
        buf[(*len)++] = (uint8_t) value;
        return value;
    }
    if (value < 0x4000) {               /* 2 octets */
        buf[(*len)++] = (uint8_t) (0x80 | (value >> 8));
        buf[(*len)++] = (uint8_t) (value & 0xFF);
        return value;
    }
    /* Fragmentation: not produced (our packets are small). Encode a multiple of
     * 0x4000 and let the caller loop — but since we never hit this, clamp. */
    buf[(*len)++] = (uint8_t) (0xC0 | (value >> 14));
    return (value >> 14) << 14;
}

static int enc_open_type(uint8_t *buf, int cap, int *len,
                         const uint8_t *data, int n)
{
    /* A zero-length open type is encoded as a single zero octet (T.38 B.2). */
    uint8_t zero = 0;
    if (n == 0) { data = &zero; n = 1; }
    if (*len + 3 + n > cap) return -1;
    int wrote = enc_length(buf, len, n);
    if (wrote != n) return -1;          /* would fragment — unsupported here */
    memcpy(&buf[*len], data, (size_t) n);
    *len += n;
    return 0;
}

/* Decode a length. Returns 0 (complete), 1 (fragment follows — unsupported by
 * callers), or -1 on error. */
static int dec_length(const uint8_t *buf, int limit, int *pos, int *value)
{
    if (*pos >= limit) return -1;
    uint8_t b = buf[*pos];
    if ((b & 0x80) == 0) { *value = buf[(*pos)++]; return 0; }
    if ((b & 0x40) == 0) {
        if (*pos + 1 >= limit) return -1;
        *value = ((buf[*pos] & 0x3F) << 8) | buf[*pos + 1];
        *pos += 2;
        return 0;
    }
    *value = (buf[(*pos)++] & 0x3F) << 14;
    return 1;                           /* fragment */
}

/* Decode an open type into (*obj, *n). Returns 0 on success, -1 on error or a
 * fragmented value (we don't deal with fragments). */
static int dec_open_type(const uint8_t *buf, int limit, int *pos,
                         const uint8_t **obj, int *n)
{
    int cnt;
    if (dec_length(buf, limit, pos, &cnt) != 0) return -1;
    *n = cnt;
    *obj = NULL;
    if (cnt > 0) {
        if (*pos + cnt > limit) return -1;
        *obj = &buf[*pos];
        *pos += cnt;
    }
    return 0;
}

/* ── public ──────────────────────────────────────────────────────────────── */

void nf_udptl_init(nf_udptl_t *s, int redundancy_depth, int far_max_datagram)
{
    memset(s, 0, sizeof(*s));
    if (redundancy_depth < 0) redundancy_depth = 0;
    if (redundancy_depth > NF_UDPTL_BUF - 1) redundancy_depth = NF_UDPTL_BUF - 1;
    s->red_entries = redundancy_depth;
    s->far_max_datagram = (far_max_datagram > 0 && far_max_datagram <= NF_UDPTL_MAXDGRAM)
                        ? far_max_datagram : NF_UDPTL_MAXDGRAM;
}

int nf_udptl_build(nf_udptl_t *s, const uint8_t *ifp, int ifp_len,
                   uint8_t *out, int cap)
{
    if (ifp_len < 1 || ifp_len > NF_UDPTL_MAXIFP) return -1;
    int seq = s->tx_seq_no & 0xFFFF;
    int entry = seq & NF_UDPTL_MASK;

    /* Stash in the history for use as a future secondary. */
    s->tx[entry].len = ifp_len;
    memcpy(s->tx[entry].buf, ifp, (size_t) ifp_len);

    int len = 0;
    if (cap < 4) return -1;
    out[len++] = (uint8_t) (seq >> 8);
    out[len++] = (uint8_t) (seq & 0xFF);

    if (enc_open_type(out, cap, &len, ifp, ifp_len) < 0) return -1;

    /* Error recovery: secondary-IFP (redundancy). Type byte 0x00 = secondary. */
    if (len >= cap) return -1;
    out[len++] = 0x00;

    int entries = (s->tx_seq_no > s->red_entries) ? s->red_entries : s->tx_seq_no;
    int len_entries = len;
    if (enc_length(out, &len, entries) != entries) return -1;   /* small */

    for (int m = 0; m < entries; m++) {
        int prev = len;
        int j = (entry - m - 1) & NF_UDPTL_MASK;
        if (enc_open_type(out, cap, &len, s->tx[j].buf, s->tx[j].len) < 0) {
            len = prev;
            out[len_entries] = (uint8_t) m;     /* fewer entries than planned */
            break;
        }
        if (len > s->far_max_datagram) {        /* don't exceed peer's MTU */
            len = prev;
            out[len_entries] = (uint8_t) m;
            break;
        }
    }

    s->tx_seq_no++;
    return len;
}

int nf_udptl_rx(nf_udptl_t *s, const uint8_t *buf, int len,
                nf_udptl_ifp_fn fn, void *user)
{
    if (len < 2) return -1;
    int pos = 0;
    int seq = (buf[0] << 8) | buf[1];
    pos = 2;

    const uint8_t *primary = NULL;
    int plen = 0;
    if (dec_open_type(buf, len, &pos, &primary, &plen) != 0) return -1;
    if (plen > NF_UDPTL_MAXIFP) return -1;
    if (pos >= len) return -1;

    const uint8_t *secs[NF_UDPTL_BUF];
    int seclen[NF_UDPTL_BUF];
    int total = 0;

    uint8_t ectype = buf[pos++];
    if ((ectype & 0x80) == 0) {
        /* Secondary-packet (redundancy) mode. */
        int stat;
        do {
            int count;
            if ((stat = dec_length(buf, len, &pos, &count)) < 0) return -1;
            if (total + count > NF_UDPTL_BUF) return -1;
            for (int i = 0; i < count; i++)
                if (dec_open_type(buf, len, &pos, &secs[total + i], &seclen[total + i]) != 0)
                    return -1;
            total += count;
        } while (stat > 0);
    }
    /* else: FEC error-recovery block — ignored; we use the primary only. */

    if (!s->rx_started) {               /* baseline on the first datagram */
        s->rx_started = 1;
        s->rx_seq_no = seq;
    }

    /* The wire sequence number is 16 bits and wraps; compare in that space.
     * `fwd` is how far seq is ahead of what we expect, modulo 65536. A value
     * in the backward half-window is an old/duplicate primary and is dropped
     * WITHOUT advancing rx_seq_no - otherwise one spoofed datagram (e.g.
     * seq=0xFFFF) would push rx_seq_no past every legitimate packet and stall
     * the receiver permanently. This also handles the natural 16-bit wrap. */
    int fwd = (seq - s->rx_seq_no) & 0xFFFF;
    if (fwd >= 0x8000)                  /* old / duplicate / implausible jump */
        return 0;

    /* Fill any gap from the secondaries (oldest first). The m-th secondary
     * carries sequence seq-(m+1); only `total` are present, so cap the fill at
     * the redundancy depth — which also bounds this loop to NF_UDPTL_BUF. */
    int gap = fwd;
    if (gap > total) gap = total;
    for (int m = gap; m >= 1; m--) {
        int idx = m - 1;                /* secs[0]=seq-1, secs[1]=seq-2, ...   */
        int g = (seq - m) & 0xFFFF;
        if (idx < total && seclen[idx] > 0)
            fn(user, secs[idx], seclen[idx], g);
        /* else: lost beyond redundancy depth — leave the gap (T.38 copes). */
    }

    fn(user, primary, plen, seq);
    s->rx_seq_no = (seq + 1) & 0xFFFF;
    return 0;
}
