#!/usr/bin/env python3
"""Self-check for presets-library.json against library.json: every panel slot (1..119) is
exactly one seg-0 look, every strip slot (120..169) exactly one seg-1 look, names agree,
each entry carries its audio bucket flag, and every fx 231 (GIF) look's file is present in
the keybeat assets dir."""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "../../nowplaying/firmware/board1/keybeat/assets")
lib = json.load(open(os.path.join(HERE, "presets-library.json")))
names = json.load(open(os.path.join(HERE, "library.json")))
panel, strip = names["panel"], names.get("strip", {})
assert set(range(1, 35)) <= set(map(int, panel)) and all(1 <= int(s) < 120 for s in panel), sorted(map(int, panel))
assert all(120 <= int(s) < 170 for s in strip), strip
assert set(lib) == set(panel) | set(strip), set(lib) ^ (set(panel) | set(strip))
for kind, want_id in ((panel, 0), (strip, 1)):
  for slot, entry in kind.items():
    p = lib[slot]; seg = p["seg"]
    assert isinstance(entry.get("audio"), bool), (slot, entry)
    assert p["n"] == entry["n"], (slot, p["n"], entry["n"])
    assert len(seg) == 1 and seg[0]["id"] == want_id, (slot, seg)
    if seg[0]["fx"] == 231:
      for ext in ("", ".kb"):
        assert os.path.isfile(os.path.join(ASSETS, seg[0]["n"] + ext)), (slot, seg[0]["n"] + ext)
buckets = {f"{k} {'audio' if a else 'ambient'}": sum(e['audio'] == a for e in v.values())
           for k, v in (("panel", panel), ("strip", strip)) for a in (True, False)}
print("OK:", ", ".join(f"{n} {c}" for n, c in buckets.items()))
