#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Install the gmsl1mp driver on Raspberry Pi OS (Pi 5) via DKMS, plus the
# ws-gmsl1mp overlay and the helper script. gmsl1mp is a client of the
# gmsl_max9296a deserializer driver, which is installed first from a
# gmsl-max9296a checkout (default: ../gmsl-max9296a, or set MAX9296A_DIR).
# Run from the repo root:
#
#   sudo ./scripts/install-pi5.sh [overlay params]
#
# e.g. sudo ./scripts/install-pi5.sh            # one camera, link A
#      sudo ./scripts/install-pi5.sh link-b     # two cameras
#
# Adds "camera_auto_detect=0" and "dtoverlay=ws-gmsl1mp[,params]" to
# /boot/firmware/config.txt (backup kept); reboot afterwards.

set -e
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
cd "$(dirname "$0")/.."

PKG=gmsl1mp
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
CONFIG=/boot/firmware/config.txt
PARAMS=$(IFS=,; echo "$*")

MAX9296A_DIR=${MAX9296A_DIR:-../gmsl-max9296a}
[ -x "$MAX9296A_DIR/scripts/install-pi5.sh" ] || {
	echo "gmsl-max9296a not found at $MAX9296A_DIR (clone it next to this repo or set MAX9296A_DIR)" >&2
	exit 1
}

apt-get update >/dev/null
apt-get install -y dkms device-tree-compiler i2c-tools v4l-utils \
	gstreamer1.0-tools gstreamer1.0-plugins-good \
	gstreamer1.0-plugins-ugly gstreamer1.0-plugins-bad >/dev/null

# deserializer driver first
"$MAX9296A_DIR/scripts/install-pi5.sh"

# driver (DKMS: rebuilt automatically on kernel updates); drop older versions
for v in $(dkms status -m $PKG 2>/dev/null | sed -n "s|^$PKG/\([^,:]*\).*|\1|p" | sort -u); do
	dkms remove -m $PKG -v "$v" --all >/dev/null || true
done
rm -rf /usr/src/$PKG-"$VER"
mkdir -p /usr/src/$PKG-"$VER"
cp -r dkms.conf driver /usr/src/$PKG-"$VER"/
make -C /usr/src/$PKG-"$VER"/driver clean >/dev/null 2>&1 || true
dkms install -m $PKG -v "$VER"

# overlay + helper
dtc -@ -q -I dts -O dtb -o /boot/firmware/overlays/ws-gmsl1mp.dtbo \
	dts/ws-gmsl1mp-overlay.dts
install -m 755 scripts/gmsl1mp-ctl.sh /usr/local/bin/gmsl1mp-ctl
echo i2c-dev > /etc/modules-load.d/i2c-dev.conf

cp "$CONFIG" "$CONFIG.bak.$(date +%Y%m%d-%H%M%S)"
sed -i 's/^camera_auto_detect=1/camera_auto_detect=0/' $CONFIG
grep -q '^camera_auto_detect=0' $CONFIG || echo camera_auto_detect=0 >> $CONFIG
sed -i '/^dtoverlay=ws-gmsl1mp/d; /^dtoverlay=ws-max9296a-hat/d' $CONFIG
echo "dtoverlay=ws-gmsl1mp${PARAMS:+,$PARAMS}" >> $CONFIG

echo "installed $PKG $VER; config.txt: dtoverlay=ws-gmsl1mp${PARAMS:+,$PARAMS}"
echo "reboot, then: gmsl1mp-ctl fps cam0"
