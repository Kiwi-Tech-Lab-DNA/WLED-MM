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
#   WiFi is not this script's job: the Pi pushes it over UART (PROVISION, from
#   /etc/nowplaying/wifi.json, set on the touch screen) on every Board 1 boot, so a wired-up
#   erased board rejoins on its own — that's how this script reaches it over HTTP.
#   (nowplaying-project/docs/offline-mode.md.) CLIENT_SSID/CLIENT_PASS baked into the build
#   (gitignored platformio_override.ini) still work for a board on the bench without a Pi.
#
# FULL PROVISIONING SEQUENCE (reproduce the appliance from scratch):
#   1. cp usermods/room_mic_uploader/room_mic_uploader_secrets.h.example \
#         usermods/room_mic_uploader/room_mic_uploader_secrets.h   # fill shared token
#   2. (bench without a Pi only) set CLIENT_SSID/CLIENT_PASS in platformio_override.ini
#   3. pio run -e board1 -t erase  --upload-port /dev/ttyACM0
#   4. pio run -e board1 -t upload --upload-port /dev/ttyACM0
#   5. wire it to the Pi (or use step 2), wait for it to join WiFi, then:  ./board1-config/provision-board1.sh <IP>
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
  # Never GET /json/info on Board 1 (it stalls the panel); /json/state is the liveness probe.
  if curl -s -m 3 "${BASE}/json/state" >/dev/null 2>&1; then break; fi
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
# experiments.I2S_FastPath MUST be 0: with FastPath=1 (a flash-erase default) getSamples()
#   returns pure silence on this mic — the equalizer freezes AND ingest reads RMS 0, even though
#   the mic HARDWARE is fine (direct-I2S smoketest shows full signal). Verified 2026-06-26:
#   flipping it to 0 + reboot restored capture (RMS>gate, /ingest -> matched:true). Do NOT omit.
# def.ps=170: boot into the first BUILT preset (factory default Mountain / Gravcenter, both
#   segments). Slot 170 is user data owned by the Pi (nowplaying-project/docs/builder.md): it
#   does not exist on a freshly provisioned board until bridge.py pushes it over UART, so the
#   panel stays dark for the few seconds between boot and that push.
# Autosave OFF: it rewrites presets.json from live state every 15s and corrupts the slot table.
# Glow strip (2026-09-25): WS281x (type 22) on GPIO 2 = the old SW_LADDER knob-push line,
#   starting right after the matrix at LED 2048. STRIP_LEN = pixels actually on the strip.
#   maxpwr is the auto-brightness limiter; HUB75 is EXCLUDED from it (const.h
#   EXCLUDE_FROM_ABL), so 1100 mA = ~1 A for the strip + WLED's fixed 100 mA ESP allowance.
STRIP_LEN="${STRIP_LEN:-206}"
curl -s -m 8 -X POST "${BASE}/json/cfg" -H 'Content-Type: application/json' -d '{
  "hw":{"led":{
    "total":'"$((2048 + STRIP_LEN))"',
    "maxpwr":1100,"ledma":55,
    "ins":[{"type":102,"start":0,"len":2048,"order":1,"rev":false,"skip":0,"pin":[1]},
           {"type":22,"start":2048,"len":'"$STRIP_LEN"',"order":0,"rev":false,"skip":0,"pin":[2]}],
    "matrix":{"mpc":1,"panels":[{"b":false,"r":false,"v":false,"s":false,"x":0,"y":0,"h":32,"w":64}]}
  }},
  "def":{"ps":170,"on":true,"bri":160},
  "um":{
    "AudioReactive":{
      "enabled":true,
      "digitalmic":{"type":1,"pin":[40,39,38,-1,-1,-1]},
      "experiments":{"I2S_FastPath":0},
      "sync":{"mode":0}
    },
    "Autosave":{"enabled":false}
  }
}'
echo

say "restoring the library presets + palette + GIF assets onto the board filesystem"
# The LIBRARY (panel looks = slots 1-119, strip looks = 120-169) is source-controlled here:
#   presets-library.json — the preset bodies, each sanitized to exactly its own segment. It IS
#                          the board's /presets.json: uploaded verbatim, never merged by hand.
#   library.json         — slot -> {name, audio} (what the Pi reports to the cloud).
#   board-snapshot-*/palette0.json — the Mountain custom palette (slot 3 references pal 74).
# The GIF + .kb pairs the fx 231 (Keybeat) looks reference live in the SIBLING nowplaying repo
# (firmware/board1/keybeat/assets/) — each goes to the FS ROOT ("/x.gif"): the Keybeat loader
# resolves seg names against root. Override KEYBEAT_DIR if your checkout layout differs.
#
# Built presets (170-209) and /npcycle.json are NOT provisioned: they are user data, rebuilt
# from the Pi's /etc/nowplaying/presets.json over UART every time the board boots.
#
# !! DESTRUCTIVE ON A LIVED-IN BOARD !! This REPLACES /presets.json with the repo library;
# hand edits made in the WLED UI are lost. That is the point (reproducible appliance).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEYBEAT_DIR="${KEYBEAT_DIR:-$(cd "${HERE}/../../nowplaying/firmware/board1/keybeat" 2>/dev/null && pwd || true)}"
PALETTE="${PALETTE:-${HERE}/board-snapshot-2026-10-07/palette0.json}"
python3 "${HERE}/check-library.py" || { echo "FAIL: library self-check failed — not uploading"; exit 1; }

