#ifndef NF_UDPTL_H
#define NF_UDPTL_H

#include <stdint.h>

/*
 * nf_udptl - the UDPTL transport for T.38 (ITU-T T.38 Annex B), our own code.
 *
 * A UDPTL datagram carries a 16-bit sequence number, one "primary" IFP packet,
 * and error-recovery information. We implement the common SIP profile:
 * UDPTL with REDUNDANCY (the primary is followed by copies of the N previous
 * IFP packets), so a lost datagram is recovered from the next one's secondaries.
 * (FEC is not implemented; an incoming FEC error-recovery block is ignored and
 * only the primary is used.)
 *
 * Encoding helpers follow T.38 Annex B "open type" / length rules:
 *   length 0..0x7F            -> 1 octet
 *   length 0x80..0x3FFF       -> 2 octets (0x8000 | value)
 *   (fragmentation 0xC0|.. is not produced; IFP packets are small)
 */

#define NF_UDPTL_BUF      16            /* circular history depth (power of two) */
#define NF_UDPTL_MASK     (NF_UDPTL_BUF - 1)
#define NF_UDPTL_MAXIFP   512           /* max IFP packet we carry               */
#define NF_UDPTL_MAXDGRAM 1400          /* max UDPTL datagram we build           */

typedef struct {
    int tx_seq_no;                      /* next sequence number to send          */
    int red_entries;                    /* redundancy depth (secondaries to add) */
    int far_max_datagram;               /* peer's T38FaxMaxDatagram               */
    struct { int len; uint8_t buf[NF_UDPTL_MAXIFP]; } tx[NF_UDPTL_BUF];

    int rx_started;                     /* have we seen the first rx datagram     */
    int rx_seq_no;                      /* next sequence number we expect         */
} nf_udptl_t;

/* Delivered for each newly received IFP packet, in ascending sequence order
 * (gap-fillers recovered from redundancy come before the primary). */
typedef void (*nf_udptl_ifp_fn)(void *user, const uint8_t *ifp, int ifp_len, int seq);

void nf_udptl_init(nf_udptl_t *s, int redundancy_depth, int far_max_datagram);

/* Build a UDPTL datagram carrying ifp[0..ifp_len). Returns datagram length in
 * out[0..cap), or -1 on error. */
int  nf_udptl_build(nf_udptl_t *s, const uint8_t *ifp, int ifp_len,
                    uint8_t *out, int cap);

/* Parse a received UDPTL datagram; invokes fn() for each newly received IFP
 * packet (recovering gaps from redundancy where possible). Returns 0 on success,
 * -1 if the datagram is malformed. */
int  nf_udptl_rx(nf_udptl_t *s, const uint8_t *buf, int len,
                 nf_udptl_ifp_fn fn, void *user);

#endif /* NF_UDPTL_H */
