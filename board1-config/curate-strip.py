#!/usr/bin/env python3
"""Builder step 2: curate the back-strip library, one effect at a time, on the live board.

Walks the board's 1D audio-reactive effects (--all: every 1D-capable effect), applies each
to the strip segment (id 1) and asks keep / skip. A keep snapshots the live segment as a
seg-1-only library preset in presets-library.json (slots 80..99) and names it in
library.json["strip"]. Resumable: effects already in the strip library are skipped.
Restores the strip's original look on exit. Never touches seg 0.

    python3 curate-strip.py [--all] [--host 192.168.1.121]
    keys:  y keep   n skip   p next palette   q quit
"""
import argparse, json, re, sys, urllib.request
from pathlib import Path

HERE = Path(__file__).parent
LIB, PRESETS = HERE / "library.json", HERE / "presets-library.json"
STRIP_SLOTS = range(80, 100)
SEG = 1
KEEP = ("start", "stop", "startY", "stopY", "fx", "pal", "sx", "ix", "c1", "c2", "c3",
        "o1", "o2", "o3", "col", "n", "on", "bri", "mi", "rev")


def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
        return json.loads(r.read())


def post(host, state):
    req = urllib.request.Request(f"http://{host}/json/state", json.dumps(state).encode(),
                                 {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()


def candidates(host, all_1d):
    names, fxdata = get(host, "/json/eff"), get(host, "/json/fxdata")
    out = []
    for i, (name, meta) in enumerate(zip(names, fxdata)):
        flags = meta.split(";")[3] if meta.count(";") >= 3 else ""
        two_d_only = "2" in flags and not re.search(r"[01]", flags)
        audio = "v" in flags or "f" in flags
        if two_d_only or (not audio and not all_1d) or name.startswith("RSVD"):
            continue
        out.append((i, name.replace(" ☾", "").strip()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="every 1D-capable effect, not just audio")
    ap.add_argument("--host", default="192.168.1.121")
    a = ap.parse_args()

    lib = json.loads(LIB.read_text())
    presets = json.loads(PRESETS.read_text())
    lib.setdefault("strip", {})
    done_fx = {presets[s]["seg"][0]["fx"] for s in lib["strip"]}
    free = [s for s in STRIP_SLOTS if str(s) not in presets]

    original = get(a.host, "/json/state")["seg"][SEG]
    npal = len(get(a.host, "/json/pal"))
    todo = [(i, n) for i, n in candidates(a.host, a.all) if i not in done_fx]
    print(f"{len(todo)} effects to review, {len(free)} strip slots free, {len(lib['strip'])} kept so far")

    try:
        for fx, name in todo:
            if not free:
                print("no strip slots left (80..99)"); break
            pal = 74  # the known-good Mountain gradient; p cycles from here
            post(a.host, {"seg": [{"id": SEG, "fx": fx, "pal": pal}]})
            while True:
                k = input(f"fx {fx:3d} {name:<22} pal {pal:3d}  [y/n/p/q] ").strip().lower()
                if k == "p":
                    pal = (pal + 1) % npal
                    post(a.host, {"seg": [{"id": SEG, "pal": pal}]})
                elif k in ("y", "n", "q"):
                    break
            if k == "q":
                break
            if k == "y":
                live = get(a.host, "/json/state")["seg"][SEG]
                seg = {"id": SEG, **{key: live[key] for key in KEEP if key in live}}
                seg["n"] = "Back Strip"
                slot = free.pop(0)
                presets[str(slot)] = {"n": name, "seg": [seg]}
                lib["strip"][str(slot)] = name
                PRESETS.write_text(json.dumps(presets, separators=(",", ":")) + "\n")
                LIB.write_text(json.dumps(lib, indent=1) + "\n")
                print(f"   kept as slot {slot}")
    finally:
        restore = {"id": SEG, **{key: original[key] for key in KEEP if key in original}}
        post(a.host, {"seg": [restore]})
        print(f"strip restored; {len(lib['strip'])} strip modes in library.json")


if __name__ == "__main__":
    main()
