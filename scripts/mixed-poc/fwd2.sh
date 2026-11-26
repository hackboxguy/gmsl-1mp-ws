#!/bin/sh
# From a working pure-GMSL2 vd56g4 stream (pure2.sh), add gmsl1mp's GMSL1 writes
# in gmsl1mp order, capturing after each. Stops at the first I2C failure.
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2) || { echo "I2C lost after: $*"; exit 1; }; }
cap() { printf "%-36s " "$1"; CSIOUT=0x02 sh /home/pi/vdcap.sh 10 2>&1 | grep -o "bytes=[0-9]*\|link down.*"; }
cap "pure GMSL2"
w 0x0011 0x0f;                                              cap "f1 0x0011=0x0f"
w 0x0b06 0x87; w 0x0b0d 0xb2; w 0x0b07 0x86;                cap "f2 0x0b06/0d/07"
w 0x0322 0x30;                                              cap "f3 0x0322=0x30"
w 0x0006 0x9f;                                              cap "f4 0x0006=0x9f (A GMSL1)"
w 0x0012 0x10;                                              cap "f5 0x0012=0x10"
w 0x0b0f 0x01; w 0x0b96 0x1b;                               cap "f6 0x0b0f/96"
w 0x0f00 0x03;                                              cap "f7 0x0f00=3 (A+B)"
