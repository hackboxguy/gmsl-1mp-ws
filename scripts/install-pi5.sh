#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Install the gmsl1mp driver on Raspberry Pi OS (Pi 5) via DKMS, plus the
# ws-gmsl1mp overlay and the helper script. Run from the repo root:
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

apt-get update >/dev/null
apt-get install -y dkms device-tree-compiler i2c-tools v4l-utils \
	gstreamer1.0-tools gstreamer1.0-plugins-good \
	gstreamer1.0-plugins-ugly gstreamer1.0-plugins-bad >/dev/null

# driver (DKMS: rebuilt automatically on kernel updates)
if dkms status -m $PKG -v "$VER" | grep -q $PKG; then
	dkms remove -m $PKG -v "$VER" --all >/dev/null
fi
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
sed -i '/^dtoverlay=ws-gmsl1mp/d' $CONFIG
echo "dtoverlay=ws-gmsl1mp${PARAMS:+,$PARAMS}" >> $CONFIG

echo "installed $PKG $VER; config.txt: dtoverlay=ws-gmsl1mp${PARAMS:+,$PARAMS}"
echo "reboot, then: gmsl1mp-ctl fps cam0"