if [ -n "${KEYBEAT_DIR}" ] && [ -d "${KEYBEAT_DIR}/assets" ]; then
  n=0
  for f in "${KEYBEAT_DIR}"/assets/*.gif "${KEYBEAT_DIR}"/assets/*.kb; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    curl -s -m 30 -F "data=@${f};filename=/${base}" "${BASE}/edit" >/dev/null \
      && n=$((n+1)) || printf '  FAILED /%s\n' "$base"
  done
  echo "  uploaded ${n} GIF/.kb file(s)"
else
  echo "  WARN: keybeat assets not found (KEYBEAT_DIR='${KEYBEAT_DIR:-<unset>}') — the fx 231 looks will be blank."
fi

if [ -f "${PALETTE}" ]; then
  curl -s -m 15 -F "data=@${PALETTE};filename=/palette0.json" "${BASE}/edit" >/dev/null \
    && echo "  uploaded /palette0.json (Mountain gradient)" || echo "  FAILED /palette0.json"
else
  echo "  WARN: palette not found (PALETTE='${PALETTE}') — Mountain will render with the wrong colors."
fi

LIB="$(mktemp)"
python3 -c "import json,sys; json.dump(json.load(open(sys.argv[1])), open(sys.argv[2],'w'), separators=(',',':'))" \
  "${HERE}/presets-library.json" "${LIB}"
curl -s -m 30 -F "data=@${LIB};filename=/presets.json" "${BASE}/edit" >/dev/null \
  && echo "  uploaded /presets.json (library: panel 1-119, strip 120-169)" || echo "  FAILED /presets.json"
rm -f "${LIB}"

# Seed the mode-knob cycle order. The firmware falls back to a compiled default if this file
# is missing, so this is a nicety (keeps the web preset-manager's "cycle order" panel populated
# from first boot). After provisioning, reorder from the web app — no reflash.
NPCYCLE="$(dirname "$0")/npcycle-default.json"
if [ -f "${NPCYCLE}" ]; then
  curl -s -m 15 -F "data=@${NPCYCLE};filename=/npcycle.json" "${BASE}/edit" >/dev/null \
    && echo "  uploaded /npcycle.json (mode-knob cycle order)" || echo "  FAILED /npcycle.json"
fi

say "default state: on, moderate brightness"
curl -s -m 6 -X POST "${BASE}/json/state" -H 'Content-Type: application/json' \
  -d '{"on":true,"bri":140}' >/dev/null

say "reboot to apply the LED bus + mic driver + boot preset"
curl -s -m 6 -X POST "${BASE}/json/state" -H 'Content-Type: application/json' -d '{"rb":true}' >/dev/null || true

say "verifying after reboot"
# No /json/info here either: the LED total comes from /json/cfg — summed over the buses, because
# hw.led.total is a compatibility field WLED no longer maintains (it read 30 on a working board).
ok=0
for i in $(seq 1 30); do
  out=$(curl -s -m 3 "${BASE}/json/cfg" 2>/dev/null) || { sleep 2; continue; }
  count=$(printf '%s' "$out" | python3 -c 'import sys,json;print(sum(i.get("len",0) for i in json.load(sys.stdin).get("hw",{}).get("led",{}).get("ins",[])))' 2>/dev/null || true)
  if [ "$count" = "$((2048 + STRIP_LEN))" ]; then ok=1; break; fi
  sleep 2
done

if [ "$ok" = 1 ]; then
  echo "PASS: bus lengths sum to $((2048 + STRIP_LEN)) (64x32 matrix + ${STRIP_LEN}-px strip)"
else
  echo "FAIL: matrix not active after reboot (bus lengths != $((2048 + STRIP_LEN))). Check the HUB75 build (WLED_ENABLE_HUB75MATRIX) and bus type."
  exit 1
fi

say "verifying the boot preset (170 = Mountain / Gravcenter once the Pi has pushed it)"
# Slot 170 is pushed by the Pi, not by this script. Until that happens the board boots with no
# preset and the panel is dark — expected on a fresh provision, so that case is a WARN, not FAIL.
# Mountain is fx 230 pal 74 in the library (pal 74 is what the board has always run; the old
# "pal 255 = /palette0.json" note in equalizer/README.md is superseded by the rescued library).
sleep 2
st=$(curl -s -m 6 "${BASE}/json/state" 2>/dev/null) || true
ps=$(printf '%s' "$st" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("ps",""))' 2>/dev/null || true)
seg_fx=$(printf '%s' "$st" | python3 -c 'import sys,json;d=json.load(sys.stdin);print((d.get("seg") or [{}])[0].get("fx",""))' 2>/dev/null || true)
seg_pal=$(printf '%s' "$st" | python3 -c 'import sys,json;d=json.load(sys.stdin);print((d.get("seg") or [{}])[0].get("pal",""))' 2>/dev/null || true)
segs=$(printf '%s' "$st" | python3 -c 'import sys,json;print(len([s for s in json.load(sys.stdin).get("seg",[]) if s.get("stop",0)]))' 2>/dev/null || true)
if [ "$ps" = "170" ] && [ "$seg_fx" = "230" ] && [ "$seg_pal" = "74" ] && [ "$segs" = "2" ]; then
  echo "PASS: boot preset 170 applied (Mountain fx=${seg_fx} pal=${seg_pal}, ${segs} segments = panel + strip)."
else
  echo "WARN: boot preset 170 not (yet) applied (ps='${ps}', fx='${seg_fx}', pal='${seg_pal}', segments=${segs})."
  echo "      Fresh board: wait for the Pi's np-bridge to push its presets.json over UART, then reboot and re-check."
fi
