# Flowe OS

The device OS lives in `xphone-os` — purpose-built firmware for
Xteink e-ink devices (ESP32-C3). See its [README](xphone-os/README.md)
for architecture and current state.

> **Hardware status:** developed and hardware-tested on the **Xteink X3**.
> The X4 build target compiles but is not validated on hardware.

`freeink-sdk` is the vendored [FreeInk SDK](https://opencollective.com/freeink)
(MIT): the BoardConfig, display, SD card, input, and battery libraries the
firmware builds against.

## Quickstart

```sh
cd xphone-os
pio run -e x3            # or -e x4
pio run -e x3 -t upload  # USB flash (device awake, data cable)
pio device monitor --baud 115200
```

Or flash via SD card: copy `.pio/build/x3/firmware.bin` to the SD root as
`update.bin` and reboot.

For companion-protocol learnings, start with
[../docs/x4-core-learnings.md](../docs/x4-core-learnings.md).

[CrossPoint Reader](https://github.com/daveallie/crosspoint) (MIT) served as
the reference implementation during development; the EPUB reader engine and
file transfer server in xphone-os are ported/derived from it. A local
checkout at `firmware/x4-os` is gitignored — it isn't part of this repo.
