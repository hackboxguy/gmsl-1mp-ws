#!/bin/sh
# Apply gmsl1mp's GMSL1 global init in groups on top of a working pure-GMSL2
# vd56g4 stream (link B -> pipe Z -> port B), capturing after each group.
B=10; D=0x48
w() { i2ctransfer -f -y $B w3@$D $(printf "0x%02x 0x%02x 0x%02x" $(($1>>8)) $(($1&255)) $2); }
r() { i2ctransfer -f -y $B w2@$D $(printf "0x%02x 0x%02x" $(($1>>8)) $(($1&255))) r1; }
cap() { printf "%-40s " "$1"; sh /home/pi/vdcap.sh 10 2>&1 | grep bytes; }
step() {
  case $1 in
  1) w 0x0006 0x9f ;;                                             # link A -> GMSL1
  2) w 0x0011 0x0f; w 0x0012 0x10 ;;
  3) w 0x0b06 0x87; w 0x0b0d 0xb2; w 0x0b07 0x86; w 0x0b0f 0x01; w 0x0b96 0x1b ;;
  4) w 0x0f00 0x01 ;;                                             # GMSL1 link A enable
  5) w 0x0322 0x30 ;;
  6) w 0x0314 0x00; w 0x0316 0x5e; w 0x0317 0x0e; w 0x0319 0x08 ;;
  7) w 0x031d 0xc0 ;;
  8) w 0x0325 0x80 ;;
  9) w 0x0010 0x26 ;;                                             # one-shot reset, link B
  esac
}
cap "baseline"
for s in ${STEPS:-1 2 3 4 5 6 7 8 9}; do step $s; sleep 1; cap "after step $s"; done
