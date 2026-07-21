CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  += -ltiff -ljpeg -lm

# spandsp appears ONLY as a test oracle: the offline cross-check harnesses
# (nf_t4check, nf_interop, nf_ecmtest, nf_modemtest) compare our code against
# real spandsp. The fax engine itself - sip_fax and everything it links - is
# entirely our own code and builds without spandsp.
SPANDSP_CFLAGS = $(shell pkg-config --cflags spandsp)
SPANDSP_LIBS   = $(shell pkg-config --libs spandsp)

# The fax engine is all our own: nf_t4 codec, nf_t30 protocol, nf_fax driver,
# nf_color T.42/JPEG colour codec, and the modem DSP layer (nf_dsp support,
# nf_hdlc framing, nf_v21 FSK, nf_qam engine with the nf_v27/nf_v29/nf_v17
# modems on top).
OBJS = sip_fax.o sip.o sip_util.o g711.o nf_t4.o nf_t30.o nf_fax.o nf_color.o nf_dsp.o nf_hdlc.o nf_v21.o nf_qam.o nf_v29.o nf_v27.o nf_v17.o nf_v8.o nf_v34.o nf_udptl.o nf_t38.o

# The full fax-engine source set that every end-to-end test harness links (the
# sip_* front-end files are excluded — each harness supplies its own main()).
# Referenced by the nf_faxloop2/ctctest/interop/ecmtest/xfer/v34fax/t38* rules
# so a new engine module is added in one place, not nine.
ENGINE_SRCS = nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_v8.c nf_v34.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c

sip_fax: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

sip_fax.o: sip_fax.c sip.h sip_util.h nf_t30.h
sip.o:     sip.c sip.h sip_util.h g711.h
sip_util.o: sip_util.c sip_util.h
g711.o:    g711.c g711.h
nf_t4.o:   nf_t4.c nf_t4.h
nf_t30.o:  nf_t30.c nf_t30.h nf_fax.h nf_t4.h nf_color.h
nf_color.o: nf_color.c nf_color.h
nf_dsp.o:  nf_dsp.c nf_dsp.h
nf_hdlc.o: nf_hdlc.c nf_hdlc.h nf_dsp.h
nf_v21.o:  nf_v21.c nf_v21.h nf_dsp.h
nf_qam.o:  nf_qam.c nf_qam.h nf_dsp.h
nf_v29.o:  nf_v29.c nf_v29.h nf_qam.h nf_dsp.h
nf_v27.o:  nf_v27.c nf_v27.h nf_qam.h nf_dsp.h
nf_v17.o:  nf_v17.c nf_v17.h nf_qam.h nf_dsp.h
nf_v8.o:   nf_v8.c nf_v8.h nf_dsp.h
# nf_v34: the V.34 data pump + the half-duplex session driver (T.30 Annex F)
# that nf_fax drives via NF_MODEM_V34 - linked into everything nf_fax is.
nf_v34.o:  nf_v34.c nf_v34.h nf_v34_superconstellation.h nf_dsp.h nf_hdlc.h

nf_fax.o:  nf_fax.c nf_v8.c nf_v34.c nf_fax.h nf_dsp.h nf_hdlc.h nf_v21.h nf_v29.h nf_v27.h nf_v17.h nf_v34.h
nf_udptl.o: nf_udptl.c nf_udptl.h
nf_t38.o:  nf_t38.c nf_t38.h nf_udptl.h nf_fax.h

