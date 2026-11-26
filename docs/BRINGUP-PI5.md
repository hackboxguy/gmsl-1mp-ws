# Bring-up: Raspberry Pi 5 + Waveshare MAX9296A GMSL HAT + GMSL-1MP-Camera-A

Status 2026-11-26: **both camera paths validated**, one camera at a time, with
the open driver:

- link A → HAT CSI1 → Pi CAM0: 1280×960 UYVY at 30.5 fps;
- link B → HAT CSI0 → Pi CAM1: 30.3–30.45 fps (same camera moved to HAT GMSL1).

Both cameras streaming at once is still untested, pending a second camera.

## 1. Hardware

| item | value |
|---|---|
| Pi | Raspberry Pi 5, Raspberry Pi OS trixie, kernel 6.18.50+rpt-rpi-2712 |
| HAT | Waveshare MAX9296-GMSL-HAT (MAX9296A, dev-id 0x94 at reg 0x000d, 7-bit 0x48) |
| camera | Waveshare GMSL-1MP-Camera-A: MAX96701 GMSL1 serializer (ID reg 0x1e = 0x45, default 7-bit 0x40), on-camera ISP, YUV422 8-bit, 1280×960 @ 30 |
| PoC | supplied by the HAT (12 V). It was on at power-up on this rig, with no GPIO needed. |

### Connectors (as cabled)

```
camera ──FAKRA──> HAT GMSL0 (link A) ─┐                  ┌─ port A ─> HAT CSI0 ─ (J4 / Pi CAM1, 2nd camera, planned)
camera ──FAKRA──> HAT GMSL1 (link B) ─┴─ MAX9296A ───────┴─ port B ─> HAT CSI1 ─FFC─> J3 / Pi CAM/DISP0
```

- HAT **CSI0 = MAX9296A MIPI port A** (controller 1), HAT **CSI1 = port B**
  (controller 2). Proven: video appeared on HAT CSI1 only after routing the
  pipe to controller 2 (`0x042d = 0x2a`).
- Pi CAM0 (J3) = `rp1-cfe 1f00110000.csi`, I2C bus **10**. Pi CAM1 (J4) =
  `1f00128000.csi`, I2C bus 11.
- The deserializer's I2C reaches the Pi over **both** FFCs: with the J4 ribbon
  fitted, 0x48 answers on bus 10 and on bus 11. A bus-11 scan also shows an
  unidentified device at 0x1a, which bus 10 does not show. By default the overlay
  keeps both camera nodes on bus 10 and leaves bus 11 disabled. `i2c-cam1` moves
  them to bus 11; this was tested working with link B only (30.27 fps). Never enable
  both, because two Pi I2C masters would then drive the same deserializer.

### I2C map on bus 10 (one camera, link up)

| 7-bit | device |
|---|---|
| 0x48 | MAX9296A |
| 0x40 → **0x42** | MAX96701, remapped by the driver (link A). Link B goes to 0x44. |
| 0x50–0x53, 0x58–0x5b | camera-side devices behind the serializer (ISP/EEPROM?), unused |

## 2. Install

```sh
git clone https://github.com/hackboxguy/gmsl-1mp-ws.git
cd gmsl-1mp-ws
sudo ./scripts/install-pi5.sh          # one camera on link A -> HAT CSI1 -> CAM0
# sudo ./scripts/install-pi5.sh link-b # + second camera on link B -> HAT CSI0 -> CAM1
sudo reboot
```

The script uses DKMS (rebuilt on kernel updates). It compiles the overlay into
`/boot/firmware/overlays/ws-gmsl1mp.dtbo`, installs `gmsl1mp-ctl` and sets
`camera_auto_detect=0` plus `dtoverlay=ws-gmsl1mp[,…]`. Overlay parameters are
documented at the top of `dts/ws-gmsl1mp-overlay.dts`.

If Waveshare's own package is installed, comment out its
`dtoverlay=gmsl-1mp-camera-a…` line. Both drivers cannot own the same
deserializer.

## 3. Use

```sh
gmsl1mp-ctl fps cam0                  # -> ~30.5 fps
gmsl1mp-ctl snap cam0 /tmp/a.jpg
gmsl1mp-ctl record cam0 /tmp/a.mkv 10 # H.264 via x264enc (Pi 5 has no HW encoder)
gmsl1mp-ctl capture cam0 30 /tmp/a.uyvy
gmsl1mp-ctl preview cam0              # needs a display (GMSL1MP_SINK=kmssink for console)
gmsl1mp-ctl preview cam0 cam1         # both cameras side by side
cat /sys/bus/i2c/devices/10-0042/status
```

