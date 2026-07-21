/*
 * sip_fax - a fax CLI over a TCP audio pipe or a real SIP/RTP call.
 *
 * The fax engine is our own (nf_t4 codec + nf_t30 protocol + nf_fax driver);
 * spandsp is still linked, but only nf_fax uses it, for the V-series modems,
 * HDLC and tones. Audio is 16-bit signed LE linear PCM, mono, 8 kHz.
 *
 *   sip_fax --send  doc.pam   --listen 5000            (sender, server)
 *   sip_fax --receive out.tiff --connect host:5000     (receiver, client)
 *
 * Transport (--listen / --connect) and fax role (--send / --receive) are
 * orthogonal; any of the four combinations works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <tiffio.h>

#include "nf_t30.h"     /* our own fax engine (no spandsp t4/t30/fax) */
#include "sip.h"
#include "sip_util.h"   /* ts_add_ms / ts_until_ms for the 20 ms media pacing */

#define SAMPLES_PER_BLOCK 160          /* 20 ms at 8 kHz */
#define BYTES_PER_BLOCK   (SAMPLES_PER_BLOCK * 2)
#define FAX_LINE_WIDTH    1728         /* A4 at R8 / 204 dpi */
#define FLUSH_BLOCKS      50           /* ~1 s trailing flush after Phase E (20 ms audio blocks) */
#define T38_FLUSH_TICKS   34           /* ~1 s trailing flush after Phase E (30 ms T.38 ticks)  */

/* ------------------------------------------------------------------ */
/* TCP transport helpers                                              */
/* ------------------------------------------------------------------ */

static int tcp_listen(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t) port);

    if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        perror("listen");
        close(s);
        return -1;
    }

    fprintf(stderr, "listening on port %d ...\n", port);
    int c = accept(s, NULL, NULL);
    if (c < 0) perror("accept");
    close(s);
    return c;
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int s = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) fprintf(stderr, "connect to %s:%s failed\n", host, port);
    return s;
}

/* Returns total bytes (==n) on success, 0 on clean EOF, -1 on error.
 * Does not log: at end-of-call the peer/relay may reset the link, which the
 * run loop treats as a normal "connection ended" event, not a failure. */
static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t) r;
    }
    return (int) got;
}

/* Returns n on success, -1 on error. */
static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        put += (size_t) w;
    }
    return (int) n;
}

/* ------------------------------------------------------------------ */
/* Bilevel PAM (P7) / PBM (P4, P1) -> Group-4 TIFF, strict 1728-wide   */
/* ------------------------------------------------------------------ */

/* Open tiff_path and apply the standard Group-4 fax tags for a 1728-wide,
 * `height`-tall bilevel page. Returns the TIFF*, or NULL on failure. */
static TIFF *fax_tiff_open(const char *tiff_path, int height)
{
    TIFF *tif = TIFFOpen(tiff_path, "w");
    if (!tif) {
        fprintf(stderr, "error: cannot open %s for writing\n", tiff_path);
        return NULL;
    }
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, FAX_LINE_WIDTH);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 1);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_CCITT_T6);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
    TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, 204.0f);
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, 196.0f);
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
    return tif;
}

/* Parse one PAM header value: scans tokens until ENDHDR. The "P7" magic has
 * already been consumed. Fills width/height/depth/maxval; returns 0 on success. */
static int pam_read_header(FILE *f, int *width, int *height,
                           int *depth, int *maxval)
{
    char line[256];
    *width = *height = *depth = *maxval = -1;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (strncmp(line, "ENDHDR", 6) == 0) break;

        char key[32];
        int val;
        if (sscanf(line, "%31s %d", key, &val) == 2) {
            if      (strcmp(key, "WIDTH")  == 0) *width  = val;
            else if (strcmp(key, "HEIGHT") == 0) *height = val;
            else if (strcmp(key, "DEPTH")  == 0) *depth  = val;
            else if (strcmp(key, "MAXVAL") == 0) *maxval = val;
        }
        /* TUPLTYPE and others are tolerated/ignored */
    }

    if (*width < 0 || *height < 0 || *depth < 0 || *maxval < 0) {
        fprintf(stderr, "error: incomplete PAM header\n");
        return -1;
    }
    return 0;
}

/* Reads a strict bilevel PAM body (WIDTH=1728, DEPTH=1, MAXVAL=1: one byte per
 * pixel) and writes it to an already-opened fax TIFF. Returns 0 on success. */
static int pam_body_to_tiff(FILE *f, TIFF *tif, int height)
{
    const int stride = (FAX_LINE_WIDTH + 7) / 8;   /* 216 bytes */
    uint8_t *pam_row  = malloc(FAX_LINE_WIDTH);
    uint8_t *tiff_row = malloc(stride);
    int rc = 0;

    for (int y = 0; y < height; y++) {
        if (fread(pam_row, 1, FAX_LINE_WIDTH, f) != (size_t) FAX_LINE_WIDTH) {
            fprintf(stderr, "error: PAM truncated at row %d\n", y);
            rc = -1;
            break;
        }
        memset(tiff_row, 0, stride);
        /* PAM BLACKANDWHITE: 0=black, 1=white.
         * TIFF MINISWHITE: bit 0=white, bit 1=black -> set bit when sample==0. */
        for (int x = 0; x < FAX_LINE_WIDTH; x++) {
            if (pam_row[x] == 0)
                tiff_row[x >> 3] |= (uint8_t) (0x80 >> (x & 7));
        }
        if (TIFFWriteScanline(tif, tiff_row, y, 0) < 0) {
            fprintf(stderr, "error: TIFFWriteScanline failed at row %d\n", y);
            rc = -1;
            break;
        }
    }

    free(pam_row);
    free(tiff_row);
    return rc;
}

/* Read the next unsigned integer from a netpbm header, skipping whitespace and
 * '#'-to-end-of-line comments. Consumes exactly one whitespace byte after the
 * number - which, after the last header value, is the single separator that
 * precedes a P4 raster. Returns 0 on success, -1 on EOF/garbage. */
static int pnm_read_int(FILE *f, int *out)
{
    int c, val = 0, started = 0;
    for (;;) {
        c = getc(f);
        if (c == EOF) return -1;
        if (c == '#') { while ((c = getc(f)) != EOF && c != '\n') ; continue; }
        if (isspace(c)) { if (started) break; continue; }
        if (c < '0' || c > '9') return -1;
        val = val * 10 + (c - '0');
        started = 1;
    }
    *out = val;
    return 0;
}

/* Reads a PBM body and writes it to an already-opened fax TIFF. `ascii` selects
 * P1 (ASCII '0'/'1' per pixel) vs P4 (packed bits, MSB first). In PBM a 1 means
 * black, which matches TIFF MINISWHITE/MSB2LSB exactly, so a P4 row copies
 * through verbatim. Returns 0 on success. */
static int pbm_body_to_tiff(FILE *f, TIFF *tif, int height, int ascii)
{
    const int stride = (FAX_LINE_WIDTH + 7) / 8;   /* 216 bytes */
    uint8_t *tiff_row = malloc(stride);
    int rc = 0;

    for (int y = 0; y < height; y++) {
        memset(tiff_row, 0, stride);
        if (ascii) {
            for (int x = 0; x < FAX_LINE_WIDTH; x++) {
                int v;
                if (pnm_read_int(f, &v) != 0) {
                    fprintf(stderr, "error: PBM truncated at row %d\n", y);
                    rc = -1; goto out;
                }
                if (v)   /* 1 = black */
                    tiff_row[x >> 3] |= (uint8_t) (0x80 >> (x & 7));
            }
        } else {
            if (fread(tiff_row, 1, stride, f) != (size_t) stride) {
                fprintf(stderr, "error: PBM truncated at row %d\n", y);
                rc = -1; goto out;
            }
        }
        if (TIFFWriteScanline(tif, tiff_row, y, 0) < 0) {
            fprintf(stderr, "error: TIFFWriteScanline failed at row %d\n", y);
            rc = -1; goto out;
        }
    }
out:
    free(tiff_row);
    return rc;
}

/* Reads a bilevel image (PAM P7, or PBM P4/P1) and writes a Group-4 fax TIFF.
 * Strict: the image must be bilevel and exactly FAX_LINE_WIDTH wide. Returns 0
 * on success. */
static int image_to_tiff(const char *img_path, const char *tiff_path)
{
    FILE *f = fopen(img_path, "rb");
    if (!f) { perror(img_path); return -1; }

    int c1 = getc(f), c2 = getc(f);
    if (c1 != 'P') {
        fprintf(stderr, "error: %s is not a PAM/PBM (P7/P4/P1) file\n", img_path);
        fclose(f);
        return -1;
    }

    int width = -1, height = -1;
    int is_pam = 0, pbm_ascii = 0;

    if (c2 == '7') {                        /* PAM */
        int depth, maxval;
        if (pam_read_header(f, &width, &height, &depth, &maxval) != 0) {
            fclose(f); return -1;
        }
        if (depth != 1 || maxval != 1) {
            fprintf(stderr, "error: PAM must be bilevel (got DEPTH=%d MAXVAL=%d)\n",
                    depth, maxval);
            fclose(f); return -1;
        }
        is_pam = 1;
    } else if (c2 == '4' || c2 == '1') {    /* PBM: P4 binary, P1 ASCII */
        pbm_ascii = (c2 == '1');
        if (pnm_read_int(f, &width) != 0 || pnm_read_int(f, &height) != 0) {
            fprintf(stderr, "error: bad PBM header in %s\n", img_path);
            fclose(f); return -1;
        }
    } else {
        fprintf(stderr, "error: unsupported netpbm type P%c in %s\n", c2, img_path);
        fclose(f); return -1;
    }

    if (width != FAX_LINE_WIDTH || height <= 0) {
        fprintf(stderr, "error: image must be %d wide (got WIDTH=%d HEIGHT=%d)\n",
                FAX_LINE_WIDTH, width, height);
        fclose(f); return -1;
    }

    TIFF *tif = fax_tiff_open(tiff_path, height);
    if (!tif) { fclose(f); return -1; }

    int rc = is_pam ? pam_body_to_tiff(f, tif, height)
                    : pbm_body_to_tiff(f, tif, height, pbm_ascii);

    TIFFClose(tif);
    fclose(f);
    return rc;
}

/* Returns 1 if path begins with a (classic) TIFF magic number: "II" + 42 for
 * little-endian, "MM" + 42 for big-endian. */
static int is_tiff_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[4] = { 0 };
    size_t n = fread(b, 1, 4, f);
    fclose(f);
    if (n < 4) return 0;
    if (b[0] == 'I' && b[1] == 'I' && b[2] == 42 && b[3] == 0) return 1;
    if (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 42) return 1;
    return 0;
}

/* Validate that an existing TIFF can be transmitted as-is: it must be bilevel
 * (1 bit/sample, 1 sample/pixel), which is what T.4/T.6 fax carries. Logs the
 * page count. Returns 0 if usable, -1 otherwise. spandsp reads every page (TIFF
 * directory) in turn, so a multi-page TIFF sends as a multi-page fax. */