# ── Regression harnesses (spandsp is linked only as a test oracle) ──
# Harness sources live in tests/; engine sources/headers stay at the root, so
# the test rules compile tests/*.c against the root .c files and add -I. so the
# harnesses' #include "nf_*.h" resolves to the engine headers here.
# nf_t4check  : offline T.4 codec cross-check vs spandsp t4_tx/t4_rx (Stage 1)
# nf_faxloop  : nf_fax modem/HDLC/status loopback (Stage 2a)
# nf_faxloop2 : two nf_t30 engines run a full non-ECM fax (Stage 2b/2c)
# nf_interop  : nf_t30 against spandsp's own fax engine, both directions
tests/nf_t4check: tests/nf_t4check.c nf_t4.c nf_t4.h
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_t4check.c nf_t4.c $(SPANDSP_LIBS) $(LDLIBS)
tests/nf_faxloop: tests/nf_faxloop.c nf_fax.c nf_v8.c nf_v34.c nf_fax.h nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_faxloop.c nf_fax.c nf_v8.c nf_v34.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(LDLIBS)
tests/nf_faxloop2: tests/nf_faxloop2.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_faxloop2.c $(ENGINE_SRCS) $(LDLIBS)
tests/nf_ctctest: tests/nf_ctctest.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_ctctest.c $(ENGINE_SRCS) $(LDLIBS)
tests/nf_interop: tests/nf_interop.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_interop.c $(ENGINE_SRCS) $(SPANDSP_LIBS) $(LDLIBS)
# nf_ecmtest : nf_t30 (ECM) against spandsp (ECM + T.6), with frame-loss injection
tests/nf_ecmtest: tests/nf_ecmtest.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_ecmtest.c $(ENGINE_SRCS) $(SPANDSP_LIBS) $(LDLIBS)
# nf_colortest : offline unit test for the nf_color T.42/JPEG codec
tests/nf_colortest: tests/nf_colortest.c nf_color.c nf_color.h
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_colortest.c nf_color.c -ljpeg -lm
# nf_md5test : MD5 known-answer test (guards the SIP Digest hash in sip_util.c)
tests/nf_md5test: tests/nf_md5test.c sip_util.c sip_util.h
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_md5test.c sip_util.c -lpthread
# nf_xfer : nf<->nf colour and binary-file transfer over the full ECM engine
tests/nf_xfer: tests/nf_xfer.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_xfer.c $(ENGINE_SRCS) $(LDLIBS)
# nf_modemtest : per-module oracle tests of the nf modem layer vs real spandsp
tests/nf_modemtest: tests/nf_modemtest.c nf_fax.c nf_v8.c nf_v34.c g711.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c nf_dsp.h nf_hdlc.h nf_v21.h nf_qam.h nf_v29.h nf_v27.h nf_v17.h
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_modemtest.c nf_fax.c nf_v8.c nf_v34.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c g711.c $(SPANDSP_LIBS) $(LDLIBS)
# nf_v8test : V.8 handshake (ANSam/CM/JM/CJ) oracle test vs real spandsp v8_tx/v8_rx
tests/nf_v8test: tests/nf_v8test.c nf_v8.c nf_v8.h nf_dsp.c nf_dsp.h g711.c g711.h
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_v8test.c nf_v8.c nf_dsp.c g711.c $(SPANDSP_LIBS) $(LDLIBS)
# nf_v34test : V.34 harness - smoke tests plus the real-capture regression
# checks (control-channel demod, MP/MPh, INFO sequences, HDLC user data)
# plus the TX loopback checks (txsig/txinfo/txcc/txpage - our transmitter
# against our capture-validated receivers). nf_hdlc.c is linked for the
# HDLC framing on both sides; g711.c for the txpage impairment channel.
tests/nf_v34test: tests/nf_v34test.c nf_v34.c nf_v34.h nf_dsp.c nf_dsp.h nf_hdlc.c nf_hdlc.h g711.c g711.h
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_v34test.c nf_v34.c nf_dsp.c nf_hdlc.c g711.c -lm
# nf_v34fax : end-to-end fax over V.34 (T.30 Annex F) - two nf_t30 engines in
# audio loopback run the whole Super-G3 session (V.8 V.34HDX, clause-12
# startup, control-channel T.30, primary-channel ECM page) - see check-v34fax.
tests/nf_v34fax: tests/nf_v34fax.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_v34fax.c $(ENGINE_SRCS) $(LDLIBS)

# Fast, deterministic codec check.
check: tests/nf_t4check
	./tests/nf_t4check doc.pam

# MD5 known-answer test: guards the SIP Digest hash against silent regressions.
check-md5: tests/nf_md5test
	./tests/nf_md5test

# AddressSanitizer + UBSan pass over the offline suites. Rebuilds the harnesses
# instrumented (clean first, so no stale un-instrumented binary is reused) and
# runs the memory-safety-relevant ones: the T.4 codec, the T.30 ECM/colour/file
# paths, T.38/UDPTL parsing, the full fax loop, and the MD5 KAT.
#
# ASan is the hard gate: any heap-overflow / use-after-free / OOB aborts the run
# (this is what would have caught the two heap bugs fixed in the audit). UBSan
# runs in report (recover) mode: it currently prints KNOWN-BENIGN signed-overflow
# diagnostics from the modems' deliberate modular phase arithmetic (e.g.
# nf_v17.c phase-delta subtraction) — those wrap intentionally and are pending a
# separate UB-cleanup pass (unsigned-subtract-then-cast); they are not failures.
#
# Slower than a normal run; needs spandsp like the other checks. Run `make` again
# afterwards to restore the normal optimised build. (Needs a -fsanitize compiler.)
ASAN_CFLAGS = -Wall -Wextra -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(ASAN_CFLAGS)" check check-md5 check-color check-t38 check-fax check-ecm
	@echo "== ASAN/UBSAN: all instrumented suites passed =="

