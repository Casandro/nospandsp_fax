#!/bin/bash
# usage: runsim.sh "label" [line_sim impairment args...]
label="$1"; shift
rm -f out.tiff sendA.log recvB.log sim.log
./sip_fax --send doc.pam --listen 6001 > sendA.log 2>&1 &
SPID=$!
./sip_fax --receive out.tiff --listen 6002 > recvB.log 2>&1 &
RPID=$!
sleep 0.4
timeout 120 ../line_sim -A 127.0.0.1:6001 -B 127.0.0.1:6002 "$@" > sim.log 2>&1
LRC=$?
wait $SPID 2>/dev/null; SRC=$?
wait $RPID 2>/dev/null; RRC=$?
SP=$(grep -oE 'Phase E: [A-Za-z_ ]+' sendA.log | head -1)
RP=$(grep -oE 'Phase E: [A-Za-z_ ]+' recvB.log | head -1)
RPAGE=$(grep -oE 'pages tx=[0-9]+ rx=[0-9]+' recvB.log | head -1)
DIFF="n/a"
if [ -f out.tiff ]; then
  if convert out.tiff -threshold 50% _r.pbm 2>/dev/null && convert doc.pam -threshold 50% _s.pbm 2>/dev/null; then
    DIFF=$(compare -metric AE _s.pbm _r.pbm null: 2>&1)
  else DIFF="out.tiff unreadable"; fi
  rm -f _r.pbm _s.pbm
else DIFF="NO out.tiff"; fi
printf '%-34s send[%3s %-14s] recv[%3s %-14s] %-18s pixeldiff=%s\n' \
  "$label" "$SRC" "${SP:-none}" "$RRC" "${RP:-none}" "$RPAGE" "$DIFF"
