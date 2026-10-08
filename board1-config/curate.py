#!/usr/bin/env python3
"""Builder step 2: curate the mode library, one WLED effect at a time, on the live board.

Walks EVERY effect the board offers (RSVD excluded; 2D-only effects excluded for the
strip), applies each to the chosen segment and asks keep / skip. The bucket — audio-
reactive or not — comes from WLED's fxdata flags (v/f), with Keybeat (231) forced to
audio. A keep snapshots the live segment as a one-segment library preset in
presets-library.json and names it in library.json[seg] = {slot: {"n", "audio"}}.
Skips are remembered in curation-skipped.json so the pass is resumable. The segment's
original look is restored on exit. Never touches the other segment.

    python3 curate.py panel|strip [--audio | --ambient] [--list] [--host 192.168.1.121]
    keys:  y keep   n skip   p next palette   q quit
"""
import argparse, json, re, urllib.request
from pathlib import Path

HERE = Path(__file__).parent
LIB, PRESETS, SKIPPED = HERE / "library.json", HERE / "presets-library.json", HERE / "curation-skipped.json"
SEG = {"panel": 0, "strip": 1}
SLOTS = {"panel": range(1, 120), "strip": range(120, 170)}   # built presets take 170..209
SEG_NAME = {"panel": "Front Panel", "strip": "Back Strip"}
AUDIO_OVERRIDE = {231}                                        # Keybeat: beat-synced, flagged plain "2"
KEEP = ("start", "stop", "startY", "stopY", "fx", "pal", "sx", "ix", "c1", "c2", "c3",
        "o1", "o2", "o3", "col", "n", "on", "bri", "mi", "rev")


def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
        return json.loads(r.read())


def post(host, state):
    req = urllib.request.Request(f"http://{host}/json/state", json.dumps(state).encode(),
                                 {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()


def flags_of(meta):
    return meta.split(";")[3] if meta.count(";") >= 3 else ""


def is_audio(fx, meta):
    return fx in AUDIO_OVERRIDE or bool(re.search(r"[vf]", flags_of(meta)))


def candidates(names, fxdata, seg):
    out = []
    for i, (name, meta) in enumerate(zip(names, fxdata)):
        flags = flags_of(meta)
        two_d_only = "2" in flags and not re.search(r"[01]", flags)
        if name.startswith("RSVD") or (seg == "strip" and two_d_only):
            continue
        out.append((i, name.replace(" ☾", "").strip(), is_audio(i, meta)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seg", choices=SEG)
    ap.add_argument("--audio", action="store_true", help="only the audio-reactive bucket")
    ap.add_argument("--ambient", action="store_true", help="only the non-audio bucket")
    ap.add_argument("--list", action="store_true", help="print the to-do list, touch nothing")
    ap.add_argument("--host", default="192.168.1.121")
    a = ap.parse_args()
    seg_id, seg_name = SEG[a.seg], SEG_NAME[a.seg]

    lib = json.loads(LIB.read_text()); lib.setdefault(a.seg, {})
    presets = json.loads(PRESETS.read_text())
    skipped = json.loads(SKIPPED.read_text()) if SKIPPED.exists() else {}
    skipped.setdefault(a.seg, [])
    done_fx = {presets[s]["seg"][0]["fx"] for s in lib[a.seg]} | set(skipped[a.seg])
    free = [s for s in SLOTS[a.seg] if str(s) not in presets]

    names, fxdata = get(a.host, "/json/eff"), get(a.host, "/json/fxdata")
    todo = [c for c in candidates(names, fxdata, a.seg)
            if c[0] not in done_fx and (not a.audio or c[2]) and (not a.ambient or not c[2])]
    print(f"{a.seg}: {len(todo)} effects to review, {len(free)} slots free, "
          f"{len(lib[a.seg])} kept, {len(skipped[a.seg])} skipped so far")
    if a.list:
        for fx, name, audio in todo:
            print(f"  {fx:3d} {name:<24} {'audio' if audio else 'ambient'}")
        return

    original = get(a.host, "/json/state")["seg"][seg_id]
    npal = len(get(a.host, "/json/pal"))
    try:
        for fx, name, audio in todo:
            if not free:
                print(f"no {a.seg} slots left"); break
            m = re.search(r"pal=(\d+)", fxdata[fx])
            pal = int(m.group(1)) if m else 74                # effect's own default, else Mountain gradient
            post(a.host, {"seg": [{"id": seg_id, "fx": fx, "pal": pal, "n": seg_name}]})
            while True:
                k = input(f"{'♪' if audio else ' '} fx {fx:3d} {name:<24} pal {pal:3d}  [y/n/p/q] ").strip().lower()
                if k == "p":
                    pal = (pal + 1) % npal
                    post(a.host, {"seg": [{"id": seg_id, "pal": pal}]})
                elif k in ("y", "n", "q"):
                    break
            if k == "q":
                break
            if k == "n":
                skipped[a.seg].append(fx)
                SKIPPED.write_text(json.dumps(skipped) + "\n")
                continue
            live = get(a.host, "/json/state")["seg"][seg_id]
            seg = {"id": seg_id, **{key: live[key] for key in KEEP if key in live}}
            seg["n"] = seg_name
            slot = free.pop(0)
            presets[str(slot)] = {"n": name, "seg": [seg]}
            lib[a.seg][str(slot)] = {"n": name, "audio": audio}
            PRESETS.write_text(json.dumps(presets, separators=(",", ":")) + "\n")
            LIB.write_text(json.dumps(lib, indent=1) + "\n")
            print(f"   kept as slot {slot} ({'audio' if audio else 'ambient'})")
    finally:
        restore = {"id": seg_id, **{key: original[key] for key in KEEP if key in original}}
        post(a.host, {"seg": [restore]})
        print(f"{a.seg} restored; {len(lib[a.seg])} {a.seg} modes in library.json")


if __name__ == "__main__":
    main()