static int check_tx_tiff(const char *path)
{
    TIFF *t = TIFFOpen(path, "r");
    if (!t) {
        fprintf(stderr, "error: cannot open TIFF %s\n", path);
        return -1;
    }
    uint16_t bps = 1, spp = 1;
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
    int pages = 0;
    do { pages++; } while (TIFFReadDirectory(t));
    TIFFClose(t);

    if (bps != 1 || spp != 1) {
        fprintf(stderr,
                "error: TIFF must be bilevel (BitsPerSample=1, SamplesPerPixel=1; "
                "got %u/%u). Re-encode with Group-4 compression.\n", bps, spp);
        return -1;
    }
    fprintf(stderr, "Transmitting %d-page TIFF: %s\n", pages, path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Resolution alternatives (--send-alt)                               */
/* ------------------------------------------------------------------ */

/* The caller may supply several pre-rendered TIFFs, one per resolution. At call
 * time we read the remote's advertised capabilities and transmit the highest-
 * quality one it can receive. spandsp's T.4 tx does NOT rescale, so the only way
 * to adapt is to pick the right pre-made file. */
typedef enum {
    ALT_STANDARD = 0,   /* R8  x 98  dpi, 1728 wide  (preference: lowest)  */
    ALT_FINE,           /* R8  x 196 dpi, 1728 wide                        */
    ALT_300,            /* 300 x 300 dpi, 2592 wide                        */
    ALT_SUPERFINE,      /* R8  x 391 dpi, 1728 wide                        */
    ALT_400,            /* R16 x 400 dpi, 3456 wide  (preference: highest) */
    ALT_NUM
} alt_res_t;

/* Per-resolution facts. Preference order is the enum order (higher = better).
 * width  : the only A4 page width T.30 accepts at that resolution; an
 *          unambiguous cross-check of a supplied TIFF.
 * ydpi   : expected vertical resolution, for a softer sanity warning.
 * nf_res : the NF_RES_* flag for advertising + the remote-capability query. */
static const struct {
    const char *name;
    int   width;
    int   ydpi;
    int   nf_res;
} ALT_INFO[ALT_NUM] = {
    [ALT_STANDARD]  = { "standard",  1728,  98, NF_RES_STANDARD  },
    [ALT_FINE]      = { "fine",      1728, 196, NF_RES_FINE      },
    [ALT_300]       = { "300",       2592, 300, NF_RES_300       },
    [ALT_SUPERFINE] = { "superfine", 1728, 391, NF_RES_SUPERFINE },
    [ALT_400]       = { "400",       3456, 400, NF_RES_400       },
};

struct tx_alt { alt_res_t res; const char *path; };

struct tx_doc {
    struct tx_alt alt[ALT_NUM];
    int n_alt;
    int initial;    /* index of the highest-preference alternative (initial tx file) */
    int chosen;     /* index selected by the phase-B handler; -1 until then */
    const char *color_path;     /* RGB TIFF for colour JPEG, or NULL          */
    const char *gray_path;      /* TIFF for greyscale JPEG, or NULL           */
    const char *chosen_kind;    /* "colour"/"greyscale"/NULL(=bilevel alt)    */
    int require_color;          /* abort instead of falling back from colour  */
};

static int parse_alt_res(const char *key, alt_res_t *out)
{
    for (int r = 0; r < ALT_NUM; r++)
        if (strcmp(key, ALT_INFO[r].name) == 0) { *out = (alt_res_t) r; return 0; }
    return -1;
}

/* Validate one alternative: must be a bilevel TIFF whose page width matches the
 * declared resolution (hard fail); a mismatched vertical resolution only warns.
 * Logs the page count. Returns 0 if usable. */
static int validate_alt(const char *path, alt_res_t res)
{
    const char *name = ALT_INFO[res].name;
    if (!is_tiff_file(path)) {
        fprintf(stderr, "error: --send-alt %s: %s is not a TIFF\n", name, path);
        return -1;
    }
    TIFF *t = TIFFOpen(path, "r");
    if (!t) { fprintf(stderr, "error: cannot open TIFF %s\n", path); return -1; }

    uint16_t bps = 1, spp = 1, unit = RESUNIT_INCH;
    uint32_t w = 0;
    float yr = 0;
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetFieldDefaulted(t, TIFFTAG_RESOLUTIONUNIT, &unit);
    TIFFGetField(t, TIFFTAG_YRESOLUTION, &yr);
    int pages = 0;
    do { pages++; } while (TIFFReadDirectory(t));
    TIFFClose(t);

    if (bps != 1 || spp != 1) {
        fprintf(stderr, "error: --send-alt %s (%s) must be bilevel (got %u/%u)\n",
                name, path, bps, spp);
        return -1;
    }
    if ((int) w != ALT_INFO[res].width) {
        fprintf(stderr, "error: --send-alt %s (%s): width %u does not match the "
                "expected %d for that resolution\n", name, path, w, ALT_INFO[res].width);
        return -1;
    }
    double ydpi = (unit == RESUNIT_CENTIMETER) ? yr * 2.54 : yr;
    double diff = ydpi - ALT_INFO[res].ydpi;
    if (diff < 0) diff = -diff;
    if (yr > 0 && diff > ALT_INFO[res].ydpi * 0.15)
        fprintf(stderr, "warning: --send-alt %s (%s): YRESOLUTION ~%.0f dpi, "
                "expected ~%d dpi\n", name, path, ydpi, ALT_INFO[res].ydpi);

    fprintf(stderr, "alt %s: %d-page %ux fax: %s\n", name, pages, w, path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fax engine                                                         */
/* ------------------------------------------------------------------ */

struct call_state {
    int done;
    int result;
    int verbose;
    nf_t30_t *eng;          /* the engine, for stats + alternative selection */
    struct tx_doc *doc;     /* non-NULL only when sending resolution alternatives */
    const char *tag;        /* optional per-call log prefix (Call-ID); NULL = none */
};

static void phase_e_handler(void *user_data, int result)
{
    struct call_state *cs = user_data;
    nf_t30_stats_t stats;
    nf_t30_get_stats(cs->eng, &stats);

    cs->result = result;
    cs->done = 1;

    if (cs->tag) fprintf(stderr, "[%s] ", cs->tag);
    fprintf(stderr, "Phase E: %s\n", nf_t30_completion_to_str(result));
    if (result == NF_T30_OK) {
        if (cs->tag) fprintf(stderr, "[%s] ", cs->tag);
        fprintf(stderr, "  pages tx=%d rx=%d, %dx%d, %d bps (%s)\n",
                stats.pages_tx, stats.pages_rx,
                stats.width, stats.length, stats.bit_rate,
                stats.v34 ? "Super G3 / V.34" : "G3");
        if (stats.rx_ident[0]) {
            if (cs->tag) fprintf(stderr, "[%s] ", cs->tag);
            fprintf(stderr, "  remote station id: %s\n", stats.rx_ident);
        }
        if (cs->doc && cs->doc->chosen_kind)
            fprintf(stderr, "  sent the %s version\n", cs->doc->chosen_kind);
        else if (cs->doc && cs->doc->chosen >= 0)
            fprintf(stderr, "  sent the %s alternative\n",
                    ALT_INFO[cs->doc->alt[cs->doc->chosen].res].name);
    }
}

/* Phase B fires once the remote's DIS has been received but before the tx file
 * is opened and DCS is built, so swapping the tx file here changes the page
 * actually sent. Pick the highest-preference alternative the remote can receive. */
static void phase_b_handler(void *user_data)
{
    struct call_state *cs = user_data;
    struct tx_doc *doc = cs->doc;
    if (!doc) return;

    /* Kind first: colour beats greyscale beats bilevel. The colour/grey flags
     * only become effective in the engine's kind decision, which runs after
     * this callback returns. */
    if (doc->color_path && nf_t30_remote_supports_color(cs->eng)) {
        nf_t30_set_color(cs->eng, 1);
        nf_t30_set_gray(cs->eng, 0);
        nf_t30_select_tx_file(cs->eng, doc->color_path);
        doc->chosen_kind = "colour (T.42 JPEG)";
        if (cs->verbose)
            fprintf(stderr, "phase B: selected colour (%s)\n", doc->color_path);
        return;
    }
    if (doc->require_color) {
        /* Colour was demanded but the remote cannot take it: keep the colour
         * kind armed so the engine ends the call as incompatible (DCN)
         * instead of falling back to greyscale/bilevel. */
        nf_t30_set_color(cs->eng, 1);
        nf_t30_set_gray(cs->eng, 0);
        nf_t30_select_tx_file(cs->eng, doc->color_path);
        fprintf(stderr, "phase B: remote cannot receive colour - "
                "aborting (--require-color)\n");
        return;
    }
    if (doc->gray_path && nf_t30_remote_supports_gray(cs->eng)) {
        nf_t30_set_color(cs->eng, 0);
        nf_t30_set_gray(cs->eng, 1);
        nf_t30_select_tx_file(cs->eng, doc->gray_path);
        doc->chosen_kind = "greyscale (T.81 JPEG)";
        if (cs->verbose)
            fprintf(stderr, "phase B: selected greyscale (%s)\n", doc->gray_path);
        return;
    }
    if (doc->n_alt == 0) {
        /* JPEG kinds only and the remote supports neither: arm the highest
         * offered kind so the engine ends the call as incompatible (DCN),
         * matching the single-kind --send-color behaviour. */
        if (doc->color_path) nf_t30_set_color(cs->eng, 1);
        else                 nf_t30_set_gray(cs->eng, 1);
        return;
    }
    nf_t30_set_color(cs->eng, 0);
    nf_t30_set_gray(cs->eng, 0);

    int best = -1;
    for (int r = ALT_NUM - 1; r >= 0 && best < 0; r--) {
        for (int i = 0; i < doc->n_alt; i++) {
            if (doc->alt[i].res != (alt_res_t) r) continue;
            if (nf_t30_remote_supports(cs->eng, ALT_INFO[r].nf_res)) { best = i; break; }
        }
    }
    if (best < 0) {                     /* remote supports none we offer: use lowest */
        best = 0;
        for (int i = 1; i < doc->n_alt; i++)
            if (doc->alt[i].res < doc->alt[best].res) best = i;
    }

    doc->chosen = best;
    nf_t30_select_tx_file(cs->eng, doc->alt[best].path);
    if (cs->verbose)
        fprintf(stderr, "phase B: selected %s (%s)\n",
                ALT_INFO[doc->alt[best].res].name, doc->alt[best].path);
}

/* Build and configure a fax engine for one call. Phase E completion lands in
 * *cs. Returns the engine, or NULL on failure. Shared by the TCP and SIP run
 * loops, which differ only in how they move the PCM. */
static int g_use_ecm = 1;   /* T.30 Error Correction Mode; disable with --no-ecm */
static int g_send_color = 0;     /* tx document is a colour image (T.42/JPEG)   */
static int g_send_gray  = 0;     /* tx document is a greyscale JPEG image       */
static int g_send_file  = 0;     /* tx document is an arbitrary binary file     */
static int g_recv_file  = 0;     /* receiving an arbitrary binary file          */
static int g_color_quality = 85; /* JPEG quality for colour tx                  */
static int g_calling    = 0;     /* T.30 calling party (CNG); else answer (CED) */
static int g_poll_serve = 0;     /* answer side: transmit our doc when polled   */
static int g_poll_recv  = 0;     /* call side: poll for a doc, then receive     */
static int g_v34        = 1;     /* offer V.34 (Super G3) in V.8; --no-v34 opts out.
                                  * Default on, matching a real SG3 machine: V.8
                                  * settles on the best common modulation, so a
                                  * peer without V.34 (or without V.8 at all)
                                  * falls back cleanly to classic G3.           */
static int g_require_v34 = 0;    /* --require-v34: abort (DCN, exit 1) instead of
                                  * falling back to classic G3 when V.8 fails to
                                  * negotiate V.34. Aborts before any page tx.   */
static int g_auto_redial = 1;    /* when a dialed V.34 call fails, redial once
                                  * with V.34 suppressed (classic G3): some SG3
                                  * machines advertise capabilities (e.g. JPEG)
                                  * their V.34 stack cannot actually receive.
                                  * --no-redial opts out.                       */
static int g_last_v34_failed = 0;/* the call just run negotiated V.34 and did
                                  * not complete cleanly (set by run_fax_sip)   */

/* ------------------------------------------------------------------ */
/* --debug: one switch that captures everything needed to analyse a    */
/* failed call offline (SIP + T.30 + modem trace, inbound audio, and a */
/* copy of the whole trace to <dir>/session.log).                      */
/* ------------------------------------------------------------------ */
static char g_debug_dir[512];   /* set non-empty when --debug is active */
static FILE *g_t38dump = NULL;  /* --debug: timestamped hex of every UDPTL datagram */

/* Append one datagram to the T.38 dump: "HH:MM:SS.mmm R/T Nb: <hex>". */
static void t38_dump(char dir, const uint8_t *d, int len)
{
    if (!g_t38dump) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    fprintf(g_t38dump, "%02d:%02d:%02d.%03ld %c %3dB:",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, dir, len);
    for (int i = 0; i < len && i < 256; i++) fprintf(g_t38dump, " %02x", d[i]);
    fputc('\n', g_t38dump);
    fflush(g_t38dump);
}

/* Tee stderr to <dir>/session.log while still echoing to the console. A small
 * forked relay copies the pipe to both sinks; no pthread dependency, and it
 * survives the daemon's own forks (children inherit the redirected fd 2). */
static void debug_tee_stderr(const char *logpath)
{
    int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("debug: open session.log"); return; }

    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("debug: pipe"); close(fd); return; }

    int console = dup(STDERR_FILENO);   /* the real terminal/stderr */
    if (console < 0) { close(fd); close(pipefd[0]); close(pipefd[1]); return; }

    pid_t pid = fork();
    if (pid < 0) { perror("debug: fork"); close(fd); close(pipefd[0]); close(pipefd[1]); close(console); return; }

    if (pid == 0) {
        /* relay: read the pipe, write each chunk to console + logfile */
        close(pipefd[1]);
        signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof buf)) > 0) {
            ssize_t off = 0;
            while (off < n) { ssize_t w = write(console, buf + off, (size_t)(n - off)); if (w <= 0) break; off += w; }
            off = 0;
            while (off < n) { ssize_t w = write(fd, buf + off, (size_t)(n - off)); if (w <= 0) break; off += w; }
        }
        _exit(0);
    }

    /* parent: stderr now flows into the relay */
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]); close(pipefd[1]); close(fd); close(console);
    setvbuf(stderr, NULL, _IOLBF, 0);   /* line-buffered so the log stays live */
}

