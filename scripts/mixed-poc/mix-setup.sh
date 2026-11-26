#!/bin/sh
# Mixed setup (approved minimal set) on top of gmsl1mp's boot config:
# 1MP GMSL1 on link A (gmsl1mp), vd56g4 GMSL2 on link B -> pipe Z -> port B.
# CSI_OUT_EN left on; ports are restarted per stream by the drivers.
set -e
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
r() { i2ctransfer -f -y $B w2@$D $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
v=$(r 0x0006); w 0x0006 $(( v | 0x80 ))      # link B -> GMSL2
v=$(r 0x0f00); w 0x0f00 $(( v | 0x02 ))      # link enable A+B
w 0x0010 0x06                                 # LINK_CFG=2, no one-shot
i=0; while [ $i -lt 30 ]; do [ $(( $(r 0x0013) & 8 )) -ne 0 ] && break; sleep 0.1; i=$((i+1)); done
l=$(r 0x0013); a=$(r 0x0bcb); echo "0x0013=$l 0x0bcb=$a"
if [ $(( l & 8 )) -eq 0 ] || [ $(( a & 1 )) -eq 0 ]; then echo "link not locked - stopping"; exit 1; fi
w 0x048a 0xd0; w 0x0323 0x26; w 0x0326 0x26
w 0x0052 0x02
w 0x048b 0x07; w 0x048d 0x2a; w 0x048e 0x2a; w 0x048f 0x00; w 0x0490 0x00
w 0x0491 0x01; w 0x0492 0x01; w 0x04ad 0x2a
w 0x0313 0x42
modprobe regmap-i2c
lsmod | grep -q generic_adapter || insmod /home/pi/gmsl-vd56g4/driver/generic/generic_adapter.ko
insmod /home/pi/gmsl-vd56g4/driver/vb56g4a.ko vb56g4a_opt=udc,extdes hack_port_order=${ORDER:-2}
dtoverlay vd56g4-max9296a-wshat-rpi5-cam0
sleep 20; dmesg | grep -c "vb56g4a.*loaded"
