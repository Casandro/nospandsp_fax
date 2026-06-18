#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "sip_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/* ── Minimal MD5 (RFC 1321, public domain) ───────────────────────── */

typedef unsigned int  u32;
typedef unsigned char u8;

#define ROL32(v,s) (((v)<<(s))|((v)>>(32-(s))))
#define F(x,y,z) (((x)&(y))|(~(x)&(z)))
#define G(x,y,z) (((x)&(z))|((y)&~(z)))
#define H(x,y,z) ((x)^(y)^(z))
#define I(x,y,z) ((y)^((x)|~(z)))
#define STEP(f,a,b,c,d,x,s,t) (a)=((b)+ROL32((a)+f((b),(c),(d))+(x)+(t),(s)))

typedef struct { u32 s[4]; unsigned long long n; u8 buf[64]; int bl; } MD5_CTX;

static void md5_compress(u32 s[4], const u8 *b) {
    u32 a=s[0],c2=s[1],c3=s[2],d=s[3],x[16];
    for(int i=0;i<16;i++)
        x[i]=(u32)b[i*4]|((u32)b[i*4+1]<<8)|((u32)b[i*4+2]<<16)|((u32)b[i*4+3]<<24);
    STEP(F,a,c2,c3,d,x[0], 7,0xd76aa478); STEP(F,d,a,c2,c3,x[1],12,0xe8c7b756);
    STEP(F,c3,d,a,c2,x[2],17,0x242070db); STEP(F,c2,c3,d,a,x[3],22,0xc1bdceee);
    STEP(F,a,c2,c3,d,x[4], 7,0xf57c0faf); STEP(F,d,a,c2,c3,x[5],12,0x4787c62a);
    STEP(F,c3,d,a,c2,x[6],17,0xa8304613); STEP(F,c2,c3,d,a,x[7],22,0xfd469501);
    STEP(F,a,c2,c3,d,x[8], 7,0x698098d8); STEP(F,d,a,c2,c3,x[9],12,0x8b44f7af);
    STEP(F,c3,d,a,c2,x[10],17,0xffff5bb1);STEP(F,c2,c3,d,a,x[11],22,0x895cd7be);
    STEP(F,a,c2,c3,d,x[12],7,0x6b901122); STEP(F,d,a,c2,c3,x[13],12,0xfd987193);
    STEP(F,c3,d,a,c2,x[14],17,0xa679438e);STEP(F,c2,c3,d,a,x[15],22,0x49b40821);
    STEP(G,a,c2,c3,d,x[1], 5,0xf61e2562); STEP(G,d,a,c2,c3,x[6], 9,0xc040b340);
    STEP(G,c3,d,a,c2,x[11],14,0x265e5a51);STEP(G,c2,c3,d,a,x[0],20,0xe9b6c7aa);
    STEP(G,a,c2,c3,d,x[5], 5,0xd62f105d); STEP(G,d,a,c2,c3,x[10],9,0x02441453);
    STEP(G,c3,d,a,c2,x[15],14,0xd8a1e681);STEP(G,c2,c3,d,a,x[4],20,0xe7d3fbc8);
    STEP(G,a,c2,c3,d,x[9], 5,0x21e1cde6); STEP(G,d,a,c2,c3,x[14],9,0xc33707d6);
    STEP(G,c3,d,a,c2,x[3],14,0xf4d50d87); STEP(G,c2,c3,d,a,x[8],20,0x455a14ed);
    STEP(G,a,c2,c3,d,x[13],5,0xa9e3e905); STEP(G,d,a,c2,c3,x[2], 9,0xfcefa3f8);
    STEP(G,c3,d,a,c2,x[7],14,0x676f02d9); STEP(G,c2,c3,d,a,x[12],20,0x8d2a4c8a);
    STEP(H,a,c2,c3,d,x[5], 4,0xfffa3942); STEP(H,d,a,c2,c3,x[8],11,0x8771f681);
    STEP(H,c3,d,a,c2,x[11],16,0x6d9d6122);STEP(H,c2,c3,d,a,x[14],23,0xfde5380c);
    STEP(H,a,c2,c3,d,x[1], 4,0xa4beea44); STEP(H,d,a,c2,c3,x[4],11,0x4bdecfa9);
    STEP(H,c3,d,a,c2,x[7],16,0xf6bb4b60); STEP(H,c2,c3,d,a,x[10],23,0xbebfbc70);
    STEP(H,a,c2,c3,d,x[13],4,0x289b7ec6); STEP(H,d,a,c2,c3,x[0],11,0xeaa127fa);
    STEP(H,c3,d,a,c2,x[3],16,0xd4ef3085); STEP(H,c2,c3,d,a,x[6],23,0x04881d05);
    STEP(H,a,c2,c3,d,x[9], 4,0xd9d4d039); STEP(H,d,a,c2,c3,x[12],11,0xe6db99e5);
    STEP(H,c3,d,a,c2,x[15],16,0x1fa27cf8);STEP(H,c2,c3,d,a,x[2],23,0xc4ac5665);
    STEP(I,a,c2,c3,d,x[0], 6,0xf4292244); STEP(I,d,a,c2,c3,x[7],10,0x432aff97);
    STEP(I,c3,d,a,c2,x[14],15,0xab9423a7);STEP(I,c2,c3,d,a,x[5],21,0xfc93a039);
    STEP(I,a,c2,c3,d,x[12],6,0x655b59c3); STEP(I,d,a,c2,c3,x[3],10,0x8f0ccc92);
    STEP(I,c3,d,a,c2,x[10],15,0xffeff47d);STEP(I,c2,c3,d,a,x[1],21,0x85845dd1);
    STEP(I,a,c2,c3,d,x[8], 6,0x6fa87e4f); STEP(I,d,a,c2,c3,x[15],10,0xfe2ce6e0);
    STEP(I,c3,d,a,c2,x[6],15,0xa3014314); STEP(I,c2,c3,d,a,x[13],21,0x4e0811a1);
    STEP(I,a,c2,c3,d,x[4], 6,0xf7537e82); STEP(I,d,a,c2,c3,x[11],10,0xbd3af235);
    STEP(I,c3,d,a,c2,x[2],15,0x2ad7d2bb); STEP(I,c2,c3,d,a,x[9],21,0xeb86d391);
    s[0]+=a; s[1]+=c2; s[2]+=c3; s[3]+=d;
}

