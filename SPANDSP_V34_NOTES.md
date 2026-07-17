# Findings on spandsp's V.34 implementation

Notes from reverse-engineering ITU-T V.34 receive logic for `nf_v34`, using
spandsp's source (`spandsp-master/src/v34tx.c`, `v34rx.c`,
`spandsp/private/v34.h`) as read-only algorithmic reference material — not
copied into this project. Recorded here because the findings are
non-obvious, took real effort to establish, and matter for anyone continuing
this work (or debugging spandsp itself).

## The headline finding: spandsp's V.34 primary-channel receiver doesn't work

`v34rx.c` line 28 is a comment: `THIS IS A WORK IN PROGRESS - NOT YET
FUNCTIONAL!`. That isn't stale boilerplate — it's accurate. The receive path
for the **primary channel's** training signals (J, TRN, MP during Phase 3/4
startup, at the negotiated symbol rate) is structurally absent:

- `spandsp/private/v34.h:71-79` defines RX stages `V34_RX_STAGE_INFO0` …
  `INFOH` … `TONE_A/B` … `L1_L2` … `CC` … `PRIMARY_CHANNEL`. There is no
  `RX_STAGE_J`, `RX_STAGE_TRN`, or `RX_STAGE_MP`, unlike the TX side
  (`private/v34.h:126,128,132`: `V34_TX_STAGE_TRN`, `V34_TX_STAGE_J`,
  `V34_TX_STAGE_MP` all exist).
- `process_primary_half_baud()` (`v34rx.c:2298-2355`) — the per-baud handler
  for the primary channel — has its entire body (equalizer fetch,
  `decode_baud`, target selection) wrapped in `#if 0` (lines 2334-2354).
- The Viterbi/equalizer primitives it would need
  (`equalizer_get`/`tune_equalizer`/`track_carrier`/`put_bit`/`dist_sq`,
  `v34rx.c:1618-1781`) are all dead code too.

**Practical consequence:** don't trust spandsp's primary-channel J/TRN/MP
receive logic as a reference for anything — there isn't a working
implementation to check against. This cost significant time this session
before it was discovered; the discovery is what redirected the search to the
control channel instead (see below), which is where the real capture's
mid-call renegotiation signal actually was.

## What *is* real: the control channel (Annex A / 10.2.4) receiver

`process_cc_half_baud()` (`v34rx.c:1987-2164`), driven from `cc_rx()`
(`v34rx.c:2166-2295`), decodes MP/MPh control-channel messages via
differential 4-point DPSK, descrambling, sync search and CRC — and this path
*is* live code, not `#if 0`'d. It's what actually let us decode a real,
CRC-valid MP frame from `references/v.34_modem_test.wav`.

Caveat even here: the equalizer buffer insertion at the top of
`process_cc_half_baud` (`v34rx.c:2002-2006`) is *also* `#if 0`'d, and
`cc_symbol_sync()` (called once per full baud, `v34rx.c:2012`) wasn't
inspected in depth. Treat the framing/differential-decode/CRC logic below as
solid, cross-checked reference — but the surrounding front-end plumbing in
spandsp may itself be incompletely wired up. We did not reuse spandsp's
front end; `nf_v34_cc_rx_batch()` is our own (Gardner + 4th-power Costas),
validated directly against the real capture.

### Exact conventions extracted from the live code

**Bit packing order** (`v34rx.c:2014-2026`):
```
ang1 = arctan2(sample->re, sample->im)        /* current sample's angle */
ang2 = arctan2(last_sample->re, last_sample->im)
ang3 = ang1 - ang2 + 45°                       /* differential, pre-rotated 45° */
data_bits = ang3 >> 30                          /* top 2 bits = quadrant 0..3 */
for i in 0, 1:
    bits[i] = descramble(data_bits & 1)         /* LSB descrambled FIRST */
    data_bits >>= 1
```
So the quadrant code's **LSB is the first bit in time** (descrambled first),
MSB second. `bitstream = (bitstream << 1) | bits[i]` for `i=0` then `i=1` —
the LSB-derived bit becomes the *older* bit in the shift register, MSB-derived
the *newer*. Note spandsp doesn't use `I1`/`I2` naming here at all; this
mapping to the recommendation's naming is our inference, consistent with
"`I1` is first bit in time" in the spec text.

**Rotation sign convention.** No rotate function exists in the (dead)
training path. The live rotate primitives are only used in the main
trellis-coded data phase: `rotate90_clockwise` (`v34tx.c:1186-1210`) computes
`y.re=x.im, y.im=-x.re`, i.e. multiplying by `e^{-jθ}` — clockwise is a
**negative** angle in the standard real=x/imag=y, counterclockwise-positive
convention. `rotate90_counterclockwise` (`v34rx.c:386-412`) is the exact
algebraic inverse (`e^{+jθ}`). Index 0,1,2,3 maps to 0°,90°,180°,270°
consistently with that sign.

