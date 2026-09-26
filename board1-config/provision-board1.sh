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
# experiments.I2S_FastPath MUST be 0: with FastPath=1 (a flash-erase default) getSamples()
#   returns pure silence on this mic — the equalizer freezes AND ingest reads RMS 0, even though
#   the mic HARDWARE is fine (direct-I2S smoketest shows full signal). Verified 2026-06-26:
#   flipping it to 0 + reboot restored capture (RMS>gate, /ingest -> matched:true). Do NOT omit.
# def.ps=3: boot straight into the "Mountain" Equalizer look (fx 230 + custom palette) so the
#   panel is never blank on boot. (Was 10/Blade; mountain is the hero look.)
# Autosave OFF: it rewrites presets.json from live state every 15s and corrupts the slot table.
curl -s -m 8 -X POST "${BASE}/json/cfg" -H 'Content-Type: application/json' -d '{
  "hw":{"led":{
    "total":2048,
    "ins":[{"type":102,"start":0,"len":2048,"order":1,"rev":false,"skip":0,"pin":[1]}],
    "matrix":{"mpc":1,"panels":[{"b":false,"r":false,"v":false,"s":false,"x":0,"y":0,"h":32,"w":64}]}
  }},
  "def":{"ps":3,"on":true,"bri":160},
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

