#!/bin/sh
# Timeline of a dual capture: 1MP on CAM1 (video0), vd56g4 on CAM0, optional port-B pulse.
# Usage: dualtl.sh [pulse]   (run as root)
r() { i2ctransfer -f -y 10 w2@0x48 $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
w() { i2ctransfer -f -y 10 w3@0x48 $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
up() { cut -d" " -f1 /proc/uptime; }
rm -f /tmp/tl.*
w 0x0312 0x00 2>/dev/null
( for i in $(seq 1 18); do echo "$(up) 0312=$(r 0x0312) 0108=$(r 0x0108) 012c=$(r 0x012c) 0313=$(r 0x0313)"; sleep 1; done ) > /tmp/tl.reg &
gmsl1mp-ctl setup cam1 >/dev/null 2>&1
A=$(gmsl1mp-ctl node cam1)
timeout 20 v4l2-ctl -d $A --stream-mmap --stream-count=450 --verbose 2>&1 | grep -o "bytesused: [0-9]*.*ts: [0-9.]*" > /tmp/tl.a &
sleep 3; echo "$(up) vd56g4 start" > /tmp/tl.ev
for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q vb56g4 && MD=$m && break; done
media-ctl -d $MD -r; media-ctl -d $MD -l '"csi2":4 -> "rp1-cfe-csi2_ch0":0 [1]'
media-ctl -d $MD -V '"csi2":0 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
media-ctl -d $MD -V '"csi2":4 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
V=$(media-ctl -d $MD -e rp1-cfe-csi2_ch0); v4l2-ctl -d $V --set-fmt-video=width=1124,height=1364,pixelformat=GREY
timeout 14 v4l2-ctl -d $V --stream-mmap --stream-count=150 --verbose 2>&1 | grep -o "bytesused: [0-9]*.*ts: [0-9.]*" > /tmp/tl.v &
if [ "$1" = pulse ]; then sleep 2; echo "$(up) pulse B" >> /tmp/tl.ev; w 0x0332 0x30; sleep 0.1; w 0x0332 0xf0; fi
wait
echo "== events"; cat /tmp/tl.ev
echo "== regs"; cat /tmp/tl.reg
echo "== 1MP: $(wc -l < /tmp/tl.a) bufs, short=$(grep -vc "bytesused: 2457600" /tmp/tl.a)"
grep -v "bytesused: 2457600" /tmp/tl.a | head -2
echo "== vd56g4: $(wc -l < /tmp/tl.v) bufs, full=$(grep -c "bytesused: 1549504" /tmp/tl.v)"
head -1 /tmp/tl.v; tail -1 /tmp/tl.v
