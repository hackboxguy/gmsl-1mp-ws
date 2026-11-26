#!/bin/sh
# Acceptance: acc.sh FIRST   (FIRST = 1mp | vd56g4 streams continuously)
# The other camera does 20 start/stop cycles of 15 frames meanwhile.
# Frames are judged from v4l2-ctl --verbose (bytesused, seq), not from files.
FULL_1MP=2457600; FULL_VD=1549504
gmsl1mp-ctl setup cam1 >/dev/null 2>&1; A=$(gmsl1mp-ctl node cam1)
for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q vb56g4 && MD=$m && break; done
media-ctl -d $MD -r; media-ctl -d $MD -l '"csi2":4 -> "rp1-cfe-csi2_ch0":0 [1]'
media-ctl -d $MD -V '"csi2":0 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
media-ctl -d $MD -V '"csi2":4 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
V=$(media-ctl -d $MD -e rp1-cfe-csi2_ch0); v4l2-ctl -d $V --set-fmt-video=width=1124,height=1364,pixelformat=GREY
if [ "$1" = vd56g4 ]; then LD=$V; LF=$FULL_VD; CD=$A; CF=$FULL_1MP; LN=vd56g4; CN=1MP
else LD=$A; LF=$FULL_1MP; CD=$V; CF=$FULL_VD; LN=1MP; CN=vd56g4; fi
timeout 80 v4l2-ctl -d $LD --stream-mmap --stream-count=2100 --verbose 2>&1 | grep -o "seq: *[0-9]* bytesused: [0-9]*" > /tmp/acc.long &
sleep 3
ok=0; i=0
while [ $i -lt 20 ]; do
	n=$(timeout 10 v4l2-ctl -d $CD --stream-mmap --stream-count=15 --verbose 2>&1 | grep -c "bytesused: $CF")
	[ "$n" = 15 ] && ok=$((ok+1)); i=$((i+1))
done
echo "cycles_done_at=$(cut -d' ' -f1 /proc/uptime)"
wait
tot=$(wc -l < /tmp/acc.long); full=$(grep -c "bytesused: $LF" /tmp/acc.long)
gaps=$(awk '{s=$2+0; if (NR>1 && s!=p+1) g+=s-p-1; p=s} END {print g+0}' /tmp/acc.long)
echo "$LN (continuous): $full/$tot full frames, sequence gaps: $gaps"
echo "$CN (20 start/stop cycles): $ok/20 cycles with 15/15 full frames"