say "restoring presets + palette + GIF assets onto the board filesystem"
# The looks live in the SIBLING nowplaying repo (not WLED-MM). A flash erase wipes LittleFS,
# so re-upload here:
#   - Keybeat (firmware/board1/keybeat/): six GIFs + .kb beat sidecars + presets-keybeat.json
#     (slots 10-15). Each .gif/.kb goes to the FS ROOT ("/x.gif") — the Keybeat loader resolves
#     seg names against root. The preset table carries 64x32 seg bounds; without them {"ps":N}
#     loads a 1px seg.
#   - Mountain (firmware/board1/equalizer/): the boot look = fx 230 Equalizer LF + a CUSTOM
#     palette. palette-mountain.json -> /palette0.json (this makes it WLED custom palette
#     **pal 255**: the render path resolves customPalettes[255-pal], so palette0.json == 255,
#     NOT the palcount+index number /json/info shows — see equalizer/README.md). preset-mountain
#     .json carries slot 3, which references pal 255. Custom palettes only (re)load on BOOT, so
#     the reboot below is required for the palette to exist before slot 3 renders.
# We MERGE the keybeat + mountain preset tables into the single /presets.json the board loads.
# Override KEYBEAT_DIR / EQ_DIR if your checkout layout differs.
KEYBEAT_DIR="${KEYBEAT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../nowplaying/firmware/board1/keybeat" 2>/dev/null && pwd || true)}"
EQ_DIR="${EQ_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../nowplaying/firmware/board1/equalizer" 2>/dev/null && pwd || true)}"
SHOW_DIR="${SHOW_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../nowplaying/firmware/board1/stageshow" 2>/dev/null && pwd || true)}"
if [ -n "${KEYBEAT_DIR}" ] && [ -d "${KEYBEAT_DIR}/assets" ]; then
  for f in "${KEYBEAT_DIR}"/assets/*.gif "${KEYBEAT_DIR}"/assets/*.kb; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    curl -s -m 30 -F "data=@${f};filename=/${base}" "${BASE}/edit" >/dev/null \
      && printf '  uploaded /%s\n' "$base" || printf '  FAILED /%s\n' "$base"
  done

  # Mountain custom palette -> /palette0.json (== pal 255 at render time)
  if [ -n "${EQ_DIR}" ] && [ -f "${EQ_DIR}/palette-mountain.json" ]; then
    curl -s -m 15 -F "data=@${EQ_DIR}/palette-mountain.json;filename=/palette0.json" "${BASE}/edit" >/dev/null \
      && echo "  uploaded /palette0.json (Mountain gradient -> pal 255)" || echo "  FAILED /palette0.json"
  else
    echo "  WARN: equalizer dir/palette not found (EQ_DIR='${EQ_DIR:-<unset>}') — boot look will be wrong."
  fi

  # Merge mountain slot 3 + keybeat slots 10-15 + stage-show slots 40-45 into one
  # /presets.json. The show slots MUST be in this merge: the board loads exactly this
  # file, so a provision run that omitted them would silently delete every show look
  # and leave the cue engine applying presets that no longer exist.
  #
  # !! DESTRUCTIVE ON A LIVED-IN BOARD !! This REPLACES /presets.json with only what the
  # repo knows about. The bench board carried 34 presets (hand-added GIF looks in slots
  # 1-34) that exist nowhere in git — this script would delete all of them. That is fine
  # for its stated job (rebuild a freshly-erased board) but NOT for topping up a working
  # one. Pull /presets.json off the board and merge into it instead; a snapshot lives at
  # nowplaying/firmware/board1/stageshow/presets-board-backup-*.json.
  MERGED="$(mktemp)"
  if [ -n "${EQ_DIR}" ] && [ -f "${EQ_DIR}/preset-mountain.json" ] && \
     python3 -c "import json,sys,os; kb=json.load(open('${KEYBEAT_DIR}/presets-keybeat.json')); mt=json.load(open('${EQ_DIR}/preset-mountain.json')); d={}; d.update(mt); d.update(kb); p='${SHOW_DIR}/presets-stageshow.json'; d.update(json.load(open(p))) if os.path.isfile(p) else None; json.dump(d,open('${MERGED}','w'),separators=(',',':'))" 2>/dev/null; then
    curl -s -m 15 -F "data=@${MERGED};filename=/presets.json" "${BASE}/edit" >/dev/null \
      && echo "  uploaded /presets.json (Mountain 3 + Keybeat 10-15 + stage show 40-45)" || echo "  FAILED /presets.json"
  else
    # fall back to keybeat-only so the panel at least has the keybeat looks
    curl -s -m 15 -F "data=@${KEYBEAT_DIR}/presets-keybeat.json;filename=/presets.json" "${BASE}/edit" >/dev/null \
      && echo "  uploaded /presets.json (Keybeat slots 10-15 ONLY — mountain merge failed)" || echo "  FAILED /presets.json"
  fi
  rm -f "${MERGED}"
else
  echo "  SKIP: keybeat dir not found (looked in '${KEYBEAT_DIR:-<unset>}')."
  echo "        Set KEYBEAT_DIR=/path/to/nowplaying/firmware/board1/keybeat and re-run,"
  echo "        else the panel boots blank (no preset 3 to load)."
fi

# Per-song stage shows -> /show_<apple_id>.json at the FS ROOT. NOT a /shows/ subdirectory:
# WLED's /edit upload handler cannot create directories and answers 500, which this script
# used to swallow because it only checked curl's exit code and not the HTTP status. The
# board then logged "no show for track ..." forever. Keep the status check.
if [ -n "${SHOW_DIR}" ] && [ -d "${SHOW_DIR}/shows" ]; then
  n=0; bad=0
  for f in "${SHOW_DIR}"/shows/*.json; do
    [ -e "$f" ] || continue
    code=$(curl -s -m 15 -o /dev/null -w '%{http_code}' \
      -F "data=@${f};filename=/show_$(basename "$f")" "${BASE}/edit")
    if [ "$code" = "200" ]; then n=$((n+1)); else bad=$((bad+1)); echo "  FAILED $(basename "$f") -> HTTP $code"; fi
  done
  echo "  uploaded ${n} stage show(s) as /show_<apple_id>.json${bad:+ (${bad} FAILED)}"
fi

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

say "verifying boot preset applied (panel should be showing the Mountain spectrum, not blank)"
sleep 2
st=$(curl -s -m 6 "${BASE}/json/state" 2>/dev/null) || true
ps=$(printf '%s' "$st" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("ps",""))' 2>/dev/null || true)
seg_fx=$(printf '%s' "$st" | python3 -c 'import sys,json;d=json.load(sys.stdin);print((d.get("seg") or [{}])[0].get("fx",""))' 2>/dev/null || true)
seg_pal=$(printf '%s' "$st" | python3 -c 'import sys,json;d=json.load(sys.stdin);print((d.get("seg") or [{}])[0].get("pal",""))' 2>/dev/null || true)
cpal=$(curl -s -m 6 "${BASE}/json/info" 2>/dev/null | python3 -c 'import sys,json;print(json.load(sys.stdin).get("cpalcount",0))' 2>/dev/null || echo 0)
if [ "$ps" = "3" ] && [ "$seg_fx" = "230" ] && [ "$seg_pal" = "255" ]; then
  echo "PASS: boot preset 3 'Mountain' applied (fx=${seg_fx}, pal=${seg_pal}, custom palettes loaded=${cpal})."
else
  echo "WARN: mountain boot look not fully applied (ps='${ps}', fx='${seg_fx}', pal='${seg_pal}', cpalcount=${cpal})."
  echo "      - pal must be 255 (palette0.json) and cpalcount>=1, else /palette0.json didn't load."
  echo "      - If the board reboot-loops, the seg pal is wrong (NOT 255) — see equalizer/README.md."
  echo "      Confirm the 'restoring presets + palette' step uploaded all files, then re-run."
fi
