#ifndef NF_T38_H
#define NF_T38_H

#include <stdint.h>
#include "nf_fax.h"        /* nf_fax_iface_t, nf_modem_ops_t            */

/*
 * nf_t38 - a T.38 *terminal-mode* backend for the nf_t30 protocol engine, our
 * own code (no spandsp at runtime). It plays the same role as nf_fax (the
 * V-series modem driver), but instead of modulating/demodulating audio it
 * carries the demodulated T.30 elements as T.38 IFP packets over UDPTL:
 *
 *   nf_t30  <--nf_fax_iface_t (up) / nf_modem_ops_t (down)-->  nf_t38
 *                                                               |  IFP
 *                                                            nf_udptl
 *                                                               |  datagrams
 *                                                            (caller's socket)
 *
 * Profile: T.38 version 0, UDPTL with redundancy, transferred-TCF (TCF rides
 * across the link as non-ECM data). The caller drives it by:
 *   - feeding received UDPTL datagrams to nf_t38_rx_datagram(), and
 *   - calling nf_t38_pump() on a periodic tick to emit queued tx IFP packets
 *     and advance the protocol timer.
 * Built datagrams are handed to the `send` callback supplied at init.
 */

typedef struct nf_t38 nf_t38_t;

nf_t38_t *nf_t38_init(int calling_party, const nf_fax_iface_t *iface,
                      int redundancy, int far_max_datagram,
                      void (*send)(void *user, const uint8_t *dgram, int len),
                      void *send_user);
void nf_t38_free(nf_t38_t *s);

/* The vtable nf_t30 uses to drive this backend (down direction). */
const nf_modem_ops_t *nf_t38_ops(void);

/* Feed one received UDPTL datagram; delivers IFP content up to nf_t30. */
void nf_t38_rx_datagram(nf_t38_t *s, const uint8_t *dgram, int len);

/* Periodic tx pump + timer advance. `ms` = wall-clock ms since the last call. */
void nf_t38_pump(nf_t38_t *s, int ms);

#endif /* NF_T38_H */
