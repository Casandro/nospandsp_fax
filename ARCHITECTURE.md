# Architecture

How a fax call moves through the modules. New to the codebase? Read this first,
then keep [GLOSSARY.md](GLOSSARY.md) open for the T.30/T.38/V-series jargon.

## The one fact that unlocks the repo

`nf_t30.c` — the ~2300-line T.30 protocol engine — **does not know or care which
physical layer it is talking to.** It drives a *backend* through a small vtable,
`nf_modem_ops_t` (defined in `nf_fax.h`), and reads results back through
`nf_fax_iface_t` (the "up" callbacks). Two backends implement that vtable:

- **`nf_fax.c`** — the V-series **audio** modems (real 8 kHz PCM samples).
- **`nf_t38.c`** — **T.38** fax-over-IP (IFP packets over UDPTL).

That is why you will find the same eight operations (`set_tx_type`,
`set_rx_type`, `send_hdlc`, `begin_hdlc_stream`, `set_transmit_on_idle`,
`tx`, `rx`, `free`) implemented twice, in two files, under two names
(`nf_fax_ops()` / `nf_t38_ops()`). `nf_t30` calls whichever one is installed.
`tx`/`rx` (audio samples) are NULL for the T.38 backend — it is pumped by its
own UDPTL I/O instead.

```
                        ┌───────────────────────────────────────┐
   TIFF / PAM / JPEG ──►│  nf_t4 (T.4/T.6 image codec)           │
   colour / binary  ──►│  nf_color (sRGB↔CIELAB↔JPEG, T.42)      │
                        └───────────────┬───────────────────────┘
                                        │ pages / ECM frames
                        ┌───────────────▼───────────────────────┐
                        │  nf_t30   T.30 protocol engine         │
                        │  (phases A–E, DIS/DCS, TCF, ECM,       │
                        │   non-ECM copy-quality, colour, BFT)   │
                        └──┬──────────────────────────────────┬──┘
        nf_modem_ops_t     │   (down: pick modem, send HDLC)  │   nf_fax_iface_t
        ───────────────────┤                                  ├──────────────────
                        ┌──▼───────────────┐          ┌───────▼──────────────┐
                        │ nf_fax (audio)   │          │ nf_t38 (fax-over-IP) │
                        │  owns the modems │          │  IFP encode/decode   │
                        └──┬───────────────┘          └───────┬──────────────┘
              ┌────────────▼───────────────┐                  │
              │ nf_v21 (FSK control chan)  │          ┌───────▼──────────────┐
              │ nf_qam ─ nf_v17/v29/v27ter │          │ nf_udptl (T.38 Annex │
              │ nf_hdlc (framing)          │          │ B length/redundancy) │
              │ nf_v8 / nf_v34 (Super-G3)  │          └───────┬──────────────┘
              │ nf_dsp (DDS, RRC, meters)  │                  │
              └────────────┬───────────────┘                  │
                  16-bit PCM │ 8 kHz mono            UDPTL datagrams │
                        ┌────▼──────────────────────────────────────▼────┐
                        │  Transport (chosen in sip_fax.c)                │
                        │  • TCP pipe: raw PCM over one socket            │
                        │  • SIP/RTP:  G.711 A-law media  (sip.c)         │
                        │  • UDPTL:    T.38 datagrams      (sip.c)        │
                        └─────────────────────────────────────────────────┘
```

## The layers, top to bottom

- **`sip_fax.c`** — the CLI `main()`. Parses options, loads/saves the TIFF/PAM
  image, chooses the transport (TCP pipe / SIP-RTP / T.38-UDPTL) and the fax
  role (`--send`/`--receive`), and runs the sample/datagram pump loop. The
  daemon mode (one child per inbound call) lives here too.
- **`sip.c` / `sip_util.c`** — a deliberately primitive SIP/SDP stack and the
  RTP and UDPTL sockets. `sip_util.c` holds the shared helpers (MD5 for Digest
  auth, the RNG, the header/SDP parsers).
- **`nf_t30`** — the protocol brain. Negotiation (DIS/DCS/DTC), training check
  (TCF), page signalling (MPS/EOM/EOP), ECM (PPS/PPR/CTC/EOR ladder), non-ECM
  copy-quality (RTN on ≥15 % bad lines), and the colour / binary-file profiles
  carried over ECM. It is backend-agnostic (see above).
- **Backends** — `nf_fax` (audio modems) and `nf_t38` (IFP/UDPTL). Both call the
  same `nf_fax_iface_t` up-callbacks, so `nf_t30` reacts identically either way.
- **Modem DSP** (under `nf_fax`) — `nf_v21` (300 bps FSK control channel),
  `nf_qam` (the shared fast-modem engine) with `nf_v17`/`nf_v29`/`nf_v27ter` on
  top, `nf_hdlc` (CRC-16/X.25 framing), `nf_v8`+`nf_v34` (Super-G3), and
  `nf_dsp` (DDS, RRC design, power meter, dBm0). `nf_wire.h` holds the shared
  T.30/T.38 wire constants (FCF codes, frame offsets).

## How a call flows

**Audio path (default).** `nf_t30_init()` → the engine installs `nf_fax_ops()`
and drives it with `nf_t30_tx()` / `nf_t30_rx()` (16-bit PCM, 8 kHz mono). The
caller sends CNG, the answerer CED then V.21 DIS; they negotiate, run TCF, then
carry image data over the fast modem (or V.34), page by page, until EOP → MCF →
DCN.

**T.38 path.** Call `nf_t30_t38_enable()` right after `nf_t30_init()`; the engine
swaps in `nf_t38_ops()`. Now you feed it `nf_t30_t38_rx_datagram()` per received
UDPTL datagram and call `nf_t30_t38_pump()` on a periodic tick, instead of the
audio `tx`/`rx`. The same T.30 state machine runs; only the physical layer
differs.

## Where to look for X

| You want to change… | Start in |
|---|---|
| Negotiation / capabilities (DIS/DCS bits) | `nf_t30.c` `build_dis`/`build_dcs` |
| The T.30 state machine | `nf_t30.c` `on_hdlc` / `on_status` |
| Image compression | `nf_t4.c` (bilevel), `nf_color.c` (colour) |
| A modem's training / constellation | `nf_v17.c` / `nf_v29.c` / `nf_v27.c` on `nf_qam.c` |
| T.38 packet format | `nf_t38.c` (IFP) + `nf_udptl.c` (framing) |
| SIP / SDP / RTP | `sip.c`, `sip_util.c` |
| A shared wire constant (FCF, offset) | `nf_wire.h` — **add new ones here first** |

## Tests

`make check*` targets cross-check against spandsp as an oracle (offline). See the
Makefile: `check` (T.4 codec), `check-fax`/`check-ecm` (T.30 interop),
`check-color` (colour + binary transfer), `check-t38` (T.38), `check-md5` (Digest
hash KAT), and `make asan` for a sanitizer pass over all of them.
