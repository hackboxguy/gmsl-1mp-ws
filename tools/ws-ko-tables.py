#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Dump the register tables from a Waveshare Raspberry Pi GMSL camera module.

Waveshare ships its Pi 5 GMSL drivers (ws-rpi5-gmsl.zip) as prebuilt .ko
files only. The modules are plain V4L2 i2c subdevs that push static register
tables at stream-on; this script decodes those tables from .rodata.

Table entry layout (12 bytes, little endian), recovered from
gmsl_write_reg_list() disassembly:

    struct { u32 dev; u32 op; u16 reg; u16 val; }

    dev  0 = deserializer (DT "dser-addr", 16-bit register address)
         1 = serializer   (DT "ser-addr",  8-bit register address)
         n = raw 7-bit I2C address n, 8-bit register address
    op   0 = msleep(val), otherwise write val to reg

Usage:
    ws-ko-tables.py gmsl-1mp-camera-a.ko            # every *_reg table
    ws-ko-tables.py gmsl-1mp-camera-a.ko gmsl_2ch_reg

Needs pyelftools (python3-pyelftools).
"""
import struct
import sys

from elftools.elf.elffile import ELFFile


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        elf = ELFFile(f)
        ro = elf.get_section_by_name('.rodata')
        ro_idx = elf.get_section_index('.rodata')
        data = ro.data()
        syms = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()
                if s['st_shndx'] == ro_idx and s['st_info']['type'] == 'STT_OBJECT'}
        names = sys.argv[2:] or sorted(n for n in syms
                                       if n.endswith('_reg') and syms[n]['st_size'])
        for name in names:
            s = syms[name]
            off, size = s['st_value'], s['st_size']
            print(f"/* {name}: {size // 12} entries */")
            for i in range(size // 12):
                dev, op, reg, val = struct.unpack_from('<IIHH', data, off + 12 * i)
                if op == 0:
                    print(f"  DELAY {val} ms")
                    continue
                who = {0: 'DES', 1: 'SER'}.get(dev, f'0x{dev:02x}')
                width = 4 if dev == 0 else 2
                print(f"  {who:5s} 0x{reg:0{width}x} = 0x{val:02x}")
            print()


if __name__ == '__main__':
    main()
