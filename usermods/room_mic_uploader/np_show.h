#pragma once
//
// Stage shows: the pure cue-timeline logic, kept free of Arduino/WLED headers so it
// builds and self-checks on the dev box (np_show_test.cpp). See
// nowplaying-project/docs/stage-shows.md.
//
// A show scripts only the SLOW structure of a song — section boundaries, each with a
// preset slot. The live mic still drives per-beat texture. That split is what makes the
// design survive the clock error: `position_s` comes from room-mic recognition and is
// only accurate to ~±0.5-2 s, which is invisible on a section change and would be
// hopeless on a scripted beat.
//
#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace npshow {

// A cue = "from time t, run preset slot p". v1 deliberately carries nothing else: the
// preset already stores its own effect/palette/brightness, so playback needs no new
// render code, only the existing applyPreset() path.
struct Cue {
  double  t;
  uint8_t preset;
};

typedef std::vector<Cue> Show;

// Index of the cue in effect at `pos` seconds, or -1 before the first cue.
// ponytail: linear scan. ~20 cues polled at ~16 Hz is nothing, and a stateless scan
// cannot drift or mis-seek the way a remembered cursor can when the anchor jumps
// backwards (which it does, every time recognition re-syncs a little behind).
inline int cue_at(const Show &s, double pos) {
  int hit = -1;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i].t > pos) break;                 // cues are sorted by t
    hit = (int)i;
  }
  return hit;
}

// Is the play clock too old to trust? `position_s` is re-anchored on every /nowplaying
// poll (~2 s). If recognition drops, the extrapolated position becomes fiction and the
// show would fire cues at the wrong moments — so the caller falls back to plain
// mic-reactive instead. Cheap, and not optional.
inline bool stale(uint32_t nowMs, uint32_t anchorAtMs, bool haveAnchor, uint32_t maxAgeMs) {
  return !haveAnchor || (uint32_t)(nowMs - anchorAtMs) > maxAgeMs;
}

// --- anchor acceptance -------------------------------------------------------------
// Backwards re-anchors in this band are treated as recognition jitter and IGNORED; the
// board keeps extrapolating its own clock instead. Measured on the bench (2026-08-07):
// `position_s` is accurate to about +3 s immediately after a recognition lands, but
// successive anchors can disagree by 17-25 s, because the cloud computes
//     position_s = offset + (now - last_ingest_at) + recognition_lag_s
// with recognition_lag_s a FIXED 13 s fudge for capture+songrec latency. When the real
// latency differs from 13 s, every anchor is wrong by the difference, and the sign can
// flip between polls. Accepting those blindly rewinds the show by whole sections.
//
// Forward jumps are always accepted: drift correction should never be blocked, and a
// slightly-early show reads far better than one that rewinds.
//
// Beyond SEEK_S the move is taken as real (a track change or a genuine seek), so the
// clock still follows the music rather than getting stuck extrapolating forever.
static const double ANCHOR_JITTER_S = 3.0;    // below this: ordinary wobble, accept
static const double ANCHOR_SEEK_S   = 30.0;   // above this: a real seek, accept

// ...but a DISAGREEMENT THAT PERSISTS IS NOT JITTER. If the cloud keeps reporting the
// same lower position poll after poll, the cloud is right and our extrapolation is the
// thing that is wrong — most obviously when the first anchor of a session was seeded from
// a stale leftover recognition. Rejecting forever then free-runs on a bad clock until the
// stale timeout fires, which is worse than the rewind this guard exists to prevent (seen
// on the bench, 2026-08-07). So tolerate ONE odd poll and believe the second.
static const int ANCHOR_MAX_REJECTS = 2;

// Should a freshly-polled position replace the extrapolated one? `rejects` is caller-owned
// state: consecutive rejections, reset on every acceptance.
inline bool accept_anchor(double newPos, double curPos, bool haveCur, int &rejects) {
  if (!haveCur) { rejects = 0; return true; }      // nothing to compare against yet
  double back = curPos - newPos;                   // >0 means the anchor moved backwards
  if (back <= ANCHOR_JITTER_S || back >= ANCHOR_SEEK_S) { rejects = 0; return true; }
  if (++rejects >= ANCHOR_MAX_REJECTS) { rejects = 0; return true; }   // persistent => real
  return false;
}

// Playback state: which cue is currently showing, and whether we have dropped to the
// fallback look. Small enough to keep pure, which is the point — the decisions below are
// what actually break on hardware, so they are host-tested rather than field-debugged.
struct Player {
  int  lastCue  = -1;
  bool fellBack = false;
};

// CALIBRATION KNOB (seconds). How far past a boundary the clock must be before the look
// actually changes.
//
// Without this, a clock sitting ON a boundary chatters: the position is re-anchored from
// recognition every ~2 s and is only accurate to ±0.5-2 s, so consecutive polls land on
// either side of the same cue and the preset re-applies — restarting the effect's
// animation every couple of seconds, for as long as the song sits near that boundary.
// Extrapolation between polls is monotonic, so this is purely a re-anchor artefact.
//
// Cost of raising it: a section change lands that much later. Cost of lowering it:
// chatter returns. Tune on hardware against the room's actual recognition jitter.
static const double CUE_HYSTERESIS_S = 1.5;

// One tick of playback. Returns the preset slot to apply right now, or -1 for "leave the
// panel alone" (the overwhelmingly common case — a section lasts ~25 s and this is called
// ~16 times a second).
//
// `homeSlot` is the plain mic-reactive look to fall back to when the clock goes stale.
inline int tick(Player &p, const Show &s, double pos, bool clockStale, uint8_t homeSlot) {
  if (s.empty()) return -1;

  if (clockStale) {
    if (p.fellBack) return -1;            // already fell back; don't re-apply every tick
    p.fellBack = true;
    p.lastCue  = -1;                      // so the right cue re-fires when the clock returns
    return (int)homeSlot;
  }
  p.fellBack = false;

  int idx = cue_at(s, pos);
  if (idx < 0 || idx == p.lastCue) return -1;

  // Hysteresis, but only against the boundary we would be crossing (see CUE_HYSTERESIS_S).
  // The first cue of a show is exempt: there is no previous look to hold, and holding
  // would leave the panel on whatever it was showing before the song started.
  if (p.lastCue >= 0) {
    if (idx > p.lastCue) {
      if (pos < s[idx].t + CUE_HYSTERESIS_S) return -1;          // not far enough in yet
    } else {
      if (pos > s[p.lastCue].t - CUE_HYSTERESIS_S) return -1;    // not far enough back
    }
  }

  p.lastCue = idx;
  return (int)s[idx].preset;
}

}  // namespace npshow
