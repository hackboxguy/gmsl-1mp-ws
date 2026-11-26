#!/bin/sh
# Start: gmsl1mp mixed init + dual-poc-b.sh (vd56g4 on link B, no frames).
# Revert gmsl1mp GMSL1 writes toward reset defaults, one group at a time.
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
cap() { printf "%-34s " "$1"; CSIOUT=0x02 sh /home/pi/vdcap.sh 10 2>&1 | grep -o "bytes=[0-9]*\|link down.*"; }
cap "start (mixed)"
w 0x0322 0x00;                                         cap "r1 0x0322=0"
w 0x0316 0x00; w 0x0317 0x00; w 0x0319 0x00; w 0x031d 0x00; cap "r2 soft overrides off"
w 0x0325 0x00;                                         cap "r3 0x0325=0"
w 0x0b06 0x6f; w 0x0b07 0x00; w 0x0b0d 0x00; w 0x0b0f 0x09; w 0x0b96 0x01; cap "r4 link A GMSL1 block default"
w 0x0f00 0x00;                                         cap "r5 0x0f00=0"
w 0x0012 0x00;                                         cap "r6 0x0012=0"
