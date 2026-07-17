#ifndef NF_V8_H
#define NF_V8_H

#include <stdint.h>
#include "nf_dsp.h"

/*
 * nf_v8 - ITU-T V.8 modem-type negotiation, our own implementation.
 *
 * V.8 is the handshake T.30 Annex F ("Super G3") uses in front of V.34: the
 * calling terminal solicits the answerer with CI (Call Indication) on V.21
 * channel 1, the answerer emits ANSam (a phase-reversing, amplitude-modulated
 * 2100 Hz tone that a plain G3 machine still reads as an ordinary CED) - a
 * V.8-capable answerer that never hears CI/CNG falls back to a plain CED and
 * does NOT offer V.8, so the caller MUST send CI. The caller then answers
 * with CM (Call Menu: its supported modulations) on V.21 channel 1, the
 * answerer replies with JM (Joint Menu: the common subset) on V.21 channel 2,
 * and the caller closes with CJ (three zero octets). Both directions run
 * concurrently on independent tone bands, so unlike every other modem in this
 * codebase this one is genuinely full duplex - nf_v8_tx()/nf_v8_rx() must
 * both be pumped every audio block.
 *
 * Wire format (verified this session against a real captured Super G3 call,
 * and cross-read against spandsp's v8.c for the parts we hadn't exercised):
 *   - CM carried on V.21 channel 1 (mark 980 Hz / space 1180 Hz), JM on
 *     channel 2 (mark 1650 Hz / space 1850 Hz), CJ on channel 1 (same as CM -
 *     it is sent by the calling party). 300 baud, async framing: 1 start bit
 *     (0), 8 data bits LSB first, 1 stop bit (1).
 *   - Each message = 10 mark (1) bits (preamble) then a framed sync octet
 *     (0xE0 for CM/JM/CJ) then the framed data octets, repeated continuously;
 *     a message is accepted once two consecutive repeats decode identically.
 *   - Call-function octet: tag 0x01 in bits 4:0, function code in bits 7:5,
 *     bit 7(0x80) of the octet observed set on real traffic (spandsp's
 *     decoder ignores it, so we set it to match real senders).
 *   - Modulation-mode octets: tag 0x05 in the first octet's bits 4:0, then
 *     capability bits; a 3-octet extension chain, each extension flagged by
 *     bits 5:3 == 0b010 (mask 0x38, value 0x10) in the *following* octet.
 *   - ANSam: 2100 Hz carrier, level -11 dBm0, AM by a 15 Hz tone at 1/5 the
 *     carrier's peak level (envelope = level + level/5 * sin(2*pi*15*t));
 *     ANSam-PR additionally flips the carrier phase by 180 degrees every
 *     450 ms. (Exact formula taken from spandsp's modem_connect_tones.c,
 *     since it is what our oracle test's peer emits/expects.)
 * Enum values are numerically identical to spandsp's so capability masks
 * compare directly against its `v8_parms_s`.
 */

/* Call function (CM octet 1, bits 7:5). */
enum {
    NF_V8_CALL_TBS                = 0,
    NF_V8_CALL_H324               = 1,
    NF_V8_CALL_V18                = 2,
    NF_V8_CALL_T101               = 3,
    NF_V8_CALL_T30_TX             = 4,   /* we will send a fax   */
    NF_V8_CALL_T30_RX             = 5,   /* we will receive a fax */
    NF_V8_CALL_V_SERIES           = 6,
    NF_V8_CALL_FUNCTION_EXTENSION = 7
};

/* Modulation capability bitmask (CM/JM modulation octets). */
enum {
    NF_V8_MOD_V17    = (1 << 0),
    NF_V8_MOD_V21    = (1 << 1),
    NF_V8_MOD_V22    = (1 << 2),
    NF_V8_MOD_V23HDX = (1 << 3),
    NF_V8_MOD_V23    = (1 << 4),
    NF_V8_MOD_V26BIS = (1 << 5),
    NF_V8_MOD_V26TER = (1 << 6),
    NF_V8_MOD_V27TER = (1 << 7),
    NF_V8_MOD_V29    = (1 << 8),
    NF_V8_MOD_V32    = (1 << 9),
    NF_V8_MOD_V34HDX = (1 << 10),
    NF_V8_MOD_V34    = (1 << 11),
    NF_V8_MOD_V90    = (1 << 12),
    NF_V8_MOD_V92    = (1 << 13)
};

