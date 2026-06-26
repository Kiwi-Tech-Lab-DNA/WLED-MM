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
| Audioreactive **local mic** (vs UDP-receive) | build patch keeps `SR_DMTYPE=1` (`pio-scripts/patch_audioreactive.py`) + runtime `sync.mode` | ⚠️ binary is correct; runtime `sync.mode` can be stale |
| **HUB75 64×32 matrix** (LED bus + 2D layout) | WLED runtime `cfg.json` only | ❌ wiped by erase → panel dark |

So after a from-scratch flash, WiFi comes up by itself (that's how the script can reach
the board), but the **matrix** must be restored and the **mic mode** asserted. That's
all `provision-board1.sh` does.

## Reproduce Board 1 from scratch

```sh
# 0. secrets (once): shared device token + WiFi
cp ../usermods/room_mic_uploader/room_mic_uploader_secrets.h.example \
   ../usermods/room_mic_uploader/room_mic_uploader_secrets.h     # fill NP_DEVICE_TOKEN (== Board 2's)
#    set CLIENT_SSID / CLIENT_PASS in platformio_override.ini (board1 env)

# 1. build + flash (Board 1 = /dev/ttyACM0)
pio run -e board1 -t erase  --upload-port /dev/ttyACM0     # clears any stale/corrupt cfg
pio run -e board1 -t upload --upload-port /dev/ttyACM0

# 2. wait for the board to join WiFi, then provision its runtime config
./board1-config/provision-board1.sh <board-ip>             # default 192.168.1.117
```

`provision-board1.sh` POSTs the HUB75 bus + 2D matrix and the local-I2S-mic config,
reboots, and verifies `leds.count == 2048` and the audio source is `I2S digital`. After
it prints `PASS`, the panel renders and the mic ingests — and that config persists across
normal reboots and OTA updates (only a *flash erase* wipes it again).

## Files

- `provision-board1.sh` — one-shot post-flash provisioning (idempotent; safe to re-run).
- `board1-cfg-reference.json` — a full `GET /json/cfg` snapshot of a known-good board, for
  reference / diffing. Safe to commit: WLED redacts passwords from `/json/cfg` (only
  `pskl` *lengths* appear), and the device token is not stored in WLED config.

## Panel facts (single 64×32 HUB75)

- LED bus: **type 102** (HUB75), `len 2048`, `pin:[1]` — for HUB75 the bus `pin` is the
  panel **chain length**, not a GPIO; the real R/G/B/addr/clk pins come from
  `-D SECTION9_2_PINOUT` (see `wled00/bus_manager.cpp`).
- 2D matrix: 1 panel, 64×32, no rotation/serpentine (`b/r/v/s = false`). If your physical
  panel is mirrored or scrambled, adjust those flags here or in the WLED UI (2D Config).
- `leds.pwr` always reads `0 mA` for HUB75 (excluded from WLED's power/auto-brightness
  estimator) — not a fault.
- Mic: generic I2S (`digitalmic.type 1`), pins `[SD=40, WS=39, SCK=38]`. HUB75 (LCD_CAM)
  and the mic (I2S0) coexist on the S3; the build patch disables audioreactive's default
  "HUB75 → UDP-receive-only" workaround for this board.
