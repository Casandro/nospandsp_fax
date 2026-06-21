# sip_fax (nospandsp variant)

⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️ This is slopware, made by an LLM. There is no reason to trust this code. I also claim no ownership of this code. It was created by a machine. I consider it to be public domain.

A fax CLI fully **weaned off [spandsp](https://www.soft-switch.org/)**,
cross-checked against the stock `../spandsp_fax` build via `../xcheck.sh` and
`../sweep.sh`. The entire fax engine is **our own code**; `sip_fax` builds and
links without spandsp (spandsp remains only as the test oracle in the offline
regression harnesses):

- **`nf_t4`** — ITU-T T.4 / T.6 image codec (MH 1-D, MR 2-D, MMR), bit-for-bit
  compatible with spandsp's T.4.
- **`nf_t30`** — a compact T.30 protocol engine (phases A–E, DIS/DCS
  negotiation, TCF, page signalling), wire-compatible with spandsp. It supports
  both non-ECM and **ECM** (Error Correction Mode, T.30 Annex A): the image is
  carried in 256-octet HDLC frames over the high-speed modem, with PPS/PPR
  partial-page retransmission. Non-ECM reception applies spandsp's copy-quality
  rule: damaged lines are concealed (previous line repeated) and counted, and a
  page with ≥15% bad lines (or no lines at all) is rejected with **RTN** — the
  sender retrains at a lower rate and retransmits (our sender does the same on
  receiving RTN, up to 2 retries). Honouring the receiver's **minimum scan
  line time** (DIS bits 21–23, T.4 FILL padding) is also implemented. Because the engine owns negotiation, it also
  carries two non-bilevel payloads over ECM: **colour fax** (T.30 Annex E / T.42
  CIELAB + T.81 JPEG) and **arbitrary binary files**.
- **`nf_color`** — the colour codec: sRGB ↔ CIELAB (D50) ↔ baseline JPEG
  (libjpeg), producing/consuming the T.42 codestream `nf_t30` sends over ECM.
- **`nf_fax`** — the driver that owns the modems, HDLC and fax tones (the role
  of spandsp's `fax.c` + `fax_modems.c`): it switches the active modem when the
  protocol layer asks, routes modem bits to/from HDLC or the non-ECM image
  path, and generates the CED/CNG tones.
- **`nf_v21`, `nf_qam` + `nf_v27`/`nf_v29`/`nf_v17`, `nf_hdlc`, `nf_dsp`** — the
  modem DSP layer, wire-compatible with spandsp's:
  - `nf_v21`: the 300 bps V.21 ch2 FSK control-channel modem (quadrature
    correlator rx with sync-mode baud tracking).
  - `nf_qam`: the shared fast-modem engine — polyphase root-raised-cosine
    tx/rx front ends (filters designed at init), AGC, T/2-spaced LMS
    equalizer, decision-directed carrier PI loop, and pluggable symbol timing
    recovery (Godard band-edge for V.17/V.29, Gardner for V.27ter).
  - `nf_v27`/`nf_v29`/`nf_v17`: the per-modem scramblers, training sequence
    state machines, constellations and decoders on top of `nf_qam` — V.17
    includes the 8-state trellis (Viterbi) decoder and short-train resync
    against the saved equalizer.
  - `nf_hdlc`: CRC-16/X.25 HDLC framing (bit stuffing, flags, underflow-driven
    ECM streaming).
  - `nf_dsp`: shared support — DDS, dBm0 conversions, power meter, RRC
    designer.

  Verified per-module against real spandsp by `make check-modem`
  (`nf_modemtest`): both directions per modem and rate, clean / A-law / AWGN,
  side-by-side receiver parity on identical impaired audio (`dualrx`), V.17
  short-train sequences, and tx level calibration. The end-to-end impairment
  acceptance gate is `../sweep.sh` (see below).

It interoperates with real spandsp in both directions (verified: `nf_t30` ↔
spandsp, pixel-perfect, in non-ECM and ECM). Regression harnesses: `make check`
(offline codec vs spandsp), `make check-fax` (full own-engine call + non-ECM
interop both ways), `make check-ecm` (ECM interop vs spandsp, clean and with
deliberately dropped image frames — see [Error correction mode](#error-correction-mode-ecm)),
`make check-color` (colour codec unit test + nf↔nf colour and binary-file
transfer, with frame loss — see [Colour fax & file transfer](#colour-fax--file-transfer)),
and the T.38 suites `make check-t38` (nf↔nf over UDPTL) / `make check-t38-interop`
(against spandsp's `t38_terminal`, both directions) / `make check-t38-gateway`
(against spandsp's re-modulating `t38_gateway` — see [T.38](#t38-fax-over-ip)).
spandsp 0.0.6 implements neither colour nor file transfer, so those two are
verified **nf↔nf** (between two instances of this build) rather than against th
spandsp oracle.

It offers two transports for the fax audio, and the fax role
(`--send` / `--receive`) is independent of both:

- **TCP pipe** — the two endpoints exchange **raw 16-bit signed little-endian
  linear PCM, mono, 8 kHz** over a single **TCP** connection. This is the exact
  sample format spandsp's T.30 engine produces and consumes, so there is no codec
  layer. Handy for line simulators and local testing.
- **SIP/RTP** — a real (if deliberately **primitive**) SIP call carrying
  **G.711 A-law (PCMA), 8 kHz** RTP media. One call per run, no threads; either
  place a call (UAC) or answer one inbound INVITE (UAS), optionally registering
  first. The signalling lives in `sip.c` / `sip_util.c` and is modelled on the
  fuller stack in `../sip_modem/sip_interface`.

## Build

`sip_fax` requires only `libtiff` and `libjpeg` development packages:

```sh
make
```

The offline regression harnesses (`make check`, `check-modem`, `check-fax`,
`check-ecm`) additionally need `spandsp` — purely as the cross-check oracle.

## Usage

The fax role (`--send` / `--receive`) is independent of the transport; pick
exactly one of each.

```
sip_fax ( --send <file> | --send-alt <res>:<file> ... | --receive <file.tiff>
        | --send-color <rgb.tiff> | --send-file <path> | --receive-file <path> )
        ( --listen <port> | --connect <host:port>     [TCP pipe]
        | --sip-dial <target> | --sip-answer )         [SIP/RTP]
        [--user sip:user@host] [--password <pw>] [--sip-port <port>]
        [--register] [--ident <str>] [--no-ecm] [--t38] [--color-quality <1..100>] [--verbose]
```

`--t38` opts in to **T.38** (fax over IP, the demodulated-T.30 transport); see
[T.38](#t38-fax-over-ip) below. Without it the behaviour is unchanged: G.711
audio, and any mid-call T.38 re-INVITE is declined with `488`.

**Send progress meter.** When transmitting (any transport), a progress display
estimates how much of the page is sent and the time remaining, from the bytes
of encoded image transmitted vs. the page size at the negotiated bit rate:
`page P/N  NN%  sent/total B  ETA M:SS`. On a terminal it updates one line in
place; when stderr is redirected/piped (or `--verbose` is set) it prints a
periodic line instead. Receiving is unaffected.

### TCP pipe

Sender listens, receiver connects (or any of the four combinations):

```sh
# terminal A: transmit a fax, waiting for a peer to connect
sip_fax --send doc.pam --listen 5000

# terminal B: receive into a TIFF
sip_fax --receive out.tiff --connect 127.0.0.1:5000
```

### SIP/RTP

One side answers an inbound call, the other dials it. SIP modes require
`--user sip:user@host`; `--password` (or `$SIP_PASSWORD`) supplies digest
credentials when the peer/registrar challenges.

```sh
# terminal A: answer the next inbound call and receive into a TIFF
sip_fax --receive out.tiff --sip-answer --user sip:fax@192.0.2.10

# terminal B: dial extension "fax" and transmit the page
sip_fax --send doc.pam --sip-dial fax --user sip:fax@192.0.2.10 --sip-port 5062
```

`--sip-dial <target>` accepts a full `sip:` URI, `user@host`, or a bare
user/number (resolved against the `--user` host). Add `--register` in answer
mode to REGISTER with the registrar before waiting for the call.

### Daemon mode (`--daemon`) — a concurrent inbound-fax spooler

`--sip-answer` handles exactly one inbound call and exits. `--daemon <spooldir>`
instead runs **forever**: it REGISTERs with the registrar (refreshing the
binding periodically so it stays reachable), answers **every** inbound call —
**several at once** — and writes each received fax into the spool directory.

```sh
sip_fax --daemon /var/spool/fax \
        --user sip:fax@192.0.2.10 --password secret \
        [--sip-port 5060] [--reg-interval 60] [--verbose]
```

Each received fax produces a pair of files named after one random token:

| file                  | contents                                              |
|-----------------------|-------------------------------------------------------|
| `<rand>.tiff`         | the received fax — a Group-4 multi-page TIFF (or an RGB/greyscale TIFF for a colour/grey page), exactly as `--receive` writes |
| `<rand>.invite`       | a verbatim copy of the call's original SIP INVITE     |

e.g. `a3f1c0d9e2b48157.tiff` alongside `a3f1c0d9e2b48157.invite`.

- **Receive-only.** The daemon advertises every resolution it can decode plus
  colour, and answers with G.711 audio; by default it declines mid-call T.38
  re-INVITEs with `488 Not Acceptable Here` and stays on audio. Add `--t38` and
  it accepts the gateway's T.38 switchover: the parent opens the UDPTL socket,
  answers 200, and passes that socket down to the per-call child (via `SCM_RIGHTS`
  over the control socketpair), which switches its media loop to T.38 mid-call
  (see [T.38](#t38-fax-over-ip)).
- **Concurrency.** Each call is handled in its own forked child (its own RTP
  socket and fax engine), so calls run fully in parallel and one call (or a
  crash) never stalls another. Up to 16 simultaneous calls; further inbound
  INVITEs get `486 Busy Here` until a slot frees. The parent process owns the
  single SIP socket and routes each call's in-dialog messages (ACK/BYE) by
  Call-ID; a finished or peer-ended call is torn down with BYE automatically.
- **Per-call log prefix.** With `--verbose`, every log line that belongs to a
  call is prefixed with that call's SIP **Call-ID** — `[<call-id>] …` — so the
  interleaved output of concurrent faxes (SIP signalling, `[ANSW]` T.30 trace,
  `Daemon:` status, `Phase E`) can be told apart. Lines that don't belong to a
  call (registration, "listening", shutdown) stay unprefixed. Single-shot
  `--sip-answer`/`--sip-dial` output is unchanged (no prefix).
- **Registration.** `--reg-interval <sec>` (default 60) sets how often the
  daemon re-REGISTERs; the REGISTER `Expires` is twice that. `--user` is
  required and `--password` (or `$SIP_PASSWORD`) is normally needed.
- **Shutdown.** On `SIGINT`/`SIGTERM` the daemon signals its children to finish,
  BYEs any live calls, de-registers (`Expires: 0`), and exits.

> The daemon assumes the registrar routes inbound calls for the registered AoR
> to it. The spool directory must already exist and be writable.

## Input formats (`--send`)

The format is detected from the file's magic number:

- **TIFF** (`II*`/`MM*`) — sent **as-is**, straight to spandsp's T.30 engine. Must
  be **bilevel** (1 bit/sample). A **multi-page TIFF transmits as a multi-page
  document** — spandsp sends each TIFF directory as one fax page. This is the
  native fax container and the recommended way to send documents (see
  [Multi-page documents](#multi-page-documents-pdf--tiff) below).
- **PBM** (`P4` binary / `P1` ASCII) — single page, converted to a fax TIFF.
- **PAM** (`P7`, `DEPTH 1`, `MAXVAL 1`) — single page, converted to a fax TIFF.

PBM/PAM are single-image formats and must be **bilevel and 1728 wide** (A4 at R8 /
204 dpi); they are converted to a temporary Group-4 TIFF before sending. The
receiving side always writes a standard Group-4 (CCITT T.6) TIFF, multi-page if
more than one page is received.

### Sending multiple resolutions (`--send-alt`)

A fax engine and the remote negotiate a resolution, and spandsp does **not** rescale
on transmit — it sends a TIFF at the resolution baked into its tags. So to make the
best use of a capable receiver while still working with a basic one, supply several
**pre-rendered** versions and let the tool pick:

```sh
sip_fax --send-alt fine:doc-fine.tiff \
        --send-alt 300:doc-300.tiff \
        --send-alt 400:doc-400.tiff \
        --sip-dial fax --user sip:fax@192.0.2.10
```

At call time the tool reads the receiver's advertised capabilities (its T.30 DIS)
and transmits the **highest-quality alternative the receiver can actually accept**,
falling back to the lowest supplied if it advertises none. `--send-alt` is
repeatable, mutually exclusive with `--send`, and each `<res>` is:

| `<res>`     | resolution        | required TIFF width |
|-------------|-------------------|---------------------|
| `standard`  | 204 × 98 dpi      | 1728                |
| `fine`      | 204 × 196 dpi     | 1728                |
| `superfine` | 204 × 391 dpi     | 1728                |
| `300`       | 300 × 300 dpi     | 2592                |
| `400`       | 400 × 400 dpi (R16) | 3456              |

Each `<file>` must be a **bilevel TIFF** of the matching width (rendered per the
recipes below); the tool cross-checks the width and warns on an unexpected vertical
resolution. Colour is not supported — the installed spandsp has no colour fax codecs,
so all input must be bilevel.

> The **receiving** side now advertises every bilevel resolution it can decode
> (standard … 400 dpi); a stock spandsp DIS omits 300/400 dpi, so without this a
> high-resolution sender could never negotiate up.

## Making a test image

Convert any image to a 1728-wide bilevel **PBM** (needs ImageMagick); this can be
sent directly:

```sh
convert input.png -resize 1728x -gravity center -background white \
        -extent 1728x -threshold 50% -monochrome pbm:- > doc.pbm
```

A bilevel **PAM** also works (e.g. piping the above through `pnmtopam`):

```sh
convert input.png -resize 1728x -gravity center -background white \
        -extent 1728x -threshold 50% -monochrome pbm:- \
  | pnmtopam -tupletype BLACKANDWHITE > doc.pam
```

Verify the header:

```sh
head -c 16 doc.pbm        # expect: P4 1728 300  (or P1 ...)
head -c 64 doc.pam        # expect WIDTH 1728 / DEPTH 1 / MAXVAL 1
```

## Sending a PDF (`pdffax.sh`)

The easiest way to fax a PDF is the wrapper:

```sh
./pdffax.sh doc.pdf --sip-dial fax --user sip:fax@192.0.2.10   # or --connect/--listen
```

It renders the PDF with Ghostscript into a full set of alternatives — colour
JPEG (T.42, 200 dpi), greyscale JPEG (T.81, 200 dpi) and bilevel Group-4 at
300 dpi / superfine / fine / standard — and offers them all in **one call**:
at negotiation time the best kind and resolution the receiver advertises in
its DIS wins (colour > greyscale > bilevel 400 > superfine > 300 > fine >
standard). A real bilevel-only machine gets the best bilevel rendition; an
nf-built receiver gets full colour. `PDFFAX_KEEP=1` keeps the rendered pages.

Under the hood this uses the option combination
`--send-alt … --send-color … --send-gray …`, which any caller can use
directly; with a single kind given, behaviour is unchanged (a colour-only
send to a non-colour receiver still ends as incompatible).

Add `--require-color` to forbid the fallback: if the receiver cannot take
colour, the call is aborted with DCN at phase B (exit code 1, nothing
transmitted) instead of dropping to greyscale or bilevel.

## Polling (pull a fax with T.30 DTC)

In a polled transfer the document flows the *other* way: the **answering**
station holds it and the **calling** station pulls it. The roles in T.30 are
the reverse of a normal call — the answerer transmits, the caller receives —
signalled by a DTC frame (a DIS with the X-bit set) and DIS bit 9 ("a document
is available for polling").

Serve a document for polling — it transmits the best version the *caller* can
receive, using the same `--send-alt/--send-color/--send-gray` selection as a
normal send:

```sh
# render + serve a PDF (waits for a caller to poll, then sends)
./pdffax.sh doc.pdf --poll-serve --sip-answer --user sip:fax@192.0.2.10
# or serve a single document
./sip_fax --poll-serve --send-alt fine:doc.tiff --send-color photo.tiff \
          --sip-answer --user sip:fax@192.0.2.10
```

Pull a document by calling a polling source:

```sh
./sip_fax --poll --receive out.tiff --sip-dial fax --user sip:me@192.0.2.10
```

The poll client advertises its full receive capabilities (resolutions, ECM,
colour) in the DTC, so a poll server running `pdffax.sh --poll-serve` sends
colour to a colour-capable caller and falls back to greyscale/bilevel
otherwise — exactly like a normal `pdffax.sh` send. Both transports work
(`--sip-answer`/`--sip-dial` or `--listen`/`--connect`).

### Per-mode source images (`fax_test.sh`)

`fax_test.sh` sends — or serves for polling — a *different source image per
negotiated mode*, rather than one document rendered every way. The direction
follows the transport: `--sip-dial`/`--connect` places the call and transmits
now; `--sip-answer`/`--listen` waits to be polled. Either way the best version
the far end supports is chosen at phase B. It scales each image up to the
required fax width and Floyd-Steinberg dithers the bilevel ones:


```sh
fax_test.sh --sip-dial fax --user sip:fax@192.0.2.10        # send now
fax_test.sh --sip-answer --register --user sip:fax@192.0.2.10  # serve a poll
fax_test.sh --connect 127.0.0.1:5000                        # send (TCP test)
fax_test.sh --listen 5000                                   # serve a poll (TCP test)
```

## Multi-page documents (PDF → TIFF)

To send a multi-page document, convert it to a single **multi-page Group-4 TIFF**
and `--send` that file directly — spandsp transmits one fax page per TIFF page.

The cleanest tool is **Ghostscript's `tiffg4` device**, which renders straight to
Group-4 and, because the output filename has no `%d`, writes every page into one
multi-page TIFF. Its `AdjustWidth` default rounds each page up to the standard fax
line width (1728 px for A4) and sets the non-square fax resolution tags:

```sh
gs -dBATCH -dNOPAUSE -dSAFER \
   -sDEVICE=tiffg4 \
   -r204x196 \
   -sPAPERSIZE=a4 -dPDFFitPage \
   -sOutputFile=doc.tiff \
   input.pdf

sip_fax --send doc.tiff --sip-dial fax --user sip:fax@192.0.2.10
```

Pick the resolution to match the fax mode you want to negotiate:

| Mode              | Ghostscript flag      | `--send-alt` key |
|-------------------|-----------------------|------------------|
| Standard (coarse) | `-r204x98`            | `standard`       |
| Fine              | `-r204x196`           | `fine`           |
| Superfine         | `-r204x391` (or `-r408x391`) | `superfine` |
| 300 dpi           | `-r300x300`           | `300`            |
| 400 dpi           | `-r400x400`           | `400`            |

At `-r300x300` the page is rendered at 300 dpi (width auto-adjusted, ~2592 px for
A4) and the TIFF carries 300/300 resolution tags. T.30 supports this inch-based
mode, but **the receiver must negotiate 300 dpi** or the resolutions will not match.

Rendering the same document at several of these resolutions gives you the inputs for
[`--send-alt`](#sending-multiple-resolutions---send-alt), which then auto-selects per
the receiver's capabilities instead of you having to guess.

Alternatively, ImageMagick (which itself rasterizes the PDF via Ghostscript) can
produce the same multi-page TIFF, with explicit control over thresholding:

```sh
convert -density 204x196 input.pdf \
        -resize 1728x -threshold 50% -monochrome \
        -compress Group4 doc.tiff
```

Force a clean bilevel result (`-threshold`/`-monochrome`) — PDFs usually rasterize
as grayscale, which fax cannot carry, and `--send` will reject a non-bilevel TIFF.

Verify before sending:

```sh
tiffinfo doc.tiff | grep -E 'Page|Image Width|Resolution|Compression'
# expect: Group 4, Width 1728 (at 204 dpi), one "Page N" block per page
```

## Error correction mode (ECM)

ECM (ITU-T T.30 Annex A) makes the image transfer reliable on a noisy line. The
T.6-coded page is split into **256-octet HDLC frames** sent over the high-speed
modem; the receiver checks each frame's CRC and asks for any bad/missing ones to
be re-sent (a *partial page request*, PPR) before confirming the page. This is
what real fax machines and ATAs use, and it is **on by default** here.

- It is negotiated only when **both** ends advertise it (DIS bit 27); against a
  non-ECM peer the call transparently falls back to the non-ECM path.
- The image is coded with **T.6 (MMR)** inside ECM (DIS/DCS bit 31), which is
  more compact than the 1-D/2-D coding used without ECM.
- Disable it with `--no-ecm` (e.g. to force the non-ECM path for comparison).

### Testing retransmission against spandsp

`make check-ecm` runs `nf_t30` against the stock spandsp fax engine (ECM + T.6
enabled on both) over an in-process audio loop, in both directions, and then
**deliberately drops image frames** to force the retransmission machinery to run
end-to-end — verifying the received TIFF is still pixel-identical to the source:

```sh
make check-ecm
# -- clean --                       nf2sp / sp2nf            pixeldiff 0
# -- nf sender drops tx frames 1,2  (spandsp issues a PPR)   pixeldiff 0
# -- nf receiver drops rx frames 0,3 (we issue a PPR)        pixeldiff 0
```

Frame loss is injected via two environment variables honoured by `nf_t30`, so
the harness exercises the *real* spandsp implementation on the other side:

| Variable           | Effect                                                              |
|--------------------|---------------------------------------------------------------------|
| `NF_ECM_DROP_TX`   | comma-list of FCD frame numbers the **sender** drops once (transit loss) — the receiver must PPR and the sender re-sends |
| `NF_ECM_DROP_RX`   | comma-list of FCD frame numbers the **receiver** discards once — `nf_t30` must build a PPR and the sender re-sends |

```sh
# our sender loses frames 1 and 2 on the first burst; spandsp asks for them back
NF_ECM_DROP_TX="1,2" ./nf_ecmtest nf2sp in.tiff out.tiff

# spandsp sends; we drop frames 0 and 3 on first receipt and PPR for them
NF_ECM_DROP_RX="0,3" ./nf_ecmtest sp2nf in.tiff out.tiff
```

## T.38 (fax over IP)

T.38 carries the **demodulated** T.30 protocol over IP instead of modulating it
into G.711 audio. The provider's media gateway demodulates the far end's modem
and ships the resulting HDLC frames and image bits as **IFP** packets over
**UDPTL**; we do the same on our side. This sidesteps the modem-over-RTP
fragility of audio fax entirely and is the path most ITSPs push via a mid-call re-INVITE.

T.38 here is **our own implementation** — `sip_fax` still links only
`-ltiff -ljpeg -lm`; no spandsp at runtime. The same `nf_t30` protocol engine
drives either the audio modems (`nf_fax`) or the T.38 backend (`nf_t38` +
`nf_udptl`) through a small vtable, so ECM, T.6 coding, rate fallback and the
TIFF/colour paths are all reused unchanged.

- **Opt-in.** Pass `--t38`. Without it nothing changes (audio + `488`).
- **Both roles.** As the answerer/daemon it **accepts** the gateway's T.38
  re-INVITE; as the dialer it **offers** T.38 (re-INVITE) and, if the gateway
  declines our offer but then offers its own, accepts that.
- **Profile.** T.38 version 0, **UDPTL with redundancy** (the common SIP
  profile; FEC is not produced), **transferred-TCF** rate management. The
  negotiated `T38FaxMaxDatagram` is honoured.
- **Bit order.** T.30 HDLC frame octets are bit-reversed for the LSB-first wire
  convention; non-ECM image octets are packed MSB-first — matching T.38 / spandsp
  exactly (verified by the interop oracle below).
- **Re-modulation friendly.** Carrier gateways turn our IFP back into a real modem
  signal for the far machine, which is timing-sensitive: image bursts use the
  negotiated *short* training, non-ECM data gets a zero trailer so the modem
  flushes cleanly, and HDLC frames are paced at the carrier bit rate (V.21 is only
  300 bps) with the carrier held up for the full play-out before signalling end.
  Without these the far end loses sync or never ACKs. All are exercised by the
  gateway regression below.

```sh
# receive over T.38 (primary use case) — accept the gateway's switchover
sip_fax --daemon /var/spool/fax --t38 \
        --user sip:fax@192.0.2.10 --password secret --register --verbose

# or a single inbound call
sip_fax --receive out.tiff --sip-answer --t38 --user sip:fax@192.0.2.10 --register

# send over T.38 — offer the switchover after the call is up
sip_fax --send doc.pam --sip-dial '**2' --t38 \
        --user sip:fax@192.0.2.10 --password secret --register
```

Set `NF_T38_DBG=1` to trace IFP indicators/fields (and the peer's T.38 SDP) on
stderr.

### Testing against spandsp (the T.38 oracle)

`make check-t38` runs two `nf_t30` engines over an in-process UDPTL link
(nf↔nf, no spandsp) — non-ECM and ECM, clean and with injected packet loss
(recovered by redundancy), plus a **lost-MCF** case (the post-message MCF is
dropped on the wire; the sender retransmits its PPS and the receiver
re-acknowledges, per T.30 §5) — all pixel-exact.

`make check-t38-interop` runs our T.38 backend against spandsp's real
`t38_terminal` over a UDPTL bridge, **both directions and both ECM modes**, and
checks every received page is pixel-exact:

```
-- nf SEND -> spandsp RECV, non-ECM --   send non-ecm pixeldiff OK
-- nf SEND -> spandsp RECV, ECM --       send ecm pixeldiff OK
-- spandsp SEND -> nf RECV, non-ECM --   recv non-ecm pixeldiff OK
-- spandsp SEND -> nf RECV, ECM --       recv ecm pixeldiff OK
```

This proves the IFP/UDPTL we emit is decodable by a genuine T.38 stack, and that
we correctly decode one — the same family as real carrier gateways. (spandsp is
linked only by this oracle, never by `sip_fax`.)

`make check-t38-gateway` goes one step further: our sender → spandsp's
**`t38_gateway`** (which *re-modulates* our IFP back into a V.17/V.29 audio modem
signal, exactly as a carrier gateway does) → spandsp's audio `fax` receiver, ECM
and non-ECM, pixel-exact. A plain terminal can't catch re-modulation bugs (it
decodes IFP directly); this one does, and it's what makes the live send to a real
provider work:

```
-- our sender -> spandsp gateway -> spandsp fax, non-ECM --   gw non-ecm pixeldiff OK
-- our sender -> spandsp gateway -> spandsp fax, ECM --       gw ecm pixeldiff OK
```

## Colour fax & file transfer

Owning T.30 negotiation lets the engine carry payloads the bilevel pipeline
can't. Both ride the **ECM** byte channel (so both require ECM — they refuse
`--no-ecm`) and are negotiated only when **both** ends advertise the capability.
spandsp 0.0.6 implements neither, so these interoperate **nf↔nf only** (between
two instances of this build, over TCP or SIP/RTP).

### Colour fax (T.30 Annex E / T.42 / JPEG)

A real continuous-tone colour fax: the sRGB image is converted to CIELAB (D50,
T.42 8-bit) and coded with baseline JPEG (T.81); negotiation sets DIS/DCS bits
15 + 27 + 68 + 69. The received page is written as an RGB TIFF.

```sh
# render your colour image to an RGB TIFF first, then send it
convert photo.png photo.tiff
sip_fax --send-color photo.tiff --connect 127.0.0.1:5000      # or --sip-dial ...
sip_fax --receive out.tiff      --listen 5000                 # writes an RGB TIFF
```

Colour is **lossy** (JPEG + the CIELAB round-trip), so the received image is
close, not identical; quality is `--color-quality <1..100>` (default 85). Highly
saturated colours are limited by the T.42 8-bit CIELAB gamut. A plain `--receive`
accepts colour automatically (it advertises the capability and writes RGB when a
colour page arrives). Colour input must be an **RGB TIFF** (convert other formats
first, as above).

**Greyscale** is the same mode with a single luminance (L\*) component — DCS bit
68 set, bit 69 clear — and is much higher fidelity (no chroma gamut limits):

```sh
sip_fax --send-gray photo.tiff --connect 127.0.0.1:5000   # 1-component T.81 JPEG
sip_fax --receive  out.tiff    --listen 5000              # writes a greyscale TIFF
```

A colour-capable receiver accepts greyscale too (it advertises both); the page is
written as an 8-bit greyscale TIFF.

### Arbitrary binary file transfer

Any file — a voicemail recording, a document, anything — carried **byte-exact**
over the ECM channel using a small private header (`NFFX1` + name + length).
Gated by the BFT capability bit; this is a private nf↔nf profile, not T.434.

```sh
sip_fax --send-file voicemail.wav --connect 127.0.0.1:5000   # or --sip-dial ...
sip_fax --receive-file out.wav    --listen 5000              # byte-identical copy
```

Because ECM verifies every frame and retransmits losses, the delivered file is
identical to the source even on a lossy link. `make check-color` proves this end
to end, including a run that drops FCD frames mid-transfer (PPR retransmission
still reconstructs the exact codestream / file).

## Regression testing

- `make check` — offline T.4 codec cross-check vs spandsp.
- `make check-modem` — the modem-layer oracle matrix vs real spandsp: per
  modem/rate/direction, with inline impairments (`--snr`, `--alaw`, `--gain`,
  `--foff`) and side-by-side receiver parity (`dualrx`).
- `make check-fax` / `check-ecm` / `check-color` — full-call suites (own engine
  loopback, interop with the spandsp fax engine both ways, ECM with injected
  frame loss, colour/file transfer).
- `../xcheck.sh` — the two `sip_fax` builds against each other over TCP in all
  four sender/receiver pairings.
- `../sweep.sh [--quick]` — the acceptance gate: both builds across a line
  impairment matrix (noise, frequency offset, tilt, group delay, THD, phase
  jitter, clock slip, gain) via `../line_sim`, all four pairings. Rule:
  wherever spandsp→spandsp completes, every pairing involving this build must
  complete at least as well.

## Inspecting a received fax

```sh
tiffinfo out.tiff               # Group-4, 1728 wide, MinIsWhite, one Page per page
tiff2pdf out.tiff -o out.pdf    # view all pages
```

## Notes / non-goals

- One call/connection, one fax per run. No reconnect/retry logic.
- `--receive` always writes TIFF.
- `--verbose` enables `nf_t30`'s own T.30 protocol logging (and one-line SIP
  traffic logging) for debugging.
- **Colour fax** is the T.42/T.81 (CIELAB + baseline JPEG) continuous-tone mode
  only; T.43/T.45 codings, greyscale-only mode, 12-bit components, higher colour
  resolutions, and non-default illuminant/gamut are not implemented. Colour and
  binary-file transfer are **nf↔nf** (no spandsp oracle; not tested against a
  physical colour fax machine). The binary-file profile is private, not T.434.
- **ECM scope:** the common path — FCD/RCP frames, PPS, PPR-driven
  retransmission, multi-block and multi-page — is implemented and interop-tested.
  The rarer escalation signals (CTC/CTR rate-drop, EOR/ERR, RNR/RR flow control)
  are handled minimally; `nf_t30` instead caps the retransmission rounds and, if a
  block still cannot be completed, disconnects. Clean-channel and single-loss
  cases (the usual ones) are covered.

### SIP stack scope

The SIP/RTP support is intentionally minimal — just enough to carry one fax:

- **PCMA (G.711 A-law) at 8 kHz only** — the format the fax engine speaks, with
  no resampling. T.38 is not used; this is audio (G.711 pass-through) fax.
- Single call, no threads; UAC (dial) or UAS (answer) with an optional single
  REGISTER. RTP media is paced against the wall clock at 20 ms.
- Best-effort signalling: one 401/407 digest retry and simple fixed
  retransmits, not the full RFC 3261 transaction timer machinery.
- IPv4 only. The fuller, threaded, multi-call/multi-codec implementation lives
  in `../sip_modem/sip_interface`.