# Per-module modem-layer oracle matrix (grows with the spandsp replacement).
check-modem: tests/nf_modemtest
	./tests/nf_modemtest tones
	./tests/nf_modemtest hdlc
	./tests/nf_modemtest v21
	./tests/nf_modemtest v29
	./tests/nf_modemtest v27
	./tests/nf_modemtest v17
	./tests/nf_modemtest v17short

# V.8 handshake vs real spandsp, both roles, clean and A-law.
check-v8: tests/nf_v8test
	./tests/nf_v8test
	./tests/nf_v8test alaw

# V.34: shaper-table smoke test plus the
# real-capture regression checks: control-channel QPSK lock, MP/MPh frame
# decodes (Type 0 + Type 1 precoder coefficients), Phase-2 INFO sequences,
# the control-channel HDLC/T.30 user data, and the primary-channel page
# decode (both ECM image blocks recovered as FCS-valid FCD frames - the
# strongest correctness signal in the project).
check-v34: tests/nf_v34test
	./tests/nf_v34test shapers
	./tests/nf_v34test modetab
	./tests/nf_v34test txrates
	./tests/nf_v34test probe
	./tests/nf_v34test info references/v.34_modem_test.wav
	./tests/nf_v34test ctrl references/v.34_modem_test.wav
	./tests/nf_v34test shellmap
	./tests/nf_v34test trellis
	./tests/nf_v34test viterbi
	./tests/nf_v34test fullchain
	./tests/nf_v34test mphunt references/v.34_modem_test.wav
	./tests/nf_v34test mph1 references/v.34_modem_test.wav
	./tests/nf_v34test infodec references/v.34_modem_test.wav
	./tests/nf_v34test ccdata references/v.34_modem_test.wav
	./tests/nf_v34test page references/v.34_modem_test.wav
	./tests/nf_v34test txsig
	./tests/nf_v34test txinfo
	./tests/nf_v34test txcc
	./tests/nf_v34test ccresync
	./tests/nf_v34test txpage
	./tests/nf_v34test ccimp
	./tests/nf_v34test pageimp
	./tests/nf_v34test recover

