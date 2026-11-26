# gmsl-1mp-ws

Open-source Linux driver for the **Waveshare GMSL-1MP-Camera-A** (1280×960
YUV422 @ 30 fps, on-camera ISP, MAX96701 GMSL1 serializer) behind a
**MAX9296A** deserializer, for example the Waveshare MAX9296-GMSL-HAT on a
Raspberry Pi 5.

```
camera (ISP + MAX96701) ──GMSL1 coax──> MAX9296A ──MIPI CSI-2──> Pi 5 rp1-cfe
```

It replaces Waveshare's binary-only `gmsl-1mp-camera-a.ko`. The register
sequence was recovered from that module and documented
([docs/REGISTER-SEQUENCE.md](docs/REGISTER-SEQUENCE.md)). The open driver adds:

- **per-camera routing in DT**: GMSL link (A/B) → MAX9296A MIPI port (A/B) →
  CSI-2 virtual channel, so a camera can leave on either HAT CSI connector
  (the vendor driver only drives port A / HAT CSI0);
- **two cameras on two Pi CSI receivers** from one shared deserializer, with
  serializer address remapping (0x40 → 0x42 / 0x44);
- frames from the first stream after boot (the vendor driver drops it);
- DKMS packaging, so it is not tied to one exact kernel build;
- `v4l2-compliance` clean (46/46).

## Layout

```
driver/   gmsl1mp.ko: gmsl1mp_serdes.c (host-independent serdes core)
          + gmsl1mp_v4l2.c (generic V4L2 async subdev frontend)
dts/      ws-gmsl1mp-overlay.dts: Raspberry Pi 5 overlay (1 or 2 cameras)
scripts/  install-pi5.sh (DKMS + overlay), gmsl1mp-ctl.sh (media graph,
          capture, fps, snapshot, preview, H.264 record)
tools/    ws-ko-tables.py: decode register tables from Waveshare's .ko
docs/     REGISTER-SEQUENCE.md, BRINGUP-PI5.md
```

## Quick start (Raspberry Pi 5, Raspberry Pi OS)

```sh
sudo ./scripts/install-pi5.sh            # camera on HAT GMSL0, HAT CSI1 -> Pi CAM0
sudo reboot
gmsl1mp-ctl fps cam0                     # ~30.5 fps
gmsl1mp-ctl snap cam0 /tmp/a.jpg
gmsl1mp-ctl record cam0 /tmp/a.mkv 10
```

Second camera on HAT GMSL1 with HAT CSI0 → Pi CAM1: `sudo ./scripts/install-pi5.sh link-b`.
Other routings use overlay parameters (`a-port`, `b-port`, `a-cam1`, `b-cam0`,
`no-link-a`, `dser-addr`, …); see the header of
[dts/ws-gmsl1mp-overlay.dts](dts/ws-gmsl1mp-overlay.dts).

Build only: `make -C driver` (native, kernel headers installed) or
`make -C driver KERNEL_SRC=… ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-`.
Linux ≥ 6.8.

## Status

| target | state |
|---|---|
| Pi 5, one camera (link A → port B → CAM0) | **validated**, 30.5 fps |
| Pi 5, two cameras (link B → port A → CAM1) | implemented; missing-camera path tested, awaiting 2nd camera |
| Jetson Orin Nano (JetPack / Yocto L4T r39) | planned: tegracam frontend over the same serdes core, both cameras on port A by VC ([BRINGUP-PI5.md §7](docs/BRINGUP-PI5.md#7-jetson-orin-nano-planned)) |

## License

GPL-2.0, see [LICENSE](LICENSE).
