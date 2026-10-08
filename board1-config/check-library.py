#!/usr/bin/env python3
"""Self-check for presets-library.json: 34 panel slots, each exactly one segment
(id 0), and every fx 231 (GIF) look's file present in the keybeat assets dir."""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "../../nowplaying/firmware/board1/keybeat/assets")
lib = json.load(open(os.path.join(HERE, "presets-library.json")))
names = json.load(open(os.path.join(HERE, "library.json")))["panel"]
assert sorted(map(int, lib)) == list(range(1, 35)), sorted(map(int, lib))
assert names == {k: v["n"] for k, v in lib.items()}
for slot, p in lib.items():
  seg = p["seg"]
  assert len(seg) == 1 and seg[0]["id"] == 0, (slot, seg)
  if seg[0]["fx"] == 231:
    for ext in ("", ".kb"):
      assert os.path.isfile(os.path.join(ASSETS, seg[0]["n"] + ext)), (slot, seg[0]["n"] + ext)
print(f"OK: {len(lib)} panel slots")
