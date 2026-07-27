# xphone-os — a focus device for the Xteink X3

> **Hardware status:** developed and hardware-tested on the **Xteink X3**.
> The X4 env (`pio run -e x4`) compiles — same ESP32-C3, different panel
> (SSD1677 800×480) and battery path — but is not validated on hardware.

> For project history/architecture see docs/xphone-os-m0-m3-handoff.md
> (M0–M3 handoff). This README's "Current state" reflects work past M3.

Purpose-built companion OS for Xteink e-ink devices (ESP32-C3). A
distraction-blocking *focus* device driven by a paired iPhone — NOT an email/
messaging client. Built with CrossPoint Reader as the reference implementation
(reader engine and file transfer server ported from it, MIT), consuming the
vendored FreeInk SDK (`../freeink-sdk`) via symlink lib_deps.

## Current state (2026-07-15)

Branch `codex/x4-ancs-inbox`. Flashes over USB (4-pin data cable) or SD card.

**Product — six focus apps, 3×2 launcher:**
* **Block** — triggers iOS Screen Time shields via the companion BLE protocol;
  local minute countdown; wake shows the last-known locked screen instantly
  ("until 10:30 AM", seeded from NVS) then live-syncs.
* **Priorities** — to-do snapshot from the phone; doubles as the dormant sleep
  screen (fetched pre-sleep even if the app was never opened).
* **Today** — agenda/reminders card from iOS EventKit (`TodayManager`), plus a
  weather band under the header: condition+temp left, high/low right, collapsing
  to nothing when the card carried no weather. The phone produces those two
  strings in `WeatherProvider` (WeatherKit, Open-Meteo as the keyless fallback).
  Needs on-glass validation after Calendar/Reminders permission.
* **Notifications** — iOS notifications via ANCS (incl. iMessage/SMS, which
  arrive as the Social/Other category — no category filtering). Detail view,
  clear-one/clear-all. Newest 10 entries persist across sleep in NVS; on wake
  ANCS re-backfills up to 20 Notification Center items into that seed.
* **Reader** — EPUB library grid + page renderer (section.bin cache, covers,
  12/14/16pt). Procedural book glyph until a bitmap icon is sourced.
* **Workout** — sets-only exercise tracker synced from iPhone; can own the
  sleep face when sleeping from that scene. Procedural dumbbell glyph for now.
* **Settings** opens from the launcher BACK soft-key (not a tile); **About**
  (with wake/heap/battery diagnostics) lives inside Settings. Camera/Inbox/
  Messages were removed to keep the surface focused.

**Power model:** press power = sleep (deep sleep, GPIO3 wake); hold 2.5s =
restart; 10-min idle auto-sleep (2-min during an active block, which does NOT
pin the device awake — the phone enforces the block). Wake restores the
last scene (persisted in NVS — RTC memory did NOT survive the X3's power-on-
reset wake). Dormant frame = priorities list + a padlock "Block active until
…" stamp when a block is running.

**Wake is quiet:** resync only the on-glass scene's rail (others sync lazily on
open), retry ≤3, ANCS backfill ≤20; a small sync dot sits left of the battery
while syncing. Per-scene dirty scoping means an unrelated card never repaints
the current scene.

**Sizes (x3):** ~96.5 KB RAM (29%) / ~837 KB flash (12.8%). vs CrossPoint
109 KB / 5.35 MB.

### Dev workflow
* Build: `pio run -e x3` (default) or `-e x4`. Both must pass.
* **Flash over USB** (preferred — gives serial too): `pio run -e x3 -t upload
  --upload-port /dev/cu.usbmodem*` (~20s, hash-verified). The X3 needs a 4-pin
  data pogo cable (2-pin is charge-only). Deep sleep drops the USB port — press
  power to wake before flashing.
* **Serial**: 115200. Capture with a pyserial reset-and-read (DTR/RTS pulse);
  the device is silent when idle (e-ink logs on events only). This is the main
  debugging tool — e.g. the ANCS queue-overflow and encrypted-vs-connected
  resync bugs were both diagnosed live over serial.