/* Turn on every diagnostic knob under one flag. `dir` may be NULL (auto name).
 * Returns the chosen directory (points into g_debug_dir). */
static const char *setup_debug(const char *dir)
{
    if (dir && dir[0]) {
        snprintf(g_debug_dir, sizeof g_debug_dir, "%s", dir);
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(g_debug_dir, sizeof g_debug_dir,
                 "faxdbg-%04d%02d%02d-%02d%02d%02d-%d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (int) getpid());
    }
    if (mkdir(g_debug_dir, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "debug: mkdir %s: %s\n", g_debug_dir, strerror(errno));

    /* Lower-layer traces (T.30 frame dispatch, V.34 session, T.38 IFP). */
    setenv("NFFAXDBG", "1", 0);
    setenv("NFV34DBG", "1", 0);
    setenv("NF_T38_DBG", "1", 0);
    /* Our own logging: full SIP messages + wall-clock timestamps. */
    setenv("NF_SIP_FULL", "1", 0);
    setenv("NF_LOG_TS", "1", 0);
    /* Capture inbound decoded PCM for offline replay (--replay-rx), unless the
     * caller already pointed it somewhere. 16-bit LE, 8 kHz, mono. */
    if (!getenv("NF_RX_AUDIO_DUMP")) {
        static char pcm[600];
        snprintf(pcm, sizeof pcm, "%s/rx.pcm", g_debug_dir);
        setenv("NF_RX_AUDIO_DUMP", pcm, 1);
    }
    if (!getenv("NF_TX_AUDIO_DUMP")) {
        static char pcm[600];
        snprintf(pcm, sizeof pcm, "%s/tx.pcm", g_debug_dir);
        setenv("NF_TX_AUDIO_DUMP", pcm, 1);
    }

    char logpath[600];
    snprintf(logpath, sizeof logpath, "%s/session.log", g_debug_dir);
    debug_tee_stderr(logpath);

    fprintf(stderr,
        "=== sip_fax debug mode ===\n"
        "  artifacts   : %s/\n"
        "    session.log   full SIP + T.30 + modem trace (this output)\n"
        "    rx.pcm        inbound audio, replay with:\n"
        "                    sip_fax --replay-rx %s/rx.pcm --receive out.tiff --verbose\n"
        "==========================\n",
        g_debug_dir, g_debug_dir);
    return g_debug_dir;
}

/* Append a one-shot post-mortem to <dir>/report.txt (and echo it): the
 * negotiated parameters and outcome, so a failure can be triaged at a glance. */
static void debug_write_report(nf_t30_t *fax, int sending, int rc)
{
    if (!g_debug_dir[0] || !fax) return;
    nf_t30_stats_t st;
    nf_t30_get_stats(fax, &st);

    char path[600];
    snprintf(path, sizeof path, "%s/report.txt", g_debug_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;

    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(f,
        "sip_fax debug report\n"
        "  time            : %s\n"
        "  role            : %s\n"
        "  outcome         : %s (rc=%d)\n"
        "  remote id       : %s\n"
        "  modem           : %s\n"
        "  bit rate        : %d bps\n"
        "  ECM             : %s\n"
        "  pages sent      : %d\n"
        "  pages received  : %d\n",
        when,
        sending ? "sender" : "receiver",
        rc == 0 ? "OK" : "FAILED", rc,
        st.rx_ident[0] ? st.rx_ident : "(none)",
        nf_t30_modem_name(fax),
        st.bit_rate,
        st.ecm ? "yes" : "no",
        st.pages_tx, st.pages_rx);
    fclose(f);

    fprintf(stderr, "debug: wrote %s\n", path);
}

static nf_t30_t *fax_open(int sending, const char *file, const char *ident,
                          int verbose, struct call_state *cs)
{
    /* The T.30 calling party is normally the image sender, but for polling the
     * caller receives (poll) and the answerer sends (poll-serve). */
    nf_t30_t *eng = nf_t30_init(g_calling);
    if (!eng) {
        fprintf(stderr, "error: nf_t30_init failed\n");
        return NULL;
    }
    cs->eng = eng;
    if (g_poll_serve) nf_t30_set_poll_serve(eng, 1);
    if (g_poll_recv)  nf_t30_set_poll_receive(eng, 1);
    nf_t30_set_ecm(eng, g_use_ecm);
    /* V.34 (T.30 Annex F) half-duplex fixes the primary-channel image direction
     * to caller->answerer. Polling reverses the document direction (the answerer
     * is the image source), which the V.34 session engine does not yet support,
     * so polling calls always use classic G3 (still negotiated via V.8). */
    nf_t30_set_v34(eng, g_v34 && !g_poll_serve && !g_poll_recv);
    nf_t30_set_require_v34(eng, g_require_v34);
    nf_t30_set_tx_ident(eng, ident);
    nf_t30_set_phase_e_handler(eng, phase_e_handler, cs);
    if (verbose) nf_t30_set_verbose(eng, 1);

    if (g_send_color) {
        /* Colour image (T.42/JPEG over ECM). */
        nf_t30_set_color(eng, 1);
        nf_t30_set_color_quality(eng, g_color_quality);
        nf_t30_set_supported_resolutions(eng, NF_RES_STANDARD | NF_RES_FINE);
        nf_t30_set_tx_file(eng, file);
    } else if (g_send_gray) {
        /* Greyscale image as a T.81 JPEG fax over ECM. */
        nf_t30_set_gray(eng, 1);
        nf_t30_set_color_quality(eng, g_color_quality);
        nf_t30_set_supported_resolutions(eng, NF_RES_STANDARD | NF_RES_FINE);
        nf_t30_set_tx_file(eng, file);
    } else if (g_send_file) {
        /* Arbitrary binary file (private nf<->nf profile over ECM). */
        nf_t30_set_file_tx(eng, file);
    } else if (g_recv_file) {
        /* Receiving an arbitrary binary file. */
        nf_t30_set_file_rx(eng, file);
    } else if (cs->doc) {
        /* Sending alternatives (bilevel resolutions and/or JPEG kinds):
         * advertise every resolution we hold a file for (else DCS
         * construction would reject our own file), and let phase B pick the
         * kind and resolution per the remote's DIS. The default tx file is
         * the highest bilevel one (phase B always re-selects). */
        int mask = NF_RES_STANDARD;
        for (int i = 0; i < cs->doc->n_alt; i++)
            mask |= ALT_INFO[cs->doc->alt[i].res].nf_res;
        if (cs->doc->color_path || cs->doc->gray_path) {
            mask |= NF_RES_FINE;            /* the JPEG kinds' anchor */
            nf_t30_set_color_quality(eng, g_color_quality);
        }
        nf_t30_set_supported_resolutions(eng, mask);
        nf_t30_set_phase_b_handler(eng, phase_b_handler, cs);
        nf_t30_set_tx_file(eng, file);
    } else if (sending) {
        nf_t30_set_supported_resolutions(eng,
            NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400);
        nf_t30_set_tx_file(eng, file);
    } else {
        /* Receiving an image: advertise all bilevel resolutions so a high-res
         * sender can negotiate up, and advertise colour so a colour sender can
         * negotiate it (a colour page is written as an RGB TIFF). */
        nf_t30_set_supported_resolutions(eng,
            NF_RES_STANDARD | NF_RES_FINE | NF_RES_SUPERFINE | NF_RES_300 | NF_RES_400);
        nf_t30_set_color_capable(eng, 1);
        nf_t30_set_rx_file(eng, file);
    }

    nf_t30_set_transmit_on_idle(eng, 1);
    return eng;
}

/* Map a finished call's state to a process exit code. */
static int fax_result(const struct call_state *cs, int link_lost)
{
    if (!cs->done) {
        fprintf(stderr, "error: call did not complete (%s)\n",
                link_lost ? "link lost before Phase E" : "no Phase E");
        return 1;
    }
    return (cs->result == NF_T30_OK) ? 0 : 1;
}

/* Transmit progress meter. Call ~1x/second from a send loop while transmitting.
 * On a TTY (and not verbose, to avoid clobbering log lines) it redraws one line
 * in place with a carriage return; otherwise it emits a periodic plain line.
 * `next` is a 1 Hz throttle deadline the caller owns; `mode` is computed once
 * (-1 = uninit, 0 = plain line, 1 = in-place). Pass done=1 at the end to close
 * the in-place line with a newline. */
static void tx_progress(nf_t30_t *fax, struct timespec *next, int *mode,
                        int verbose, int done)
{
    if (*mode < 0)
        *mode = (!verbose && isatty(fileno(stderr))) ? 1 : 0;

    int page = 0, pages = 0, rate = 0;
    size_t sent = 0, total = 0;
    int active = nf_t30_tx_progress(fax, &page, &pages, &sent, &total, &rate);

    if (done) {
        if (*mode == 1) fputc('\n', stderr);    /* terminate the in-place line */
        return;
    }
    if (!active || total == 0) return;
    if (ts_until_ms(next) > 0) return;          /* throttle to ~1 Hz */
    ts_add_ms(next, 1000);

    double pct = 100.0 * (double) sent / (double) total;
    long eta = (rate > 0) ? (long) (((double) (total - sent) * 8.0) / rate + 0.5) : -1;
    char etabuf[24];
    if (eta >= 0) snprintf(etabuf, sizeof etabuf, "%ld:%02ld", eta / 60, eta % 60);
    else          snprintf(etabuf, sizeof etabuf, "?");

    const char *modem = nf_t30_modem_name(fax);
    if (*mode == 1)
        fprintf(stderr, "\r[fax] page %d/%d  %3.0f%%  %zu/%zu B  %s %dbps  ETA %s    ",
                page + 1, pages, pct, sent, total, modem, rate, etabuf);
    else
        fprintf(stderr, "[fax] page %d/%d %.0f%% (%zu/%zu B) %s %dbps ETA %s\n",
                page + 1, pages, pct, sent, total, modem, rate, etabuf);
    fflush(stderr);
}

/* Pull one 20 ms tx block from the fax engine, zero-padding a short final
 * block so the caller always transmits a full frame. Returns the real count. */
static int fax_tx_block(nf_t30_t *fax, int16_t *out)
{
    int n = nf_t30_tx(fax, out, SAMPLES_PER_BLOCK);
    if (n < SAMPLES_PER_BLOCK)
        memset(out + n, 0, (size_t) (SAMPLES_PER_BLOCK - n) * 2);
    return n;
}

/* Block until `deadline` for either fd to become readable. Returns select()'s
 * result (>0 with the ready set in *rfds); <=0 means the tick is due or nothing
 * arrived, and the caller's inner service loop should break to run the tick.
 * This is the shared inner-loop plumbing of every select-based media pump. */
static int media_wait(int fd_a, int fd_b, const struct timespec *deadline, fd_set *rfds)
{
    long w = ts_until_ms(deadline);
    if (w <= 0) return 0;
    FD_ZERO(rfds);
    FD_SET(fd_a, rfds);
    FD_SET(fd_b, rfds);
    int maxfd = fd_a > fd_b ? fd_a : fd_b;
    struct timeval tv = { w / 1000, (w % 1000) * 1000 };
    return select(maxfd + 1, rfds, NULL, NULL, &tv);
}

static int run_fax(int fd, int sending, const char *file, const char *ident,
                   int verbose, struct tx_doc *doc)
{
    struct call_state cs = { 0, NF_T30_OK, verbose, NULL, doc, NULL };

    nf_t30_t *fax = fax_open(sending, file, ident, verbose, &cs);
    if (!fax) return -1;

    int16_t out[SAMPLES_PER_BLOCK], in[SAMPLES_PER_BLOCK];
    int flush = 0;
    int eof = 0;
    struct timespec next_prog; clock_gettime(CLOCK_MONOTONIC, &next_prog);
    int pmode = -1;

    for (;;) {
        if (sending) tx_progress(fax, &next_prog, &pmode, verbose, 0);
        fax_tx_block(fax, out);

        /* Any socket failure (clean EOF, or a reset at end-of-call) just
         * means the link is gone; switch to finalizing locally. After the
         * final DCN, T.30 reaches Phase E on its own, fed only silence -
         * it needs no further bytes from the peer. */
        if (!eof && write_full(fd, out, BYTES_PER_BLOCK) < 0)
            eof = 1;

        if (!eof) {
            int r = read_full(fd, in, BYTES_PER_BLOCK);
            if (r <= 0) eof = 1;
            else nf_t30_rx(fax, in, r / 2);
        }
        if (eof) {
            memset(in, 0, sizeof(in));
            nf_t30_rx(fax, in, SAMPLES_PER_BLOCK);
        }

        /* Once either side has finished (local completion or link gone),
         * run a bounded number of extra cycles, then stop. */
        if ((cs.done || eof) && ++flush > FLUSH_BLOCKS) break;
    }

    if (sending) tx_progress(fax, &next_prog, &pmode, verbose, 1);
    int rc = fax_result(&cs, eof);
    debug_write_report(fax, sending, rc);
    nf_t30_free(fax);

    return rc;
}

/* Offline debug: replay a captured inbound PCM stream (16-bit signed LE, 8 kHz
 * mono — as written by the daemon's NF_RX_AUDIO_DUMP) through the receiver,
 * configured exactly as a live receive. Reproduces real-call reception failures
 * deterministically and at full speed (T.30 timeouts are sample-driven), with
 * no live call needed. Pair with --receive <tiff>. */
static int run_fax_replay(const char *pcm_path, const char *tiff_path, int verbose)
{
    FILE *f = fopen(pcm_path, "rb");
    if (!f) { perror("open replay pcm"); return 1; }

    struct call_state cs = { 0, NF_T30_OK, verbose, NULL, NULL, NULL };
    nf_t30_t *fax = fax_open(0 /* receive */, tiff_path, "sip_fax", verbose, &cs);
    if (!fax) { fclose(f); return 1; }

    int16_t out[SAMPLES_PER_BLOCK], in[SAMPLES_PER_BLOCK];
    int flush = 0, eof = 0;
    long blocks = 0;

    for (;;) {
        nf_t30_tx(fax, out, SAMPLES_PER_BLOCK);          /* tx discarded */
        size_t got = eof ? 0 : fread(in, sizeof(int16_t), SAMPLES_PER_BLOCK, f);
        if (got < (size_t) SAMPLES_PER_BLOCK) {
            memset(in + got, 0, (SAMPLES_PER_BLOCK - got) * sizeof(int16_t));
            if (got == 0) eof = 1;                       /* feed trailing silence */
        }
        nf_t30_rx(fax, in, SAMPLES_PER_BLOCK);
        if ((cs.done || eof) && ++flush > FLUSH_BLOCKS) break;
        if (++blocks > 90000) break;                     /* safety cap (~30 min) */
    }

    fclose(f);
    nf_t30_free(fax);
    return fax_result(&cs, eof);
}

/* ------------------------------------------------------------------ */
/* SIP run loop: drive the fax engine over a live RTP media leg.      */
/* ------------------------------------------------------------------ */

/* Unlike the TCP loop (which is paced by the lock-step peer), an RTP leg must
 * be paced against the wall clock: one 20 ms frame from fax_tx() per tick,
 * with received RTP fed into fax_rx() as it arrives. */
static int run_fax_sip(sip_media_t *m, int sending, const char *file,
                       const char *ident, int verbose, struct tx_doc *doc)
{
    struct call_state cs = { 0, NF_T30_OK, verbose, NULL, doc, NULL };

    nf_t30_t *fax = fax_open(sending, file, ident, verbose, &cs);
    if (!fax) return -1;

    int16_t out[SAMPLES_PER_BLOCK], in[SAMPLES_PER_BLOCK];
    int flush = 0;
    int ended = 0;
    int pmode = -1;
    /* DEBUG (diagnostic only): dump inbound decoded PCM (16-bit LE 8 kHz mono)
     * for offline signal analysis. Gated by NF_RX_AUDIO_DUMP=<path>. */
    FILE *adump = getenv("NF_RX_AUDIO_DUMP") ? fopen(getenv("NF_RX_AUDIO_DUMP"), "wb") : NULL;
    /* Symmetric TX dump for offline analysis (e.g. verifying our V.8 CM/CI
     * timing against the received ANSam). Gated by NF_TX_AUDIO_DUMP=<path>. */
    FILE *tdump = getenv("NF_TX_AUDIO_DUMP") ? fopen(getenv("NF_TX_AUDIO_DUMP"), "wb") : NULL;

    struct timespec next_tick, next_prog;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    next_prog = next_tick;
    ts_add_ms(&next_tick, 20);

    for (;;) {
        if (sip_stop_requested()) break;   /* SIGTERM/SIGINT: unwind so main sends BYE */
        if (sending) tx_progress(fax, &next_prog, &pmode, verbose, 0);
        /* Until the next 20 ms tick, service whichever socket is ready:
         * decode inbound RTP into the fax receiver, answer in-dialog SIP. */
        fd_set rfds;
        while (media_wait(m->rtp_sock, m->sip_sock, &next_tick, &rfds) > 0) {
            if (FD_ISSET(m->rtp_sock, &rfds)) {
                int n = sip_media_rx(m, in, SAMPLES_PER_BLOCK);
                if (n > 0) {
                    if (adump) fwrite(in, sizeof(int16_t), (size_t) n, adump);
                    nf_t30_rx(fax, in, n);
                }
            }
            if (FD_ISSET(m->sip_sock, &rfds)) {
                if (sip_media_poll_sip(m)) ended = 1;
                /* The peer re-INVITEd us to T.38 and we accepted: abandon the
                 * audio engine; the caller restarts the call over T.38. */
                if (sip_media_is_t38(m)) { if (adump) fclose(adump); if (tdump) fclose(tdump); nf_t30_free(fax); return -2; }
            }
        }

        /* Tick: pull one frame from the fax transmitter and send it as RTP. */
        ts_add_ms(&next_tick, 20);
        fax_tx_block(fax, out);
        if (tdump) fwrite(out, sizeof(int16_t), SAMPLES_PER_BLOCK, tdump);
        sip_media_tx(m, out, SAMPLES_PER_BLOCK);

        /* After local completion (Phase E) or a peer hang-up, run a bounded
         * trailing flush so the final T.30 frames make it onto the wire. */
        if ((cs.done || ended) && ++flush > FLUSH_BLOCKS) break;
    }

    if (sending) tx_progress(fax, &next_prog, &pmode, verbose, 1);
    if (adump) fclose(adump);
    if (tdump) fclose(tdump);
    {   /* Remember a failed V.34 attempt so the dialer can redial classic G3. */
        nf_t30_stats_t st;
        nf_t30_get_stats(fax, &st);
        g_last_v34_failed = st.v34 && (!cs.done || cs.result != NF_T30_OK);
    }
    int rc = fax_result(&cs, ended);
    debug_write_report(fax, sending, rc);
    nf_t30_free(fax);

    return rc;
}

/* ------------------------------------------------------------------ */
/* T.38 run loop: drive the fax engine over a T.38/UDPTL media leg.    */
/* ------------------------------------------------------------------ */

static void t38_send_cb(void *user, const uint8_t *dgram, int len)
{
    t38_dump('T', dgram, len);
    sip_t38_tx((sip_media_t *) user, dgram, len);
}

static int run_fax_t38(sip_media_t *m, int sending, const char *file,
                       const char *ident, int verbose, struct tx_doc *doc)
{
    struct call_state cs = { 0, NF_T30_OK, verbose, NULL, doc, NULL };
    nf_t30_t *fax = fax_open(sending, file, ident, verbose, &cs);
    if (!fax) return -1;
    /* Run the T.30 engine over T.38; the protocol restarts on this backend. */
    nf_t30_t38_enable(fax, 2 /*redundancy*/,
                      m->t38_far_datagram > 0 ? m->t38_far_datagram : 300,
                      t38_send_cb, m);
    if (g_debug_dir[0]) {
        char p[600]; snprintf(p, sizeof p, "%s/t38.log", g_debug_dir);
        g_t38dump = fopen(p, "w");
    }

    int flush = 0, ended = 0, pmode = -1;
    struct timespec next_tick, next_prog;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    next_prog = next_tick;
    ts_add_ms(&next_tick, 30);

    for (;;) {
        if (sip_stop_requested()) break;   /* SIGTERM/SIGINT: unwind so main sends BYE */
        if (sending) tx_progress(fax, &next_prog, &pmode, verbose, 0);
        /* Until the next 30 ms tick, drain inbound UDPTL and service SIP. */
        fd_set rfds;
        while (media_wait(m->t38_sock, m->sip_sock, &next_tick, &rfds) > 0) {
            if (FD_ISSET(m->t38_sock, &rfds)) {
                uint8_t dg[2048];
                int n = sip_t38_rx(m, dg, sizeof dg);
                if (n > 0) { t38_dump('R', dg, n); nf_t30_t38_rx_datagram(fax, dg, n); }
            }
            if (FD_ISSET(m->sip_sock, &rfds)) {
                if (sip_media_poll_sip(m)) ended = 1;
            }
        }
        /* Tick: advance the T.38 tx pump and the protocol timer. */
        ts_add_ms(&next_tick, 30);
        nf_t30_t38_pump(fax, 30);

        if ((cs.done || ended) && ++flush > T38_FLUSH_TICKS) break;
    }

    if (sending) tx_progress(fax, &next_prog, &pmode, verbose, 1);
    if (g_t38dump) { fclose(g_t38dump); g_t38dump = NULL; }
    int rc = fax_result(&cs, ended);
    debug_write_report(fax, sending, rc);
    nf_t30_free(fax);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Daemon mode: a long-lived, concurrent inbound-fax spooler.         */
/*                                                                    */
/* The parent owns the single listening SIP socket (all signalling:   */
/* registration, INVITE acceptance, in-dialog responses, per-call     */
/* BYE) and forks one child per call. Each child runs ONLY the media  */
/* (its own RTP socket + a receiving fax engine), writes its TIFF,    */
/* and exits. In-dialog requests for every call return to the one     */
/* listening port (our Contact stays :sip_port), so the parent        */
/* demultiplexes them by Call-ID. A per-child control socket (a unix  */
/* socketpair) lets the parent say "hang up" and, for a mid-call T.38 */
/* switchover, pass the freshly-opened UDPTL socket fd down to the    */
/* child (SCM_RIGHTS); SIGCHLD/waitpid tells the parent a fax done.   */
/* ------------------------------------------------------------------ */

#define MAX_DCALLS 16          /* concurrent inbound calls (else 486 Busy)   */

/* Parent->child control messages over the per-call unix socketpair. */
enum { DCTL_STOP = 'S', DCTL_T38 = 'T' };
struct dctl_msg {
    char               op;            /* DCTL_STOP / DCTL_T38              */
    struct sockaddr_in peer;          /* T.38 UDPTL peer (DCTL_T38)        */
    int                far_datagram;  /* peer T38FaxMaxDatagram (DCTL_T38) */
};

/* Send a control message; if fd >= 0 it is passed to the child via SCM_RIGHTS. */
static int dctl_send(int ctrl, const struct dctl_msg *m, int fd)
{
    struct iovec iov = { (void *) m, sizeof(*m) };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(&u, 0, sizeof(u));
        mh.msg_control = u.buf;
        mh.msg_controllen = sizeof(u.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(ctrl, &mh, 0) < 0 ? -1 : 0;
}

/* Receive a control message; *fd gets a passed descriptor or -1. Returns the
 * payload byte count (sizeof struct), 0 on EOF, <0 on error. */
static int dctl_recv(int ctrl, struct dctl_msg *m, int *fd)
{
    struct iovec iov = { m, sizeof(*m) };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = u.buf;
    mh.msg_controllen = sizeof(u.buf);
    *fd = -1;
    ssize_t r = recvmsg(ctrl, &mh, 0);
    if (r <= 0) return (int) r;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
        memcpy(fd, CMSG_DATA(c), sizeof(int));
    return (int) r;
}

/* Daemon: write a <base>.meta sidecar next to <base>.tiff recording the fax
 * metadata (remote station id, receive time, negotiated call params). */
static void write_meta_sidecar(const char *tiff_path, nf_t30_t *fax)
{
    nf_t30_stats_t st;
    nf_t30_get_stats(fax, &st);
    if (st.pages_rx < 1) return;          /* nothing received: no sidecar */

    char meta[512];
    size_t L = strlen(tiff_path);
    if (L > 5 && strcmp(tiff_path + L - 5, ".tiff") == 0)
        snprintf(meta, sizeof meta, "%.*s.meta", (int) (L - 5), tiff_path);
    else
        snprintf(meta, sizeof meta, "%s.meta", tiff_path);

    FILE *f = fopen(meta, "w");
    if (!f) { perror("spool .meta"); return; }
    char iso[40] = "";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", tm);
    fprintf(f, "remote_id=%s\n", st.rx_ident);
    fprintf(f, "received=%s\n", iso);
    fprintf(f, "pages=%d\n", st.pages_rx);
    fprintf(f, "resolution=%dx%d\n", st.x_resolution, st.y_resolution);
    fprintf(f, "bit_rate=%d\n", st.bit_rate);
    fprintf(f, "mode=%s\n", st.v34 ? "Super-G3/V.34" : "G3");
    fprintf(f, "ecm=%s\n", st.ecm ? "on" : "off");
    fclose(f);
}

/* Child-side T.38 media loop after a mid-call switchover: pump the UDPTL socket
 * (already installed in m->t38_sock) against a fresh receiving fax engine. The
 * far end re-runs T.30 Phase B over T.38. Ends on completion or a parent stop. */
static int run_fax_t38_child(sip_media_t *m, int ctrl_fd, const char *tiff_path,
                             const char *ident, int verbose)
{
    struct call_state cs = { 0, NF_T30_OK, verbose, NULL, NULL, NULL };
    nf_t30_t *fax = fax_open(0 /* receive */, tiff_path, ident, verbose, &cs);
    if (!fax) return 1;
    cs.tag = m->call_id[0] ? m->call_id : NULL;   /* tag log lines with the Call-ID */
    nf_t30_set_log_tag(fax, m->call_id);
    nf_t30_t38_enable(fax, 2 /*redundancy*/,
                      m->t38_far_datagram > 0 ? m->t38_far_datagram : 300,
                      t38_send_cb, m);

    int flush = 0, ended = 0;
    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    ts_add_ms(&next_tick, 30);

    for (;;) {
        fd_set rfds;
        while (media_wait(m->t38_sock, ctrl_fd, &next_tick, &rfds) > 0) {
            if (FD_ISSET(m->t38_sock, &rfds)) {
                uint8_t dg[2048];
                int n = sip_t38_rx(m, dg, sizeof dg);
                if (n > 0) { t38_dump('R', dg, n); nf_t30_t38_rx_datagram(fax, dg, n); }
            }
            if (FD_ISSET(ctrl_fd, &rfds)) {
                struct dctl_msg msg; int rfd = -1;
                dctl_recv(ctrl_fd, &msg, &rfd);   /* any message / EOF = stop */
                if (rfd >= 0) close(rfd);
                ended = 1;
            }
        }
        ts_add_ms(&next_tick, 30);
        nf_t30_t38_pump(fax, 30);
        if ((cs.done || ended) && ++flush > T38_FLUSH_TICKS) break;
    }

    if (m->t38_sock >= 0) close(m->t38_sock);
    write_meta_sidecar(tiff_path, fax);
    nf_t30_free(fax);
    return fax_result(&cs, ended);
}

struct dcall {
    int          active;
    pid_t        pid;
    int          stop_w;       /* parent end of the unix control socketpair  */
    int          peer_hung_up; /* the peer already sent BYE/CANCEL           */
    long         invite_cseq;  /* CSeq of the accepted INVITE (retransmit id)*/
    int          t38;          /* a T.38 re-INVITE has been accepted+handed off */
    long         reinvite_cseq;/* CSeq of that accepted T.38 re-INVITE       */
    int          t38_local_port;/* our UDPTL port (to re-answer retransmits)  */
    char         call_id[256];
    char         base[40];     /* spool basename (for logging)               */
    sip_media_t  call;         /* dialog state + sip_sock(listen) for BYE    */
};

static volatile sig_atomic_t g_sigchld = 0;
static volatile sig_atomic_t g_term    = 0;
static void daemon_on_sigchld(int s) { (void) s; g_sigchld = 1; }
static void daemon_on_term(int s)    { (void) s; g_term = 1; }

/* Nudge a child's control socket so its media loop wakes and finalizes. */
static void daemon_stop_child(int fd)
{
    if (fd < 0) return;
    struct dctl_msg m;
    memset(&m, 0, sizeof(m));
    m.op = DCTL_STOP;
    dctl_send(fd, &m, -1);
}

/* Hand a freshly-opened UDPTL socket (and the T.38 peer/params) to the media
 * child so it can switch from audio to T.38. Returns 0 on success. */
static int daemon_handoff_t38(int ctrl, int udptl_fd,
                              const struct sockaddr_in *peer, int far_datagram)
{
    struct dctl_msg m;
    memset(&m, 0, sizeof(m));
    m.op = DCTL_T38;
    m.peer = *peer;
    m.far_datagram = far_datagram;
    return dctl_send(ctrl, &m, udptl_fd);
}

/* Receive-only media loop for a forked child: pump RTP<->fax on the 20 ms
 * tick with NO SIP socket. Exits on fax completion or when stop_fd becomes
 * readable (the parent says hang up). */
static int run_fax_media(sip_media_t *m, int stop_fd, const char *tiff_path,
                         const char *ident, const char *audio_dump, int verbose)
{
    struct call_state cs = { 0, NF_T30_OK, verbose, NULL, NULL, NULL };

    nf_t30_t *fax = fax_open(0 /* receive */, tiff_path, ident, verbose, &cs);
    if (!fax) return 1;
    cs.tag = m->call_id[0] ? m->call_id : NULL;   /* tag log lines with the Call-ID */
    nf_t30_set_log_tag(fax, m->call_id);

    /* Optional: dump the decoded inbound PCM (16-bit LE, 8 kHz mono) for offline
     * replay/debugging. Gated by NF_RX_AUDIO_DUMP in the parent. */
    FILE *adump = audio_dump ? fopen(audio_dump, "wb") : NULL;

    int16_t out[SAMPLES_PER_BLOCK], in[SAMPLES_PER_BLOCK];
    int flush = 0, ended = 0;

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    ts_add_ms(&next_tick, 20);

    for (;;) {
        fd_set rfds;
        while (media_wait(m->rtp_sock, stop_fd, &next_tick, &rfds) > 0) {
            if (FD_ISSET(m->rtp_sock, &rfds)) {
                int n = sip_media_rx(m, in, SAMPLES_PER_BLOCK);
                if (n > 0) {
                    if (adump) fwrite(in, sizeof(int16_t), (size_t) n, adump);
                    nf_t30_rx(fax, in, n);
                }
            }
            if (FD_ISSET(stop_fd, &rfds)) {
                struct dctl_msg msg; int rfd = -1;
                int rr = dctl_recv(stop_fd, &msg, &rfd);
                if (rr > 0 && msg.op == DCTL_T38) {
                    /* Mid-call T.38 switchover: the parent accepted the re-INVITE
                     * and passed us the UDPTL socket. Drop the audio engine and
                     * run the T.38 media loop; the far end restarts T.30 there. */
                    if (adump) fclose(adump);
                    nf_t30_free(fax);
                    m->t38_sock = rfd;
                    m->t38_peer = msg.peer;
                    m->t38_far_datagram = msg.far_datagram;
                    m->t38_active = 1;
                    return run_fax_t38_child(m, stop_fd, tiff_path, ident, verbose);
                }
                ended = 1;          /* DCTL_STOP / parent gone */
            }
        }

        ts_add_ms(&next_tick, 20);
        fax_tx_block(fax, out);
        sip_media_tx(m, out, SAMPLES_PER_BLOCK);

        if ((cs.done || ended) && ++flush > FLUSH_BLOCKS) break;
    }

    if (adump) fclose(adump);
    write_meta_sidecar(tiff_path, fax);
    nf_t30_free(fax);                 /* flushes + closes the multi-page TIFF */
    return fax_result(&cs, ended);
}

/* Receive one SIP datagram on the listening socket, up to timeout_ms. Returns
 * its length (NUL-terminated into buf), 0 on timeout, -1 on error/EINTR. */
static int daemon_sip_recv(int sock, char *buf, int cap, int timeout_ms,
                           struct sockaddr_in *from)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return r;
    socklen_t sl = sizeof(*from);
    ssize_t n = recvfrom(sock, buf, (size_t) cap - 1, 0,
                         (struct sockaddr *) from, &sl);
    if (n <= 0) return (int) n;
    buf[n] = '\0';
    return (int) n;
}

static struct dcall *dcall_find(struct dcall *c, const char *call_id)
{
    for (int i = 0; i < MAX_DCALLS; i++)
        if (c[i].active && strcmp(c[i].call_id, call_id) == 0) return &c[i];
    return NULL;
}

static struct dcall *dcall_free_slot(struct dcall *c)
{
    for (int i = 0; i < MAX_DCALLS; i++)
        if (!c[i].active) return &c[i];
    return NULL;
}

/* Reap exited children; BYE any call the peer didn't already end, free slots. */
static void daemon_reap(struct dcall *calls)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_DCALLS; i++) {
            if (!calls[i].active || calls[i].pid != pid) continue;
            int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (!calls[i].peer_hung_up)
                sip_call_send_bye(&calls[i].call);
            fprintf(stderr, "[%s] Daemon: call %s finished (pid %d, rc %d)%s.\n",
                    calls[i].call_id, calls[i].base, (int) pid, rc,
                    calls[i].peer_hung_up ? " [peer BYE]" : "");
            if (calls[i].stop_w >= 0) close(calls[i].stop_w);
            calls[i].active = 0;
            break;
        }
    }
}

/* Accept a new INVITE: spool its bytes, answer it, and fork the media child. */
static void daemon_accept(struct dcall *calls, sip_media_t *listen,
                          const sip_config_t *cfg, const char *invite,
                          struct sockaddr_in *from)
{
    char cid[256] = "";
    sip_hdr(invite, "Call-ID", cid, sizeof(cid));

    struct dcall *slot = dcall_free_slot(calls);
    if (!slot) {
        fprintf(stderr, "Daemon: at capacity (%d calls), declining INVITE.\n",
                MAX_DCALLS);
        sip_uas_decline(listen, invite, from, "486 Busy Here");
        return;
    }

    char base[40];
    gen_hex(base, 16);
    char tiff_path[512], inv_path[512], pcm_path[512];
    snprintf(tiff_path, sizeof(tiff_path), "%s/%s.tiff",   cfg->spool_dir, base);
    snprintf(inv_path,  sizeof(inv_path),  "%s/%s.invite", cfg->spool_dir, base);
    snprintf(pcm_path,  sizeof(pcm_path),  "%s/%s.pcm",    cfg->spool_dir, base);
    const char *adump = getenv("NF_RX_AUDIO_DUMP") ? pcm_path : NULL;

    /* Save the original INVITE next to where the received TIFF will land. */
    FILE *f = fopen(inv_path, "w");
    if (f) { fwrite(invite, 1, strlen(invite), f); fclose(f); }
    else   perror("spool .invite");

    sip_media_t call;
    if (sip_uas_accept(cfg, listen, invite, from, &call) != 0) {
        fprintf(stderr, "Daemon: INVITE %s not acceptable, ignoring.\n", base);
        unlink(inv_path);
        return;
    }
    /* call.log_callid was set by sip_uas_accept (daemon mode), so this dialog's
     * SIP log lines carry the Call-ID. */

    int stop[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, stop) != 0) {
        perror("socketpair");
        sip_call_send_bye(&call);
        if (call.rtp_sock >= 0) close(call.rtp_sock);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(stop[0]); close(stop[1]);
        sip_call_send_bye(&call);
        if (call.rtp_sock >= 0) close(call.rtp_sock);
        return;
    }
    if (pid == 0) {
        /* Child: keep only the media. Drop the listening socket + write end. */
        close(stop[1]);
        /* Also drop the parent-side write ends of every OTHER active call that
         * we inherited across the fork. If we kept them, (a) they leak, and
         * (b) a sibling child's stop[0] would never see EOF when the parent
         * dies, because this process would still hold a write end — defeating
         * the orphan-detection that lets an abandoned child finalize. */
        for (int i = 0; i < MAX_DCALLS; i++)
            if (calls[i].active && calls[i].stop_w >= 0) close(calls[i].stop_w);
        if (listen->sip_sock >= 0) close(listen->sip_sock);
        call.sip_sock = -1;                 /* never touch SIP from the child */
        int rc = run_fax_media(&call, stop[0], tiff_path,
                               cfg->local_user[0] ? cfg->local_user : "sip_fax",
                               adump, cfg->verbose);
        if (call.rtp_sock >= 0) close(call.rtp_sock);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Parent: the child owns rtp_sock + the read end; we keep the dialog. */
    close(stop[0]);
    if (call.rtp_sock >= 0) { close(call.rtp_sock); call.rtp_sock = -1; }

    char cseq[64] = "";
    sip_hdr(invite, "CSeq", cseq, sizeof(cseq));
    slot->active       = 1;
    slot->pid          = pid;
    slot->stop_w       = stop[1];
    slot->peer_hung_up = 0;
    slot->invite_cseq  = strtol(cseq, NULL, 10);
    snprintf(slot->call_id, sizeof(slot->call_id), "%s", cid);
    snprintf(slot->base,    sizeof(slot->base),    "%s", base);
    slot->call = call;          /* struct copy: sip_sock(listen), dialog, peer */
    fprintf(stderr, "[%s] Daemon: accepted call -> %s.tiff (pid %d).\n",
            cid, base, (int) pid);
}

/* Dispatch one received SIP message in the parent. */
static void daemon_dispatch(struct dcall *calls, sip_media_t *listen,
                            const sip_config_t *cfg, char *buf,
                            struct sockaddr_in *from)
{
    char cid[256] = ""; sip_hdr(buf, "Call-ID", cid, sizeof(cid));

    if (cfg->verbose) {
        char line[120]; int i = 0;
        while (buf[i] && buf[i] != '\r' && buf[i] != '\n' && i < 119) {
            line[i] = buf[i]; i++;
        }
        line[i] = '\0';
        if (cid[0]) fprintf(stderr, "[%s] SIP RX %s\n", cid, line);
        else        fprintf(stderr, "SIP RX %s\n", line);
    }

    if (sip_response_code(buf) > 0) return;   /* responses (REGISTER 2xx, ...) */

    char method[32]; sip_method(buf, method, sizeof(method));
    struct dcall *c = cid[0] ? dcall_find(calls, cid) : NULL;

    if (strcasecmp(method, "INVITE") == 0) {
        if (!c) { daemon_accept(calls, listen, cfg, buf, from); return; }
        /* In-dialog re-INVITE (Call-ID already matched): require it to come
         * from the call's peer, else a guessed Call-ID could redirect media. */
        if (from->sin_addr.s_addr != c->call.sip_peer.sin_addr.s_addr) {
            if (cfg->verbose)
                fprintf(stderr, "[%s] ignoring off-dialog re-INVITE (source mismatch)\n",
                        c->call_id);
            return;
        }
        /* Same Call-ID: a retransmitted initial INVITE, a retransmitted T.38
         * re-INVITE, or a fresh re-INVITE (T.38 switchover), told apart by CSeq. */
        char cseq[64] = ""; sip_hdr(buf, "CSeq", cseq, sizeof(cseq));
        long cs = strtol(cseq, NULL, 10);
        if (cs == c->invite_cseq) {
            sip_call_resend_ok(&c->call, from);          /* dup initial INVITE */
        } else if (c->t38 && cs == c->reinvite_cseq) {
            sip_t38_reanswer(&c->call, buf, from, c->t38_local_port);  /* dup re-INVITE */
        } else if (cfg->enable_t38 && !c->t38) {
            /* T.38 switchover: accept, then pass the UDPTL socket to the child. */
            int fdg = 0, lp = 0;
            int ufd = sip_t38_accept(&c->call, buf, from, &fdg, &lp);
            if (ufd >= 0 && daemon_handoff_t38(c->stop_w, ufd, &c->call.t38_peer, fdg) == 0) {
                c->t38 = 1;
                c->reinvite_cseq = cs;
                c->t38_local_port = lp;
                fprintf(stderr, "[%s] Daemon: switched call %s to T.38.\n", c->call_id, c->base);
            } else if (ufd < 0) {
                sip_dialog_respond(&c->call, buf, "488 Not Acceptable Here", from);
            } else {
                fprintf(stderr, "[%s] Daemon: T.38 hand-off failed for %s.\n", c->call_id, c->base);
            }
            if (ufd >= 0) close(ufd);    /* the child owns it now */
            c->call.t38_sock = -1;
        } else {
            sip_dialog_respond(&c->call, buf, "488 Not Acceptable Here", from);
        }
        return;
    }

    if (!c) return;   /* ACK/BYE/etc. for an unknown dialog: ignore */

    /* In-dialog request with a matching Call-ID must also come from the call's
     * peer; otherwise a guessed Call-ID would let any host end the call. */
    if (from->sin_addr.s_addr != c->call.sip_peer.sin_addr.s_addr) {
        if (cfg->verbose)
            fprintf(stderr, "[%s] ignoring off-dialog %s (source mismatch)\n",
                    c->call_id, method);
        return;
    }

    if (strcasecmp(method, "BYE") == 0 || strcasecmp(method, "CANCEL") == 0) {
        sip_dialog_respond(&c->call, buf, "200 OK", from);
        if (!c->peer_hung_up) {
            c->peer_hung_up = 1;
            daemon_stop_child(c->stop_w);
            fprintf(stderr, "[%s] Daemon: peer ended call %s (%s).\n", c->call_id, c->base, method);
        }
    }
    /* ACK: nothing to do (the child is already running). */
}

/* Stop all children, BYE their peers, unregister, close the socket. */
static void daemon_shutdown(struct dcall *calls, sip_media_t *listen,
                            const sip_config_t *cfg)
{
    fprintf(stderr, "Daemon: shutting down.\n");
    for (int i = 0; i < MAX_DCALLS; i++)
        if (calls[i].active)
            daemon_stop_child(calls[i].stop_w);
    for (int i = 0; i < MAX_DCALLS; i++) {
        if (!calls[i].active) continue;
        int status; waitpid(calls[i].pid, &status, 0);
        if (!calls[i].peer_hung_up) sip_call_send_bye(&calls[i].call);
        if (calls[i].stop_w >= 0) close(calls[i].stop_w);
        calls[i].active = 0;
    }
    if (cfg->do_register) {            /* best-effort de-register (Expires: 0) */
        sip_config_t z = *cfg;
        z.reg_expires = 0;
        sip_daemon_register(listen, &z);
    }
    if (listen->sip_sock >= 0) close(listen->sip_sock);
}

static int run_daemon(const sip_config_t *cfg)
{
    sip_media_t listen;
    if (sip_daemon_listen(cfg, &listen) != 0) return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));      /* no SA_RESTART: signals interrupt select */
    sa.sa_handler = daemon_on_sigchld; sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = daemon_on_term;    sigaction(SIGINT,  &sa, NULL);
                                       sigaction(SIGTERM, &sa, NULL);

    struct dcall calls[MAX_DCALLS];
    memset(calls, 0, sizeof(calls));

    fprintf(stderr, "Daemon: listening on UDP %d, spooling to %s, "
            "re-REGISTER every %ds.\n",
            cfg->local_sip_port, cfg->spool_dir, cfg->reg_interval);

    struct timespec next_reg;
    clock_gettime(CLOCK_MONOTONIC, &next_reg);
    ts_add_ms(&next_reg, (long) cfg->reg_interval * 1000);

    while (!g_term) {
        if (g_sigchld) { g_sigchld = 0; daemon_reap(calls); }

        long until_reg = ts_until_ms(&next_reg);
        if (until_reg <= 0) {
            sip_daemon_register(&listen, cfg);
            clock_gettime(CLOCK_MONOTONIC, &next_reg);
            ts_add_ms(&next_reg, (long) cfg->reg_interval * 1000);
            until_reg = (long) cfg->reg_interval * 1000;
        }

        long wms = until_reg < 2000 ? until_reg : 2000;   /* also wake to reap */
        if (wms < 0) wms = 0;

        char buf[8192];
        struct sockaddr_in from;
        int r = daemon_sip_recv(listen.sip_sock, buf, sizeof(buf), (int) wms, &from);
        if (r <= 0) continue;        /* timeout or EINTR (signal): loop back */

        daemon_dispatch(calls, &listen, cfg, buf, &from);
    }

    daemon_shutdown(calls, &listen, cfg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s (--send <file> | --receive <file.tiff>)\n"
        "          ( --listen <port> | --connect <host:port>     [TCP pipe]\n"
        "          | --sip-dial <target> | --sip-answer )         [SIP/RTP]\n"
        "          [--user sip:user@host] [--password <pw>] [--sip-port <port>]\n"
        "          [--register] [--ident <str>] [--verbose]\n"
        "\n"
        "  Sends/receives a fax. Two transports, both carrying G.711 8 kHz audio:\n"
        "    TCP pipe  raw 16-bit LE linear PCM over a single TCP connection.\n"
        "    SIP/RTP   a real SIP call with A-law (PCMA) RTP media.\n"
        "  Transport and fax role (--send/--receive) are independent.\n"
        "\n"
        "  --send <file>         page source to transmit: a TIFF (sent as-is,\n"
        "                        multi-page documents supported), or a single\n"
        "                        %d-wide bilevel PBM (P4/P1) / PAM (P7)\n"
        "  --send-alt <res>:<file>  add a resolution alternative (repeatable):\n"
        "                        the tool sends the best one the receiver supports.\n"
        "                        <res> = standard|fine|superfine|300|400, each a\n"
        "                        bilevel TIFF of the matching width. Excludes --send;\n"
        "                        combines with --send-color/--send-gray (the best\n"
        "                        kind the receiver supports wins: colour > grey >\n"
        "                        bilevel; see pdffax.sh)\n"
        "  --receive <file.tiff> receive into a (multi-page) Group-4 TIFF (or an\n"
        "                        RGB TIFF if a colour fax is received)\n"
        "  --send-color <rgb.tiff>  send a colour image as a real T.30 colour fax\n"
        "                        (T.42/JPEG over ECM); input must be an RGB TIFF\n"
        "  --send-gray <file.tiff>  send an image as a greyscale T.81 JPEG fax\n"
        "  --require-color       fail the call (DCN, exit 1) if the receiver\n"
        "                        cannot take colour, instead of falling back to\n"
        "                        a greyscale/bilevel alternative\n"
        "  --send-file <path>    send an arbitrary file (e.g. voicemail) byte-exact\n"
        "                        over ECM (nf<->nf only). Excludes --send\n"
        "  --receive-file <path> receive an incoming --send-file transfer\n"
        "  --color-quality <1..100>  JPEG quality for --send-color (default 85)\n"
        "\n"
        "  --poll-serve          polling source: answer the call and TRANSMIT the\n"
        "                        document when the caller polls (combine with the\n"
        "                        --send* options; with --send-alt/-color/-gray the\n"
        "                        best version the caller can take is sent). See\n"
        "                        pdffax.sh --poll-serve.\n"
        "  --poll                polling client: place the call and RECEIVE a\n"
        "                        document the answerer offers (needs --receive)\n"
        "\n"
        "  --listen <port>       TCP transport: act as server\n"
        "  --connect <host:port> TCP transport: act as client\n"
        "\n"
        "  --sip-dial <target>   SIP: place a call (UAC). target is a sip: URI,\n"
        "                        user@host, or bare user/number on the --user host\n"
        "  --sip-answer          SIP: answer one inbound INVITE (UAS)\n"
        "  --user sip:user@host  SIP identity (required for SIP modes)\n"
        "  --password <pw>       SIP digest password (or set $SIP_PASSWORD)\n"
        "  --sip-port <port>     local SIP UDP port (default 5060)\n"
        "  --register            answer mode: REGISTER upstream before answering\n"
        "  --t38                 negotiate T.38 fax over UDPTL: --sip-dial offers it\n"
        "                        via re-INVITE; --sip-answer accepts an inbound T.38\n"
        "                        re-INVITE (else stays G.711 audio). Default off.\n"
        "\n"
        "  --daemon <spooldir>   run forever: REGISTER (refreshing periodically),\n"
        "                        answer every inbound call concurrently, and spool\n"
        "                        each received fax as <spooldir>/<rand>.tiff with a\n"
        "                        copy of its INVITE as <spooldir>/<rand>.invite.\n"
        "                        Needs --user (and usually --password); receive-only.\n"
        "  --reg-interval <sec>  daemon re-REGISTER cadence (default 60; Expires=2x)\n"
        "\n"
        "  --ident <str>         local station identifier (max 20 chars) sent as\n"
        "                        TSI/CSI/CIG in T.30 phase B and shown to the peer;\n"
        "                        the remote's id is logged and stored in the received\n"
        "                        TIFF/daemon .meta. Default \"sip_fax\"; \"\" sends none.\n"
        "  --no-ecm              disable T.30 Error Correction Mode (ECM on by default)\n"
        "  --v34 / --no-v34      offer / suppress V.34 (Super G3) in V.8 negotiation.\n"
        "                        On by default (like a real SG3 machine): used when\n"
        "                        both peers support it, otherwise V.8 falls back to\n"
        "                        the best common G3 modem (V.17/V.29/V.27), or to the\n"
        "                        classic CNG/CED->DIS flow for a non-V.8 peer.\n"
        "                        (Polling always uses classic G3.)\n"
        "  --require-v34         fail the call (DCN, exit 1) BEFORE sending any page\n"
        "                        data if V.8 does not negotiate V.34 (Super G3),\n"
        "                        instead of falling back to classic G3. Implies --v34.\n"
        "  --no-redial           don't redial as classic G3 after a dialed V.34\n"
        "                        call fails (default: one automatic redial with\n"
        "                        V.34 suppressed - some SG3 machines advertise\n"
        "                        capabilities their V.34 stack cannot receive)\n"
        "  --verbose             enable T.30 protocol logging (and SIP traffic)\n"
        "  --debug [--debug-dir D]\n"
        "                        full diagnostic capture for analysing failures:\n"
        "                        implies --verbose, logs whole SIP messages and\n"
        "                        lower-layer (T.30/V.34/T.38) traces with wall-clock\n"
        "                        timestamps, saves inbound audio (rx.pcm) and a\n"
        "                        session.log + report.txt under a debug directory\n"
        "                        (auto-named, or --debug-dir D). Replay the audio\n"
        "                        offline with --replay-rx D/rx.pcm --receive out.tiff\n",
        argv0, FAX_LINE_WIDTH);
}

