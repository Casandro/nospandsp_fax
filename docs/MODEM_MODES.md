# Fax modem modes

This is a reference for every modem mode the fax stack in this repository
implements, and how they are selected. It is derived from the code, not from
the ITU specs in the abstract — each section cites where the behaviour lives.

The stack speaks two fax profiles over an audio (G.711) leg, plus a packet
transport:

- **Classic G3 / Super G3 over audio** — the real modem waveforms, generated
  and demodulated by this code (`nf_v21.c`, `nf_v27.c`, `nf_v29.c`,
  `nf_v17.c`, `nf_v34.c`), driven by the T.30 state machine (`nf_t30.c`) via
  the audio backend (`nf_fax.c`).
- **T.38 (fax-over-IP)** — the same T.30 procedure, but the modem signals are
  carried as UDPTL/IFP packets instead of audio (`nf_t38.c`, `nf_udptl.c`).
  No waveform is generated; each modem "mode" becomes a T.38 indicator/data
  type.

The internal modem selector is `enum { NF_MODEM_* }` in `nf_fax.h`.

---

## 1. Call setup: tones and negotiation

| Mode | Direction | Signal | Purpose | Code |
|------|-----------|--------|---------|------|
| `NF_MODEM_CNG` | caller | 1100 Hz calling tone | "a fax is calling" | `nf_fax.c` |
| `NF_MODEM_CED` | answerer | 2100 Hz answer tone (plain, or AM'd = ANSam) | "a fax is answering" | `nf_fax.c`, `nf_v8.c` |
| `NF_MODEM_V8` | both | V.8 CI / ANSam / CM / JM / CJ | negotiate whether to use V.34 (Super G3) | `nf_v8.c` |

Two setup paths:

- **Classic G3** — caller sends CNG, answerer sends CED, then both drop to the
  V.21 control channel and exchange DIS/DCS. This is the path taken when V.34
  is disabled (`--no-v34`) or the peer is not V.8-capable. A caller with V.34
  disabled deliberately behaves as a plain-G3 terminal: it ignores ANSam and
  waits for DIS (see `nf_t30.c` `start()`).
- **T.30 Annex F (V.8)** — used only when V.34 is enabled. ANSam (an amplitude-
  modulated 2100 Hz tone) advertises V.8; the caller answers with CM, the
  answerer with JM, the caller closes with CJ. The negotiated modulation set
  is the AND of both ends' capabilities. If V.8 selects V.34 HDX the call
  continues into the V.34 session; otherwise it falls back to classic G3.

The ANSam detector classifies AM vs a flat CED tone; a flat tone means the
peer will not do V.8 and the caller falls back immediately so it catches the
answerer's first DIS (`nf_v8.c`).

---

## 2. Control channel (classic G3): V.21

| Mode | Rate | Modulation | Use |
|------|------|-----------|-----|
| `NF_MODEM_V21` | 300 bit/s | FSK, HDLC frames | all T.30 phase-B/C/D signalling (DIS, DCS, CFR, PPS, MCF, DCN, …) |

Half-duplex, 300 bit/s FSK carrying HDLC frames. This is the control channel
for classic G3 only — in a V.34 call the control channel is a V.34 QPSK
channel instead (see §4). Implemented in `nf_v21.c`; framing in `nf_hdlc.c`.

### Station identifiers (TSI / CSI / CIG)

In phase B each side may announce a 20-character station identifier (usually
the fax number), sent as an HDLC frame immediately before its DIS/DCS/DTC in
the same carrier burst:

| Frame | Sender | Precedes | Local option |
|-------|--------|----------|--------------|
| **TSI** | transmitter | DCS | `--ident` |
| **CSI** | answerer | DIS | `--ident` |
| **CIG** | poll caller | DTC | `--ident` |

The local identifier is set with `--ident <str>` (max 20 chars; default
`"sip_fax"`; an empty string sends none). The FIF is plain ASCII, left-
justified and space-padded — with the identifier characters in **reverse
order** (T.30 §5.2.2 / spandsp convention; e.g. id "91182" travels as
"28119"), the per-octet bit order untouched (verified against a real
machine). Our local id is sent over **classic G3 (audio)** — streamed with
the DIS/DCS frame — and **T.38** (as IFP), but **not** over the V.34 control
channel: a real SG3 machine times out on a two-frame TSI+DCS burst there, so
V.34 sends DCS/DIS/DTC alone. The **remote**
station id is captured, logged at Phase E, embedded in the received TIFF's
`ImageDescription`/`Software`/`DateTime` tags, and — in `--daemon` mode —
written to a `<base>.meta` sidecar alongside the `.tiff`/`.invite`. See
`build_id_frame`/`send_ctrl_burst`/`capture_far_ident` in `nf_t30.c`.

---

## 3. Image (message) modems — classic G3

The page image (and, in ECM, the FCD frames) is carried by one of three
message modems. The negotiated rate is chosen from a fallback ladder,
`FB[]` in `nf_t30.c`, highest first, restricted to the modems both ends
allow:

| Rate (bit/s) | Modem | DCS speed bits | Notes |
|-------------:|-------|----------------|-------|
| 14400 | `NF_MODEM_V17` | DISBIT6 | |
| 12000 | `NF_MODEM_V17` | DISBIT6 \| DISBIT4 | |
|  9600 | `NF_MODEM_V17` | DISBIT6 \| DISBIT3 | preferred 9600 |
|  9600 | `NF_MODEM_V29` | DISBIT3 | |
|  7200 | `NF_MODEM_V17` | DISBIT6 \| DISBIT4 \| DISBIT3 | preferred 7200 |
|  7200 | `NF_MODEM_V29` | DISBIT4 \| DISBIT3 | |
|  4800 | `NF_MODEM_V27TER` | DISBIT4 | |
|  2400 | `NF_MODEM_V27TER` | 0 | lowest fallback |

Per-modem summary:

- **V.27ter** (`nf_v27.c`) — 2400 and 4800 bit/s.
- **V.29** (`nf_v29.c`) — 7200 and 9600 bit/s.
- **V.17** (`nf_v17.c`) — 7200, 9600, 12000 and 14400 bit/s, with the
  long/short training-sequence variants T.30 uses for retrains.

All three are the standard T.4/T.30 half-duplex image carriers; they run the
page after DCS/training, with V.21 handling the surrounding control frames.

---

## 4. Super G3: V.34 half-duplex (`NF_MODEM_V34`)

V.34 (ITU-T V.34 half-duplex, used by T.30 Annex F) is the "Super G3" mode.
It is engaged only when V.8 negotiates `V34HDX`. Implementation is
`nf_v34.c` / `nf_v34.h`; the whole clause-12 half-duplex session (Phase 2
tone handshake, Phase 3 training, control-channel start-up, primary-channel
image transfer, and Figure-25/26 renegotiation) lives there.

**Symbol (baud) rates** — six, per `enum NF_V34_RATE_*` (`nf_v34.h`) and the
carrier table in `nf_v34.c`:

| Index | Symbol rate (baud) |
|------:|--------------------|
| 0 | 2400 |
| 1 | 2743 |
| 2 | 2800 |
| 3 | 3000 |
| 4 | 3200 |
| 5 | 3429 (= 24000/7 exactly) |

**Primary-channel data rates** — 2400 to 33600 bit/s in 2400 bit/s steps
(the `R` rows of the Table-8 data-rate maps in `nf_v34.c`):

    2400, 4800, 7200, 9600, 12000, 14400, 16800,
    19200, 21600, 24000, 26400, 28800, 31200, 33600

The set actually reachable depends on the negotiated symbol rate; the top
rate offered defaults to the highest the symbol rate supports (up to 33600)
and can be capped with the `NFV34MAXRATE` environment variable. The mask of
available primary rates per symbol rate is `nf_v34_rate_mask()`.

**Control channel** — inside a V.34 call the T.30 frames (DIS/DCS/…) travel
on a V.34 control channel, not V.21: 600-baud DPSK on a ~1200 Hz carrier
(call side) / 2400 Hz carrier with a 1800 Hz guard tone (answer side), at
either **1200 bit/s** (4-point QPSK) or **2400 bit/s** (16-point), selected
during start-up. See `nf_v34.h` (`cc_rate`) and the control-channel demod in
`nf_v34.c`.

`nf_fax.c` maps T.30's channel choice onto the session: any negotiated rate
above 2400 bit/s means the V.34 **primary** channel, ≤2400 means the
**control** channel, 0 means start-up (`v34_sub_mode()`).

---

## 5. What is advertised, and how to control it

`v8_capability_mask()` in `nf_t30.c` builds the V.8 CM/JM modulation set the
stack offers:

- **V.21** — always.
- **V.17**, **V.29**, **V.27ter** — when the corresponding modem is allowed.
- **V.34 HDX** — only when V.34 is enabled (`nf_t30_set_v34()` /
  `sip_fax --v34`, on by default).

`sip_fax` options that affect mode selection:

- `--v34` / `--no-v34` — offer or suppress V.34 (Super G3) in V.8. With
  `--no-v34` the caller acts as a plain-G3 terminal (ignores ANSam).
- `--require-v34` — abort (DCN, exit 1) rather than fall back to classic G3
  if V.8 does not settle on V.34.
- `--no-redial` — by default a dialed V.34 call that fails is redialed once as
  classic G3 (some SG3 machines advertise capabilities their V.34 stack can't
  actually receive); this opts out.
- `NFV34MAXRATE=<bit/s>` — cap the advertised V.34 primary rate.

Non-modem modulation choices negotiated separately (not modem modes, but
part of the same DIS/DCS exchange): ECM on/off, MH/MR/MMR (T.4 1-D / 2-D /
T.6) coding, resolution, and the T.42/T.81 colour and greyscale JPEG image
kinds.

---

## 6. T.38 (fax over IP)

Over a T.38 leg the same T.30 modems are represented as UDPTL/IFP
indicators and data types instead of generated waveforms (`nf_t38.c`):

| T.38 indicator / data type | Corresponds to |
|----------------------------|----------------|
| `CNG`, `CED` | calling / answer tone |
| `V21` | 300 bit/s control channel |
| `V27_2400`, `V27_4800` | V.27ter |
| `V29_7200`, `V29_9600` | V.29 |
| `V17_7200`, `V17_9600`, `V17_12000`, `V17_14400` (short/long train) | V.17 |

V.34/Super G3 is **not** carried over T.38 here — T.38 uses the classic-G3
modem set only. (V.34 is an audio-leg feature.)

---

## Summary

| Profile | Negotiation | Control | Image / data carrier |
|---------|-------------|---------|----------------------|
| Classic G3 (audio) | CNG/CED + V.21, or V.8 | V.21 300 bit/s | V.27ter 2400–4800, V.29 7200–9600, V.17 7200–14400 |
| Super G3 (audio) | V.8 (Annex F) → V.34 HDX | V.34 cc 1200/2400 bit/s | V.34 primary 2400–33600 bit/s, 6 symbol rates |
| T.38 (IP) | CNG/CED + V.21 | V.21 (as IFP) | V.27ter / V.29 / V.17 (as IFP) |
