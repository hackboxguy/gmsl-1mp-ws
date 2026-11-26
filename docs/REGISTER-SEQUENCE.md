# Register sequence: MAX9296A + MAX96701 (GMSL-1MP-Camera-A)

Where the driver's register values come from, what is known about each, and
how the vendor sequence was split into the driver's global / per-link /
stream steps.

> **Since the split** (gmsl_max9296a, repo gmsl-max9296a), the values below live in two places:
> - **The deserializer driver** does the chip init, PoC, ports, pipe maps, the soft
>   DT override (from `gmsl,data-type`) and the per-stream port start.
> - **This camera driver** hands over its MAX9296A GMSL1 block values
>   (`0x0b06/07/0d/0f/96`), soft BPP 8 and the YUV422 8/10-bit mux flag
>   (`0x0322`, which turned out to be per pipe: bit 4 + pipe) at link claim
>   (`gmsl1mp_ser.c`), and keeps the MAX96701 sequence.
>
> The stream gate is no longer `0x0313` on/off. `CSI_OUT_EN` stays on and each
> MIPI port is released from PHY standby (`0x0332`) per stream.

## 1. Source

Waveshare distributes its Raspberry Pi 5 support as
`https://files.waveshare.com/wiki/MAX9296-GMSL-Deser-Module/ws-rpi5-gmsl.zip`:
prebuilt `.ko` files per Pi kernel (no source), `.dtbo` overlays and two shell
scripts. The 1MP module is `gmsl-1mp-camera-a.ko` (`MODULE_LICENSE("GPL")`,
compatible `ws,gmsl-1mp-camera-a`).

It is a plain V4L2 i2c subdev that writes static register tables. The tables
are in `.rodata` as `gmsl_1ch_linka_reg`, `gmsl_1ch_linkb_reg`, `gmsl_2ch_reg`
(chosen by the DT `sensor-type` string) and an empty `gmsl_1280_960_30fps_reg`.
Entry layout, recovered from the `gmsl_write_reg_list()` disassembly:

```c
struct { u32 dev; u32 op; u16 reg; u16 val; };   /* 12 bytes */
/* dev 0 = deserializer (dser-addr, 16-bit reg), 1 = serializer (ser-addr,
 * 8-bit reg), other = literal 7-bit I2C address (8-bit reg).
 * op 0 = msleep(val), else write. */
```

`tools/ws-ko-tables.py <module.ko>` dumps them. The output for the
6.18.50+rpt-rpi-2712 build is reproduced in §4.

`gmsl_set_stream()` (stream on):

1. write deserializer `0x0313 = 0x42` (CSI output enable)
2. if the mode changed since the last stream-on (i.e. the first time): write
   the whole table (which begins with a RESET_ALL and `0x0313 = 0x00`)

Stream off writes `0x0313 = 0x00`. Probe touches no hardware.

## 2. Bugs in the vendor flow (all avoided by gmsl1mp)

| # | Symptom | Cause |
|---|---|---|
| 1 | **First stream after boot delivers no frames**; the second works | step 1 enables the CSI output, then step 2's table resets it to `0x00`. Only the 2nd stream-on (table skipped) leaves it on. |
| 2 | No frames with the FFC on the HAT's **CSI1** connector | every table routes the video pipe to MIPI controller 1 = port A = HAT CSI0 (`0x042D/0x046D = 0x15`). HAT CSI1 is port B (controller 2, `0x2A`). |
| 3 | rp1-cfe programs a 1200 Mbps D-PHY for a 600 Mbps link | overlay `link-frequencies = 1200000000` vs deserializer DPLL `0x0320 = 0x26` (600 Mbps/lane). The Pi receiver happens to tolerate it. |
| 4 | Link B single-camera table maps nothing | `gmsl_1ch_linkb_reg` never writes the pipe Y map; it relies on whatever is there. |

## 3. Decoded meaning and where each write lives in gmsl1mp

Field names come from the MAX9296A register map where known. Rows marked "as
vendor" are reproduced without an independent decode. They are validated only
by the fact that streaming works.

### 3.1 Global init: `des_global_init[]`, once per MAX9296A