/* Split a "sip:user@host" AoR into user/host. Returns 0 on success. */
static int parse_aor(const char *aor, char *user, int ulen, char *host, int hlen)
{
    const char *colon = strchr(aor, ':');
    const char *at    = strchr(aor, '@');
    if (!colon || !at || at < colon) return -1;
    int un = (int) (at - colon - 1);
    if (un <= 0 || un >= ulen) return -1;
    memcpy(user, colon + 1, (size_t) un);
    user[un] = '\0';
    strncpy(host, at + 1, (size_t) hlen - 1);
    host[hlen - 1] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    const char *send_file = NULL;
    const char *recv_file = NULL;
    const char *send_color = NULL;       /* --send-color <rgb.tiff> */
    const char *send_gray = NULL;        /* --send-gray <tiff>      */
    const char *send_file_arg = NULL;    /* --send-file <path>      */
    const char *recv_file_arg = NULL;    /* --receive-file <path>   */
    int color_quality = 85;              /* --color-quality         */
    const char *listen_port = NULL;
    const char *connect_arg = NULL;
    const char *sip_dial = NULL;
    const char *aor = NULL;
    const char *password = NULL;
    const char *ident = "sip_fax";
    int sip_answer = 0;
    int do_register = 0;
    int sip_port = 5060;
    int verbose = 0;
    const char *daemon_spool = NULL;     /* --daemon <spooldir> */
    int reg_interval = 60;               /* --reg-interval <sec> */
    int enable_t38 = 0;                  /* --t38 (offer/accept T.38) */
    const char *replay_rx = NULL;        /* --replay-rx <pcm> (offline debug) */
    int debug = 0;                       /* --debug: capture everything        */
    const char *debug_dir = NULL;        /* --debug-dir <dir>                  */

    signal(SIGPIPE, SIG_IGN);   /* writing to a closed link returns EPIPE, not a fatal signal */

    struct tx_doc altdoc;
    memset(&altdoc, 0, sizeof(altdoc));
    altdoc.chosen = -1;

    static struct option opts[] = {
        { "send",       required_argument, 0, 's' },
        { "send-alt",   required_argument, 0, 'X' },
        { "send-color", required_argument, 0, 'C' },
        { "send-colour",required_argument, 0, 'C' },
        { "send-gray",  required_argument, 0, 'Y' },
        { "send-grey",  required_argument, 0, 'Y' },
        { "require-color",  no_argument,   0, 1001 },
        { "require-colour", no_argument,   0, 1001 },
        { "poll-serve",     no_argument,   0, 1002 },
        { "poll",           no_argument,   0, 1003 },
        { "send-file",  required_argument, 0, 'F' },
        { "receive",    required_argument, 0, 'r' },
        { "receive-file",required_argument, 0, 'G' },
        { "color-quality", required_argument, 0, 'Q' },
        { "listen",     required_argument, 0, 'l' },
        { "connect",    required_argument, 0, 'c' },
        { "sip-dial",   required_argument, 0, 'D' },
        { "sip-answer", no_argument,       0, 'A' },
        { "user",       required_argument, 0, 'u' },
        { "password",   required_argument, 0, 'p' },
        { "sip-port",   required_argument, 0, 'P' },
        { "register",   no_argument,       0, 'R' },
        { "daemon",     required_argument, 0, 1004 },
        { "reg-interval", required_argument, 0, 1005 },
        { "replay-rx",  required_argument, 0, 1006 },
        { "t38",        no_argument,       0, 1007 },
        { "no-t38",     no_argument,       0, 1008 },
        { "ident",      required_argument, 0, 'i' },
        { "no-ecm",     no_argument,       0, 'E' },
        { "v34",        no_argument,       0, 1009 },
        { "no-v34",     no_argument,       0, 1010 },
        { "require-v34",no_argument,       0, 1011 },
        { "no-redial",  no_argument,       0, 1012 },
        { "debug",      no_argument,       0, 1013 },
        { "debug-dir",  required_argument, 0, 1014 },
        { "verbose",    no_argument,       0, 'v' },
        { "help",       no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "s:r:l:c:D:Au:p:P:Ri:vh", opts, NULL)) != -1) {
        switch (ch) {
        case 's': send_file = optarg; break;
        case 'X': {
            char *colon = strchr(optarg, ':');
            if (!colon) {
                fprintf(stderr, "error: --send-alt needs <res>:<file>\n");
                usage(argv[0]); return 2;
            }
            char key[16];
            size_t kl = (size_t) (colon - optarg);
            if (kl >= sizeof(key)) kl = sizeof(key) - 1;
            memcpy(key, optarg, kl);
            key[kl] = '\0';
            alt_res_t r;
            if (parse_alt_res(key, &r) != 0) {
                fprintf(stderr, "error: unknown --send-alt resolution '%s' "
                        "(use standard|fine|superfine|300|400)\n", key);
                return 2;
            }
            for (int i = 0; i < altdoc.n_alt; i++)
                if (altdoc.alt[i].res == r) {
                    fprintf(stderr, "error: duplicate --send-alt resolution '%s'\n", key);
                    return 2;
                }
            altdoc.alt[altdoc.n_alt].res  = r;
            altdoc.alt[altdoc.n_alt].path = colon + 1;
            altdoc.n_alt++;
            break;
        }
        case 'r': recv_file = optarg; break;
        case 'l': listen_port = optarg; break;
        case 'c': connect_arg = optarg; break;
        case 'D': sip_dial = optarg; break;
        case 'A': sip_answer = 1; break;
        case 'u': aor = optarg; break;
        case 'p': password = optarg; break;
        case 'P': sip_port = atoi(optarg); break;
        case 'R': do_register = 1; break;
        case 1004: daemon_spool = optarg; break;
        case 1005: reg_interval = atoi(optarg); break;
        case 1007: enable_t38 = 1; break;
        case 1008: enable_t38 = 0; break;
        case 1006: replay_rx = optarg; break;
        case 'i': ident = optarg; break;
        case 'C': send_color = optarg; break;
        case 'Y': send_gray = optarg; break;
        case 'F': send_file_arg = optarg; break;
        case 'G': recv_file_arg = optarg; break;
        case 'Q': color_quality = atoi(optarg); break;
        case 1001: altdoc.require_color = 1; break;
        case 1002: g_poll_serve = 1; break;
        case 1003: g_poll_recv = 1; break;
        case 'E': g_use_ecm = 0; break;
        case 1009: g_v34 = 1; break;
        case 1010: g_v34 = 0; break;
        case 1011: g_require_v34 = 1; break;
        case 1012: g_auto_redial = 0; break;
        case 1013: debug = 1; break;
        case 1014: debug = 1; debug_dir = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    /* --debug: one switch that turns on full protocol logging, captures the
     * inbound audio, and tees the whole trace to <dir>/session.log. Do this
     * before any transport starts so nothing is missed. */
    if (debug) {
        verbose = 1;
        setup_debug(debug_dir);
    }

    /* Offline debug: replay a captured PCM stream through the receiver. */
    if (replay_rx) {
        if (!recv_file) {
            fprintf(stderr, "error: --replay-rx needs --receive <tiff>\n");
            return 2;
        }
        return run_fax_replay(replay_rx, recv_file, verbose);
    }

    /* Daemon mode is its own self-contained transport+role: a long-lived,
     * registering, receive-only spooler. Handle it before the send/receive and
     * transport validation the one-shot modes use. */
    if (daemon_spool) {
        if (send_file || recv_file || altdoc.n_alt || send_color || send_gray ||
            send_file_arg || recv_file_arg || listen_port || connect_arg ||
            sip_dial || sip_answer || g_poll_serve || g_poll_recv) {
            fprintf(stderr, "error: --daemon cannot be combined with --send*/"
                    "--receive*/--poll* or another transport\n");
            return 2;
        }
        if (!aor) {
            fprintf(stderr, "error: --daemon needs --user sip:user@host\n");
            return 2;
        }
        if (reg_interval < 10) {
            fprintf(stderr, "error: --reg-interval must be >= 10 seconds\n");
            return 2;
        }
        if (access(daemon_spool, W_OK) != 0) {
            fprintf(stderr, "error: spool dir '%s' not writable: %s\n",
                    daemon_spool, strerror(errno));
            return 2;
        }

        sip_config_t dcfg;
        memset(&dcfg, 0, sizeof(dcfg));
        if (parse_aor(aor, dcfg.local_user, sizeof(dcfg.local_user),
                      dcfg.registrar_host, sizeof(dcfg.registrar_host)) != 0) {
            fprintf(stderr, "error: --user must be sip:user@host\n");
            return 2;
        }
        if (!password) password = getenv("SIP_PASSWORD");
        if (password) {
            strncpy(dcfg.password, password, sizeof(dcfg.password) - 1);
            dcfg.password[sizeof(dcfg.password) - 1] = '\0';
        }
        dcfg.dial           = 0;
        dcfg.do_register    = 1;
        dcfg.local_sip_port = sip_port;
        dcfg.reg_interval   = reg_interval;
        dcfg.reg_expires    = reg_interval * 2;
        dcfg.verbose        = verbose;
        dcfg.daemon_mode    = 1;
        dcfg.enable_t38     = enable_t38;   /* --t38: accept the gateway's switchover */
        strncpy(dcfg.spool_dir, daemon_spool, sizeof(dcfg.spool_dir) - 1);
        return run_daemon(&dcfg);
    }

    /* --send-alt / --send-color / --send-gray may be combined: the best kind
     * the receiver supports is chosen at phase B. Everything else stays
     * mutually exclusive. */
    int tx_img_kinds = (altdoc.n_alt > 0) + (send_color != NULL) + (send_gray != NULL);
    /* Exactly one top-level mode must be selected: a file send/receive, a fax
     * receive, or an image-sending combination (which tx_img_kinds folds to one). */
    int kinds_set = (send_file != NULL) + (recv_file != NULL)
                  + (send_file_arg != NULL) + (recv_file_arg != NULL)
                  + (tx_img_kinds > 0);
    if (kinds_set != 1) {
        fprintf(stderr, "error: specify exactly one of --send / --send-file / "
                "--receive / --receive-file, or an image-sending combination of "
                "--send-alt / --send-color / --send-gray\n");
        usage(argv[0]);
        return 2;
    }
    int combined = tx_img_kinds > 1;
    g_send_color = (send_color != NULL) && !combined;
    g_send_gray  = (send_gray != NULL) && !combined;
    g_send_file  = (send_file_arg != NULL);
    g_recv_file  = (recv_file_arg != NULL);
    g_color_quality = color_quality;
    /* Colour/greyscale JPEG and binary-file transfer are carried over ECM; they
     * cannot run with --no-ecm. */
    if ((g_send_color || g_send_gray || g_send_file || g_recv_file) && !g_use_ecm) {
        fprintf(stderr, "error: --send-color / --send-gray / --send-file / --receive-file "
                "require ECM (remove --no-ecm)\n");
        return 2;
    }
    if (send_color && !is_tiff_file(send_color)) {
        fprintf(stderr, "error: --send-color needs an RGB TIFF "
                "(convert first, e.g. `convert in.png out.tiff`)\n");
        return 2;
    }
    if (send_gray && !is_tiff_file(send_gray)) {
        fprintf(stderr, "error: --send-gray needs a TIFF "
                "(convert first, e.g. `convert in.png out.tiff`)\n");
        return 2;
    }
    if (altdoc.require_color && !send_color) {
        fprintf(stderr, "error: --require-color needs --send-color\n");
        return 2;
    }
    if (g_require_v34 && !g_v34) {
        fprintf(stderr, "error: --require-v34 conflicts with --no-v34\n");
        return 2;
    }

    int n_transport = (listen_port != NULL) + (connect_arg != NULL)
                    + (sip_dial != NULL) + (sip_answer != 0);
    if (n_transport != 1) {
        fprintf(stderr, "error: specify exactly one transport "
                "(--listen / --connect / --sip-dial / --sip-answer)\n");
        usage(argv[0]);
        return 2;
    }

    int use_sip = (sip_dial != NULL || sip_answer);
    int sending = (send_file != NULL) || (altdoc.n_alt > 0) || send_color || send_gray || send_file_arg;

    /* Polling inverts the document direction: the answering "poll server" holds
     * the document and transmits it when the calling "poll client" requests it
     * with a DTC. So the poll server answers but sends, and the poll client
     * calls but receives. The T.30 calling party follows the transport role,
     * not the send/receive role. */
    if (g_poll_serve && g_poll_recv) {
        fprintf(stderr, "error: --poll-serve and --poll are mutually exclusive\n");
        return 2;
    }
    if (g_poll_serve) {
        if (!sending) {
            fprintf(stderr, "error: --poll-serve needs a document to serve "
                    "(--send / --send-alt / --send-color / --send-gray)\n");
            return 2;
        }
        if (!(sip_answer || listen_port)) {
            fprintf(stderr, "error: --poll-serve must answer the call "
                    "(--sip-answer or --listen)\n");
            return 2;
        }
    }
    if (g_poll_recv) {
        if (!recv_file) {
            fprintf(stderr, "error: --poll needs --receive <file> to receive into\n");
            return 2;
        }
        if (!(sip_dial || connect_arg)) {
            fprintf(stderr, "error: --poll must place the call "
                    "(--sip-dial or --connect)\n");
            return 2;
        }
    }
    g_calling = g_poll_recv ? 1 : g_poll_serve ? 0 : sending;

    /* Resolve the SIP identity and password up front (env fallback keeps the
     * secret out of `ps aux`). */
    sip_config_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    if (use_sip) {
        if (!aor || parse_aor(aor, scfg.local_user, sizeof(scfg.local_user),
                              scfg.registrar_host, sizeof(scfg.registrar_host)) != 0) {
            fprintf(stderr, "error: SIP modes need --user sip:user@host\n");
            usage(argv[0]);
            return 2;
        }
        if (!password) password = getenv("SIP_PASSWORD");
        if (password) {
            strncpy(scfg.password, password, sizeof(scfg.password) - 1);
            scfg.password[sizeof(scfg.password) - 1] = '\0';
        }
        scfg.dial           = (sip_dial != NULL);
        scfg.do_register    = do_register;
        scfg.local_sip_port = sip_port;
        scfg.reg_expires    = 120;
        scfg.verbose        = verbose;
        scfg.enable_t38     = enable_t38;
        if (sip_dial)
            strncpy(scfg.target, sip_dial, sizeof(scfg.target) - 1);
    }

    /* Validate the resolution alternatives (if any) and note the highest-
     * preference one as the initial/default tx file. */
    struct tx_doc *doc = NULL;
    if (altdoc.n_alt > 0) {
        for (int i = 0; i < altdoc.n_alt; i++)
            if (validate_alt(altdoc.alt[i].path, altdoc.alt[i].res) != 0)
                return 1;
        int hi = 0;
        for (int i = 1; i < altdoc.n_alt; i++)
            if (altdoc.alt[i].res > altdoc.alt[hi].res) hi = i;
        altdoc.initial = hi;
        doc = &altdoc;
    }
    if (combined) {
        altdoc.color_path = send_color;
        altdoc.gray_path = send_gray;
        doc = &altdoc;
    }

    /* Prepare the page source to transmit before opening the connection. With
     * --send-alt the (already-validated) highest-preference TIFF is the initial
     * file; the phase-B handler may switch it. A plain --send TIFF is sent as-is
     * (multi-page supported); a single-page PBM/PAM is converted to a temp TIFF. */
    char tmp_tiff[] = "/tmp/sip_fax_tx_XXXXXX.tif";
    int made_tmp = 0;
    const char *fax_file = recv_file;          /* overwritten below when sending */

    if (combined) {
        fax_file = (altdoc.n_alt > 0) ? altdoc.alt[altdoc.initial].path
                 : send_color ? send_color : send_gray;
    } else if (send_color) {
        fax_file = send_color;                 /* RGB TIFF, read by the colour codec */
    } else if (send_gray) {
        fax_file = send_gray;                  /* TIFF, sent as greyscale JPEG */
    } else if (send_file_arg) {
        fax_file = send_file_arg;              /* raw file, sent verbatim */
    } else if (recv_file_arg) {
        fax_file = recv_file_arg;              /* raw file output path */
    } else if (doc) {
        fax_file = altdoc.alt[altdoc.initial].path;
    } else if (sending) {
        if (is_tiff_file(send_file)) {
            if (check_tx_tiff(send_file) != 0) return 1;
            fax_file = send_file;              /* passthrough, no conversion */
        } else {
            int tfd = mkstemps(tmp_tiff, 4);
            if (tfd < 0) { perror("mkstemps"); return 1; }
            close(tfd);
            if (image_to_tiff(send_file, tmp_tiff) != 0) {
                unlink(tmp_tiff);
                return 1;
            }
            fax_file = tmp_tiff;
            made_tmp = 1;
        }
    }

    int rc;

    if (use_sip) {
        /* Ensure a SIGTERM/SIGINT (e.g. `timeout` firing) ends the call with a
         * BYE/CANCEL instead of leaving it dangling on the far end/gateway. */
        sip_install_hangup_signals();
        for (;;) {
            sip_media_t media;
            if (sip_media_establish(&scfg, &media) != 0) {
                if (made_tmp) unlink(tmp_tiff);
                return 1;
            }
            if (scfg.enable_t38 && scfg.dial && sip_offer_t38(&media, &scfg)) {
                /* Dialer: offered T.38 via re-INVITE and the peer accepted. */
                rc = run_fax_t38(&media, sending, fax_file, ident, verbose, doc);
            } else {
                rc = run_fax_sip(&media, sending, fax_file, ident, verbose, doc);
                if (rc == -2)    /* answerer accepted a T.38 re-INVITE mid-call */
                    rc = run_fax_t38(&media, sending, fax_file, ident, verbose, doc);
            }
            sip_media_hangup(&media);
            if (sip_stop_requested()) break;   /* stop signal: no redial */
            /* A dialed call that negotiated V.34 but failed gets one redial as
             * classic G3: some SG3 machines advertise capabilities their V.34
             * stack cannot actually receive (seen in the field with JPEG). */
            if (rc != 0 && g_last_v34_failed && g_auto_redial && scfg.dial &&
                g_v34 && !g_require_v34) {
                fprintf(stderr, "V.34 call failed; redialing without V.34 "
                        "(classic G3)...\n");
                g_v34 = 0;
                g_last_v34_failed = 0;
                if (doc) { doc->chosen = -1; doc->chosen_kind = NULL; }
                continue;
            }
            break;
        }
    } else {
        /* TCP transport. */
        int fd;
        if (listen_port) {
            fd = tcp_listen(atoi(listen_port));
        } else {
            char host[256];
            const char *colon = strrchr(connect_arg, ':');
            if (!colon) {
                fprintf(stderr, "error: --connect needs host:port\n");
                if (made_tmp) unlink(tmp_tiff);
                return 2;
            }
            size_t hlen = (size_t) (colon - connect_arg);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, connect_arg, hlen);
            host[hlen] = '\0';
            fd = tcp_connect(host, colon + 1);
        }
        if (fd < 0) {
            if (made_tmp) unlink(tmp_tiff);
            return 1;
        }
        rc = run_fax(fd, sending, fax_file, ident, verbose, doc);
        close(fd);
    }

    if (made_tmp) unlink(tmp_tiff);
    return rc;
}