* **Flash over SD** (fallback): copy the build to SD root as `update.bin`; the
  boot updater flashes it. SD/reader is flaky — verify (unmount/remount + cmp)
  before trusting. Settings → SD Firmware Update picks any `.bin` (rollback).
* **iOS app**: `xcodebuild -project ios/X4Companion.xcodeproj -scheme
  X4Companion -destination 'id=<UDID>' -allowProvisioningUpdates build` then
  `xcrun devicectl device install app --device <UDID> <app>`.
* **Design first**: `tools/x4-screen-lab` (X3 mode) + `X3-LAYOUT-GUIDE.md`.

### Known TODOs / open notes
* **Today on-glass validation** — iOS EventKit producer shipped (`TodayManager`);
  confirm Calendar/Reminders grant → sync → day buckets/overdue on device, plus
  wake with the NVS-cached Today card, and the weather band (needs a real
  WeatherKit/Open-Meteo fetch on the phone, not just a synced snapshot).
* **Launcher icons** — Block/Today/Notifications sources are still 96px, scaled
  by `tools/xphone-icons/build_launcher_icons.py` to 104px (not 120 — comments
  elsewhere are stale). Re-source those three at ≥208px, keep Priorities weight
  matched, regenerate. Reader/Workout still use procedural glyphs.
* **Notification delta sync** — NVS already seeds the newest 10 across sleep;
  wakes still run a full ≤20 ANCS backfill. Optional: raise persist cap or skip
  UIDs/dates already known.
* Camera could return under Settings if ever wanted.

---

Below is the original M1 scope for reference; see the handoff doc for M2–M3.

The FreeInk SDK is consumed read-only from the sibling `../x4-os` checkout via
symlink lib_deps: `BoardConfig`, `EInkDisplay`, `SDCardManager`,
`InputManager`, `BatteryMonitor`. The two UI fonts are the x4-os builtin
`EpdFontData` headers (Ubuntu 12pt regular/bold), consumed via file symlinks
in `src/fonts/`.

## M1 scope

* **Input** (`src/Input.h`) — logical buttons (Up/Down/Left/Right/Confirm/
  Back) over the SDK `InputManager` ADC ladders. Fixed default mapping (the
  x4-os defaults): front buttons Back/Confirm/Left/Right = hardware 0/1/2/3
  on the GPIO1 ladder, side Up/Down = 4/5 on the GPIO2 ladder. Polled from
  `loop()` every 10ms; 5ms debounce lives in the SDK.
* **Scene manager** (`src/Scene.{h,cpp}`) — one active scene
  (onEnter/onExit/handleInput/render), `switchTo()`, dirty-flag rendering:
  scenes repaint only on state change, never periodically. FULL refresh on
  scene switch, FAST (differential) refresh for selection moves. Single task,
  all scenes static instances, no heap in the loop.
* **Text** (`src/Gfx.{h,cpp}`, `src/Fonts.cpp`) — bespoke ~150-line blitter
  over the raw framebuffer, consuming the SDK's uncompressed EpdFontData
  glyph format directly (packing verified against x4-os
  `GfxRenderer::renderCharImpl`). The full GfxRenderer was rejected: it drags
  x4-os `lib/hal` wholesale (WiFi via HalPowerManager, HalStorage/SdFat via
  Bitmap.h) plus Logging/Utf8/MiniBidi/uzlib. Only the font headers'
  bitmap/glyph/interval arrays are referenced, so their kern matrices
  (~24KB/font) and ligature tables compile out — the whole two-weight font
  stack costs **81,853 bytes** of flash. Kerning/ligatures are skipped (UI
  labels only). The UI renders in **logical portrait** (X3: 528x792, X4:
  480x800, phone-style); `Gfx::drawPixel()` rotates 90 degrees CW into the
  native landscape framebuffer, matching CrossPoint's default
  `GfxRenderer::Portrait` transform.
* **Launcher** (`src/scenes/`) — status bar ("xphone" left, battery percent
  right) over a 3-column app grid: Block, Inbox, Messages, Notifications,
  Camera, Today, Settings, About. Selection = rounded border, moves with
  clamping. Every app opens a shared placeholder scene ("parked for M2")
  except **About**: version, panel, boot-to-first-paint ms, heap
  free/min/largest block.
