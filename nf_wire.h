#ifndef NF_WIRE_H
#define NF_WIRE_H

/*
 * Shared T.30/T.38 wire vocabulary — the protocol constants more than one
 * module has to agree on. Before this header existed each file re-derived
 * these values (a bare 0x8C annotated with an FCF_MCF comment in nf_t38.c, the
 * literal 3 for the frame header offset scattered through nf_t30.c). Keep the
 * one-definition rule: any NEW wire constant that a second file needs goes
 * HERE first, rather than being re-derived at the call site.
 */

/* HDLC address / control octets (T.30 §5.3). */
#define ADDR            0xFF
#define CTL_FINAL       0x13
#define CTL_NONFINAL    0x03

/* Octets preceding a frame's information field: ADDR + CTL + FCF. Indexing a
 * frame's payload therefore starts at buf[T30_INFO_OFF]. */
#define T30_INFO_OFF    3

/* FCF base values (t30_fcf.h). Dispatch is on (fcf | 0x01) to ignore the X bit. */
#define FCF_DIS 0x80
#define FCF_DCS 0x82
#define FCF_CSI 0x40
#define FCF_CIG 0x41     /* calling subscriber id (before DTC, polling)      */
#define FCF_TSI 0x42
#define FCF_CFR 0x84
#define FCF_FTT 0x44
#define FCF_MCF 0x8C
#define FCF_RTP 0xCC     /* retrain positive: page OK, but retrain          */
#define FCF_RTN 0x4C     /* retrain negative: page rejected, retransmit     */
#define FCF_EOP 0x2E
#define FCF_MPS 0x4E
#define FCF_EOM 0x8E
#define FCF_DCN 0xFA
/* ECM (T.30 Annex A) */
#define FCF_PPS 0xBE     /* partial page signal                       */
#define FCF_PPR 0xBC     /* partial page request (32-octet frame map) */
#define FCF_RNR 0xEC     /* receiver not ready                        */
#define FCF_RR  0x6E     /* receiver ready                            */
#define FCF_CTC 0x12     /* continue to correct                       */
#define FCF_CTR 0xC4     /* response to CTC                           */
#define FCF_EOR 0xCE     /* end of retransmission                     */
#define FCF_ERR 0x1C     /* response to EOR                           */
#define FCF_NULL 0x00

#endif /* NF_WIRE_H */
