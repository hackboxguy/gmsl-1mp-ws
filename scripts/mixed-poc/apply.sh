#!/bin/sh
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2) || { echo "I2C lost"; exit 1; }; }
cap() { printf "%-40s " "$1"; CSIOUT=${CS:-0x02} sh /home/pi/vdcap.sh 10 2>&1 | grep -o "bytes=[0-9]*\|link down.*"; }
cap "working state"
w 0x031d 0xc0;                                     cap "a1 0x031d=0xc0 (PHY0 DPLL off)"
w 0x031d 0xe6;                                     cap "a1b 0x031d=0xe6 (overrides + PHY0 600)"
w 0x0320 0x26;                                     cap "a2 0x0320=0x26"
CS=0x42;                                           cap "a3 0x0313=0x42"
w 0x0316 0x5e; w 0x0317 0x0e; w 0x0319 0x08;       cap "a4 soft dt/bpp"
w 0x0325 0x80;                                     cap "a5 0x0325=0x80"
w 0x0010 0x06;                                     cap "a6 0x0010=0x06"
w 0x040b 0x07; w 0x040d 0x1e; w 0x040e 0x1e; w 0x0411 0x01; w 0x0412 0x01; w 0x042d 0x15; cap "a7 pipe X maps (1MP)"