* **Battery** — included (cheap): SDK `BatteryMonitor`. X3 reads the BQ27220
  I2C fuel gauge (`-DFREEINK_BATTERY_I2C_GAUGE=1`, SDA20/SCL0 per
  BoardConfig); X4 reads the ADC divider on GPIO0. Read failure renders
  `--%`.

## Controls

| Button | Launcher | App / About |
|--------|----------|-------------|
| Up/Down (side) | move selection by row (clamped) | — |
| Left/Right (front) | move selection by column (clamped) | — |
| Confirm (front) | open selected app | — |
| Back (front) | — | return to launcher |

Known M1 limitation: input is not sampled during an e-ink refresh (single
task, no async poll task yet), so presses landing mid-refresh are dropped.

## SD firmware update / recovery

The device has no recovery button flow of its own — **any `update.bin` placed
at the SD-card root is flashed at the next boot**. `src/SdUpdate.cpp` runs
right after display init, before anything else:

1. Mount SD. No card or no `/update.bin` → log and continue normal boot.
2. Validate the image end-to-end (ESP header magic, segment table, XOR
   checksum, SHA256 trailer — the same pass CrossPoint's
   `firmware_flash::validateImageFile` runs) so a truncated/corrupt file is
   never booted.
3. Stream it into the **inactive** OTA app slot (raw `esp_partition` writes,
   4 KiB chunks) while a growing black progress bar is drawn on the e-ink
   (1 full + 4 fast refreshes).
4. Rename `/update.bin` → `/update.bin.flashed` **before** switching the boot
   partition, so the freshly booted firmware never reflashes the same file in
   a loop. (If the rename fails it deletes the file; if neither works the boot
   partition is left untouched.)
5. Write otadata raw to select the new slot (same
   `ota_boot::switchTo` scheme as CrossPoint — the esp_ota_* API is bypassed
   because factory-bootloader-patched images fail `esp_image_verify`), then
   restart.

Any failure logs over serial, draws an X pattern on the panel, leaves the
boot partition untouched, and continues into the normal boot — the device
never bricks.

**Getting back to CrossPoint:** copy a CrossPoint release `firmware.bin`
(the OTA image, offset 0x10000 — the same file CrossPoint's own SD updater
accepts) to the SD root as `update.bin` and reboot. xphone-os flashes it into
the other OTA slot and hands over. To flash the same image again later, rename
`update.bin.flashed` back to `update.bin`.

## Envs

Runtime X3/X4 detection is sidestepped: each env compiles exactly one
`-DFREEINK_DEVICE_*`, so the right panel driver and geometry are fixed at
build time.

| env | device | panel | resolution |
|-----|--------|-------|------------|
| `x3` (default) | Xteink X3 | UC8253 | 792x528 |
| `x4` | Xteink X4 | SSD1677 | 800x480 |

## Build / flash

```bash
pio run -e x3              # build X3
pio run -e x3 -t upload    # flash X3
pio run -e x4 -t upload    # flash X4
pio device monitor         # 115200 baud, boot report
```

### Releases

Tag `main` with `fw-vX.Y.Z` to have CI build and publish `flowe-x3.bin` and
`flowe-x4.bin` with checksums. Users can download the firmware from the GitHub
releases page and copy the appropriate binary to an SD card as `update.bin`.

## Measured size (M1)

| env | RAM | Flash |
|-----|-----|-------|
| `x3` | 22.2% — 72,804 / 327,680 bytes | 7.1% — 462,350 / 6,553,600 bytes |
| `x4` | 20.9% — 68,524 / 327,680 bytes | 7.0% — 460,444 / 6,553,600 bytes |

Delta vs M0.1 (x3): **+404 B RAM, +102,394 B flash** — of which 81,853 B is
the two Ubuntu-12 fonts (bitmaps + glyph tables + interval tables); the rest
is InputManager, BatteryMonitor (+ Wire on x3 for the gauge), and the
scene/launcher code. The ~4.3KB RAM gap between envs is the framebuffer
(X3 792x528/8 = 52,272 B vs X4 800x480/8 = 48,000 B, static in `EInkDisplay`).

## M2 — BLE companion + ANCS rails

