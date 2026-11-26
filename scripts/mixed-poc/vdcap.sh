#!/bin/sh
# Capture N frames from the vd56g4 on Pi CAM0 (guards against the rp1-cfe oops).
N=${1:-60}
r() { i2ctransfer -f -y 10 w2@0x48 $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
l=$(r 0x0013); if [ $(( l & 8 )) -eq 0 ] || ! i2ctransfer -f -y 10 w2@0x40 0x00 0x0d r1 >/dev/null 2>&1; then echo "GMSL2 link down (0x13=$l) - not capturing"; exit 1; fi
for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q vb56g4 && MD=$m && break; done
media-ctl -d $MD -r; media-ctl -d $MD -l '"csi2":4 -> "rp1-cfe-csi2_ch0":0 [1]'
media-ctl -d $MD -V '"csi2":0 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
media-ctl -d $MD -V '"csi2":4 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
V=$(media-ctl -d $MD -e rp1-cfe-csi2_ch0)
v4l2-ctl -d $V --set-fmt-video=width=1124,height=1364,pixelformat=GREY
[ "$CSIOUT" = skip ] || i2ctransfer -f -y 10 w3@0x48 0x03 0x13 ${CSIOUT:-0x42}
rm -f /tmp/v.grey; timeout 20 v4l2-ctl -d $V --stream-mmap --stream-count=$N --stream-to=/tmp/v.grey 2>&1 | tail -1
echo "bytes=$(stat -c %s /tmp/v.grey 2>/dev/null || echo 0) (frame = 1533136)"
