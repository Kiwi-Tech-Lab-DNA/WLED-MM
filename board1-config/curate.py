#!/usr/bin/env python3
"""Builder step 2: curate the mode library, one WLED effect at a time, on the live board.

First walks the library presets already in this segment's bucket (keep = unchanged,
skip = deleted from the library), then EVERY effect the board offers (RSVD excluded;
panel = 2D-capable only, strip = not 2D-only), applies each and asks keep / skip. The bucket — audio-
reactive or not — comes from WLED's fxdata flags (v/f), with Keybeat (231) forced to
audio. A keep snapshots the live segment as a one-segment library preset in
presets-library.json and names it in library.json[seg] = {slot: {"n", "audio"}}.
Decisions are remembered in curation-state.json so the pass is resumable. The segment's
original look is restored on exit. Never touches the other segment.

    python3 curate.py panel|strip [--audio | --ambient] [--list] [--host 192.168.1.121]
    keys:  y keep   n skip   p next palette   q quit
"""
import argparse, json, re, urllib.request
from pathlib import Path

HERE = Path(__file__).parent
LIB, PRESETS, STATE = HERE / "library.json", HERE / "presets-library.json", HERE / "curation-state.json"
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
        if name.startswith("RSVD") or (seg == "strip" and two_d_only) or (seg == "panel" and "2" not in flags):
            continue  # panel = 2D-capable only (WLED expands 1D effects onto a matrix, but they're strip looks)
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
    state = json.loads(STATE.read_text()) if STATE.exists() else {}
    st = state.setdefault(a.seg, {"reviewed_slots": [], "kept_fx": [], "skipped_fx": []})
    free = [s for s in SLOTS[a.seg] if str(s) not in presets]
    names, fxdata = get(a.host, "/json/eff"), get(a.host, "/json/fxdata")
    want = lambda audio: (not a.audio or audio) and (not a.ambient or not audio)

    # phase 1: the library presets already in this bucket; phase 2: every raw effect
    slots = [s for s, e in sorted(lib[a.seg].items(), key=lambda kv: int(kv[0]))
             if int(s) not in st["reviewed_slots"] and want(e["audio"])]
    done_fx = set(st["kept_fx"]) | set(st["skipped_fx"])
    todo = [c for c in candidates(names, fxdata, a.seg) if c[0] not in done_fx and want(c[2])]
    print(f"{a.seg}: {len(slots)} library presets + {len(todo)} effects to review, {len(free)} slots free, "
          f"{len(st['reviewed_slots'])} presets reviewed, {len(st['kept_fx'])} effects kept, {len(st['skipped_fx'])} skipped so far")
    if a.list:
        for s in slots:
            print(f"  slot {s:>3} {lib[a.seg][s]['n']:<24} {'audio' if lib[a.seg][s]['audio'] else 'ambient'}  (fx {presets[s]['seg'][0]['fx']})")
        for fx, name, audio in todo:
            print(f"  fx {fx:3d} {name:<24} {'audio' if audio else 'ambient'}")
        return

    def save():
        PRESETS.write_text(json.dumps(presets, separators=(",", ":")) + "\n")
        LIB.write_text(json.dumps(lib, indent=1) + "\n")
        STATE.write_text(json.dumps(state) + "\n")

    def ask(label):
        while True:
            k = input(f"{label}  [y/n/q] ").strip().lower()
            if k in ("y", "n", "q"):
                return k

    original = get(a.host, "/json/state")["seg"][seg_id]
    npal = len(get(a.host, "/json/pal"))
    try:
        for s in slots:
            e = lib[a.seg][s]
            post(a.host, {"seg": [presets[s]["seg"][0]]})           # the preset's own look, as stored
            k = ask(f"{'♪' if e['audio'] else ' '} slot {s:>3} {e['n']:<24}")
            if k == "q":
                return
            if k == "n":
                del lib[a.seg][s]; del presets[s]; free = sorted(free + [int(s)])
                print(f"   removed slot {s}")
            else:
                st["reviewed_slots"].append(int(s))
            save()
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
                return
            if k == "n":
                st["skipped_fx"].append(fx); save()
                continue
            live = get(a.host, "/json/state")["seg"][seg_id]
            seg = {"id": seg_id, **{key: live[key] for key in KEEP if key in live}}
            seg["n"] = seg_name
            slot = free.pop(0)
            presets[str(slot)] = {"n": name, "seg": [seg]}
            lib[a.seg][str(slot)] = {"n": name, "audio": audio}
            st["kept_fx"].append(fx); save()
            print(f"   kept as slot {slot} ({'audio' if audio else 'ambient'})")
    finally:
        restore = {"id": seg_id, **{key: original[key] for key in KEEP if key in original}}
        post(a.host, {"seg": [restore]})
        print(f"{a.seg} restored; {len(lib[a.seg])} {a.seg} modes in library.json")


if __name__ == "__main__":
    main()
