# Board 1 provisioning (Now-Playing appliance)

Board 1 is the **room ear**: an ESP32-S3 running this WLED-MM fork with a HUB75 LED
matrix **and** an I2S MEMS mic, plus the `room_mic_uploader` usermod that POSTs room
audio to the cloud (`../usermods/room_mic_uploader/`).

**The goal is a reproducible appliance: once set up, it is always fully functional.**
The catch is that some of what makes Board 1 work lives in WLED's *runtime config*
(`cfg.json` on flash), not in the firmware binary — so a clean build + flash erase does
**not** reproduce a working board on its own. This directory makes that last step a
single command.

## What's compile-time vs runtime

| Setting | Where it lives | Survives a flash erase? |
|---|---|---|
| WiFi creds | `-D CLIENT_SSID/CLIENT_PASS` in `platformio_override.ini` (gitignored) | ✅ baked into the binary |
| Device token | `room_mic_uploader_secrets.h` (gitignored) | ✅ baked into the binary |
| **Knob encoders** (3× Seesaw, b2-13) | `lib_deps` (Adafruit seesaw + BusIO) in `[env:board1]` of `platformio_override.ini` (gitignored) — the `room_mic_uploader` usermod reads them on Wire1 (SDA47/SCL48) | ✅ baked into the binary |
| Audioreactive **local mic** (vs UDP-receive) | build patch keeps `SR_DMTYPE=1` (`pio-scripts/patch_audioreactive.py`) + runtime `sync.mode` | ⚠️ binary is correct; runtime `sync.mode` can be stale |
| **HUB75 64×32 matrix** (LED bus + 2D layout) | WLED runtime `cfg.json` only | ❌ wiped by erase → panel dark |
| **Color order** (RGB, not the GRB default) | WLED runtime `cfg.json` (`hw.led.ins[0].order=1`) | ❌ wiped by erase → R/G swapped (red shows green) |
| **Boot preset** (`def.ps=3` → Mountain spectrum on boot) | WLED runtime `cfg.json` | ❌ wiped by erase → panel blank on boot |
| **Keybeat presets + GIF assets** | LittleFS (`presets.json` + 6 GIFs/`.kb`), sourced from the sibling `nowplaying` repo | ❌ wiped by erase → no looks to load |

So after a from-scratch flash, WiFi comes up by itself (that's how the script can reach
the board), but the **matrix** must be restored and the **mic mode** asserted. That's
all `provision-board1.sh` does.

## Reproduce Board 1 from scratch

```sh
# 0. secrets (once): shared device token + WiFi
cp ../usermods/room_mic_uploader/room_mic_uploader_secrets.h.example \
   ../usermods/room_mic_uploader/room_mic_uploader_secrets.h     # fill NP_DEVICE_TOKEN (== Board 2's)
#    set CLIENT_SSID / CLIENT_PASS in platformio_override.ini (board1 env)
#    the [env:board1] override must also carry the knob deps (gitignored, so not in a clone):
#      lib_deps = ${env:esp32S3_8MB_PSRAM_M_opi.lib_deps}
#        adafruit/Adafruit seesaw Library @ ^1.7.0
#        adafruit/Adafruit BusIO @ ^1.16.0

# 1. build + flash (Board 1 = /dev/ttyACM0)
pio run -e board1 -t erase  --upload-port /dev/ttyACM0     # clears any stale/corrupt cfg
pio run -e board1 -t upload --upload-port /dev/ttyACM0

# 2. wait for the board to join WiFi, then provision its runtime config
./board1-config/provision-board1.sh <board-ip>             # default 192.168.1.117
```

`provision-board1.sh` POSTs the HUB75 bus + 2D matrix, the **RGB color order**, the
local-I2S-mic config (incl. `I2S_FastPath=0` — the silent-mic fix), and the **`def.ps=3`
boot preset**; uploads the **Mountain palette + preset and the Keybeat presets.json + GIF
assets** from the sibling `nowplaying` repo; reboots; and verifies `leds.count == 2048`,
the audio source is `I2S digital`, and the boot preset applied (`ps == 3`, `fx == 230`,
`pal == 255` → the Mountain spectrum on the panel). After it prints `PASS`, the panel
renders the Mountain look in correct colors and the mic ingests — and that config persists
across normal reboots and OTA updates (only a *flash erase* wipes it again).

The Keybeat assets live in the **sibling** `nowplaying` repo
(`../../nowplaying/firmware/board1/keybeat/`), not here — the script auto-locates them
relative to its own path and falls back to `$KEYBEAT_DIR` if your checkout differs.

## Files

- `provision-board1.sh` — one-shot post-flash provisioning (idempotent; safe to re-run).
- `npcycle-default.json` — the mode-knob cycle order (`{"cycle":[slot ids],"lyrics":bool}`),
  seeded to `/npcycle.json` on LittleFS. The `room_mic_uploader` firmware reads it at boot and
  the web preset-manager rewrites it live (`POST /json/state {"npcycle":{…}}`) — reorder looks
  with no reflash. Missing/invalid → the firmware's compiled default stands (real safety net).
- `board1-cfg-reference.json` — a full `GET /json/cfg` snapshot of a known-good board, for
  reference / diffing. Safe to commit: WLED redacts passwords from `/json/cfg` (only
  `pskl` *lengths* appear), and the device token is not stored in WLED config.

## Panel facts (single 64×32 HUB75)

- LED bus: **type 102** (HUB75), `len 2048`, `pin:[1]` — for HUB75 the bus `pin` is the
  panel **chain length**, not a GPIO; the real R/G/B/addr/clk pins come from
  `-D SECTION9_2_PINOUT` (see `wled00/bus_manager.cpp`).
- **Color order: RGB (`order:1`), not WLED's GRB default (`0`).** This panel's wiring
  swaps R↔G under GRB — logical red shows green, green shows red (blue is unaffected).
  Verified on hardware by driving solid R/G/B fills. If a future panel shows wrong colors
  again, re-test with solid fills and pick the order that makes all three primaries correct.
- 2D matrix: 1 panel, 64×32, no rotation/serpentine (`b/r/v/s = false`). If your physical
  panel is mirrored or scrambled, adjust those flags here or in the WLED UI (2D Config).
- `leds.pwr` always reads `0 mA` for HUB75 (excluded from WLED's power/auto-brightness
  estimator) — not a fault.
- Mic: generic I2S (`digitalmic.type 1`), pins `[SD=40, WS=39, SCK=38]`. HUB75 (LCD_CAM)
  and the mic (I2S0) coexist on the S3; the build patch disables audioreactive's default
  "HUB75 → UDP-receive-only" workaround for this board.
