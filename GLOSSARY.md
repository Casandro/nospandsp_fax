# Glossary

The T.30 / T.38 / V-series vocabulary this codebase assumes you already know.
Alphabetical. See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit.

## Call phases (T.30)

- **Phase A** — call setup (dialling, CNG/CED tones, answer).
- **Phase B** — pre-message negotiation: capabilities (DIS) → parameters (DCS)
  → training check (TCF). Re-entered mid-document after an **EOM**.
- **Phase C** — in-message: the actual page image is sent over the fast modem.
- **Phase D** — post-message: end-of-page/document signalling and its
  acknowledgement (MPS/EOM/EOP → MCF/RTP/RTN).
- **Phase E** — call release (DCN, hang up).

## Signalling (FCF = Facsimile Control Field; the frame's command byte)

- **CNG** — Calling tone (1100 Hz), the caller "I am a fax" beep.
- **CED** — Called station identification tone (2100 Hz), the answerer's.
- **NSF / NSS / NSC** — Non-Standard Facilities/Setup/Command (vendor-specific).
- **CSI / CIG / TSI** — Called / Calling / Transmitting Subscriber Identification
  (the station-ID string frames).
- **DIS** — Digital Identification Signal: the answerer's *capabilities* (bit
  field: resolutions, rates, ECM, colour…). Built in `build_dis`.
- **DTC** — Digital Transmit Command: DIS sent by a station that wants to
  *poll* (receive) a document. Same skeleton as DIS with a flag set.
- **DCS** — Digital Command Signal: the *chosen* parameters for this session
  (a subset of what DIS offered). Built in `build_dcs`.
- **TCF** — Training Check: a burst of zeros the sender transmits at the chosen
  rate so the receiver can confirm the line supports it. Fail → **FTT**.
- **CFR** — Confirmation to Receive: "training OK, send the page."
- **FTT** — Failure To Train: "drop a rate and retry TCF."
- **MPS** — Multi-Page Signal: page done, another follows at the same params.
- **EOM** — End Of Message: page done; return to **Phase B** to renegotiate
  (e.g. a resolution change) before the next page.
- **EOP** — End Of Procedure: last page; end the document.
- **MCF** — Message Confirmation: "page received OK."
- **RTP** — Retrain Positive: page OK, but retrain before the next.
- **RTN** — Retrain Negative: page rejected (too many bad lines); retransmit.
- **DCN** — Disconnect: end the call.
- **PPS** — Partial Page Signal (ECM): end of a block; carries a second FCF
  (NULL/MPS/EOM/EOP) saying what comes next.
- **PPR** — Partial Page Request (ECM): a 32-octet bitmap of which frames in the
  block were bad and must be resent.
- **RNR / RR** — Receiver Not Ready / Receiver Ready (ECM flow control).
- **CTC / CTR** — Continue To Correct / response (ECM): drop to a lower rate and
  keep retransmitting the block.
- **EOR / ERR** — End Of Retransmission / response (ECM): give up retransmitting
  this block at this rate.

## Modes and codings

- **ECM** — Error Correction Mode (T.30 Annex A): image carried in 256-octet
  HDLC frames with CRC; bad frames are re-requested via PPR. Reliable but
  chatty. Without it ("non-ECM"), damaged scan lines are concealed and a page
  with ≥15 % bad lines is rejected with RTN.
- **FCD** — Facsimile Coded Data: an ECM data frame (the 256-octet payload).
- **RCP** — Return to Control for Partial page: marks the end of an ECM block.
- **BFT** — Binary File Transfer: send arbitrary files instead of an image
  (a private profile here, over ECM).
- **T.4** — the G3 bilevel image codec: **MH** (Modified Huffman, 1-D) and
  **MR** (Modified READ, 2-D).
- **T.6** — MMR (Modified Modified READ): pure 2-D coding, ECM only.
- **T.42** — the continuous-tone colour representation (CIELAB) carried as
  **T.81** (baseline **JPEG**). "Annex E" colour fax.
- **Annex E / Annex F** — T.30 Annex E = continuous-tone colour; Annex F = fax
  over V.34 ("Super G3").
- **Minimum scan line time** — the receiver's per-line processing floor (DIS
  bits 21–23); the sender pads short lines with T.4 FILL bits to honour it.
- **Copy quality / bad lines** — non-ECM: a decoded line that fails is replaced
  by the previous one ("concealed") and counted; the count drives RTN.

## Modems (ITU-T V-series)

- **V.21** — 300 bps FSK; the low-speed **control channel** that carries all the
  HDLC signalling frames above.
- **V.27ter** — 2400 / 4800 bps (the slow fast-modem fallback).
- **V.29** — 7200 / 9600 bps.
- **V.17** — 7200–14400 bps, trellis-coded (8-state Viterbi), with a short-train
  resync for warm receivers.
- **V.34** — up to 33600 bps; "Super G3" fax when paired with T.30 Annex F.
- **V.8** — the modulation-negotiation handshake (CM/JM/CJ, ANSam) that selects
  V.34 before T.30 begins.
- **short / long train** — V.17 training length: long for a cold receiver (TCF),
  short for image bursts after CFR (warm receiver).

## Framing and transport

- **HDLC** — the bit-stuffed, flag-delimited, CRC-16/X.25 frame format all the
  signalling above travels in (`nf_hdlc.c`). Info field starts after the
  ADDR+CTL+FCF header — `T30_INFO_OFF` (= 3) in `nf_wire.h`.
- **T.38** — fax over IP: the T.30 signalling and image data repackaged as
  **IFP** (Internet Facsimile Protocol) packets instead of modem audio.
- **IFP** — one T.38 message: either an *indicator* (carrier/tone state) or a
  *data* field (HDLC or non-ECM image octets). Encoded in `nf_t38.c`.
- **UDPTL** — the UDP transport for IFP (T.38 Annex B), with a redundancy scheme
  that repeats recent packets so loss is recoverable (`nf_udptl.c`).
- **RTP** — the media transport for the audio (G.711 A-law) path (`sip.c`).
- **SIP / SDP** — session signalling / media description for the IP call setup.
- **dBm0** — the reference power level; a full-scale (±32767) sine is +3.14 dBm0
  in the spandsp-matched convention `nf_dsp` uses.