# End-to-end fax over V.34 (T.30 Annex F): a real page transferred between two
# nf_t30 engines entirely over the V.34 half-duplex session, verified
# pixel-exact, plus a transcript check that the Annex F sequence actually ran
# (clause-12 startup, no-TCF phase B, primary-channel ECM, PPS/MCF turnarounds).
check-v34fax: tests/nf_v34fax
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _v34.tiff 2>/dev/null
	NFV34DBG=1 ./tests/nf_v34fax _v34.tiff _v34out.tiff verbose 2>_v34sess.log
	@grep -q 'rx INFO0a' _v34sess.log && echo " INFO0c/INFO0a exchange OK"       || (echo " FAIL: no INFO0 exchange"; exit 1)
	@grep -q 'L1/L2 line probing' _v34sess.log && echo " tones + L1/L2 probing OK" || (echo " FAIL: no line probing"; exit 1)
	@grep -q 'rx INFOh' _v34sess.log && echo " INFOh OK"                         || (echo " FAIL: no INFOh"; exit 1)
	@grep -q 'phase-3 trained' _v34sess.log && echo " phase-3 TRN training OK"   || (echo " FAIL: no phase-3 training"; exit 1)
	@grep -q 'MPh frame' _v34sess.log && echo " control-channel MPh handshake OK" || (echo " FAIL: no MPh handshake"; exit 1)
	@grep -q 'Annex F: DCS on V.34 control channel -> CFR' _v34sess.log && echo " Annex F phase B (DCS->CFR, no TCF) OK" || (echo " FAIL: no Annex F phase B"; exit 1)
	@grep -q 'rx primary burst' _v34sess.log && echo " primary-channel ECM page OK" || (echo " FAIL: no primary-channel burst"; exit 1)
	@grep -q 'rx cc frame fcf=0xbf' _v34sess.log && echo " PPS over control channel OK" || (echo " FAIL: no PPS"; exit 1)
	@echo "-- Stage 3: stable turnarounds take the short Sh/S̄h resync (12.6), not full PPh/MPh --"
	@grep -q 'tx cc stream: Sh/S̄h/ALT/E short resync' _v34sess.log && echo " Sh short resync used (12.6) OK" || (echo " FAIL: no Sh resync"; exit 1)
	@grep -q 'rx cc burst: Sh short resync (no MPh' _v34sess.log && echo " Sh vs PPh discrimination OK" || (echo " FAIL: Sh not discriminated on rx"; exit 1)
	@compare -metric AE _v34.tiff _v34out.tiff null: && echo " v34 pixeldiff OK"
	@echo "-- Stage 4: 2400 bit/s (16-point) control channel available (opt-in NFV34CC2400=1, clean lines) --"
	NFV34DBG=1 NFV34CC2400=1 ./tests/nf_v34fax _v34.tiff _v34out2400.tiff verbose 2>_v34sess2400.log
	@grep -q 'control-channel user-data rate -> 2400 bit/s' _v34sess2400.log && echo " cc_rate=2400 negotiated (10.2.4) OK" || (echo " FAIL: 2400 cc not negotiated"; exit 1)
	@compare -metric AE _v34.tiff _v34out2400.tiff null: && echo " v34 cc-2400 pixeldiff OK"
	@echo "-- full fine page (multi-block: PPS-NULL / MCF between 256-frame ECM blocks) --"
	NFV34DBG=1 ./tests/nf_v34fax example_pages/fine.tiff _v34out2.tiff verbose 2>_v34sess2.log
	@grep -q 'PPS fcf2=0x00' _v34sess2.log && echo " multi-block PPS-NULL OK"    || (echo " FAIL: no PPS-NULL between blocks"; exit 1)
	@grep -q 'tx cc stream: Sh/S̄h/ALT/E short resync' _v34sess2.log && echo " Sh resync between ECM blocks OK" || (echo " FAIL: no Sh resync between blocks"; exit 1)
	@compare -metric AE example_pages/fine.tiff _v34out2.tiff null: && echo " v34 full-page pixeldiff OK"
	NFV34DBG=1 ./tests/nf_v34fax _v34.tiff _v34out3.tiff verbose 9600 2>_v34sess3.log
	@grep -q 'primary rate selected: 9600' _v34sess3.log && echo " capped-rate negotiation (9600) OK" || (echo " FAIL: no 9600 rate negotiation"; exit 1)
	@compare -metric AE _v34.tiff _v34out3.tiff null: && echo " v34 capped-rate pixeldiff OK"
	@echo "-- Stage 3: mid-call transient line hit -> ECM/PPR recovery, still pixel-exact --"
	NFV34DBG=1 NFV34HIT="7.5:9.5:22" ./tests/nf_v34fax _v34.tiff _v34outh.tiff verbose 2>_v34sessh.log
	@grep -q 'ECM block incomplete' _v34sessh.log && echo " mid-call hit damaged the block (PPR path exercised) OK" || (echo " FAIL: hit did no damage - test not testing anything"; exit 1)
	@grep -qE 'rate fallback: burst quality|resend|ECM block incomplete.*send PPR' _v34sessh.log && echo " mid-call recovery (PPR retransmit and/or 12.4 renegotiation) OK" || (echo " FAIL: no recovery path ran"; exit 1)
	@compare -metric AE _v34.tiff _v34outh.tiff null: && echo " v34 mid-call-hit pixeldiff OK"
	@rm -f _v34.tiff _v34out.tiff _v34sess.log _v34out2.tiff _v34sess2.log _v34out3.tiff _v34sess3.log _v34outh.tiff _v34sessh.log

