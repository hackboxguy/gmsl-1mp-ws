#!/bin/sh
# Single-camera 1MP regression (run as root): fps, full frames, compliance, 20 start/stop, snapshot.
CAM=${1:-cam1}
gmsl1mp-ctl fps $CAM 2>&1 | tail -1
gmsl1mp-ctl setup $CAM >/dev/null; A=$(gmsl1mp-ctl node $CAM)
echo "frames: $(timeout 20 v4l2-ctl -d $A --stream-mmap --stream-count=150 --verbose 2>&1 | grep -o "bytesused: [0-9]*" | sort | uniq -c | tr '\n' ' ')"
SD=$(for s in /sys/class/video4linux/v4l-subdev*; do grep -q gmsl1mp $s/name && echo /dev/${s##*/}; done | head -1)
echo "compliance: $(v4l2-compliance -u $SD 2>&1 | grep '^Total')"
ok=0; for i in $(seq 1 20); do n=$(timeout 10 v4l2-ctl -d $A --stream-mmap --stream-count=15 --verbose 2>&1 | grep -c "bytesused: 2457600"); [ "$n" = 15 ] && ok=$((ok+1)); done
echo "start/stop cycles with 15/15 full frames: $ok/20"
rm -f /tmp/s.jpg; gmsl1mp-ctl snap $CAM /tmp/s.jpg >/dev/null 2>&1; echo "snapshot $(stat -c %s /tmp/s.jpg) bytes"
