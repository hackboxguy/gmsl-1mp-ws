#!/bin/sh
# Hack test: ht.sh FIRST ORDER   FIRST = 1mp|vd (camera already streaming), ORDER = vd56g4 hack_port_order
r() { i2ctransfer -f -y 10 w2@0x48 $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
[ $(( $(r 0x0013) & 8 )) -eq 0 ] && { echo "GMSL2 link down"; exit 1; }
echo ${2:-2} > /sys/module/vb56g4a/parameters/hack_port_order
gmsl1mp-ctl setup cam1 >/dev/null 2>&1; A=$(gmsl1mp-ctl node cam1)
for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q vb56g4 && MD=$m && break; done
media-ctl -d $MD -r; media-ctl -d $MD -l '"csi2":4 -> "rp1-cfe-csi2_ch0":0 [1]'
media-ctl -d $MD -V '"csi2":0 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
media-ctl -d $MD -V '"csi2":4 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
V=$(media-ctl -d $MD -e rp1-cfe-csi2_ch0); v4l2-ctl -d $V --set-fmt-video=width=1124,height=1364,pixelformat=GREY
s() { timeout $3 v4l2-ctl -d $1 --stream-mmap --stream-count=$2 --verbose 2>&1 | grep -o "bytesused: [0-9]*" | sort | uniq -c | tr "\n" " "; }
if [ "$1" = vd ]; then
  (s $V 360 20 > /tmp/h.first) & sleep 3; s $A 150 12 > /tmp/h.second; wait
  echo "vd56g4 (first): $(cat /tmp/h.first)"; echo "1MP (second):   $(cat /tmp/h.second)"
else
  (s $A 360 20 > /tmp/h.first) & sleep 3; s $V 150 12 > /tmp/h.second; wait
  echo "1MP (first):     $(cat /tmp/h.first)"; echo "vd56g4 (second): $(cat /tmp/h.second)"
fi
dmesg | grep HACK | tail -2
