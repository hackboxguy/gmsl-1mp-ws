#!/bin/sh
# PoC (datasheet Fig. 27 topology): GMSL1 1MP on link A (gmsl1mp, port A ->
# Pi CAM1) + GMSL2 VD56G4 on link B -> pipe Z -> MIPI port B -> Pi CAM0.
# Run as root after boot with dtoverlay=ws-gmsl1mp,a-port=0,a-cam1.
set -e
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
r() { i2ctransfer -f -y $B w2@$D $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
# Link B's GMSL1 block back to reset defaults (gmsl1mp's GMSL1 init set them
# for both links; 0x0c96 CONV_GMSL1_EN / 0x0c07 DBL+HVEN must not apply to GMSL2).
if [ -z "$KEEP_GMSL1_B" ]; then
  w 0x0c06 0x6f; w 0x0c07 0x00; w 0x0c0d 0x00; w 0x0c0f 0x09; w 0x0c96 0x01
fi
v=$(r 0x0006); w 0x0006 $(( v | 0x80 ))      # link B -> GMSL2, link A stays GMSL1
# 0x0f00 LINK_EN gates EVERY link (not only GMSL1): gmsl1mp set only link A's
# bit, which blocks link B's GMSL2 video from the CSI outputs.
v=$(r 0x0f00); w 0x0f00 $(( v | 0x02 ))
# CTRL0: RESET_ONESHOT | REG_ENABLE | LINK_CFG=2 (GMSL2 single link B), AUTO_LINK=0
# Link B up: by default WITHOUT a CTRL0 one-shot (a data-path reset while GMSL1
# link A runs breaks GMSL2 video output); just select link B and poll for lock.
w 0x0010 ${CTRL0:-0x06}
i=0; while [ $i -lt 30 ]; do [ $(( $(r 0x0013) & 8 )) -ne 0 ] && break; sleep 0.1; i=$((i+1)); done
l=$(r 0x0013); a=$(r 0x0bcb)
echo "0x0010=$(r 0x0010) 0x0013=$l (bit3 GMSL2 lock)  0x0bcb=$a (bit0 GMSL1 link A lock)"
if [ $(( l & 8 )) -eq 0 ] || [ $(( a & 1 )) -eq 0 ]; then echo "link not locked - stopping"; exit 1; fi
# After a data-path reset in mixed mode the GMSL2 link's video does not reach the
# CSI outputs until link A is flipped to GMSL2 and back (no reset) - measured.
[ -z "$NOTOGGLE" ] && { v=$(r 0x0006); w 0x0006 $(( v | 0x40 )); sleep 0.2; w 0x0006 $v; sleep 0.5; }
echo "after 0x0006 toggle: 0x0013=$(r 0x0013) 0x0bcb=$(r 0x0bcb)"
# MIPI port B: 4 lanes, PHY2+PHY3 DPLL 600 Mbps/lane
w 0x048a 0xd0; w 0x0323 0x26; w 0x0326 0x26
# pipe Z (MAX96717F pipe Z = stream ID 2): RAW8 + FS + FE -> MIPI port B
w 0x0052 0x02
w 0x048b 0x07; w 0x048d 0x2a; w 0x048e 0x2a; w 0x048f 0x00; w 0x0490 0x00
w 0x0491 0x01; w 0x0492 0x01; w 0x04ad 0x2a
[ -n "$NOLOAD" ] && exit 0
modprobe regmap-i2c
insmod /home/pi/gmsl-vd56g4/driver/generic/generic_adapter.ko
insmod /home/pi/gmsl-vd56g4/driver/vb56g4a.ko vb56g4a_opt=udc,extdes
dtoverlay vd56g4-max9296a-wshat-rpi5-cam0
