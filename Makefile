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
OBJS = sip_fax.o sip.o sip_util.o g711.o nf_t4.o nf_t30.o nf_fax.o nf_color.o nf_dsp.o nf_hdlc.o nf_v21.o nf_qam.o nf_v29.o nf_v27.o nf_v17.o nf_udptl.o nf_t38.o

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

nf_fax.o:  nf_fax.c nf_fax.h nf_dsp.h nf_hdlc.h nf_v21.h nf_v29.h nf_v27.h nf_v17.h
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
tests/nf_faxloop: tests/nf_faxloop.c nf_fax.c nf_fax.h nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_faxloop.c nf_fax.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(LDLIBS)
tests/nf_faxloop2: tests/nf_faxloop2.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_faxloop2.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(LDLIBS)
tests/nf_interop: tests/nf_interop.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_interop.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(SPANDSP_LIBS) $(LDLIBS)
# nf_ecmtest : nf_t30 (ECM) against spandsp (ECM + T.6), with frame-loss injection
tests/nf_ecmtest: tests/nf_ecmtest.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_ecmtest.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(SPANDSP_LIBS) $(LDLIBS)
# nf_colortest : offline unit test for the nf_color T.42/JPEG codec
tests/nf_colortest: tests/nf_colortest.c nf_color.c nf_color.h
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_colortest.c nf_color.c -ljpeg -lm
# nf_xfer : nf<->nf colour and binary-file transfer over the full ECM engine
tests/nf_xfer: tests/nf_xfer.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_xfer.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(LDLIBS)
# nf_modemtest : per-module oracle tests of the nf modem layer vs real spandsp
tests/nf_modemtest: tests/nf_modemtest.c nf_fax.c g711.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c nf_dsp.h nf_hdlc.h nf_v21.h nf_qam.h nf_v29.h nf_v27.h nf_v17.h
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_modemtest.c nf_fax.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c g711.c $(SPANDSP_LIBS) $(LDLIBS)

# Fast, deterministic codec check.
check: tests/nf_t4check
	./tests/nf_t4check doc.pam

# Per-module modem-layer oracle matrix (grows with the spandsp replacement).
check-modem: tests/nf_modemtest
	./tests/nf_modemtest tones
	./tests/nf_modemtest hdlc
	./tests/nf_modemtest v21
	./tests/nf_modemtest v29
	./tests/nf_modemtest v27
	./tests/nf_modemtest v17
	./tests/nf_modemtest v17short

# Full fax checks: own-engine loopback + interop with spandsp, both directions.
check-fax: tests/nf_faxloop tests/nf_faxloop2 tests/nf_interop
	@convert doc.pam -threshold 50% -density 204x196 -units PixelsPerInch -compress group4 _in.tiff 2>/dev/null
	./tests/nf_faxloop
	./tests/nf_faxloop2 _in.tiff _out.tiff
	./tests/nf_interop nf2sp _in.tiff _o1.tiff
	./tests/nf_interop sp2nf _in.tiff _o2.tiff
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
tests/nf_t38loop: tests/nf_t38loop.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_t38loop.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(LDLIBS)

tests/nf_udptltest: tests/nf_udptltest.c nf_udptl.c nf_udptl.h
	$(CC) $(CFLAGS) -I. -o $@ tests/nf_udptltest.c nf_udptl.c

# nf_t38oracle : our nf_t30 T.38 backend against spandsp's real t38_terminal over
# an in-process UDPTL bridge, both directions (we send / we receive). Proves our
# IFP/UDPTL output is decodable by a genuine T.38 stack and vice-versa. spandsp
# is linked here as a test oracle only.
tests/nf_t38oracle: tests/nf_t38oracle.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_t38oracle.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(SPANDSP_LIBS) $(LDLIBS)

# nf_t38gw : our nf_t30 T.38 sender -> spandsp t38_gateway (T.38<->audio re-modulation,
# exactly what a carrier gateway does) -> spandsp audio fax receiver. This exercises the
# re-modulation path that a plain T.38 terminal can't (training length, carrier drain,
# HDLC pacing). spandsp linked as oracle only.
tests/nf_t38gw: tests/nf_t38gw.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c
	$(CC) $(CFLAGS) $(SPANDSP_CFLAGS) -I. -o $@ tests/nf_t38gw.c nf_t30.c nf_t38.c nf_udptl.c nf_fax.c nf_t4.c nf_color.c nf_dsp.c nf_hdlc.c nf_v21.c nf_qam.c nf_v29.c nf_v27.c nf_v17.c $(SPANDSP_LIBS) $(LDLIBS)

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
	rm -f sip_fax $(OBJS) \
	      tests/nf_t4check tests/nf_faxloop tests/nf_faxloop2 tests/nf_interop \
	      tests/nf_ecmtest tests/nf_modemtest tests/nf_colortest tests/nf_xfer \
	      tests/nf_t38loop tests/nf_udptltest tests/nf_t38oracle tests/nf_t38gw nf_t4.o \
	      _to1.tiff _to2.tiff _to3.tiff _to4.tiff _tg1.tiff _tg2.tiff \
	      _in.tiff _out.tiff _o1.tiff _o2.tiff _e.tiff _e1.tiff _e2.tiff _e3.tiff _e4.tiff \
	      _c.tiff _c_out.tiff _c_lo.tiff _g_out.tiff _f.bin _f_out.bin _f_lo.bin \
	      _t.tiff _t1.tiff _t2.tiff _t3.tiff _t4.tiff _t5.tiff

.PHONY: clean check check-fax check-ecm check-color check-modem check-t38 check-t38-interop check-t38-gateway