| reg | value | meaning |
|---|---|---|
| 0x0010 | 0x80 | CTRL0.RESET_ALL, then 200 ms |
| 0x0313 | 0x00 | BACKTOP12: CSI output off |
| 0x0011 | 0x0f | as vendor |
| 0x0b06/0x0c06 | 0x87 | GMSL1 link A/B control: as vendor |
| 0x0b0d/0x0c0d | 0xb2 | GMSL1 link A/B: as vendor |
| 0x0b07/0x0c07 | 0x86 | GMSL1 link A/B input config (DBL/HVEN family): as vendor |
| 0x0322 | 0x30 | as vendor |
| 0x0005 | 0x80 | as vendor |
| 0x02bc | 0x90 → 0x80 | MFP4 GPIO_A: output high for 200 ms, then low |
| 0x02bd | 0x84 | MFP4 GPIO_B. Measured: this pulse does **not** power-cycle the camera (the serializer keeps its remapped address). |
| 0x0006 | 0x1f | both links in GMSL1 mode (GMSL2 bits clear) |
| 0x0010 | 0x35 | CTRL0: AUTO_LINK, RESET_ONESHOT, link cfg |
| 0x0012 | 0x10 | as vendor, then 200 ms |
| 0x0330 | 0x04 | MIPI PHY: 2 × 4-lane ports |
| 0x0314, 0x0316, 0x0317, 0x0319, 0x031d, 0x0325 | 00, 5e, 0e, 08, c0, 80 | BACKTOP soft data-type / bpp overrides for GMSL1 YUV422 (`0x0316 = 0x40|0x1e`). `0x0319 = 0x08` is from the link-B/2ch tables (power-on value 0x00) and is applied always. |
| 0x0b0f/0x0c0f | 0x01 | as vendor |
| 0x0b96/0x0c96 | 0x1b | as vendor |
| 0x0f00 | 0x00 | GMSL1 link enable mask cleared; links are enabled per camera |

### 3.2 Per link: `gmsl1mp_link_add()`

| reg | value | meaning |
|---|---|---|
| 0x044a (port A) / 0x048a (port B) | bits[7:6] = lanes−1 | MIPI controller 1/2 lane count (vendor: 0xc0 on both) |
| 0x0320 (port A) / 0x0323 (port B) | 0x20 \| (Mbps/100) | D-PHY PLL override, from DT `link-frequencies` (2 × f). Vendor sets 0x0320 = 0x26 only; port B was left at power-on 0x2f (1500 Mbps). |
| pipe base 0x040b (link A, pipe X) / 0x044b (link B, pipe Y) | | |
| +0x02/+0x03 | 0x1e → vc<<6 \| 0x1e | map0: YUV422-8 video, VC0 → VC `vc-id` |
| +0x04/+0x05 | 0x00 → vc<<6 | map1: frame start |
| +0x06/+0x07 | 0x01 → vc<<6 \| 0x01 | map2: frame end |
| +0x22 (0x042d / 0x046d) | 0x15 / 0x2a | destination controller for maps 0–2: 0b01 = controller 1 = **port A (HAT CSI0)**, 0b10 = controller 2 = **port B (HAT CSI1)** |
| +0x00 | 0x07 | enable maps 0–2 |
| 0x0f00 | \|= 1 << link | enable this GMSL1 link (bit0 A, bit1 B), then wait for lock |
| 0x0bcb / 0x0ccb | bit0 | GMSL1 link A/B lock (polled, 1 s timeout) |

Vendor 2ch mapping for link B was VC1 on port A (`0x044e = 0x5e`). gmsl1mp
makes VC and port per-camera DT properties instead.

Then the serializer (MAX96701, 8-bit registers, default 7-bit address 0x40):

| step | write | meaning |
|---|---|---|
| remap | `0x40: 0x00 = new << 1`, 200 ms | own I2C address (vendor 2ch: 0x88 → 0x44 for link A, 0x84 → 0x42 for link B) |
| | `0x04 = 0x47`, 200 ms | config link on, serial video off |
| | `0x0f = 0xbf`, 200 ms | as vendor |
| | `0x07 = 0x84` | as vendor (DBL / bus width family) |
| | `0x40 = 0x0d` | as vendor |
| | `0x04 = 0x87`, 200 ms | serial video on |

Reg 0x1e reads 0x45 (device ID) and is logged at probe. Only the serializer
of the link being added can still be at 0x40: every other enabled link was
remapped first. That is the same reason Waveshare's 2ch table enables link A
alone, remaps, then link B alone, remaps.

### 3.3 Stream: `gmsl1mp_stream()`

`0x0313 = 0x42` when the first camera on the deserializer starts and `0x00`
when the last one stops. It is reference counted because the CSI output
enable is global to both ports.

