#!/bin/sh
# PoC: VD56G4 (GMSL2) on link A alongside gmsl1mp's 1MP (GMSL1) on link B.
# Run as root after boot with dtoverlay=ws-gmsl1mp,link-b,no-link-a.
set -e
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
r() { i2ctransfer -f -y $B w2@$D $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
v=$(r 0x0006); w 0x0006 $(( v | 0x40 ))     # link A -> GMSL2, link B stays GMSL1
# CTRL0: LINK_CFG=0 (dual link: both PHYs active - required for mixed
# GMSL2+GMSL1; LINK_CFG=1 "link A only" silences MIPI port B), AUTO_LINK=0,
# REG_ENABLE, RESET_ONESHOT
w 0x0010 ${CTRL0:-0x24}
sleep 1
l=$(r 0x0013); b=$(r 0x0ccb)
echo "0x0010=$(r 0x0010) 0x0013=$l (bit3 = GMSL2 lock)  0x0ccb=$b (bit0 = GMSL1 link B lock)"
if [ $(( l & 8 )) -eq 0 ] || [ $(( b & 1 )) -eq 0 ]; then echo "link not locked - stopping"; exit 1; fi
# MIPI port B: 4 lanes, 600 Mbps/lane. Must be WRITTEN (power-on values
# read the same but the port stays silent until programmed).
w 0x048a 0xd0; w 0x0323 0x26
# pipe Z (MAX96717F pipe Z sends stream ID 2): RAW8 + FS + FE -> MIPI port B
w 0x048b 0x07; w 0x048d 0x2a; w 0x048e 0x2a; w 0x048f 0x00; w 0x0490 0x00
w 0x0491 0x01; w 0x0492 0x01; w 0x04ad 0x2a
modprobe regmap-i2c
insmod /home/pi/gmsl-vd56g4/driver/generic/generic_adapter.ko
insmod /home/pi/gmsl-vd56g4/driver/vb56g4a.ko vb56g4a_opt=udc,extdes
dtoverlay vd56g4-max9296a-wshat-rpi5-cam0
