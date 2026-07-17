#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "sip.h"
#include "sip_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "g711.h"              /* linear_to_alaw / alaw_to_linear (vendored G.711) */

/*
 * Primitive single-call SIP/RTP stack. See sip.h for the scope. The wire
 * formats here mirror ../sip_modem/sip_interface/call.c; what is stripped is
 * the threading, the multi-call bookkeeping, the codec menu (PCMA only), and
 * the full transaction timer state machine (best-effort retransmits instead).
 */

#define PCMA_PT      8         /* RTP payload type for G.711 A-law          */
#define RTP_TS_INCR  160       /* 20 ms at 8 kHz                            */
#define SIP_TIMEOUT  32000     /* Timer B: give up an INVITE with no response yet */
#define SIP_TIMER_C  180000    /* Timer C: give up an INVITE that is ringing (>3 min) */

/* ── Low-level send/recv ──────────────────────────────────────────────── */

static void sip_send(int sock, const char *buf, int len, struct sockaddr_in *to)
{
    sendto(sock, buf, (size_t)len, 0, (struct sockaddr *)to, sizeof(*to));
}

/* Fill a Via branch token: the RFC 3261 magic cookie + 12 random hex digits.
 * `branch` must hold at least 20 bytes (7 + 12 + NUL). */
static void gen_branch(char *branch)
{
    memcpy(branch, "z9hG4bK", 7);
    gen_hex(branch + 7, 12);
}

static void vlog(const sip_media_t *m, const char *dir, const char *msg);

/* Format a SIP message into a local buffer and send it to `to`, clamping the
 * (untruncated) snprintf length so a would-be-oversized message never makes
 * sendto read past the buffer, then log it. For the bodyless responses and
 * dialog requests we build ad hoc; the retransmit-cached 200 OK/ACK and the
 * two deliberately-unlogged sends (provisional 100/180, T.38 ACK) keep their
 * own inline builders. */
static void sip_sendf(sip_media_t *m, struct sockaddr_in *to, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void sip_sendf(sip_media_t *m, struct sockaddr_in *to, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int) sizeof buf) n = (int) sizeof buf - 1;
    sip_send(m->sip_sock, buf, n, to);
    vlog(m, "TX", buf);
}

/* Wait up to timeout_ms for one SIP datagram. Returns its length (NUL
 * terminated into buf), 0 on timeout, -1 on error. */
static int sip_recv(int sock, char *buf, int cap, int timeout_ms,
                    struct sockaddr_in *from)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return r;            /* 0 = timeout, <0 = error */

    socklen_t sl = sizeof(*from);
    ssize_t n = recvfrom(sock, buf, (size_t)cap - 1, 0,
                         (struct sockaddr *)from, &sl);
    if (n <= 0) return (int)n;
    buf[n] = '\0';
    return (int)n;
}

/* Optional wall-clock prefix ("HH:MM:SS.mmm ") on every protocol log line, so
 * SIP and T.30 events can be correlated in time. Enabled by NF_LOG_TS (set by
 * sip_fax --debug). Cached so the common (no-timestamp) path stays cheap. */
static void nf_log_ts(FILE *f)
{
    static int en = -1;
    if (en < 0) en = getenv("NF_LOG_TS") != NULL;
    if (!en) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d.%03ld ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

static void vlog(const sip_media_t *m, const char *dir, const char *msg)
{
    if (!m->verbose) return;
    /* Full-message dump (all headers) vs. just the request/status line.
     * Enabled by NF_SIP_FULL (set by sip_fax --debug). */
    static int full = -1;
    if (full < 0) full = getenv("NF_SIP_FULL") != NULL;

    nf_log_ts(stderr);
    if (m->log_callid && m->call_id[0])
        fprintf(stderr, "[%s] ", m->call_id);

    if (full) {
        fprintf(stderr, "SIP %s (%zu bytes):\n", dir, strlen(msg));
        for (const char *p = msg; *p; ) {
            const char *nl = strpbrk(p, "\r\n");
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len > 0) fprintf(stderr, "    | %.*s\n", len, p);
            if (!nl) break;
            p = nl + ((nl[0] == '\r' && nl[1] == '\n') ? 2 : 1);
        }
        return;
    }

    char first[120];
    int i = 0;
    while (msg[i] && msg[i] != '\r' && msg[i] != '\n' && i < (int)sizeof(first) - 1) {
        first[i] = msg[i]; i++;
    }
    first[i] = '\0';
    fprintf(stderr, "SIP %s %s\n", dir, first);
}

/* ── Local IP / address resolution ───────────────────────────────────── */

/* Discover the source IP the kernel would use to reach dest (a dotted-quad),
 * via a throwaway connected UDP socket. Returns 0 on success. */
static int get_local_ip(const char *dest, char *out, int outlen)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(5060);
    if (inet_pton(AF_INET, dest, &a.sin_addr) != 1) { close(s); return -1; }
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    socklen_t sl = sizeof(a);
    if (getsockname(s, (struct sockaddr *)&a, &sl) < 0) { close(s); return -1; }
    close(s);
    const char *ip = inet_ntoa(a.sin_addr);
    if (!ip) return -1;
    strncpy(out, ip, (size_t)outlen - 1);
    out[outlen - 1] = '\0';
    return 0;
}

/* Resolve host:5060 into *addr (IPv4). Returns 0 on success. */
static int resolve_host(const char *host, struct sockaddr_in *addr)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, "5060", &hints, &res) != 0 || !res) return -1;
    *addr = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    return 0;
}

/* ── RTP ──────────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  vpxcc;
    uint8_t  mpt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
} rtp_hdr_t;

/* Bind an ephemeral local RTP port and seed the stream. Returns 0 / local port
 * via *port. */