## 4. Vendor tables (verbatim dump)

Decoded from `modules/6.18.50+rpt-rpi-2712/gmsl-1mp-camera-a.ko`. `SER` =
serializer at `ser-addr` (0x40). `0x42`/`0x44` are literal addresses.

`gmsl_1ch_linka_reg` (51 entries):

```
DES 0x0010=0x80  DELAY 200  DES 0x0313=0x00  DES 0x0011=0x0f  DES 0x0f00=0x01
DES 0x0b06=0x87  DES 0x0c06=0x87  DES 0x0b0d=0xb2  DES 0x0c0d=0xb2
DES 0x0b07=0x86  DES 0x0c07=0x86  DES 0x0322=0x30  DES 0x0005=0x80
DES 0x02bc=0x90  DELAY 200  DES 0x02bc=0x80  DES 0x02bd=0x84  DELAY 200
DES 0x0006=0x1f  DES 0x044a=0xc0  DES 0x048a=0xc0  DES 0x0010=0x35
DES 0x0012=0x10  DELAY 200
DES 0x040b=0x07  DES 0x042d=0x15  DES 0x040d=0x1e  DES 0x040e=0x1e
DES 0x040f=0x00  DES 0x0410=0x00  DES 0x0411=0x01  DES 0x0412=0x01
DES 0x0330=0x04  DES 0x0320=0x26  DES 0x0314=0x00  DES 0x0316=0x5e
DES 0x0317=0x0e  DES 0x031d=0xc0  DES 0x0325=0x80  DES 0x0b0f=0x01
DES 0x0c0f=0x01  DES 0x0b96=0x1b  DES 0x0c96=0x1b
SER 0x04=0x47  DELAY 200  SER 0x0f=0xbf  DELAY 200  SER 0x07=0x84
SER 0x40=0x0d  SER 0x04=0x87  DELAY 200
```

`gmsl_1ch_linkb_reg` (44 entries) is the same, except that it has
`0x0f00 = 0x02` and **no** pipe-mapping block (`0x040b`…`0x0412`, `0x042d`),
and it adds `0x0319 = 0x08` after `0x0314`.

`gmsl_2ch_reg` (74 entries) is the common prefix up to `0x0012=0x10 / DELAY`,
then:

```
DES 0x040b=0x07  DES 0x042d=0x15  DES 0x040d=0x1e  DES 0x040e=0x1e
DES 0x040f=0x00  DES 0x0410=0x00  DES 0x0411=0x01  DES 0x0412=0x01
DES 0x044b=0x07  DES 0x046d=0x15  DES 0x044d=0x1e  DES 0x044e=0x5e
DES 0x044f=0x00  DES 0x0450=0x40  DES 0x0451=0x01  DES 0x0452=0x41
DES 0x0330=0x04  DES 0x0320=0x26  DES 0x0314=0x00  DES 0x0319=0x08
DES 0x0316=0x5e  DES 0x0317=0x0e  DES 0x031d=0xc0  DES 0x0325=0x80
DES 0x0b0f=0x01  DES 0x0c0f=0x01  DES 0x0b96=0x1b  DES 0x0c96=0x1b
DES 0x0f00=0x01  SER 0x00=0x88  DELAY 200
0x44 0x04=0x47  DELAY 200  0x44 0x0f=0xbf  DELAY 200  0x44 0x07=0x84
0x44 0x40=0x0d  0x44 0x04=0x87  DELAY 200
DES 0x0f00=0x02  SER 0x00=0x84  DELAY 200
0x42 0x04=0x47  DELAY 200  0x42 0x0f=0xbf  DELAY 200  0x42 0x07=0x84
0x42 0x40=0x0d  0x42 0x04=0x87  DELAY 200
DES 0x0f00=0x03
```

The vendor overlay's `ser-addr` is 0x40 in every mode. In 2ch mode link A ends
up at 0x44 and link B at 0x42. gmsl1mp uses each camera node's `reg` instead
(overlay defaults: link A 0x42, link B 0x44).

## 5. Other I2C devices seen on the camera bus

With one camera connected and the link up, the bus also shows `0x50`–`0x53` and
`0x58`–`0x5b`, which are behind the serializer. They are probably the
camera's ISP and EEPROM. The driver never addresses them. With two cameras
they will exist twice behind the same deserializer and collide, which is
harmless while nothing talks to them. If ISP access is ever needed, use the
MAX96701 I2C address translation registers to separate them.
