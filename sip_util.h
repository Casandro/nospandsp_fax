#ifndef SIP_UTIL_H
#define SIP_UTIL_H

#include <time.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Small helpers shared by sip_interface.c (the registrar/answer/dial driver)
 * and call.c (per-call SIP/RTP handling): MD5 for digest auth, a tiny PRNG for
 * SIP tokens, line-oriented SIP message parsing, and monotonic-clock math. */

/* ── MD5 (RFC 1321) ───────────────────────────────────────────────── */

/* Hex-encode the MD5 of NUL-terminated string s into out (33 bytes incl. the
 * NUL). Used to compute SIP digest authentication responses. */
void md5_hex(const char *s, char out[33]);

/* ── Random hex tokens ────────────────────────────────────────────── */

/* Weak time/pid/tid seed, retained only as the last-resort fallback inside
 * rng_bytes() when no OS randomness source is available. Not for security use. */
unsigned int rng_seed(void);

/* Fill buf with n cryptographically-strong random bytes (getrandom/urandom). */
void rng_bytes(void *buf, size_t n);

/* A cryptographically-strong random 32-bit value (RTP SSRC/seq/timestamp). */
uint32_t rng_u32(void);

/* Fill buf with n random lowercase hex digits plus a NUL (needs n+1 bytes).
 * Cryptographically strong. Used for SIP branch/tag tokens and Call-IDs. */
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

/* ── Family-agnostic socket addresses (IPv4 + IPv6) ───────────────── *
 *
 * The SIP/RTP stack stores every peer address as a sockaddr_storage and
 * runs its sockets dual-stack (an AF_INET6 socket with IPV6_V6ONLY off,
 * falling back to AF_INET on kernels without IPv6). On such a socket an
 * IPv4 peer appears as a v4-mapped IPv6 address (::ffff:a.b.c.d); these
 * helpers treat the mapped and plain forms as the same address, and
 * sa_ntop() always prints the plain dotted-quad. */

/* sockaddr length for the stored family (for sendto/connect/bind). */
socklen_t sa_len(const struct sockaddr_storage *ss);

/* Port in host byte order (0 if the family is unknown). */
int  sa_port(const struct sockaddr_storage *ss);
void sa_set_port(struct sockaddr_storage *ss, int port);

/* Numeric address text without brackets, v4-mapped unmapped to dotted-quad.
 * Returns buf ("" on error). buf should hold INET6_ADDRSTRLEN (46) bytes. */
const char *sa_ntop(const struct sockaddr_storage *ss, char *buf, size_t len);

/* Address (and port) equality, v4-mapped-aware. */
int  sa_same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b);
int  sa_same_addr_port(const struct sockaddr_storage *a, const struct sockaddr_storage *b);

/* Parse a numeric IPv4/IPv6 address (no brackets) + port into *ss in a form
 * usable as a destination on a socket of family sock_af (IPv4 text becomes a
 * v4-mapped address when sock_af is AF_INET6). Returns 0 on success. */
int  sa_from_ip(struct sockaddr_storage *ss, int sock_af, const char *ip, int port);

/* Convert *ss in place into a destination usable on a socket of family
 * sock_af (v4 <-> v4-mapped). Returns -1 for a real IPv6 address when
 * sock_af is AF_INET (unreachable from a v4-only socket). */
int  sa_map_to_af(struct sockaddr_storage *ss, int sock_af);

/* ── Monotonic time (CLOCK_MONOTONIC) ─────────────────────────────── */

/* Add ms milliseconds to ts, normalizing the nanosecond field. */
void ts_add_ms(struct timespec *ts, long ms);

/* Milliseconds from now until deadline (negative if it is already past). */
long ts_until_ms(const struct timespec *deadline);

#endif /* SIP_UTIL_H */