# App-level V.34 (Super G3): two sip_fax processes over the TCP loopback
# transport, exactly as a real deployment runs, with V.34 the default. Proves
# (1) V.34 end-to-end, pixel-exact, actually using V.34 (not V.17); and
# (2) clean fallback to classic G3 when only one side offers V.34, both
#     directions, still pixel-exact.
check-v34app: sip_fax
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _va_in.tiff 2>/dev/null
	@echo "-- V.34 both sides: end-to-end over TCP, must use V.34 and be pixel-exact --"
	@P=52534; rm -f _va_out.tiff _va_s.log _va_r.log; \
	  ./sip_fax --send _va_in.tiff --listen $$P --v34 --verbose >_va_s.log 2>&1 & S=$$!; \
	  sleep 0.5; \
	  ./sip_fax --receive _va_out.tiff --connect 127.0.0.1:$$P --v34 --verbose >_va_r.log 2>&1 & R=$$!; \
	  wait $$S; SE=$$?; wait $$R; RE=$$?; \
	  test $$SE -eq 0 -a $$RE -eq 0 || (echo " FAIL: sip_fax exit ($$SE/$$RE)"; cat _va_s.log _va_r.log; exit 1)
	@grep -q 'V8 negotiated V.34 half-duplex' _va_s.log && grep -q 'V8 negotiated V.34 half-duplex' _va_r.log \
	  && echo " both sides negotiated V.34 half-duplex OK" || (echo " FAIL: V.34 not negotiated on both sides"; exit 1)
	@grep -q 'Super G3 / V.34' _va_s.log && grep -q 'Super G3 / V.34' _va_r.log \
	  && echo " Phase E reports Super G3 / V.34 OK" || (echo " FAIL: Phase E did not report V.34"; exit 1)
	@compare -metric AE _va_in.tiff _va_out.tiff null: && echo " v34app pixeldiff OK"
	@echo "-- fallback A: caller --v34, answerer --no-v34 -> classic G3, pixel-exact --"
	@P=52535; rm -f _va_out.tiff _va_s.log _va_r.log; \
	  ./sip_fax --send _va_in.tiff --listen $$P --v34 --verbose >_va_s.log 2>&1 & S=$$!; \
	  sleep 0.5; \
	  ./sip_fax --receive _va_out.tiff --connect 127.0.0.1:$$P --no-v34 --verbose >_va_r.log 2>&1 & R=$$!; \
	  wait $$S; SE=$$?; wait $$R; RE=$$?; \
	  test $$SE -eq 0 -a $$RE -eq 0 || (echo " FAIL: sip_fax exit ($$SE/$$RE)"; cat _va_s.log _va_r.log; exit 1)
	@! grep -q 'Super G3 / V.34' _va_s.log && ! grep -q 'Super G3 / V.34' _va_r.log \
	  && echo " fell back to G3 (no V.34) OK" || (echo " FAIL: unexpectedly used V.34"; exit 1)
	@compare -metric AE _va_in.tiff _va_out.tiff null: && echo " fallback-A pixeldiff OK"
	@echo "-- fallback B: caller --no-v34, answerer --v34 -> classic G3, pixel-exact --"
	@P=52536; rm -f _va_out.tiff _va_s.log _va_r.log; \
	  ./sip_fax --send _va_in.tiff --listen $$P --no-v34 --verbose >_va_s.log 2>&1 & S=$$!; \
	  sleep 0.5; \
	  ./sip_fax --receive _va_out.tiff --connect 127.0.0.1:$$P --v34 --verbose >_va_r.log 2>&1 & R=$$!; \
	  wait $$S; SE=$$?; wait $$R; RE=$$?; \
	  test $$SE -eq 0 -a $$RE -eq 0 || (echo " FAIL: sip_fax exit ($$SE/$$RE)"; cat _va_s.log _va_r.log; exit 1)
	@! grep -q 'Super G3 / V.34' _va_s.log && ! grep -q 'Super G3 / V.34' _va_r.log \
	  && echo " fell back to G3 (no V.34) OK" || (echo " FAIL: unexpectedly used V.34"; exit 1)
	@compare -metric AE _va_in.tiff _va_out.tiff null: && echo " fallback-B pixeldiff OK"
	@rm -f _va_in.tiff _va_out.tiff _va_s.log _va_r.log