/* Final result delivered to the caller of nf_v8. */
enum {
    NF_V8_STATUS_IN_PROGRESS = 0,
    NF_V8_STATUS_V8_CALL     = 1,   /* CM/JM/CJ completed; negotiated set valid */
    NF_V8_STATUS_NON_V8_CALL = 2,   /* no ANSam/CM ever seen -> fall back to G3 */
    NF_V8_STATUS_FAILED      = 3    /* V.8 started but did not complete in time */
};

typedef struct {
    int32_t  call_function;   /* NF_V8_CALL_*                      */
    uint32_t modulations;     /* NF_V8_MOD_* mask, negotiated (AND) */
    int      ansam_am;        /* answer tone seen (caller side): -1 unknown,
                               * 0 = plain 2100 Hz tone (ANS/CED - peer did NOT
                               * offer V.8), 1 = 15 Hz-AM'd (ANSam - V.8 offered) */
} nf_v8_result_t;

typedef void (*nf_v8_result_handler_t)(void *user, int status, const nf_v8_result_t *r);

/* ── channel-parameterised V.21 FSK sub-layer (async octet framing) ───── */

#define NF_V8_FSK_SPAN 26           /* samples/baud at 300 baud, 8 kHz      */
#define NF_V8_MSG_MAX  64           /* plenty for CM/JM (we send <= 4 bytes) */
/* CI is emitted as a burst of 4 segments, each = 10 preamble + framed sync +
 * framed call-function octet (spandsp send_ci sends 4). Room for that plus the
 * looping CM/JM/CJ single segments (<= 4 payload octets). */
#define NF_V8_TXBITS_MAX (10 * (10 + 10 * (1 + NF_V8_MSG_MAX)))

/* Sliding coherent-correlator length for the 2100 Hz ANSam/ANS detector.
 * 240 samples = 30 ms: a ~33 Hz Goertzel bin (rejects the 980/1180/1650/1850
 * FSK bands and our own CI echo) that averages over ~0.45 of a 15 Hz AM cycle
 * so the correlator magnitude never collapses on ANSam's amplitude dips. */
#define NF_V8_ANSAM_SPAN 240

typedef struct {
    int      active;
    int      loop;                 /* 1 = repeat the armed bits forever (CM/JM/CJ) */
    int      done;                 /* oneshot: set once the last bit was emitted   */
    uint32_t phase;
    int32_t  rates[2];              /* [0]=space(0's freq), [1]=mark(1's freq) */
    int32_t  cur_rate;
    float    scaling;
    int      baud_frac;
    uint8_t  bits[NF_V8_TXBITS_MAX]; /* preamble + framed sync + framed octets */
    int      nbits, pos;
} nf_v8_fsk_tx_t;

typedef struct {
    uint32_t phase[2];
    int32_t  rate[2];
    nf_cpx_t window[2][NF_V8_FSK_SPAN];
    nf_cpx_t dot[2];
    int      buf_ptr;
    nf_power_t power;
    int16_t  last_sample;
    int32_t  on_power, off_power;
    int      signal_present;
    int      baud_phase;
    int      last_bit;
    /* async deframer + preamble/sync detector (spandsp-compatible 20-bit
     * shift register: newest bit enters at bit 19, data = bits 18..11). */
    uint32_t bit_stream;
    int      flags_run;               /* bits of sustained HDLC-flag periodicity */
    int      have_sync;
    int      bit_cnt;
    uint8_t  rx_data[NF_V8_MSG_MAX];  /* octets of the segment being framed  */
    int      rx_len;
    uint8_t  saved_msg[NF_V8_MSG_MAX];/* previous segment, for the 2x-match check */
    int      saved_len;
    uint8_t  valid_msg[NF_V8_MSG_MAX];/* latched once two copies matched     */
    int      valid_len;
    int      have_msg;                /* valid_msg/valid_len are ready       */
    int      zero_run;                /* consecutive all-zero octets (-> CJ) */
    int      have_cj;
} nf_v8_fsk_rx_t;

