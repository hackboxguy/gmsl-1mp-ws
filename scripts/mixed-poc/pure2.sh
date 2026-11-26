#!/bin/sh
# Bisection: pure GMSL2 bring-up of the vd56g4 on link B -> pipe Z -> port B -> Pi CAM0,
# no GMSL1 init at all. Breaks the 1MP until the next power cycle.
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
r() { i2ctransfer -f -y $B w2@$D $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
CSI=1f00110000.csi
echo $CSI > /sys/bus/platform/drivers/rp1-cfe/unbind 2>/dev/null
rmmod vb56g4a 2>/dev/null
w 0x0010 0x80; sleep 0.3                     # RESET_ALL
echo "after reset: 0x0006=$(r 0x0006) 0x0010=$(r 0x0010) 0x0001=$(r 0x0001) 0x0330=$(r 0x0330)"
w 0x0313 0x00
w 0x0005 0x80; w 0x02bc 0x90; sleep 0.2; w 0x02bc 0x80; w 0x02bd 0x84   # 12 V PoC
sleep 1.5                                    # camera head boot
w 0x0001 0x01                                # 3 Gbps
w 0x0010 0x22; sleep 1                       # one-shot, LINK_CFG=2 (link B), AUTO_LINK=0
l=$(r 0x0013); echo "0x0013=$l"
[ $(( l & 8 )) -eq 0 ] && { echo "no GMSL2 lock"; exit 1; }
w 0x048a 0xd0; w 0x0323 0x26; w 0x0326 0x26  # port B: 4 lanes, 600 Mbps
w 0x0052 0x02
w 0x048b 0x07; w 0x048d 0x2a; w 0x048e 0x2a; w 0x048f 0x00; w 0x0490 0x00
w 0x0491 0x01; w 0x0492 0x01; w 0x04ad 0x2a
w 0x0313 0x02
modprobe regmap-i2c
lsmod | grep -q generic_adapter || insmod /home/pi/gmsl-vd56g4/driver/generic/generic_adapter.ko
insmod /home/pi/gmsl-vd56g4/driver/vb56g4a.ko vb56g4a_opt=udc,extdes
if dtoverlay -l | grep -q vd56g4; then echo $CSI > /sys/bus/platform/drivers/rp1-cfe/bind
else dtoverlay vd56g4-max9296a-wshat-rpi5-cam0; fi
sleep 20
sleep 6; dmesg | grep "vb56g4a.*\(locked\|loaded\|rror\)\|$CSI: Using sensor" | tail -4
