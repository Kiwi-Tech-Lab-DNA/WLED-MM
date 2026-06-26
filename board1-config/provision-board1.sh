#!/usr/bin/env bash
# provision-board1.sh — make a freshly-flashed Board 1 fully functional.
#
# WHY THIS EXISTS
#   Board 1's appliance config lives in WLED's flash config (cfg.json), NOT in the
#   firmware binary. A clean build/erase therefore comes up WRONG in two ways that a
#   fresh checkout can't know about:
#     1. LED output defaults to a generic 30-LED strip on GPIO2 -> the HUB75 panel is
#        dark. The 64x32 matrix (bus + 2D layout) is runtime config, not a build flag.
#     2. AudioReactive may sit in UDP-receive mode (no local mic). The room_mic_uploader
#        build patch already keeps SR_DMTYPE=1, but we set the mic config explicitly so a
#        board that was ever booted in receive mode (stale sync.mode) is corrected too.
#   WiFi is the exception — it IS baked in (CLIENT_SSID/CLIENT_PASS in the gitignored
#   platformio_override.ini), so a fresh erase reconnects on its own; that's how this
#   script reaches the board over HTTP.
#
# FULL PROVISIONING SEQUENCE (reproduce the appliance from scratch):
#   1. cp usermods/room_mic_uploader/room_mic_uploader_secrets.h.example \
#         usermods/room_mic_uploader/room_mic_uploader_secrets.h   # fill shared token
#   2. set CLIENT_SSID/CLIENT_PASS in platformio_override.ini (board1 env)
#   3. pio run -e board1 -t erase  --upload-port /dev/ttyACM0
#   4. pio run -e board1 -t upload --upload-port /dev/ttyACM0
#   5. wait for the board to join WiFi, then:  ./board1-config/provision-board1.sh <IP>
#   After this the panel renders and the mic ingests — fully functional, persists across
#   reboots/OTA (only a flash erase wipes it, which is why this script exists).
#
# Usage: ./provision-board1.sh [board-ip]   (default 192.168.1.117)

set -euo pipefail
IP="${1:-192.168.1.117}"
BASE="http://${IP}"

say() { printf '\n=== %s ===\n' "$1"; }

say "waiting for ${IP} to respond"
for i in $(seq 1 30); do
  if curl -s -m 3 "${BASE}/json/info" >/dev/null 2>&1; then break; fi
  sleep 2
  [ "$i" = 30 ] && { echo "FAIL: board not reachable at ${IP}"; exit 1; }
done

say "applying HUB75 64x32 matrix + local-I2S-mic config"
# HUB75: bus type 102, len 2048 (=64x32). For HUB75 the bus 'pin' field is repurposed as
# the panel CHAIN LENGTH (1), not a GPIO — the real pins come from SECTION9_2_PINOUT.
# COLOR ORDER must be 1 (RGB). The panel wiring swaps R<->G under WLED's GRB default (0):
#   logical red showed green, logical green showed red (blue was correct). order=1 fixes it.
#   Verified on hardware by driving solid R/G/B fills. Do NOT revert to 0.
# AudioReactive: local generic-I2S mic (type 1) on SD=40/WS=39/SCK=38, sync.mode 0 (not UDP).
# def.ps=10: boot straight into Keybeat preset 10 (Blade) so the panel is never blank on boot.
# Autosave OFF: it rewrites presets.json from live state every 15s and corrupts the slot table.
curl -s -m 8 -X POST "${BASE}/json/cfg" -H 'Content-Type: application/json' -d '{
  "hw":{"led":{
    "total":2048,
    "ins":[{"type":102,"start":0,"len":2048,"order":1,"rev":false,"skip":0,"pin":[1]}],
    "matrix":{"mpc":1,"panels":[{"b":false,"r":false,"v":false,"s":false,"x":0,"y":0,"h":32,"w":64}]}
  }},
  "def":{"ps":10,"on":true,"bri":200},
  "um":{
    "AudioReactive":{
      "enabled":true,
      "digitalmic":{"type":1,"pin":[40,39,38,-1,-1,-1]},
      "sync":{"mode":0}
    },
    "Autosave":{"enabled":false}
  }
}'
echo

