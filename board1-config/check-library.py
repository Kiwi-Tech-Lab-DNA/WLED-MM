#!/usr/bin/env python3
"""Self-check for presets-library.json against library.json: every panel slot (1..34) is
exactly one seg-0 look, every strip slot (80..99) exactly one seg-1 look, names agree,
and every fx 231 (GIF) look's file is present in the keybeat assets dir."""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "../../nowplaying/firmware/board1/keybeat/assets")
lib = json.load(open(os.path.join(HERE, "presets-library.json")))
names = json.load(open(os.path.join(HERE, "library.json")))
panel, strip = names["panel"], names.get("strip", {})
assert sorted(map(int, panel)) == list(range(1, 35)), sorted(map(int, panel))
assert all(80 <= int(s) < 100 for s in strip), strip
assert set(lib) == set(panel) | set(strip), set(lib) ^ (set(panel) | set(strip))
for kind, want_id in ((panel, 0), (strip, 1)):
  for slot, name in kind.items():
    p = lib[slot]; seg = p["seg"]
    assert p["n"] == name, (slot, p["n"], name)
    assert len(seg) == 1 and seg[0]["id"] == want_id, (slot, seg)
    if seg[0]["fx"] == 231:
      for ext in ("", ".kb"):
        assert os.path.isfile(os.path.join(ASSETS, seg[0]["n"] + ext)), (slot, seg[0]["n"] + ext)
print(f"OK: {len(panel)} panel slots, {len(strip)} strip slots")