Separately, and this is the subtle one: `process_cc_half_baud` computes
angles via `arctan2(sample->re, sample->im)` — **arguments swapped** versus
the header's own `arctan2(y, x)` signature (`spandsp/arctan2.h:47`). Working
through the algebra: this swap is equivalent to `spandsp_angle = (90° -
standard_angle) mod 360°` — a *reflection* across the 45°/225° diagonal, not
a rotation. But because only the *difference* `ang1 - ang2` is ever used, the
reflection's effect on individual angles cancels into a clean result:
`spandsp_delta = -(standard_delta)`. In other words, despite looking like an
odd/arbitrary implementation detail, the net effect on the differential
decode is just a sign flip — equivalent to negating the naive quadrant-index
difference (matches this project's validated "rev" convention in
`nf_v34_mp_feed_symbol`).

**Differential state**: `s->last_sample` (`private/v34.h:716/722`) stores the
raw **received** (post-derotation) complex sample, updated every baud
(`v34rx.c:2162`, `2332`). No decision-directed regeneration, no phase
unwrapping — pure consecutive-sample differencing, exactly what
`nf_v34_mp_feed_symbol` does.

**CRC-16**: `crc_itu16_bits()` (`crc.c:172-187`), polynomial `0x8408`
(bit-reversed `0x1021`), LSB-first, **no complement anywhere**. Init:
`s->crc = 0xFFFF` (`v34rx.c:1265, 2055`). TX side confirms symmetry:
`crc_bit_block()` (`v34tx.c:345-360`) computes and appends the CRC **raw**,
no XOR (`v34tx.c:415-417`). This matches the recommendation's own text
("load the shift register with all ones, shift in the sequence, output the
register directly") exactly — no hidden final XOR/inversion, unlike the
*unrelated* byte-table `crc_itu16_append/check` elsewhere in spandsp (used
for HDLC), which does complement and checks against the magic residue
`0xF0B8`. **Do not confuse the two CRC helpers in spandsp** — they look
similar but behave differently.

One mechanism difference worth flagging: spandsp validates by continuing the
*same* running CRC register through the transmitted CRC field bits
themselves, then checking for a **zero residual** at the end (the classic
"append CRC, verify residual is zero" trick) — see `v34rx.c:2077,2089`. Our
own implementation (`nf_v34_mp_rx_t`) instead computes the CRC over the data
bits only and **compares it to the transmitted CRC value**. Both are
mathematically equivalent for the same polynomial/bit order, but if you're
cross-referencing the two implementations side by side, don't expect the
intermediate register states to match — only the final pass/fail verdict.

**MP/MPh frame sync search** (`v34rx.c:2053`): a free 19-bit sliding mask
over the descrambled bitstream: `(bitstream & 0x7FFFE) == 0x7FFFC`. Worked
out bit-by-bit, this requires bits 2-18 (17 bits) = all ones and bit 1 = 0 —
a run of 17 ones followed by a 0 start bit — with bit 0 (the very next bit)
read separately as the type flag (`v34rx.c:2059-2068`), selecting the
expected frame length (85 bits for Type 0, 187 for Type 1). This is exactly
the free sliding-window search this project's `search_mp()`/
`nf_v34_mp_feed_bit()` do — **no fixed symbol-count alignment from the end
of TRN is used or needed**. Start bits are skipped from the running CRC
every 17 bits (`s->mp_count % 17 != 0` gate, `v34rx.c:2077`).

**E sequence**: 20 consecutive ones (`bitstream & 0xFFFFF == 0xFFFFF`,
gated on `mp_seen==1`, `v34rx.c:2039`) marks the end of the MP/MP' exchange
and the start of real user data (duplex) or completion of MPh (half-duplex).
This matched exactly what we found in the real capture: four repeated,
identical MP frames immediately followed by a run of ones long enough to
contain the E sequence.

**Field byte layout**: every 8 collected bits, `info_buf[...] =
bit_reverse8(bitstream & 0xFF)` (`v34rx.c:2082`) — the stored field bytes are
**bit-reversed relative to the raw bitstream order**. This doesn't affect
sync detection or the CRC check (both operate directly on the pre-reversal
bit values), but matters if you go on to decode the *content* of fields
(rate masks, precoder coefficients, etc.) the way spandsp's
`process_rx_mp`/`process_rx_mph` do.

**MP vs MPh**: `s->duplex` selects `process_rx_mp` (Table 20/21 layout,
"MP") vs `process_rx_mph` (Table 23/24 layout, "MPh") — both share the
identical sync/CRC mechanism above, just different field layouts. This is a
duplex-vs-half-duplex distinction in the *terminology*, not in which
physical channel carries the message — our recording is a duplex V.34 fax
call and uses the "MP" (Table 20/21) layout even though it's carried over
what the recommendation's Annex A calls the "control channel".

## Why this mattered for `nf_v34`

Armed with the above, `nf_v34_mp_rx_t`/`nf_v34_mp_feed_symbol()` (see
`nf_v34.h`/`nf_v34.c`) decoded a real, fully self-consistent, CRC-16-valid
MP Type-0 frame from the actual recording at a mid-call rate-renegotiation
event: max rate 33600 bit/s, 16-state trellis (matching the trellis
decoder already built and validated independently this session), non-linear
encoding enabled, expanded constellation shaping, all rates 2400-33600
enabled, repeated four times identically and immediately followed by the
20-one E sequence — exactly the MP/MP'/E structure the recommendation
describes. See `make check-v34`'s `mphunt` mode for the regression check.