To reload the driver without a reboot, first unbind the receiver, because rp1-cfe
holds the module:
`echo 1f00128000.csi | sudo tee /sys/bus/platform/drivers/rp1-cfe/unbind`
(`1f00110000.csi` for CAM0). Then run `rmmod gmsl1mp; modprobe gmsl1mp`, and finally
write the same name to `…/rp1-cfe/bind`.

A raw frame on a host: `ffmpeg -f rawvideo -pix_fmt uyvy422 -s 1280x960 -i a.uyvy a.png`.

## 4. Verification, stage by stage

```sh
sudo i2ctransfer -f -y 10 w2@0x48 0x00 0x0d r1   # 0x94  deserializer alive
sudo i2ctransfer -f -y 10 w2@0x48 0x0b 0xcb r1   # bit0 = GMSL1 link A locked (0x0ccb: link B)
sudo i2ctransfer -f -y 10 w1@0x42 0x1e r1        # 0x45  serializer, remapped
cat /sys/bus/i2c/devices/10-0042/status          # everything above + routing
gmsl1mp-ctl fps cam0                              # frames
```

`status` while idle on the validated rig:

```
link=A port=B vc=0 lanes=4 link_freq=300000000 ser_addr=0x42
gmsl1_lock_a=1 gmsl1_lock_b=0 gmsl1_en=0x01 csi_out=0x00 streaming=0
port_lanes_reg=0xd0 port_dpll_reg=0x26
pipe_map_en=0x07 map0_dst=0x1e dphy_dest=0x2a
ser_id=0x45
```

## 5. Results

| test | result |
|---|---|
| probe (global init + link A) | ~2 s, serializer remapped 0x40 → 0x42 |
| first stream after boot | frames immediately (vendor driver: none, see REGISTER-SEQUENCE §2) |
| frame rate | 30.44–30.50 fps, 1280×960 UYVY, byte count exact |
| repeated start/stop | OK |
| driver unbind/rmmod/reload, no power cycle | OK (serializer found at 0x42 and reused) |
| `v4l2-compliance -u` (sensor subdev) | 46/46, 0 warnings |
| H.264 record 5 s | 150 frames, 30/1, 4.93 s |
| `link-b` with no second camera | link B probe fails cleanly (`-ENOLINK`); link A unaffected, 30.47 fps |
| link B only (camera on GMSL1, HAT CSI0 → J4), `link-b,no-link-a` | serializer 0x40 → 0x44, port A, 30.37–30.42 fps, valid image |
| `link-b` with only the link-B camera present | link A fails cleanly (`-ENOLINK`); link B 30.43 fps, H.264 5 s = 150 frames at 30/1 |
| link B reload (unbind rp1-cfe, rmmod, modprobe, bind) | OK, 30.45 fps |
| link B with I2C on bus 11 (`i2c-cam1`) | OK, 30.27 fps |

## 6. Dual camera: what to check when camera 2 + J4 ribbon arrive

1. Cable camera 2 to HAT **GMSL1** and HAT **CSI0** to Pi **J4**.
2. `sudo sed -i 's/^dtoverlay=ws-gmsl1mp.*/dtoverlay=ws-gmsl1mp,link-b/' /boot/firmware/config.txt`, reboot.
3. Expect in dmesg: `link B: serializer at 0x44, id 0x45` and
   `link B -> MIPI port A, VC0, 4 lanes @ 600 Mbps/lane`.
4. `gmsl1mp-ctl fps cam1`, then both at once: `gmsl1mp-ctl fps cam0 & gmsl1mp-ctl fps cam1`.
5. Each path is already validated on its own (§5). What remains new here is
   both links locked at once and both ports streaming simultaneously.

Two cameras into **one** Pi CSI port, separated by virtual channel (what
Waveshare's `GMSL_2CH` mode does), is not supported on the Pi by this driver:
rp1-cfe takes one source subdev per receiver. The serdes core already supports
it (`deser-port` + `vc-id` per camera), which is the topology needed on the
Jetson adapter (§7).

## 7. Jetson Orin Nano (planned)

The Waveshare Jetson MAX9296A board brings out **port A only**, as the
gmsl-vd56g4 bring-up found (`gmsl-vd56g4/docs/ORIN-WAVESHARE-BRINGUP.md` §3).
There, both cameras are `deser-port = <0>` with `vc-id = <0>` / `<1>` on one Orin
CSI port. Plan:

- reuse `gmsl1mp_serdes.c` unchanged;
- add `gmsl1mp_tegracam.c` (tegracam / NVIDIA camera framework frontend, one
  sensor node per camera, `vc-id` per node) for JetPack 6/7 and Yocto L4T r39,
  selected with `make board=orin NV_OOT=…` as in gmsl-vd56g4;
- the Jetson overlay should mirror Waveshare's tegra DT (lane/pixel-clock
  fields for YUV422 at 600 Mbps/lane).