**Scope.** The launcher now runs a BLE stack after its first paint ("UI
first, radios second"): the same **X4 Companion** GATT service as x4-os
(service `6E400001-…CA9E`, card write `…0002`, action notify `…0003`, same
card/command JSON — the existing iOS companion app connects unchanged;
camera image transfer is the one thing stripped), plus an **ANCS client**
(`src/ble/`, ported from x4-os `src/companion/`) that turns the X4 into an
iPhone notification receiver. Completed notifications land in a bounded
`NotificationStore`; the **Notifications** app renders them newest-first
(Up/Down scroll, OK clears, Back home). The launcher status bar shows a
solid dot when the iPhone is connected and a hollow circle while
advertising; About shows BLE/ANCS state, peer address, stored-notification
count and free heap. All BLE payload parsing happens on the main loop —
BLE-host-task callbacks only copy raw bytes into queues (x4-os
`nimble_host` stack-fault learning).

**Pairing flow.** ANCS only works over an encrypted, **bonded** link:

1. Flash and boot; the launcher paints, then the device advertises as
   `X4 Companion`.
2. Connect from the iOS companion app (or iOS Settings > Bluetooth). When
   the X4's ANCS client starts service discovery on the phone, iOS raises a
   **Bluetooth pairing request** popup on the iPhone — accept it. That bonds
   the devices and unlocks the ANCS service; new iPhone notifications then
   stream in (pre-existing/silent ones are deliberately skipped).
3. **Re-pair after trouble** (e.g. `ANCS waiting for encryption` forever, or
   after reflashing wipes the X4's bond table): iPhone Settings > Bluetooth >
   ⓘ next to the X4 > **Forget This Device**, then also clear the X4 side by
   erasing its NVS bond store (`pio run -t erase` then reflash, or wait for a
   Settings app in a later milestone), and connect again.

**Store bounds.** `NotificationStore` is a fixed ring of **32 entries x
232 B** (uid + millis + `appId[32]` + `title[64]` + `message[128]`), ~7.4 KB
static RAM, overwrite-oldest, no heap, no `std::string`. The ported ANCS
client additionally keeps x4-os's 30-slot working set for attribute
assembly; its Data Source reassembly buffer is capped at 512 B and raw
packets are marshalled through a static 6-deep FreeRTOS queue (~3.1 KB).

### Measured size (M2)

| env | RAM | Flash |
|-----|-----|-------|
| `x3` | 29.1% — 95,276 / 327,680 bytes | 13.1% — 859,637 / 6,553,600 bytes |
| `x4` | 27.8% — 91,004 / 327,680 bytes | 13.1% — 857,683 / 6,553,600 bytes |

Delta vs M1 (x3): **+22,472 B RAM, +397,287 B flash** — almost entirely the
NimBLE controller/host + Arduino BLE wrapper; the notification store (7.4 KB)
and ANCS queue (3.1 KB) are the deliberate static costs.

### Soft-key bar (M2.1 chrome)

A persistent bar at the bottom of every scene shows up to 4 small
rounded-top tabs, aligned left-to-right with the 4 physical bottom-front
buttons (SDK ladder order **Back=0, Confirm=1, Left=2, Right=3**, the same
fixed map as `Input.h`). Each scene declares its labels via the
`Scene::softKeys()` virtual (4 static strings; `nullptr` hides a tab);
`SceneManager::renderIfDirty` draws the bar after every scene render so
per-scene chrome cannot drift, and since labels are static per scene it
repaints exactly when the scene does. Scenes reserve the bottom
`Scene::SOFTKEY_BAR_H` (44 px) for it. Current labels:

| scene | Back | Confirm | Left | Right |
|---|---|---|---|---|
| Launcher | — | OPEN | PREV | NEXT |
| Notifications | BACK | CLEAR | UP | DOWN |
| Placeholder / About | BACK | — | — | — |

The launcher grid is now square icon tiles (tile side = column width,
158 px on X3 / 142 px on X4; monogram placeholder icon, label below),
vertically centered between the status bar and the soft-key bar. In
Notifications the front Left/Right buttons scroll (matching their UP/DOWN
tabs) in addition to the top-edge pair.