/* ── ANSam generator / detector ────────────────────────────────────────── */

typedef struct {
    uint32_t tone_phase, mod_phase;
    int32_t  tone_rate, mod_rate;
    float    level, mod_level;
    int      phase_reversing;
    int      hop_timer;              /* samples to next 180-degree flip     */
    int      silence_left, tone_left;/* samples remaining in each stage     */
} nf_v8_ansam_tx_t;

/* Frequency-selective 2100 Hz answer-tone detector (ANSam and plain ANS/CED).
 * A sliding coherent correlator at 2100 Hz gives a tone-energy estimate; a
 * matched sliding sum of x^2 gives total energy. Presence is a *ratio* test
 * (tone energy vs total energy), so it is level-independent AND immune to
 * ANSam's 15 Hz amplitude modulation - the ratio stays ~1 through the AM dips,
 * unlike a bare power-presence detector which resets on every dip. It fires on
 * a plain unmodulated 2100 Hz tone too (so we still interoperate with peers
 * that answer with plain ANS/CED). A separate slow power envelope is tracked
 * only to *classify* the tone as AM'd (ANSam -> peer offered V.8) or flat
 * (plain CED -> peer will not do V.8); classification is advisory/logged. */
typedef struct {
    uint32_t phase;                   /* 2100 Hz downconvert oscillator      */
    int32_t  rate;
    nf_cpx_t window[NF_V8_ANSAM_SPAN];/* sliding correlator taps             */
    nf_cpx_t dot;                     /* running complex sum                  */
    float    e_window[NF_V8_ANSAM_SPAN];/* sliding x^2                        */
    float    e_dot;                   /* running energy sum                  */
    int      buf_ptr;
    int      filled;                  /* samples until the window is primed   */
    float    floor_pwr;               /* absolute mean-power floor            */
    int      present_run;             /* consecutive samples with tone present */
    /* AM-vs-flat classification (advisory) */
    nf_power_t env_pm;                /* slow envelope meter (tracks 15 Hz AM) */
    int32_t  env_min, env_max;
    int      am_win;                  /* samples into the current AM window   */
    int      am_class;                /* -1 unknown, 0 flat (ANS), 1 AM (ANSam) */
} nf_v8_ansam_rx_t;

/* ── the V.8 engine ──────────────────────────────────────────────────── */

typedef struct nf_v8_s {
    int calling_party;
    nf_v8_result_handler_t handler;
    void *handler_user;
    int reported;

    int state;
    int32_t timer;                    /* samples left in the current phase (or -1 = unbounded) */
    int32_t ci_gap;                   /* caller: samples left in the inter-CI-burst gap */

    uint32_t our_modulations;
    int32_t  our_call_function;
    nf_v8_result_t result;

    nf_v8_fsk_tx_t   fsk_tx;
    nf_v8_fsk_rx_t   fsk_rx;
    nf_v8_ansam_tx_t ansam_tx;
    nf_v8_ansam_rx_t ansam_rx;
} nf_v8_t;

/* calling_party: 1 = caller (sends CI to solicit ANSam, then CM/CJ, listens
 *                    for ANSam then JM);
 *                0 = answerer (sends ANSam/JM, listens for CM then CJ). */
void nf_v8_init(nf_v8_t *s, int calling_party, uint32_t our_modulations,
                int32_t our_call_function,
                nf_v8_result_handler_t handler, void *user);

int nf_v8_tx(nf_v8_t *s, int16_t *amp, int max_len);
int nf_v8_rx(nf_v8_t *s, const int16_t *amp, int len);

#endif /* NF_V8_H */