static void md5_init(MD5_CTX *ctx) {
    ctx->s[0]=0x67452301; ctx->s[1]=0xefcdab89;
    ctx->s[2]=0x98badcfe; ctx->s[3]=0x10325476;
    ctx->n=0; ctx->bl=0;
}

static void md5_update(MD5_CTX *ctx, const void *data, int len) {
    const u8 *p = (const u8 *)data;
    ctx->n += (unsigned long long)len * 8;
    while (len--) {
        ctx->buf[ctx->bl++] = *p++;
        if (ctx->bl == 64) { md5_compress(ctx->s, ctx->buf); ctx->bl=0; }
    }
}

static void md5_final(u8 digest[16], MD5_CTX *ctx) {
    ctx->buf[ctx->bl++] = 0x80;
    if (ctx->bl > 56) {
        while (ctx->bl < 64) ctx->buf[ctx->bl++] = 0;
        md5_compress(ctx->s, ctx->buf); ctx->bl=0;
    }
    while (ctx->bl < 56) ctx->buf[ctx->bl++] = 0;
    unsigned long long n = ctx->n;
    for (int i=0;i<8;i++) ctx->buf[56+i]=(u8)(n>>(i*8));
    md5_compress(ctx->s, ctx->buf);
    for (int i=0;i<4;i++) for(int j=0;j<4;j++)
        digest[i*4+j]=(u8)(ctx->s[i]>>(j*8));
}

void md5_hex(const char *s, char out[33]) {
    MD5_CTX ctx; u8 d[16];
    md5_init(&ctx);
    md5_update(&ctx, s, (int)strlen(s));
    md5_final(d, &ctx);
    static const char h[]="0123456789abcdef";
    for(int i=0;i<16;i++){ out[i*2]=h[d[i]>>4]; out[i*2+1]=h[d[i]&0xf]; }
    out[32]='\0';
}

/* ── Random hex tokens ────────────────────────────────────────────── */

unsigned int rng_seed(void) {
    static unsigned int counter = 0;
    return (unsigned int)time(NULL)
         ^ (unsigned int)getpid()
         ^ (unsigned int)(uintptr_t)pthread_self()
         ^ __sync_fetch_and_add(&counter, 1);
}

void gen_hex(char *buf, int n) {
    static const char h[] = "0123456789abcdef";
    unsigned int seed = rng_seed();
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        buf[i] = h[(seed >> 24) & 0xF];
    }
    buf[n] = '\0';
}

/* ── SIP message parsing ──────────────────────────────────────────── */

char *sip_hdr(const char *msg, const char *name, char *dst, int dstlen) {
    int nl = (int)strlen(name);
    const char *p = msg;
    while (*p) {
        if (strncasecmp(p, name, (size_t)nl) == 0 && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            const char *end = p;
            while (*end && *end != '\r' && *end != '\n') end++;
            int len = (int)(end - p);
            if (len >= dstlen) len = dstlen - 1;
            memcpy(dst, p, (size_t)len);
            dst[len] = '\0';
            return dst;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return NULL;
}

void sip_method(const char *msg, char *out, int outlen) {
    const char *e = msg;
    while (*e && *e != ' ' && *e != '\r' && *e != '\n') e++;
    int n = (int)(e - msg);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, msg, (size_t)n);
    out[n] = '\0';
}

const char *sip_body(const char *msg) {
    const char *p = strstr(msg, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

int sip_response_code(const char *msg) {
    if (strncmp(msg, "SIP/2.0 ", 8) != 0) return 0;
    return atoi(msg + 8);
}

void parse_quoted(const char *hdrstr, const char *key, char *out, int outlen) {
    out[0] = '\0';
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *p = strstr(hdrstr, pat);
    if (!p) return;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return;
    int n = (int)(e - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
}

/* ── Monotonic time (CLOCK_MONOTONIC) ─────────────────────────────── */

void ts_add_ms(struct timespec *ts, long ms) {
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

long ts_until_ms(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (deadline->tv_sec  - now.tv_sec)  * 1000L
         + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}
