#ifndef G711_H
#define G711_H

#include <stdint.h>

/* G.711 A-law (PCMA) companding - the only part of spandsp that the SIP/RTP
 * layer (sip.c) ever used. Vendored here so sip.c no longer depends on
 * libspandsp; this is the first step in weaning this build off spandsp.
 *
 * The algorithm is ITU-T G.711 A-law, transcribed from spandsp's g711.h inline
 * routines, so it is bit-for-bit identical to the spandsp build - the
 * cross-check harness relies on that to detect any real divergence. */

/* Encode a linear 16-bit sample to an A-law octet. */
uint8_t linear_to_alaw(int linear);

/* Decode an A-law octet to a linear 16-bit sample. */
int16_t alaw_to_linear(uint8_t alaw);

#endif /* G711_H */