# Full fax checks: own-engine loopback + interop with spandsp, both directions.
check-fax: tests/nf_faxloop tests/nf_faxloop2 tests/nf_interop
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _in.tiff 2>/dev/null
	./tests/nf_faxloop
	./tests/nf_faxloop2 _in.tiff _out.tiff
	@echo "-- T.30 station id (TSI/CSI) round-trips both directions --"
	NF_T30_V34=0 ./tests/nf_faxloop2 _in.tiff _out.tiff verbose 2>&1 | tee /tmp/nf_id.log >/dev/null
	@grep -q '\[CALL\] remote station id: "nf_fax"' /tmp/nf_id.log && echo " caller captured peer station id OK" || (echo " FAIL: caller did not capture peer station id"; exit 1)
	@grep -q '\[ANSW\] remote station id: "nf_fax"' /tmp/nf_id.log && echo " answerer captured peer station id OK" || (echo " FAIL: answerer did not capture peer station id"; exit 1)
	@rm -f /tmp/nf_id.log
	@echo "-- T.30 Annex F: with V.34 enabled both sides must negotiate V.8 before DIS/DCS --"
	NF_T30_V34=1 ./tests/nf_faxloop2 _in.tiff _out.tiff verbose 2>&1 | tee /tmp/nf_v8_callflow.log >/dev/null
	@grep -q '\[CALL\] V8 negotiated' /tmp/nf_v8_callflow.log && echo " caller V.8 negotiated OK" || (echo " FAIL: caller never logged a V.8 negotiation"; exit 1)
	@grep -q '\[ANSW\] V8 negotiated' /tmp/nf_v8_callflow.log && echo " answerer V.8 negotiated OK" || (echo " FAIL: answerer never logged a V.8 negotiation"; exit 1)
	@rm -f /tmp/nf_v8_callflow.log
	./tests/nf_interop nf2sp _in.tiff _o1.tiff
	./tests/nf_interop sp2nf _in.tiff _o2.tiff
	@echo "-- V.34 offered to a non-V.34 (plain-G3) spandsp peer: must fall back cleanly --"
	NF_T30_V34=1 ./tests/nf_interop nf2sp _in.tiff _o1.tiff && compare -metric AE _in.tiff _o1.tiff null: && echo " nf2sp+v34-offer pixeldiff OK"
	NF_T30_V34=1 ./tests/nf_interop sp2nf _in.tiff _o2.tiff && compare -metric AE _in.tiff _o2.tiff null: && echo " sp2nf+v34-offer pixeldiff OK"
	@rm -f _in.tiff _out.tiff _o1.tiff _o2.tiff

# ECM interop against spandsp, both directions, clean and with injected frame loss.
# Each run must finish OK; the received TIFF must be pixel-identical to the input.
check-ecm: tests/nf_ecmtest
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _e.tiff 2>/dev/null
	@echo "-- clean --"
	./tests/nf_ecmtest nf2sp _e.tiff _e1.tiff && compare -metric AE _e.tiff _e1.tiff null: && echo " nf2sp pixeldiff OK"
	./tests/nf_ecmtest sp2nf _e.tiff _e2.tiff && compare -metric AE _e.tiff _e2.tiff null: && echo " sp2nf pixeldiff OK"
	@echo "-- nf sender drops tx FCD frames 1,2 (spandsp must request retransmission) --"
	NF_ECM_DROP_TX="1,2" ./tests/nf_ecmtest nf2sp _e.tiff _e3.tiff && compare -metric AE _e.tiff _e3.tiff null: && echo " nf2sp+loss pixeldiff OK"
	@echo "-- nf receiver drops rx FCD frames 0,3 (we must send PPR, spandsp resends) --"
	NF_ECM_DROP_RX="0,3" ./tests/nf_ecmtest sp2nf _e.tiff _e4.tiff && compare -metric AE _e.tiff _e4.tiff null: && echo " sp2nf+loss pixeldiff OK"
	@rm -f _e.tiff _e1.tiff _e2.tiff _e3.tiff _e4.tiff

# ECM rate fallback (T.30 Annex A.4.3): the sender withholds all FCD frames while
# the rate is too high, forcing the CTC/CTR ladder to drop the modem step until
# the block gets through. Delivery must stay pixel-exact; at the lowest rate the
# transfer must fail cleanly via EOR/ERR (not hang).
check-ctc: tests/nf_ctctest
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _ct.tiff 2>/dev/null
	@echo "-- single CTC step (14400 blocked -> 12000) --"
	NF_ECM_DROP_ABOVE=13000 NF_T30_V34=0 ./tests/nf_ctctest _ct.tiff _ct1.tiff && compare -metric AE _ct.tiff _ct1.tiff null: && echo " single-CTC pixeldiff OK"
	@echo "-- multi-step CTC (down to 7200) --"
	NF_ECM_DROP_ABOVE=8000 NF_T30_V34=0 ./tests/nf_ctctest _ct.tiff _ct2.tiff && compare -metric AE _ct.tiff _ct2.tiff null: && echo " multi-CTC pixeldiff OK"
	@echo "-- lowest rate still fails: must EOR/ERR and end cleanly (expect non-zero) --"
	@NF_T30_MODEMS=v27 NF_ECM_DROP_ABOVE=1000 NF_T30_V34=0 ./tests/nf_ctctest _ct.tiff _ct3.tiff; \
	 if [ $$? -ne 0 ]; then echo " EOR clean-fail OK"; else echo " EOR clean-fail FAIL (expected failure)"; exit 1; fi
	@rm -f _ct.tiff _ct1.tiff _ct2.tiff _ct3.tiff

