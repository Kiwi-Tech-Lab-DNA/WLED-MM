#!/usr/bin/env python3
"""Read-only rescue of Board 1's live library into the repo (Builder step 1).

Downloads every look-related file off the board's LittleFS into a byte-exact
snapshot dir (+ MANIFEST.txt), derives presets-library.json (one entry per
panel slot, seg 0 only) and library.json (slot -> name), and copies the GIF +
.kb pairs into the sibling nowplaying repo's keybeat assets dir, which is where
provision-board1.sh already reads them from.  Never writes to the board.
Never touch /json/info: it stalls the panel.

usage: rescue-board1.py [board-ip]        (default 192.168.1.121)
"""
import hashlib, json, os, re, shutil, sys, time, urllib.request

BASE = f"http://{sys.argv[1] if len(sys.argv) > 1 else '192.168.1.121'}"
HERE = os.path.dirname(os.path.abspath(__file__))
SNAP = os.path.join(HERE, f"board-snapshot-{time.strftime('%Y-%m-%d')}")
ASSETS = os.path.join(HERE, "../../nowplaying/firmware/board1/keybeat/assets")
# cfg.json holds WiFi creds -> never in git; shows/album/logs are not library content.
SKIP = re.compile(r"^/(cfg\.json|album\.json|default\.log|version-info\.json|show_.*\.json)$")

def get(path):
  return urllib.request.urlopen(f"{BASE}/edit?download={path}", timeout=30).read()

os.makedirs(SNAP, exist_ok=True)
manifest, bad = [], []
for f in sorted(json.loads(urllib.request.urlopen(f"{BASE}/edit?list=/", timeout=10).read()), key=lambda f: f["name"]):
  name = f["name"]
  if SKIP.match(name): continue
  data = get(name)
  open(os.path.join(SNAP, name.lstrip("/")), "wb").write(data)
  manifest.append(f"{name.lstrip('/')}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}")
  if len(data) != f["size"]: bad.append(f"{name}: listed {f['size']} got {len(data)}")
open(os.path.join(SNAP, "MANIFEST.txt"), "w").write("name\tsize\tsha256\n" + "\n".join(manifest) + "\n")

# WLED leaves whitespace runs where slots were deleted -> not valid JSON until stripped.
raw = open(os.path.join(SNAP, "presets.json"), encoding="utf-8").read()
presets = json.loads(re.sub(r",\s*\}", "}", re.sub(r",\s*,", ",", re.sub(r"\{\s*,", "{", raw))))
lib = {}
for slot, p in sorted(presets.items(), key=lambda kv: int(kv[0])):
  if not 1 <= int(slot) <= 34: continue                       # 40-45 = stage shows, dropped
  p["seg"] = [s for s in p["seg"] if s.get("id") == 0]        # panel mode = seg 0 only
  lib[slot] = p
json.dump(lib, open(os.path.join(HERE, "presets-library.json"), "w"), indent=1)
json.dump({"panel": {k: v["n"] for k, v in lib.items()}, "strip": {}},
          open(os.path.join(HERE, "library.json"), "w"), indent=1)

for fn in sorted(os.listdir(SNAP)):
  if fn.endswith((".gif", ".gif.kb")): shutil.copy2(os.path.join(SNAP, fn), ASSETS)

print(f"snapshot: {SNAP} ({len(manifest)} files); library: {len(lib)} panel slots")
print("size mismatches:", bad or "none")
