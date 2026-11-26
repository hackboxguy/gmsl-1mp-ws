#!/bin/sh
# Single-camera vd56g4 regression (run as root): frame rate and full frames, 20 start/stop cycles.
FULL=1549504
for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q vb56g4 && MD=$m && break; done
media-ctl -d $MD -r; media-ctl -d $MD -l '"csi2":4 -> "rp1-cfe-csi2_ch0":0 [1]'
media-ctl -d $MD -V '"csi2":0 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
media-ctl -d $MD -V '"csi2":4 [fmt:Y8_1X8/1124x1364 field:none colorspace:raw]'
V=$(media-ctl -d $MD -e rp1-cfe-csi2_ch0); v4l2-ctl -d $V --set-fmt-video=width=1124,height=1364,pixelformat=GREY
timeout 20 v4l2-ctl -d $V --stream-mmap --stream-count=150 --verbose 2>&1 | grep -o "bytesused: [0-9]* .*ts: [0-9.]*" > /tmp/rv
awk -v F=$FULL '{if ($2==F) f++; t[NR]=$NF} END {printf "frames: %d/%d full, %.2f fps\n", f, NR, (NR-1)/(t[NR]-t[1])}' /tmp/rv
ok=0; for i in $(seq 1 20); do n=$(timeout 10 v4l2-ctl -d $V --stream-mmap --stream-count=15 --verbose 2>&1 | grep -c "bytesused: $FULL"); [ "$n" = 15 ] && ok=$((ok+1)); done
echo "start/stop cycles with 15/15 full frames: $ok/20"