# Colour fax (T.42/JPEG) + binary file transfer, nf<->nf. Colour is lossy, so it
# is judged by PSNR; the file transfer and the post-retransmission codestream are
# byte-exact. A frame-loss run exercises ECM retransmission of both payloads.
check-color: tests/nf_colortest tests/nf_xfer
	./tests/nf_colortest
	@convert -size 96x96 plasma:fractal -depth 8 -type truecolor _c.tiff 2>/dev/null
	@head -c 70000 /dev/urandom > _f.bin
	@echo "-- colour nf<->nf (clean) --"
	./tests/nf_xfer color _c.tiff _c_out.tiff
	@compare -metric PSNR _c.tiff _c_out.tiff null: 2>&1; echo " dB (colour, clean)"
	@echo "-- colour nf<->nf with FCD frame loss (tx 1,4 / rx 2) --"
	NF_ECM_DROP_TX="1,4" ./tests/nf_xfer color _c.tiff _c_lo.tiff
	@compare -metric PSNR _c.tiff _c_lo.tiff null: 2>&1; echo " dB (colour, tx loss)"
	@echo "-- greyscale nf<->nf (clean) --"
	./tests/nf_xfer gray _c.tiff _g_out.tiff
	@echo "  grey rx: $$(identify -format '%wx%h %[channels]' _g_out.tiff 2>/dev/null)"
	@echo "-- binary file nf<->nf (clean, byte-exact) --"
	./tests/nf_xfer file _f.bin _f_out.bin && cmp _f.bin _f_out.bin && echo " file OK (byte-exact)"
	@echo "-- binary file nf<->nf with FCD frame loss (tx 0,3 / rx 5) --"
	NF_ECM_DROP_TX="0,3" NF_ECM_DROP_RX="5" ./tests/nf_xfer file _f.bin _f_lo.bin && cmp _f.bin _f_lo.bin && echo " file OK (byte-exact, with loss)"
	@rm -f _c.tiff _c_out.tiff _c_lo.tiff _g_out.tiff _f.bin _f_out.bin _f_lo.bin

# T.38 terminal mode, nf<->nf over an in-process UDPTL pipe (our own UDPTL + IFP
# codec; no spandsp). Non-ECM and ECM, clean and with injected UDPTL packet loss
# (recovered by redundancy). Pixel-exact.
tests/nf_t38loop: tests/nf_t38loop.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_t38loop.c $(ENGINE_SRCS) $(LDLIBS)

tests/nf_udptltest: tests/nf_udptltest.c nf_udptl.c nf_udptl.h
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_udptltest.c nf_udptl.c

# nf_t38oracle : our nf_t30 T.38 backend against spandsp's real t38_terminal over
# an in-process UDPTL bridge, both directions (we send / we receive). Proves our
# IFP/UDPTL output is decodable by a genuine T.38 stack and vice-versa. spandsp
# is linked here as a test oracle only.
tests/nf_t38oracle: tests/nf_t38oracle.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_t38oracle.c $(ENGINE_SRCS) $(SPANDSP_LIBS) $(LDLIBS)

# nf_t38gw : our nf_t30 T.38 sender -> spandsp t38_gateway (T.38<->audio re-modulation,
# exactly what a carrier gateway does) -> spandsp audio fax receiver. This exercises the
# re-modulation path that a plain T.38 terminal can't (training length, carrier drain,
# HDLC pacing). spandsp linked as oracle only.
tests/nf_t38gw: tests/nf_t38gw.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_t38gw.c $(ENGINE_SRCS) $(SPANDSP_LIBS) $(LDLIBS)

