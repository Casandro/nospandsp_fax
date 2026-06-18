#ifndef SIP_UTIL_H
#define SIP_UTIL_H

#include <time.h>

/* Small helpers shared by sip_interface.c (the registrar/answer/dial driver)
 * and call.c (per-call SIP/RTP handling): MD5 for digest auth, a tiny PRNG for
 * SIP tokens, line-oriented SIP message parsing, and monotonic-clock math. */

/* ── MD5 (RFC 1321) ───────────────────────────────────────────────── */

/* Hex-encode the MD5 of NUL-terminated string s into out (33 bytes incl. the
 * NUL). Used to compute SIP digest authentication responses. */
void md5_hex(const char *s, char out[33]);

/* ── Random hex tokens ────────────────────────────────────────────── */

/* A PRNG seed mixing time, pid, thread id and a per-process atomic counter, so
 * threads created within the same second don't collide (e.g. on RTP SSRCs). */
unsigned int rng_seed(void);

/* Fill buf with n random lowercase hex digits plus a NUL (needs n+1 bytes).
 * Used for SIP branch/tag tokens and Call-IDs. */
void gen_hex(char *buf, int n);

/* ── SIP message parsing ──────────────────────────────────────────── */

/* Copy the value of header `name` into dst (NUL-terminated, truncated to
 * dstlen). Case-insensitive. Returns dst if the header is present, else NULL. */
char *sip_hdr(const char *msg, const char *name, char *dst, int dstlen);

/* Copy the method (first token of the request line) into out. */
void sip_method(const char *msg, char *out, int outlen);

/* Return a pointer to the message body (just past the CRLF CRLF), or NULL. */
const char *sip_body(const char *msg);

/* "SIP/2.0 NNN ..." → NNN; 0 if msg is a request rather than a response. */
int sip_response_code(const char *msg);

/* Extract a key="value" token (e.g. realm, nonce) from an auth header into out
 * (NUL-terminated, truncated to outlen); out is set empty if not found. */
void parse_quoted(const char *hdrstr, const char *key, char *out, int outlen);

/* ── Monotonic time (CLOCK_MONOTONIC) ─────────────────────────────── */

/* Add ms milliseconds to ts, normalizing the nanosecond field. */
void ts_add_ms(struct timespec *ts, long ms);

/* Milliseconds from now until deadline (negative if it is already past). */
long ts_until_ms(const struct timespec *deadline);

#endif /* SIP_UTIL_H */