static int rtp_open(sip_media_t *m, int *port)
{
    m->rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->rtp_sock < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = 0;
    if (bind(m->rtp_sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(m->rtp_sock); m->rtp_sock = -1; return -1;
    }
    socklen_t sl = sizeof(a);
    getsockname(m->rtp_sock, (struct sockaddr *)&a, &sl);
    *port = ntohs(a.sin_port);
    m->rtp_ssrc = rng_u32();
    m->rtp_seq  = (uint16_t) rng_u32();
    m->rtp_ts   = rng_u32();
    return 0;
}

/* Symmetric-RTP/UDPTL latch guard: accept a media source only if it plausibly
 * is our peer - same IP as the SDP-negotiated media address (`neg`) or as the
 * signalling peer. This still follows a NAT-mapped PORT change (the reason
 * symmetric RTP exists) but refuses to hand our media stream to an unrelated
 * host. Callers latch to the first plausible source and then lock it. */
static int media_src_plausible(const sip_media_t *m, const struct sockaddr_in *src,
                               const struct sockaddr_in *neg)
{
    return src->sin_addr.s_addr == neg->sin_addr.s_addr ||
           src->sin_addr.s_addr == m->sip_peer.sin_addr.s_addr;
}

void sip_media_tx(sip_media_t *m, const int16_t *pcm, int n)
{
    if (!m->have_rtp_dest || m->rtp_sock < 0) return;
    if (n > 160) n = 160;
    uint8_t pkt[12 + 160];
    rtp_hdr_t *h = (rtp_hdr_t *)pkt;
    h->vpxcc = 0x80;
    h->mpt   = PCMA_PT;
    h->seq   = htons(m->rtp_seq);
    h->ts    = htonl(m->rtp_ts);
    h->ssrc  = htonl(m->rtp_ssrc);
    for (int i = 0; i < n; i++)
        pkt[12 + i] = linear_to_alaw(pcm[i]);
    sendto(m->rtp_sock, pkt, (size_t)(12 + n), 0,
           (struct sockaddr *)&m->remote_rtp, sizeof(m->remote_rtp));
    m->rtp_seq++;
    m->rtp_ts += RTP_TS_INCR;
}

int sip_media_rx(sip_media_t *m, int16_t *pcm, int max)
{
    if (m->rtp_sock < 0) return 0;
    uint8_t pkt[2048];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    ssize_t r = recvfrom(m->rtp_sock, pkt, sizeof(pkt), MSG_DONTWAIT,
                         (struct sockaddr *)&src, &sl);
    if (r <= 12) return 0;

    /* Symmetric RTP: adopt the peer's actual (NAT-mapped) source ONCE from a
     * plausible address, then lock it; ignore media from any other source.
     * Without this a single unsolicited packet redirected our TX stream. */
    if (!m->rtp_latched) {
        if (!media_src_plausible(m, &src, &m->remote_rtp))
            return 0;                       /* unrelated host: drop */
        m->remote_rtp  = src;
        m->rtp_latched = 1;
    } else if (src.sin_addr.s_addr != m->remote_rtp.sin_addr.s_addr ||
               src.sin_port        != m->remote_rtp.sin_port) {
        return 0;                           /* not our latched peer: drop */
    }

    if ((pkt[1] & 0x7F) != PCMA_PT) return 0;
    int hdr = 12 + (pkt[0] & 0x0F) * 4;
    int n = (int)r - hdr;
    if (n <= 0) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        pcm[i] = alaw_to_linear(pkt[hdr + i]);
    return n;
}

/* ── SDP ──────────────────────────────────────────────────────────────── */

static int build_sdp(const char *local_ip, int rtp_port, char *sdp, int cap)
{
    long t = (long)time(NULL);
    return snprintf(sdp, (size_t)cap,
        "v=0\r\n"
        "o=- %ld %ld IN IP4 %s\r\n"
        "s=sip_fax\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %d RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=sendrecv\r\n",
        t, t, local_ip, local_ip, rtp_port);
}

/* Pull the audio destination (c=/m=) out of an SDP body and confirm PCMA (PT 8)
 * is offered. Returns 1 on success, filling remote_ip/port. */
static int parse_sdp(const char *body, char *remote_ip, int *port)
{
    if (!body) return 0;
    int has_pcma = 0;
    *port = 0;
    remote_ip[0] = '\0';

    const char *p = body;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\r' && *eol != '\n') eol++;
        if (p[0] == 'c' && p[1] == '=') {
            /* Search for the address only within this c= line, not the rest of
             * the body: a malformed c= must not borrow an IP4 from a later line. */
            const char *ip = strstr(p, "IP4 ");
            if (ip && ip < eol) {
                ip += 4;
                int n = (int)(eol - ip);
                if (n < 0) n = 0;
                if (n > 63) n = 63;
                memcpy(remote_ip, ip, (size_t)n);
                remote_ip[n] = '\0';
            }
        } else if (p[0] == 'm' && p[1] == '=') {
            int pv;
            if (sscanf(p + 2, "audio %d", &pv) == 1) *port = pv;
            /* scan the payload-type list for 8 (PCMA) */
            char line[256] = {0};
            const char *e = p;
            while (*e && *e != '\r' && *e != '\n') e++;
            int ml = (int)(e - p);
            if (ml >= (int)sizeof(line)) ml = sizeof(line) - 1;
            memcpy(line, p, (size_t)ml);
            char *tok = strtok(line, " \t");   /* m=audio */
            tok = strtok(NULL, " \t");          /* port    */
            tok = strtok(NULL, " \t");          /* RTP/AVP */
            while ((tok = strtok(NULL, " \t")) != NULL)
                if (atoi(tok) == PCMA_PT) has_pcma = 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return (has_pcma && *port > 0 && remote_ip[0]) ? 1 : 0;
}

/* Point m->remote_rtp at remote_ip:port. */
static void set_remote_rtp(sip_media_t *m, const char *remote_ip, int port)
{
    memset(&m->remote_rtp, 0, sizeof(m->remote_rtp));
    m->remote_rtp.sin_family = AF_INET;
    m->remote_rtp.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, remote_ip, &m->remote_rtp.sin_addr);
    m->have_rtp_dest = 1;
    m->rtp_latched   = 0;   /* re-latch to the newly negotiated address */
}

/* ── T.38 / UDPTL media ─────────────────────────────────────────────────── */

#define T38_MAX_DATAGRAM 300

/* Build our T.38 (image/udptl) SDP offer/answer. */
static int build_t38_sdp(const char *local_ip, int t38_port, char *sdp, int cap)
{
    long t = (long) time(NULL);
    return snprintf(sdp, (size_t) cap,
        "v=0\r\n"
        "o=- %ld %ld IN IP4 %s\r\n"
        "s=sip_fax\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=image %d udptl t38\r\n"
        "a=T38FaxVersion:0\r\n"
        "a=T38MaxBitRate:14400\r\n"
        "a=T38FaxRateManagement:transferredTCF\r\n"
        "a=T38FaxMaxBuffer:1000\r\n"
        "a=T38FaxMaxDatagram:%d\r\n"
        "a=T38FaxUdpEC:t38UDPRedundancy\r\n",
        t, t, local_ip, local_ip, t38_port, T38_MAX_DATAGRAM);
}

/* Detect a T.38 offer in an SDP body. Returns 1 and fills remote_ip / *port /
 * *far_datagram if an "m=image ... udptl t38" media line is present. */
static int parse_t38_sdp(const char *body, char *remote_ip, int *port, int *far_datagram)
{
    if (!body) return 0;
    int found = 0;
    *port = 0;
    *far_datagram = T38_MAX_DATAGRAM;
    remote_ip[0] = '\0';

    const char *p = body;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\r' && *eol != '\n') eol++;
        if (p[0] == 'c' && p[1] == '=') {
            /* Search within this c= line only (see parse_sdp). */
            const char *ip = strstr(p, "IP4 ");
            if (ip && ip < eol) {
                ip += 4;
                int n = (int) (eol - ip);
                if (n < 0) n = 0;
                if (n > 63) n = 63;
                memcpy(remote_ip, ip, (size_t) n);
                remote_ip[n] = '\0';
            }
        } else if (p[0] == 'm' && p[1] == '=' && strncmp(p, "m=image", 7) == 0) {
            /* Require udptl+t38 on THIS m= line, not anywhere in the body. */
            const char *u = strstr(p, "udptl"), *t = strstr(p, "t38");
            int pv;
            if (u && u < eol && t && t < eol &&
                sscanf(p + 2, "image %d", &pv) == 1 && pv > 0) { *port = pv; found = 1; }
        } else if (p[0] == 'a' && p[1] == '=' &&
                   strncmp(p, "a=T38FaxMaxDatagram:", 20) == 0) {
            int dg = atoi(p + 20);
            if (dg > 0) *far_datagram = dg;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return (found && *port > 0 && remote_ip[0]) ? 1 : 0;
}

/* Bind an ephemeral UDP port for T.38/UDPTL media. */
static int udptl_open(sip_media_t *m, int *port)
{
    m->t38_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->t38_sock < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0;
    if (bind(m->t38_sock, (struct sockaddr *) &a, sizeof(a)) < 0) {
        close(m->t38_sock); m->t38_sock = -1; return -1;
    }
    socklen_t sl = sizeof(a);
    getsockname(m->t38_sock, (struct sockaddr *) &a, &sl);
    *port = ntohs(a.sin_port);
    return 0;
}

static void set_t38_peer(sip_media_t *m, const char *ip, int port)
{
    memset(&m->t38_peer, 0, sizeof(m->t38_peer));
    m->t38_peer.sin_family = AF_INET;
    m->t38_peer.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, ip, &m->t38_peer.sin_addr);
    m->t38_latched = 0;   /* re-latch to the newly negotiated address */
}

int sip_media_is_t38(const sip_media_t *m) { return m->t38_active; }

void sip_t38_tx(sip_media_t *m, const uint8_t *dgram, int len)
{
    if (m->t38_sock < 0) return;
    sendto(m->t38_sock, dgram, (size_t) len, 0,
           (struct sockaddr *) &m->t38_peer, sizeof(m->t38_peer));
}

int sip_t38_rx(sip_media_t *m, uint8_t *buf, int max)
{
    if (m->t38_sock < 0) return 0;
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    ssize_t r = recvfrom(m->t38_sock, buf, (size_t) max, MSG_DONTWAIT,
                         (struct sockaddr *) &src, &sl);
    if (r <= 0) return 0;
    int dbg = getenv("NF_T38_DBG") != NULL;
    /* Symmetric UDPTL: adopt the peer's actual source ONCE from a plausible
     * address, then lock it; ignore datagrams from any other source. */
    if (!m->t38_latched) {
        if (!media_src_plausible(m, &src, &m->t38_peer)) {
            if (dbg) fprintf(stderr, "[T38 sock] drop %zd bytes from %s:%d "
                             "(implausible; expected %s:%d)\n", r,
                             inet_ntoa(src.sin_addr), ntohs(src.sin_port),
                             inet_ntoa(m->t38_peer.sin_addr), ntohs(m->t38_peer.sin_port));
            return 0;                       /* unrelated host: drop */
        }
        m->t38_peer    = src;
        m->t38_latched = 1;
        if (dbg) fprintf(stderr, "[T38 sock] latched to %s:%d\n",
                         inet_ntoa(src.sin_addr), ntohs(src.sin_port));
    } else if (src.sin_addr.s_addr != m->t38_peer.sin_addr.s_addr ||
               src.sin_port != m->t38_peer.sin_port) {
        if (dbg) fprintf(stderr, "[T38 sock] drop %zd bytes from %s:%d "
                         "(not latched peer %s:%d)\n", r,
                         inet_ntoa(src.sin_addr), ntohs(src.sin_port),
                         inet_ntoa(m->t38_peer.sin_addr), ntohs(m->t38_peer.sin_port));
        return 0;                           /* not our latched peer: drop */
    }
    if (dbg) fprintf(stderr, "[T38 sock] rx %zd bytes from %s:%d\n", r,
                     inet_ntoa(src.sin_addr), ntohs(src.sin_port));
    return (int) r;
}

/* Copy the bare sip: URI out of a To/From/Contact header value: the text
 * inside <...>, else up to the first ';' or whitespace. */
static void extract_uri(const char *hdr, char *out, int cap)
{
    const char *lt = strchr(hdr, '<');
    const char *s, *e;
    if (lt) {
        s = lt + 1;
        e = strchr(s, '>');
        if (!e) e = s + strlen(s);
    } else {
        s = hdr;
        e = s;
        while (*e && *e != ';' && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n')
            e++;
    }
    int n = (int)(e - s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
}

/* ── Digest auth ──────────────────────────────────────────────────────── */

/* Does the challenge's qop-options list contain the "auth" token (as opposed
 * to only "auth-int", which we can't do without hashing the body)? */
static int qop_has_auth(const char *qop)
{
    if (!qop) return 0;
    for (const char *p = qop; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - s);
        while (len > 0 && s[len - 1] == ' ') len--;
        if (len == 4 && strncmp(s, "auth", 4) == 0) return 1;
    }
    return 0;
}

/* Build a Digest Authorization/Proxy-Authorization header into out. Uses
 * qop=auth (with a fresh cnonce and nc) when the challenge offers it - which
 * makes the response non-replayable and interoperable with modern servers -
 * and falls back to the RFC 2069 form otherwise. Buffers are sized so a
 * maximum-length realm/nonce/uri is hashed in full rather than silently
 * truncated (which would compute the digest over the wrong input). */
static void build_auth(const char *user, const char *pass, const char *method,
                       const char *uri, const char *realm, const char *nonce,
                       const char *qop, const char *opaque,
                       int proxy, char *out, int cap)
{
    char ha1_in[640], ha2_in[768], resp_in[512], ha1[33], ha2[33], resp[33];
    snprintf(ha1_in, sizeof(ha1_in), "%s:%s:%s", user, realm, pass);
    snprintf(ha2_in, sizeof(ha2_in), "%s:%s", method, uri);
    md5_hex(ha1_in, ha1);
    md5_hex(ha2_in, ha2);

    char opq[192] = "";
    if (opaque && opaque[0]) snprintf(opq, sizeof(opq), ",opaque=\"%s\"", opaque);

    if (qop_has_auth(qop)) {
        char cnonce[17];
        gen_hex(cnonce, 16);
        const char *nc = "00000001";     /* one use per fresh cnonce */
        snprintf(resp_in, sizeof(resp_in), "%s:%s:%s:%s:auth:%s",
                 ha1, nonce, nc, cnonce, ha2);
        md5_hex(resp_in, resp);
        snprintf(out, (size_t)cap,
            "%s: Digest username=\"%s\",realm=\"%s\",nonce=\"%s\",uri=\"%s\","
            "response=\"%s\",algorithm=MD5,qop=auth,nc=%s,cnonce=\"%s\"%s\r\n",
            proxy ? "Proxy-Authorization" : "Authorization",
            user, realm, nonce, uri, resp, nc, cnonce, opq);
    } else {
        snprintf(resp_in, sizeof(resp_in), "%s:%s:%s", ha1, nonce, ha2);
        md5_hex(resp_in, resp);
        snprintf(out, (size_t)cap,
            "%s: Digest username=\"%s\",realm=\"%s\",nonce=\"%s\","
            "uri=\"%s\",response=\"%s\",algorithm=MD5%s\r\n",
            proxy ? "Proxy-Authorization" : "Authorization",
            user, realm, nonce, uri, resp, opq);
    }
}

/* Pull realm/nonce (and qop/opaque, if present) out of a 401/407. Buffers are
 * the fixed sizes the callers declare: realm/nonce[256], qop[64], opaque[192]. */
static void challenge(const char *msg, char *realm, char *nonce,
                      char *qop, char *opaque)
{
    char hdr[600] = "";
    if (!sip_hdr(msg, "WWW-Authenticate", hdr, sizeof(hdr)))
        sip_hdr(msg, "Proxy-Authenticate", hdr, sizeof(hdr));
    parse_quoted(hdr, "realm", realm, 256);
    parse_quoted(hdr, "nonce", nonce, 256);
    parse_quoted(hdr, "qop", qop, 64);
    parse_quoted(hdr, "opaque", opaque, 192);
}

/* ── REGISTER (answer mode, optional) ─────────────────────────────────── */

static int do_register(sip_media_t *m, const sip_config_t *cfg,
                       struct sockaddr_in *reg_addr)
{
    char call_id[40], tok[20];
    gen_hex(tok, 16);
    snprintf(call_id, sizeof(call_id), "reg-%s", tok);
    char realm[256] = "", nonce[256] = "", qop[64] = "", opaque[192] = "";
    int cseq = 1;

    for (int attempt = 0; attempt < 2; attempt++) {
        char branch[20], from_tag[16];
        gen_branch(branch);
        gen_hex(from_tag, 10);

        char uri[300];
        snprintf(uri, sizeof(uri), "sip:%s", cfg->registrar_host);

        char auth[800] = "";
        if (attempt == 1)
            build_auth(cfg->local_user, cfg->password, "REGISTER", uri,
                       realm, nonce, qop, opaque, 0, auth, sizeof(auth));

        char req[2048];
        int n = snprintf(req, sizeof(req),
            "REGISTER %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
            "From: <sip:%s@%s>;tag=%s\r\n"
            "To: <sip:%s@%s>\r\n"
            "Call-ID: %s@%s\r\n"
            "CSeq: %d REGISTER\r\n"
            "Contact: <sip:%s@%s:%d>\r\n"
            "Max-Forwards: 70\r\n"
            "Expires: %d\r\n"
            "%s"
            "Content-Length: 0\r\n"
            "\r\n",
            uri, m->local_ip, m->local_sip_port, branch,
            cfg->local_user, cfg->registrar_host, from_tag,
            cfg->local_user, cfg->registrar_host,
            call_id, m->local_ip, cseq++,
            cfg->local_user, m->local_ip, m->local_sip_port,
            cfg->reg_expires, auth);
        if (n >= (int)sizeof(req)) n = (int)sizeof(req) - 1;
        sip_send(m->sip_sock, req, n, reg_addr);
        vlog(m, "TX", req);

        char resp[8192];
        struct sockaddr_in from;
        int r = sip_recv(m->sip_sock, resp, sizeof(resp), 4000, &from);
        if (r <= 0) { fprintf(stderr, "REGISTER: no response\n"); return -1; }
        vlog(m, "RX", resp);

        int code = sip_response_code(resp);
        if (code >= 200 && code < 300) {
            fprintf(stderr, "Registered with %s.\n", cfg->registrar_host);
            return 0;
        }
        if ((code == 401 || code == 407) && attempt == 0 && cfg->password[0]) {
            challenge(resp, realm, nonce, qop, opaque);
            continue;
        }
        fprintf(stderr, "REGISTER failed (%d)\n", code);
        return -1;
    }
    return -1;
}

/* ── UAC: place a call ────────────────────────────────────────────────── */

static int build_invite(sip_media_t *m, const sip_config_t *cfg,
                        const char *target_uri, const char *from_tag,
                        const char *branch, int cseq, int rtp_port,
                        const char *realm, const char *nonce,
                        const char *qop, const char *opaque, int proxy,
                        char *req, int cap)
{
    char auth[900] = "";
    if (realm && realm[0] && nonce && nonce[0] && cfg->password[0])
        build_auth(cfg->local_user, cfg->password, "INVITE", target_uri,
                   realm, nonce, qop, opaque, proxy, auth, sizeof(auth));

    char sdp[512];
    int sdp_len = build_sdp(m->local_ip, rtp_port, sdp, sizeof(sdp));

    int n = snprintf(req, (size_t)cap,
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "%s"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        target_uri, m->local_ip, m->local_sip_port, branch,
        cfg->local_user, cfg->registrar_host, from_tag,
        target_uri, m->call_id, cseq,
        cfg->local_user, m->local_ip, m->local_sip_port,
        auth, sdp_len, sdp);
    if (n >= cap) n = cap - 1;
    return n;
}

static void send_ack(sip_media_t *m, const sip_config_t *cfg,
                     const char *target_uri, const char *from_tag,
                     const char *to_hdr, int cseq, int new_branch,
                     const char *fixed_branch)
{
    char branch[20];
    if (new_branch) gen_branch(branch);
    else { strncpy(branch, fixed_branch, sizeof(branch) - 1); branch[sizeof(branch)-1] = '\0'; }

    int n = snprintf(m->ok_buf, sizeof(m->ok_buf),
        "ACK %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d ACK\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        target_uri, m->local_ip, m->local_sip_port, branch,
        cfg->local_user, cfg->registrar_host, from_tag,
        to_hdr, m->call_id, cseq);
    if (n >= (int)sizeof(m->ok_buf)) n = (int)sizeof(m->ok_buf) - 1;
    m->ok_buf_len = n;            /* kept so a retransmitted 2xx can be re-ACKed */
    sip_send(m->sip_sock, m->ok_buf, n, &m->sip_peer);
}

/* Cancel an INVITE that is in progress (a provisional was received but no final
 * response arrived in time). RFC 3261 §9.1: the CANCEL reuses the INVITE's
 * top Via branch, Call-ID, From (with tag), To (no tag) and CSeq number, with
 * method CANCEL. The peer answers 200 (to the CANCEL) then 487 (to the INVITE),
 * which the caller ACKs. */
static void send_cancel(sip_media_t *m, const sip_config_t *cfg,
                        const char *target_uri, const char *from_tag,
                        const char *branch, int cseq)
{
    char req[1024];
    int n = snprintf(req, sizeof req,
        "CANCEL %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d CANCEL\r\n"
        "Content-Length: 0\r\n\r\n",
        target_uri, m->local_ip, m->local_sip_port, branch,
        cfg->local_user, cfg->registrar_host, from_tag, target_uri,
        m->call_id, cseq);
    if (n >= (int) sizeof req) n = (int) sizeof req - 1;
    sip_send(m->sip_sock, req, n, &m->sip_peer);
    vlog(m, "TX", req);
}

/* After sending a CANCEL, drain responses briefly: ACK the 487 (final response
 * to the cancelled INVITE); ignore the 200 to the CANCEL itself. If the callee
 * raced us and answered (2xx to the INVITE), ACK it and BYE so we don't leave a
 * dangling dialog. */
static void await_cancel_final(sip_media_t *m, const sip_config_t *cfg,
                               const char *target_uri, const char *from_tag,
                               const char *branch, int cseq)
{
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    ts_add_ms(&dl, 5000);
    for (;;) {
        long w = ts_until_ms(&dl);
        if (w <= 0) break;
        char resp[8192]; struct sockaddr_in from;
        int r = sip_recv(m->sip_sock, resp, sizeof resp, (int) w, &from);
        if (r <= 0) break;
        vlog(m, "RX", resp);
        int code = sip_response_code(resp);
        if (code == 0) continue;
        char csq[64] = ""; sip_hdr(resp, "CSeq", csq, sizeof csq);
        if (strstr(csq, "CANCEL")) continue;      /* 200 to our CANCEL: keep waiting */
        if (code < 200) continue;                 /* stray provisional */
        char to_hdr[300] = ""; sip_hdr(resp, "To", to_hdr, sizeof to_hdr);
        if (code >= 300) {                        /* 487 (or other) final to INVITE */
            send_ack(m, cfg, target_uri, from_tag, to_hdr, cseq, 0, branch);
            return;
        }
        /* raced 2xx: the call actually came up despite the CANCEL - ACK + BYE */
        send_ack(m, cfg, target_uri, from_tag, to_hdr, cseq, 1, branch);
        snprintf(m->bye_ruri, sizeof m->bye_ruri, "%s", target_uri);
        snprintf(m->bye_from, sizeof m->bye_from,
                 "<sip:%s@%s>;tag=%s", cfg->local_user, cfg->registrar_host, from_tag);
        snprintf(m->bye_to, sizeof m->bye_to, "%s", to_hdr);
        m->cseq = cseq + 1;
        sip_call_send_bye(m);
        return;
    }
}

static int uac_establish(const sip_config_t *cfg, sip_media_t *m)
{
    struct sockaddr_in proxy;
    if (resolve_host(cfg->registrar_host, &proxy) < 0) {
        fprintf(stderr, "cannot resolve %s\n", cfg->registrar_host);
        return -1;
    }
    m->sip_peer = proxy;

    if (get_local_ip(inet_ntoa(proxy.sin_addr), m->local_ip, sizeof(m->local_ip)) < 0) {
        fprintf(stderr, "cannot determine local IP\n");
        return -1;
    }

    /* Bind the local SIP socket; its port appears in our Via/Contact. */
    m->sip_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons((uint16_t)cfg->local_sip_port);
    if (bind(m->sip_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind SIP");
        return -1;
    }

    /* Normalize the dial target into a sip: URI. */
    char target_uri[600];
    const char *t = cfg->target;
    if (strncasecmp(t, "sip:", 4) == 0)
        snprintf(target_uri, sizeof(target_uri), "%s", t);
    else if (strchr(t, '@'))
        snprintf(target_uri, sizeof(target_uri), "sip:%s", t);
    else
        snprintf(target_uri, sizeof(target_uri), "sip:%s@%s", t, cfg->registrar_host);

    int rtp_port;
    if (rtp_open(m, &rtp_port) < 0) { fprintf(stderr, "RTP bind failed\n"); return -1; }

    char tok[20];
    gen_hex(tok, 16);
    snprintf(m->call_id, sizeof(m->call_id), "%s@%s", tok, m->local_ip);

    char from_tag[16], branch[20];
    gen_hex(from_tag, 10);
    gen_branch(branch);

    char realm[256] = "", nonce[256] = "", qop[64] = "", opaque[192] = "";
    int cseq = 1, auth_tries = 0;
    char req[2048];
    int req_len = build_invite(m, cfg, target_uri, from_tag, branch, cseq,
                               rtp_port, NULL, NULL, NULL, NULL, 0, req, sizeof(req));
    sip_send(m->sip_sock, req, req_len, &m->sip_peer);
    vlog(m, "TX", req);
    fprintf(stderr, "INVITE -> %s\n", target_uri);

    struct timespec next_tx, giveup;
    clock_gettime(CLOCK_MONOTONIC, &next_tx);
    giveup = next_tx;
    ts_add_ms(&next_tx, 500);
    ts_add_ms(&giveup, SIP_TIMEOUT);
    long rto = 500;
    int provisional = 0;

    for (;;) {
        long wg = ts_until_ms(&giveup);
        if (wg <= 0) {
            if (provisional) {
                /* Ringing but no final response within Timer C: cancel it
                 * cleanly (RFC 3261 §9.1) rather than abandon the callee. */
                fprintf(stderr, "INVITE: no final response after %ds - cancelling\n",
                        SIP_TIMER_C / 1000);
                send_cancel(m, cfg, target_uri, from_tag, branch, cseq);
                await_cancel_final(m, cfg, target_uri, from_tag, branch, cseq);
            } else {
                fprintf(stderr, "INVITE: no answer (timeout)\n");
            }
            return -1;
        }
        long wt = provisional ? wg : ts_until_ms(&next_tx);
        long wms = wt < wg ? wt : wg;
        if (wms < 0) wms = 0;

        char resp[8192];
        struct sockaddr_in from;
        int r = sip_recv(m->sip_sock, resp, sizeof(resp), (int)wms, &from);
        if (r < 0) return -1;

        if (r == 0) {                       /* timeout: retransmit INVITE */
            if (!provisional && ts_until_ms(&next_tx) <= 0) {
                sip_send(m->sip_sock, req, req_len, &m->sip_peer);
                rto *= 2; if (rto > 4000) rto = 4000;
                clock_gettime(CLOCK_MONOTONIC, &next_tx);
                ts_add_ms(&next_tx, rto);
            }
            continue;
        }
        vlog(m, "RX", resp);

        int code = sip_response_code(resp);
        if (code == 0) continue;            /* a request, not our response */
        if (code < 200) {
            /* A provisional (100/180/183...) moves us to Timer C: the callee is
             * ringing, so wait much longer (>3 min) for a final response, reset
             * on each provisional, and stop retransmitting the INVITE. */
            provisional = 1;
            clock_gettime(CLOCK_MONOTONIC, &giveup);
            ts_add_ms(&giveup, SIP_TIMER_C);
            continue;
        }

        char to_hdr[300] = "";
        sip_hdr(resp, "To", to_hdr, sizeof(to_hdr));

        if (code == 401 || code == 407) {
            send_ack(m, cfg, target_uri, from_tag, to_hdr, cseq, 0, branch);
            if (++auth_tries > 2 || !cfg->password[0]) {
                fprintf(stderr, "INVITE: authentication failed\n");
                return -1;
            }
            challenge(resp, realm, nonce, qop, opaque);
            cseq++;
            gen_branch(branch);
            req_len = build_invite(m, cfg, target_uri, from_tag, branch, cseq,
                                   rtp_port, realm, nonce, qop, opaque, code == 407,
                                   req, sizeof(req));
            sip_send(m->sip_sock, req, req_len, &m->sip_peer);
            vlog(m, "TX", req);
            provisional = 0; rto = 500;
            clock_gettime(CLOCK_MONOTONIC, &next_tx);
            giveup = next_tx;
            ts_add_ms(&next_tx, 500);
            ts_add_ms(&giveup, SIP_TIMEOUT);
            continue;
        }

        if (code < 300) {                   /* 2xx: answered */
            char rip[64]; int rport;
            if (!parse_sdp(sip_body(resp), rip, &rport)) {
                fprintf(stderr, "200 OK has no usable PCMA SDP\n");
                send_ack(m, cfg, target_uri, from_tag, to_hdr, cseq, 1, branch);
                return -1;
            }
            set_remote_rtp(m, rip, rport);
            send_ack(m, cfg, target_uri, from_tag, to_hdr, cseq, 1, branch);

            /* Record dialog state so hangup() can emit an in-dialog BYE. */
            snprintf(m->bye_ruri, sizeof(m->bye_ruri), "%s", target_uri);
            snprintf(m->bye_from, sizeof(m->bye_from),
                     "<sip:%s@%s>;tag=%s", cfg->local_user, cfg->registrar_host, from_tag);
            snprintf(m->bye_to, sizeof(m->bye_to), "%s", to_hdr);
            m->cseq = cseq + 1;
            fprintf(stderr, "Connected to %s (rtp %s:%d)\n", target_uri, rip, rport);
            return 0;
        }

        send_ack(m, cfg, target_uri, from_tag, to_hdr, cseq, 0, branch);
        fprintf(stderr, "call rejected (%d)\n", code);
        return -1;
    }
}

/* ── UAS: answer one inbound call ─────────────────────────────────────── */

/* Send the 100/180/200 answer to an inbound INVITE and record the dialog state,
 * WITHOUT waiting for the ACK. Shared by single-shot uas_answer() (which then
 * runs the ACK-wait loop) and the daemon's sip_uas_accept() (whose ACK is
 * serviced on the parent's main loop). Returns 0, or -1 if the INVITE carries
 * no usable PCMA SDP. */
static int uas_send_answer(const sip_config_t *cfg, sip_media_t *m,
                           const char *msg, struct sockaddr_in *from)
{
    char rip[64]; int rport;
    if (!parse_sdp(sip_body(msg), rip, &rport)) {
        fprintf(stderr, "INVITE has no usable PCMA SDP - rejecting\n");
        return -1;
    }

    char via[512] = "", fr[300] = "", to[300] = "", cseq[64] = "", contact[300] = "";
    sip_hdr(msg, "Via",     via,     sizeof(via));
    sip_hdr(msg, "From",    fr,      sizeof(fr));
    sip_hdr(msg, "To",      to,      sizeof(to));
    sip_hdr(msg, "Call-ID", m->call_id, sizeof(m->call_id));
    sip_hdr(msg, "CSeq",    cseq,    sizeof(cseq));
    sip_hdr(msg, "Contact", contact, sizeof(contact));

    int rtp_port;
    if (rtp_open(m, &rtp_port) < 0) return -1;
    set_remote_rtp(m, rip, rport);
    m->sip_peer = *from;

    char to_tag[18];
    gen_hex(to_tag, 16);
    char to_with_tag[330];
    if (strstr(to, ";tag="))
        snprintf(to_with_tag, sizeof(to_with_tag), "%s", to);
    else
        snprintf(to_with_tag, sizeof(to_with_tag), "%s;tag=%s", to, to_tag);

    /* 100 Trying, 180 Ringing (provisional, no body). */
    char prov[1024];
    for (int i = 0; i < 2; i++) {
        int code = i == 0 ? 100 : 180;
        const char *ph = i == 0 ? "Trying" : "Ringing";
        int n = snprintf(prov, sizeof(prov),
            "SIP/2.0 %d %s\r\n"
            "Via: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
            "Content-Length: 0\r\n\r\n",
            code, ph, via, fr, to_with_tag, m->call_id, cseq);
        if (n >= (int)sizeof(prov)) n = (int)sizeof(prov) - 1;   /* clamp the
            snprintf would-be length before sendto (see sip_dialog_respond) */
        sip_send(m->sip_sock, prov, n, from);
    }

    char sdp[512];
    int sdp_len = build_sdp(m->local_ip, rtp_port, sdp, sizeof(sdp));
    int n = snprintf(m->ok_buf, sizeof(m->ok_buf),
        "SIP/2.0 200 OK\r\n"
        "Via: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %s\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        via, fr, to_with_tag, m->call_id, cseq,
        cfg->local_user, m->local_ip, m->local_sip_port, sdp_len, sdp);
    if (n >= (int)sizeof(m->ok_buf)) n = (int)sizeof(m->ok_buf) - 1;
    m->ok_buf_len = n;
    sip_send(m->sip_sock, m->ok_buf, n, from);
    vlog(m, "TX", m->ok_buf);

    /* Dialog state for an in-dialog BYE we may originate. */
    extract_uri(contact[0] ? contact : fr, m->bye_ruri, sizeof(m->bye_ruri));
    snprintf(m->bye_from, sizeof(m->bye_from), "%s", to_with_tag);
    snprintf(m->bye_to,   sizeof(m->bye_to),   "%s", fr);
    m->cseq = 1;
    return 0;
}

static int uas_answer(const sip_config_t *cfg, sip_media_t *m,
                      const char *msg, struct sockaddr_in *from)
{
    if (uas_send_answer(cfg, m, msg, from) < 0)
        return -1;

    /* Wait for ACK, retransmitting the 200 OK (T1=500ms doubling, give up 32s). */
    struct timespec next_tx, giveup;
    clock_gettime(CLOCK_MONOTONIC, &next_tx);
    giveup = next_tx;
    ts_add_ms(&next_tx, 500);
    ts_add_ms(&giveup, SIP_TIMEOUT);
    long rto = 500;

    for (;;) {
        long wg = ts_until_ms(&giveup);
        if (wg <= 0) { fprintf(stderr, "no ACK for 200 OK (timeout)\n"); return -1; }
        long wt = ts_until_ms(&next_tx);
        long wms = wt < wg ? wt : wg;
        if (wms < 0) wms = 0;

        char buf[8192];
        struct sockaddr_in src;
        int r = sip_recv(m->sip_sock, buf, sizeof(buf), (int)wms, &src);
        if (r < 0) return -1;
        if (r == 0) {
            if (ts_until_ms(&next_tx) <= 0) {
                sip_send(m->sip_sock, m->ok_buf, m->ok_buf_len, from);
                rto *= 2; if (rto > 4000) rto = 4000;
                clock_gettime(CLOCK_MONOTONIC, &next_tx);
                ts_add_ms(&next_tx, rto);
            }
            continue;
        }
        vlog(m, "RX", buf);
        char method[32];
        sip_method(buf, method, sizeof(method));
        if (strcasecmp(method, "ACK") == 0) {
            fprintf(stderr, "Call answered.\n");
            return 0;
        }
        if (strcasecmp(method, "INVITE") == 0)     /* retransmitted INVITE */
            sip_send(m->sip_sock, m->ok_buf, m->ok_buf_len, from);
    }
}

/* ── Public: establish ────────────────────────────────────────────────── */

int sip_media_establish(const sip_config_t *cfg, sip_media_t *m)
{
    memset(m, 0, sizeof(*m));
    m->rtp_sock = -1;
    m->t38_sock = -1;
    m->enable_t38 = cfg->enable_t38;
    snprintf(m->local_user, sizeof(m->local_user), "%s", cfg->local_user);
    m->verbose  = cfg->verbose;
    m->local_sip_port = cfg->local_sip_port;

    if (cfg->dial)
        return uac_establish(cfg, m);

    /* Answer mode: bind the SIP port, optionally register, await an INVITE. */
    m->sip_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons((uint16_t)cfg->local_sip_port);
    if (bind(m->sip_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind SIP");
        return -1;
    }

    /* Determine a local IP for SDP/Contact: route to the registrar if known,
     * else to a public address to learn the default-route interface. */
    const char *probe = cfg->registrar_host[0] ? cfg->registrar_host : "8.8.8.8";
    struct sockaddr_in pa;
    if (resolve_host(probe, &pa) == 0)
        get_local_ip(inet_ntoa(pa.sin_addr), m->local_ip, sizeof(m->local_ip));
    if (!m->local_ip[0])
        strcpy(m->local_ip, "127.0.0.1");

    if (cfg->do_register) {
        struct sockaddr_in reg_addr;
        if (resolve_host(cfg->registrar_host, &reg_addr) < 0 ||
            do_register(m, cfg, &reg_addr) < 0)
            return -1;
    }

    fprintf(stderr, "Waiting for inbound INVITE on port %d ...\n", cfg->local_sip_port);
    for (;;) {
        char buf[8192];
        struct sockaddr_in from;
        int r = sip_recv(m->sip_sock, buf, sizeof(buf), 3600 * 1000, &from);
        if (r <= 0) continue;
        if (sip_response_code(buf) > 0) continue;   /* ignore stray responses */

        char method[32];
        sip_method(buf, method, sizeof(method));
        if (strcasecmp(method, "INVITE") != 0) continue;
        vlog(m, "RX", buf);
        if (uas_answer(cfg, m, buf, &from) == 0) return 0;
        /* INVITE not usable: keep listening for another. */
    }
}

/* ── Public: in-dialog SIP servicing + hangup ─────────────────────────── */

/* The five dialog headers echoed back in a bodyless response. */
struct sip_echo { char via[512], from[300], to[300], cid[256], cseq[64]; };
static void sip_echo_hdrs(const char *req, struct sip_echo *e)
{
    e->via[0] = e->from[0] = e->to[0] = e->cid[0] = e->cseq[0] = '\0';
    sip_hdr(req, "Via", e->via, sizeof e->via);
    sip_hdr(req, "From", e->from, sizeof e->from);
    sip_hdr(req, "To", e->to, sizeof e->to);
    sip_hdr(req, "Call-ID", e->cid, sizeof e->cid);
    sip_hdr(req, "CSeq", e->cseq, sizeof e->cseq);
}

/* Build and send a bodyless response that echoes a request's dialog headers
 * (used to answer an inbound BYE with 200, or decline a re-INVITE with 488).
 * For an in-dialog request the echoed To already carries our dialog tag. */
void sip_dialog_respond(sip_media_t *m, const char *req,
                        const char *status, struct sockaddr_in *to)
{
    struct sip_echo e;
    sip_echo_hdrs(req, &e);
    sip_sendf(m, to,
        "SIP/2.0 %s\r\n"
        "Via: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
        "Content-Length: 0\r\n\r\n",
        status, e.via, e.from, e.to, e.cid, e.cseq);
}

/* Answer an in-dialog re-INVITE with 200 OK carrying our T.38 SDP. */
static void respond_t38_200(sip_media_t *m, const char *req,
                            struct sockaddr_in *to, int t38_port)
{
    struct sip_echo e;
    sip_echo_hdrs(req, &e);
    char sdp[512];
    int sdp_len = build_t38_sdp(m->local_ip, t38_port, sdp, sizeof(sdp));
    sip_sendf(m, to,
        "SIP/2.0 200 OK\r\n"
        "Via: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
        e.via, e.from, e.to, e.cid, e.cseq, m->local_user, m->local_ip, m->local_sip_port,
        sdp_len, sdp);
}

/* Daemon helper: accept an in-dialog T.38 re-INVITE on dialog `dlg`, answering
 * 200 OK with a T.38 SDP on dlg->sip_sock. Opens a UDPTL socket and records the
 * peer + far-end max-datagram in `dlg`. Returns the UDPTL socket fd (>=0) on
 * success — the caller may hand it to another process (SCM_RIGHTS) and then close
 * its own copy — or -1 if `req` is not an acceptable T.38 offer (caller should
 * then answer 488). On success *far_datagram and *local_port are filled. */
int sip_t38_accept(sip_media_t *dlg, const char *req, struct sockaddr_in *from,
                   int *far_datagram, int *local_port)
{
    char rip[64]; int tport = 0, fdg = T38_MAX_DATAGRAM, lp = 0;
    if (getenv("NF_T38_DBG")) {
        const char *b = sip_body(req);
        fprintf(stderr, "---- re-INVITE SDP (daemon) ----\n%s\n--------------------------------\n",
                b ? b : "(none)");
    }
    if (!parse_t38_sdp(sip_body(req), rip, &tport, &fdg)) return -1;
    if (udptl_open(dlg, &lp) < 0) return -1;
    set_t38_peer(dlg, rip, tport);
    dlg->t38_far_datagram = fdg;
    dlg->t38_active = 1;
    respond_t38_200(dlg, req, from, lp);
    if (far_datagram) *far_datagram = fdg;
    if (local_port)   *local_port   = lp;
    return dlg->t38_sock;
}

/* Re-answer a retransmitted T.38 re-INVITE (an earlier 200 was lost). The UDPTL
 * socket already lives in the media child, so we just rebuild the 200 from the
 * remembered local port. */
void sip_t38_reanswer(sip_media_t *dlg, const char *req, struct sockaddr_in *from,
                      int local_port)
{
    respond_t38_200(dlg, req, from, local_port);
}

/* Does this inbound request belong to our established dialog? An in-dialog
 * BYE/re-INVITE is honoured only if its Call-ID matches AND it arrives from
 * the signalling peer captured at establish time (the proxy on the UAC path,
 * the INVITE source on the UAS path). Without this, any host that can reach
 * our SIP port could tear the call down with a bare BYE, or redirect our
 * media to an attacker-chosen address with a re-INVITE carrying a crafted
 * SDP c= line. Tag matching would be stricter, but this stack keeps only the
 * composite From/To header values, so Call-ID + source address is the check. */
static int in_dialog(const sip_media_t *m, const char *req,
                     const struct sockaddr_in *from)
{
    char cid[256] = "";
    sip_hdr(req, "Call-ID", cid, sizeof(cid));
    if (m->call_id[0] == '\0' || strcmp(cid, m->call_id) != 0)
        return 0;
    if (from->sin_addr.s_addr != m->sip_peer.sin_addr.s_addr)
        return 0;
    return 1;
}

int sip_media_poll_sip(sip_media_t *m)
{
    char buf[8192];
    struct sockaddr_in from;
    socklen_t sl = sizeof(from);
    ssize_t r = recvfrom(m->sip_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                         (struct sockaddr *)&from, &sl);
    if (r <= 0) return 0;
    buf[r] = '\0';
    vlog(m, "RX", buf);

    if (sip_response_code(buf) > 0) {
        /* A retransmitted 2xx (our ACK was lost): replay the stored ACK. */
        if (m->ok_buf_len > 0 && strncmp(buf, "SIP/2.0 2", 9) == 0 &&
            strncmp(m->ok_buf, "ACK ", 4) == 0)
            sip_send(m->sip_sock, m->ok_buf, m->ok_buf_len, &m->sip_peer);
        return 0;
    }

    char method[32];
    sip_method(buf, method, sizeof(method));

    /* Only in-dialog requests from our peer may tear down or renegotiate the
     * call; anything else on the port is ignored (no teardown, no media move). */
    if ((strcasecmp(method, "BYE") == 0 || strcasecmp(method, "INVITE") == 0) &&
        !in_dialog(m, buf, &from)) {
        if (m->verbose)
            fprintf(stderr, "ignoring off-dialog %s (Call-ID/source mismatch)\n",
                    method);
        return 0;
    }

    if (strcasecmp(method, "BYE") == 0) {
        sip_dialog_respond(m, buf, "200 OK", &from);
        m->peer_hung_up = 1;
        fprintf(stderr, "Peer hung up (BYE).\n");
        return 1;
    }
    if (strcasecmp(method, "INVITE") == 0) {
        /* In-dialog re-INVITE — typically a T.38 switchover offer. */
        char rip[64]; int tport = 0, fdg = T38_MAX_DATAGRAM;
        if (m->enable_t38 && parse_t38_sdp(sip_body(buf), rip, &tport, &fdg)) {
            /* Accept T.38: open our UDPTL socket (once), answer 200 OK with our
             * T.38 SDP, and switch media to T.38. The fax engine restarts the
             * T.30 handshake over T.38 (the far end re-runs Phase B there). */
            int lp = 0;
            if (m->t38_sock < 0 && udptl_open(m, &lp) < 0) {
                sip_dialog_respond(m, buf, "488 Not Acceptable Here", &from);
                return 0;
            }
            if (lp == 0) {              /* already open: recover our local port */
                struct sockaddr_in a; socklen_t sl = sizeof(a);
                getsockname(m->t38_sock, (struct sockaddr *) &a, &sl);
                lp = ntohs(a.sin_port);
            }
            set_t38_peer(m, rip, tport);
            m->t38_far_datagram = fdg;
            respond_t38_200(m, buf, &from, lp);
            m->t38_active = 1;
            fprintf(stderr, "Accepted T.38 re-INVITE; switching media to T.38.\n");
            if (getenv("NF_T38_DBG")) {
                const char *b = sip_body(buf);
                fprintf(stderr, "---- gateway T.38 SDP ----\n%s\n--------------------------\n",
                        b ? b : "(none)");
            }
        } else {
            /* Not T.38 (or T.38 disabled): decline; per RFC 3261 14.1 the
             * existing session continues unchanged. */
            sip_dialog_respond(m, buf, "488 Not Acceptable Here", &from);
            fprintf(stderr, "Declined re-INVITE (488 Not Acceptable Here); "
                            "staying on G.711 audio.\n");
        }
    }
    return 0;
}

void sip_call_send_bye(sip_media_t *m)
{
    if (m->sip_sock < 0 || !m->bye_ruri[0]) return;
    char branch[20];
    gen_branch(branch);
    sip_sendf(m, &m->sip_peer,
        "BYE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d BYE\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        m->bye_ruri, m->local_ip, m->local_sip_port, branch,
        m->bye_from, m->bye_to, m->call_id, m->cseq);
}

/* Send an in-dialog ACK (new branch) for a 2xx to our re-INVITE. */
static void t38_send_ack(sip_media_t *m, const char *to_hdr, int cseq)
{
    char branch[20];
    gen_branch(branch);
    char ack[1024];
    int n = snprintf(ack, sizeof(ack),
        "ACK %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\n"
        "CSeq: %d ACK\r\nContent-Length: 0\r\n\r\n",
        m->bye_ruri, m->local_ip, m->local_sip_port, branch,
        m->bye_from, to_hdr, m->call_id, cseq);
    if (n >= (int)sizeof(ack)) n = (int)sizeof(ack) - 1;   /* clamp before sendto */
    sip_send(m->sip_sock, ack, n, &m->sip_peer);
}

int sip_offer_t38(sip_media_t *m, const sip_config_t *cfg)
{
    if (m->t38_sock < 0) {
        int p;
        if (udptl_open(m, &p) < 0) return 0;
    }
    int lp;
    { struct sockaddr_in a; socklen_t sl = sizeof(a);
      getsockname(m->t38_sock, (struct sockaddr *) &a, &sl); lp = ntohs(a.sin_port); }

    char realm[256] = "", nonce[256] = "", qop[64] = "", opaque[192] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        int my_cseq = m->cseq++;
        char branch[20]; gen_branch(branch);
        char sdp[512]; int sdp_len = build_t38_sdp(m->local_ip, lp, sdp, sizeof(sdp));
        char auth[900] = "";
        if (attempt == 1 && realm[0] && cfg->password[0])
            build_auth(cfg->local_user, cfg->password, "INVITE", m->bye_ruri,
                       realm, nonce, qop, opaque, 0, auth, sizeof(auth));
        sip_sendf(m, &m->sip_peer,
            "INVITE %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
            "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\n"
            "CSeq: %d INVITE\r\nContact: <sip:%s@%s:%d>\r\n%s"
            "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
            m->bye_ruri, m->local_ip, m->local_sip_port, branch,
            m->bye_from, m->bye_to, m->call_id, my_cseq,
            cfg->local_user, m->local_ip, m->local_sip_port, auth, sdp_len, sdp);

        int retry = 0;
        for (;;) {
            char resp[8192]; struct sockaddr_in src;
            int r = sip_recv(m->sip_sock, resp, sizeof(resp), 6000, &src);
            if (r <= 0) return 0;                       /* no answer */
            vlog(m, "RX", resp);
            int code = sip_response_code(resp);
            if (code == 0) {                            /* a request mid-transaction */
                char meth[32]; sip_method(resp, meth, sizeof(meth));
                if (strcasecmp(meth, "BYE") == 0 && in_dialog(m, resp, &src)) {
                    sip_dialog_respond(m, resp, "200 OK", &src);
                    m->peer_hung_up = 1;
                    return 0;
                }
                continue;                               /* ignore others */
            }
            char to_hdr[300] = ""; sip_hdr(resp, "To", to_hdr, sizeof(to_hdr));
            if (code >= 200 && code < 300) {
                t38_send_ack(m, to_hdr[0] ? to_hdr : m->bye_to, my_cseq);
                char rip[64]; int tport = 0, fdg = T38_MAX_DATAGRAM;
                if (parse_t38_sdp(sip_body(resp), rip, &tport, &fdg)) {
                    set_t38_peer(m, rip, tport);
                    m->t38_far_datagram = fdg;
                    m->t38_active = 1;
                    fprintf(stderr, "T.38 offered and accepted; media is T.38.\n");
                    return 1;
                }
                return 0;                               /* accepted but not T.38 */
            }
            if ((code == 401 || code == 407) && attempt == 0 && cfg->password[0]) {
                t38_send_ack(m, to_hdr[0] ? to_hdr : m->bye_to, my_cseq); /* ACK the 4xx */
                challenge(resp, realm, nonce, qop, opaque);
                retry = 1;
                break;
            }
            /* Other failure (e.g. 488): the far end won't do T.38. */
            t38_send_ack(m, to_hdr[0] ? to_hdr : m->bye_to, my_cseq);
            return 0;
        }
        if (!retry) break;
    }
    return 0;
}

void sip_media_hangup(sip_media_t *m)
{
    if (!m->peer_hung_up && m->sip_sock >= 0 && m->bye_ruri[0]) {
        sip_call_send_bye(m);

        /* Best-effort: wait briefly for the 200 to BYE. */
        char resp[2048];
        struct sockaddr_in src;
        sip_recv(m->sip_sock, resp, sizeof(resp), 1000, &src);
    }
    if (m->rtp_sock >= 0) { close(m->rtp_sock); m->rtp_sock = -1; }
    if (m->t38_sock >= 0) { close(m->t38_sock); m->t38_sock = -1; }
    if (m->sip_sock >= 0) { close(m->sip_sock); m->sip_sock = -1; }
}

/* ── Daemon-mode building blocks (see sip.h) ──────────────────────────── */

int sip_daemon_register(sip_media_t *m, const sip_config_t *cfg)
{
    struct sockaddr_in reg_addr;
    if (resolve_host(cfg->registrar_host, &reg_addr) < 0) {
        fprintf(stderr, "cannot resolve registrar %s\n", cfg->registrar_host);
        return -1;
    }
    return do_register(m, cfg, &reg_addr);
}

int sip_daemon_listen(const sip_config_t *cfg, sip_media_t *m)
{
    memset(m, 0, sizeof(*m));
    m->rtp_sock       = -1;
    m->t38_sock       = -1;
    m->enable_t38     = cfg->enable_t38;
    snprintf(m->local_user, sizeof(m->local_user), "%s", cfg->local_user);
    m->verbose        = cfg->verbose;
    m->local_sip_port = cfg->local_sip_port;

    m->sip_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->sip_sock < 0) { perror("socket SIP"); return -1; }
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons((uint16_t)cfg->local_sip_port);
    if (bind(m->sip_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind SIP");
        close(m->sip_sock); m->sip_sock = -1;
        return -1;
    }

    /* Local IP for SDP/Contact: route to the registrar (else a public probe). */
    const char *probe = cfg->registrar_host[0] ? cfg->registrar_host : "8.8.8.8";
    struct sockaddr_in pa;
    if (resolve_host(probe, &pa) == 0)
        get_local_ip(inet_ntoa(pa.sin_addr), m->local_ip, sizeof(m->local_ip));
    if (!m->local_ip[0])
        strcpy(m->local_ip, "127.0.0.1");

    if (cfg->do_register && sip_daemon_register(m, cfg) < 0)
        return -1;
    return 0;
}

int sip_uas_accept(const sip_config_t *cfg, sip_media_t *listen,
                   const char *invite, struct sockaddr_in *from,
                   sip_media_t *call)
{
    memset(call, 0, sizeof(*call));
    call->rtp_sock       = -1;
    call->t38_sock       = -1;
    call->enable_t38     = cfg->enable_t38;
    call->log_callid     = cfg->daemon_mode;  /* tag concurrent calls' SIP logs */
    call->sip_sock       = listen->sip_sock;  /* parent answers in-dialog here */
    call->verbose        = listen->verbose;
    call->local_sip_port = listen->local_sip_port;
    memcpy(call->local_ip, listen->local_ip, sizeof(call->local_ip));
    snprintf(call->local_user, sizeof(call->local_user), "%s", cfg->local_user);
    return uas_send_answer(cfg, call, invite, from);
}

void sip_call_resend_ok(sip_media_t *call, struct sockaddr_in *to)
{
    if (call->ok_buf_len > 0)
        sip_send(call->sip_sock, call->ok_buf, call->ok_buf_len, to);
}

void sip_uas_decline(sip_media_t *listen, const char *invite,
                     struct sockaddr_in *from, const char *status)
{
    struct sip_echo e;
    sip_echo_hdrs(invite, &e);

    char to_tag[18];
    gen_hex(to_tag, 16);
    char to_t[330];
    if (strstr(e.to, ";tag="))
        snprintf(to_t, sizeof(to_t), "%s", e.to);
    else
        snprintf(to_t, sizeof(to_t), "%s;tag=%s", e.to, to_tag);

    sip_sendf(listen, from,
        "SIP/2.0 %s\r\n"
        "Via: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
        "Content-Length: 0\r\n\r\n",
        status, e.via, e.from, to_t, e.cid, e.cseq);
}