check-t38: tests/nf_udptltest tests/nf_t38loop
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _t.tiff 2>/dev/null
	./tests/nf_udptltest
	@echo "-- non-ECM clean --"
	./tests/nf_t38loop _t.tiff _t1.tiff noecm        && compare -metric AE _t.tiff _t1.tiff null: && echo " non-ecm pixeldiff OK"
	@echo "-- ECM clean --"
	./tests/nf_t38loop _t.tiff _t2.tiff              && compare -metric AE _t.tiff _t2.tiff null: && echo " ecm pixeldiff OK"
	@echo "-- non-ECM with UDPTL loss (drop 1/7) --"
	./tests/nf_t38loop _t.tiff _t3.tiff noecm drop=7 && compare -metric AE _t.tiff _t3.tiff null: && echo " non-ecm+loss pixeldiff OK"
	@echo "-- ECM with UDPTL loss (drop 1/5) --"
	./tests/nf_t38loop _t.tiff _t4.tiff drop=5       && compare -metric AE _t.tiff _t4.tiff null: && echo " ecm+loss pixeldiff OK"
	@echo "-- lost MCF (sender retransmits PPS, receiver re-ACKs) --"
	NF_T38_DROP_MCF=1 ./tests/nf_t38loop _t.tiff _t5.tiff && compare -metric AE _t.tiff _t5.tiff null: && echo " lost-MCF recovery pixeldiff OK"
	@rm -f _t.tiff _t1.tiff _t2.tiff _t3.tiff _t4.tiff _t5.tiff

# T.38 interop against the real spandsp t38_terminal stack, both directions and
# both ECM modes. Pixel-exact. (Requires spandsp; oracle only.)
check-t38-interop: tests/nf_t38oracle
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _t.tiff 2>/dev/null
	@echo "-- nf SEND -> spandsp RECV, non-ECM --"
	./tests/nf_t38oracle send _t.tiff _to1.tiff noecm && compare -metric AE _t.tiff _to1.tiff null: && echo " send non-ecm pixeldiff OK"
	@echo "-- nf SEND -> spandsp RECV, ECM --"
	./tests/nf_t38oracle send _t.tiff _to2.tiff       && compare -metric AE _t.tiff _to2.tiff null: && echo " send ecm pixeldiff OK"
	@echo "-- spandsp SEND -> nf RECV, non-ECM --"
	./tests/nf_t38oracle recv _t.tiff _to3.tiff noecm && compare -metric AE _t.tiff _to3.tiff null: && echo " recv non-ecm pixeldiff OK"
	@echo "-- spandsp SEND -> nf RECV, ECM --"
	./tests/nf_t38oracle recv _t.tiff _to4.tiff       && compare -metric AE _t.tiff _to4.tiff null: && echo " recv ecm pixeldiff OK"
	@rm -f _t.tiff _to1.tiff _to2.tiff _to3.tiff _to4.tiff

# T.38 through a re-modulating gateway (our sender -> spandsp t38_gateway -> spandsp
# audio fax). Proves the re-modulation path: correct training length, carrier drain
# and HDLC pacing. This is what real carrier T.38 gateways do. Pixel-exact.
check-t38-gateway: tests/nf_t38gw
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _t.tiff 2>/dev/null
	@echo "-- our sender -> spandsp gateway -> spandsp fax, non-ECM --"
	./tests/nf_t38gw nf _t.tiff _tg1.tiff noecm && compare -metric AE _t.tiff _tg1.tiff null: && echo " gw non-ecm pixeldiff OK"
	@echo "-- our sender -> spandsp gateway -> spandsp fax, ECM --"
	./tests/nf_t38gw nf _t.tiff _tg2.tiff       && compare -metric AE _t.tiff _tg2.tiff null: && echo " gw ecm pixeldiff OK"
	@rm -f _t.tiff _tg1.tiff _tg2.tiff

clean:
	rm -f sip_fax line_sim $(OBJS) \
	      tests/nf_t4check tests/nf_faxloop tests/nf_faxloop2 tests/nf_interop \
	      tests/nf_ecmtest tests/nf_ctctest tests/nf_modemtest tests/nf_colortest tests/nf_xfer \
	      tests/nf_t38loop tests/nf_udptltest tests/nf_t38oracle tests/nf_t38gw \
	      tests/nf_v34test tests/nf_v34fax tests/nf_v8test tests/nf_md5test \
	      _*.tiff _*.log _*.bin

.PHONY: clean check check-md5 asan check-fax check-ecm check-ctc check-color check-modem check-t38 check-t38-interop check-t38-gateway check-v8 check-v34 check-v34fax check-v34app