say "restoring Keybeat presets + GIF assets onto the board filesystem"
# The Keybeat looks live in the SIBLING nowplaying repo (not WLED-MM): the preset table
# (presets.json) plus six GIFs + their .kb beat sidecars. A flash erase wipes all of these
# from LittleFS, so re-upload them here. Each .gif/.kb goes to the FS ROOT (filename "/x.gif")
# because the Keybeat loader resolves seg names against the root. presets-keybeat.json carries
# the 64x32 segment bounds (start/stop/startY/stopY) — without them {"ps":N} loads a 1px seg.
# Override KEYBEAT_DIR if your checkout layout differs.
KEYBEAT_DIR="${KEYBEAT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../nowplaying/firmware/board1/keybeat" 2>/dev/null && pwd || true)}"
if [ -n "${KEYBEAT_DIR}" ] && [ -d "${KEYBEAT_DIR}/assets" ]; then
  for f in "${KEYBEAT_DIR}"/assets/*.gif "${KEYBEAT_DIR}"/assets/*.kb; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    curl -s -m 30 -F "data=@${f};filename=/${base}" "${BASE}/edit" >/dev/null \
      && printf '  uploaded /%s\n' "$base" || printf '  FAILED /%s\n' "$base"
  done
  curl -s -m 15 -F "data=@${KEYBEAT_DIR}/presets-keybeat.json;filename=/presets.json" "${BASE}/edit" >/dev/null \
    && echo "  uploaded /presets.json (Keybeat slots 10-15)" || echo "  FAILED /presets.json"
else
  echo "  SKIP: keybeat dir not found (looked in '${KEYBEAT_DIR:-<unset>}')."
  echo "        Set KEYBEAT_DIR=/path/to/nowplaying/firmware/board1/keybeat and re-run,"
  echo "        else the panel boots blank (no preset 10 to load)."
fi

say "default state: on, moderate brightness"
curl -s -m 6 -X POST "${BASE}/json/state" -H 'Content-Type: application/json' \
  -d '{"on":true,"bri":140}' >/dev/null

say "reboot to apply the LED bus + mic driver + boot preset"
curl -s -m 6 -X POST "${BASE}/json/state" -H 'Content-Type: application/json' -d '{"rb":true}' >/dev/null || true

say "verifying after reboot"
ok=0
for i in $(seq 1 30); do
  out=$(curl -s -m 3 "${BASE}/json/info" 2>/dev/null) || { sleep 2; continue; }
  count=$(printf '%s' "$out" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("leds",{}).get("count",""))' 2>/dev/null || true)
  src=$(printf '%s'  "$out" | python3 -c 'import sys,json;print((json.load(sys.stdin).get("u",{}).get("Audio Source",["?"])[0]))' 2>/dev/null || true)
  if [ "$count" = "2048" ]; then ok=1; break; fi
  sleep 2
done

if [ "$ok" = 1 ]; then
  echo "PASS: leds.count=2048 (64x32 matrix active) | Audio Source: ${src}"
  echo "If 'Audio Source' is not 'I2S digital', the room_mic_uploader build patch is missing."
else
  echo "FAIL: matrix not active after reboot (leds.count != 2048). Check the HUB75 build (WLED_ENABLE_HUB75MATRIX) and bus type."
  exit 1
fi

say "verifying boot preset applied (panel should be showing Blade, not blank)"
sleep 2
st=$(curl -s -m 6 "${BASE}/json/state" 2>/dev/null) || true
ps=$(printf '%s' "$st" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("ps",""))' 2>/dev/null || true)
seg_n=$(printf '%s' "$st" | python3 -c 'import sys,json;d=json.load(sys.stdin);print((d.get("seg") or [{}])[0].get("n",""))' 2>/dev/null || true)
if [ "$ps" = "10" ]; then
  echo "PASS: boot preset 10 applied (seg='${seg_n}', expect blade.gif)."
else
  echo "WARN: boot preset did not auto-apply (ps='${ps}', seg='${seg_n}')."
  echo "      The matrix/mic are fine, but presets.json or the GIF assets may be missing."
  echo "      Confirm the 'restoring Keybeat presets' step above uploaded all files, then re-run."
fi
