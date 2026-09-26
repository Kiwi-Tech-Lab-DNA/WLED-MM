#!/usr/bin/env python3
"""Replay a stage show through the real board cue engine, with no board.

Two modes:

    replay-show.py shows/1440783764.json            # fast-forward the whole song
    replay-show.py shows/1440783764.json --live URL # follow the live cloud clock

Fast-forward answers "does this show fire the looks I expect, in order?". --live
additionally proves the cue engine against the *real* recognition clock (the ±0.5-2 s
one from stage-shows.md §5), which is the part a desk test cannot fake.

Needs the harness built once:
    g++ -std=c++11 -o /tmp/np_show_replay ../usermods/room_mic_uploader/np_show_replay.cpp
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time
import urllib.request

HARNESS = "/tmp/np_show_replay"
STALE_MS = 30000


def script_fastforward(cues, step=0.25):
    end = cues[-1]["t"] + 20
    t = 0.0
    while t < end:
        yield f"TICK {int(t * 1000)} {t:.2f} 0"
        t += step


def script_live(url, token, track_id, seconds):
    """Follow the cloud's position_s, marking ticks stale exactly as the board would."""
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    anchor_pos, anchor_at, have, rejects = 0.0, 0.0, False, 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            d = json.load(urllib.request.urlopen(req, timeout=8))
            if d.get("state") == "playing" and str(d.get("track_id")) == str(track_id):
                cand = float(d["position_s"])
                cur = anchor_pos + (time.time() - anchor_at)
                # Mirrors npshow::accept_anchor — keep this in step with np_show.h.
                back = cur - cand
                if not have or back <= 3.0 or back >= 30.0:
                    rejects = 0
                    anchor_pos, anchor_at, have = cand, time.time(), True
                else:
                    rejects += 1
                    if rejects >= 2:            # persistent disagreement is not jitter
                        rejects = 0
                        anchor_pos, anchor_at, have = cand, time.time(), True
                        print(f"  (accepted persistent re-anchor {cur:.1f} -> {cand:.1f})", file=sys.stderr)
                    else:
                        print(f"  (ignored backwards re-anchor {cur:.1f} -> {cand:.1f})", file=sys.stderr)
            elif d.get("state") != "playing":
                have = False
        except Exception as e:                      # a dropped poll is a stale tick, not a crash
            print(f"  (poll failed: {e})", file=sys.stderr)
        for _ in range(20):                         # ~2 s of 100 ms ticks between polls
            now = time.time()
            # Before the first anchor there is no clock at all — extrapolating from
            # anchor_at=0 would hand the engine an epoch-sized position.
            pos = anchor_pos + (now - anchor_at) if have else 0.0
            stale = 0 if have and (now - anchor_at) * 1000 <= STALE_MS else 1
            yield f"TICK {int(now * 1000)} {pos:.2f} {stale}"
            time.sleep(0.1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("show")
    ap.add_argument("--live", metavar="URL", help="e.g. http://127.0.0.1:8000/api/nowplaying")
    ap.add_argument("--token")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--home", type=int, default=3)
    args = ap.parse_args()

    show = json.loads(pathlib.Path(args.show).read_text())
    cues = show["cues"]
    print(f"{show.get('artist')} - {show.get('title')}  ({len(cues)} cues, "
          f"{show.get('bpm')} BPM, track_id {show['track_id']})")

    lines = [f"HOME {args.home}"] + [f"CUE {c['t']} {c['preset']}" for c in cues]
    lines += list(script_live(args.live, args.token, show["track_id"], args.seconds)
                  if args.live else script_fastforward(cues))

    p = subprocess.run([HARNESS], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    print(p.stdout, end="")


if __name__ == "__main__":
    main()
