#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# gmsl1mp-ctl.sh - media pipeline helper for the gmsl1mp driver on
# Raspberry Pi 5 (rp1-cfe). One camera per Pi CSI connector.
#
#   gmsl1mp-ctl.sh setup   <cam0|cam1>             configure media graph
#   gmsl1mp-ctl.sh node    <cam0|cam1>             print the capture /dev/videoN
#   gmsl1mp-ctl.sh capture <cam0|cam1> <N> <file>  N raw UYVY frames to file
#   gmsl1mp-ctl.sh fps     <cam0|cam1> [N]         measure frame rate
#   gmsl1mp-ctl.sh snap    <cam0|cam1> <file.jpg>  one JPEG (needs gstreamer)
#   gmsl1mp-ctl.sh preview <cam0|cam1> [...]       live view (gstreamer)
#   gmsl1mp-ctl.sh record  <cam0|cam1> <file.mkv> [seconds]  H.264 (x264enc)
#
# "cam0" = Pi CAM/DISP0 (J3), "cam1" = Pi CAM/DISP1 (J4). Several cameras
# may be given to preview, e.g. "preview cam0 cam1" (side by side).
# setup is implied by every other command.

set -e

W=1280
H=960
MBUS=UYVY8_1X16
FMT="fmt:$MBUS/${W}x$H field:none colorspace:smpte170m"

declare -A CSI_PATH=(
	[cam0]=/sys/devices/platform/axi/1000120000.pcie/1f00110000.csi
	[cam1]=/sys/devices/platform/axi/1000120000.pcie/1f00128000.csi
)

die() { echo "ERROR: $*" >&2; exit 1; }

media_dev() {
	local p=${CSI_PATH[$1]} m
	[ -n "$p" ] || die "unknown camera port '$1' (cam0|cam1)"
	m=$(ls -d "$p"/media* 2>/dev/null | head -1)
	[ -n "$m" ] || die "$1: no media device - receiver not enabled in DT?"
	echo "/dev/$(basename "$m")"
}

sensor_entity() {
	media-ctl -d "$1" -p | sed -n 's/^- entity [0-9]*: \(gmsl1mp [^(]*\) (.*/\1/p' |
		sed 's/ *$//' | head -1
}

video_node() {
	media-ctl -d "$1" -e rp1-cfe-csi2_ch0
}

setup() {
	local md sensor
	md=$(media_dev "$1")
	sensor=$(sensor_entity "$md")
	[ -n "$sensor" ] || die "$1: no gmsl1mp sensor in $md (camera probe failed? see dmesg)"
	media-ctl -d "$md" -r
	media-ctl -d "$md" -l "'csi2':4 -> 'rp1-cfe-csi2_ch0':0 [1]"
	media-ctl -d "$md" -V "'$sensor':0 [$FMT]"
	media-ctl -d "$md" -V "'csi2':0 [$FMT]"
	media-ctl -d "$md" -V "'csi2':4 [$FMT]"
	v4l2-ctl -d "$(video_node "$md")" \
		--set-fmt-video=width=$W,height=$H,pixelformat=UYVY
}

node() { video_node "$(media_dev "$1")"; }

# gst_src <cam> [extra v4l2src properties]
gst_src() {
	echo "v4l2src device=$(node "$1") io-mode=dmabuf $2 ! video/x-raw,format=UYVY,width=$W,height=$H,framerate=30/1"
}

cmd=$1; shift || true
case "$cmd" in
setup)
	for c in "$@"; do setup "$c"; echo "$c: $(node "$c")"; done
	;;
node)
	node "$1"
	;;
capture)
	[ $# -eq 3 ] || die "capture <cam> <frames> <file>"
	setup "$1"
	v4l2-ctl -d "$(node "$1")" --stream-mmap --stream-count="$2" \
		--stream-to="$3"
	;;
fps)
	setup "$1"
	v4l2-ctl -d "$(node "$1")" --stream-mmap --stream-count="${2:-150}" \
		--stream-to=/dev/null 2>&1 | tr '<' '\n' | grep fps | tail -1
	;;
snap)
	[ $# -eq 2 ] || die "snap <cam> <file.jpg>"
	setup "$1"
	# skip a few frames so the ISP auto-exposure has settled
	gst-launch-1.0 -q $(gst_src "$1" num-buffers=15) ! videoconvert \
		! jpegenc ! multifilesink location="$2" max-files=1
	;;
preview)
	[ $# -ge 1 ] || die "preview <cam> [cam...]"
	sink=${GMSL1MP_SINK:-autovideosink}
	if [ $# -eq 1 ]; then
		setup "$1"
		exec gst-launch-1.0 $(gst_src "$1") ! videoconvert ! $sink sync=false
	fi
	pipe="compositor name=mix"
	i=0
	for c in "$@"; do
		setup "$c"
		pipe+=" sink_$i::xpos=$((i * W / 2))"
		i=$((i + 1))
	done
	pipe+=" ! videoconvert ! $sink sync=false"
	i=0
	for c in "$@"; do
		pipe+=" $(gst_src "$c") ! videoscale ! video/x-raw,width=$((W / 2)),height=$((H / 2)) ! mix.sink_$i"
		i=$((i + 1))
	done
	exec gst-launch-1.0 $pipe
	;;
record)
	[ $# -ge 2 ] || die "record <cam> <file.mkv> [seconds]"
	setup "$1"
	n=$(( ${3:-10} * 30 ))
	exec gst-launch-1.0 -e $(gst_src "$1" num-buffers=$n) ! queue \
		! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast \
		bitrate=6000 ! h264parse ! matroskamux \
		! filesink location="$2"
	;;
*)
	sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
