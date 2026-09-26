// Host self-check for the stage-show cue timeline.
//   g++ -std=c++11 -o /tmp/np_show_test np_show_test.cpp && /tmp/np_show_test
//
// Guards the two things that are easy to get silently wrong and painful to debug on
// hardware: which cue is in effect at a given position (an off-by-one fires the wrong
// look for a whole section), and the stale-anchor guard (whose failure mode is a show
// that keeps running against a frozen clock, looking plausible but drifting).

#include "np_show.h"
#include <cassert>
#include <cstdio>

using npshow::Cue;
using npshow::Show;

int main() {
  // Lithium's real cue list, trimmed (see stageshow/shows/1440783764.json).
  Show s = {{0.21, 20}, {9.03, 21}, {37.76, 23}, {63.14, 21}, {92.35, 23}, {117.68, 24}};

  // 1. Before the first cue: nothing in effect (the board keeps its current look).
  assert(npshow::cue_at(s, 0.0) == -1);

  // 2. A cue takes effect exactly at its own timestamp, not a frame later.
  assert(npshow::cue_at(s, 0.21) == 0);
  assert(npshow::cue_at(s, 9.03) == 1);

  // 3. Mid-section holds the section's cue.
  assert(npshow::cue_at(s, 50.0) == 2);
  assert(s[npshow::cue_at(s, 50.0)].preset == 23);

  // 4. Just before a boundary is still the previous section — the off-by-one that
  //    would fire the chorus look one section early.
  assert(npshow::cue_at(s, 37.75) == 1);
  assert(npshow::cue_at(s, 37.76) == 2);

  // 5. Past the last cue, the last cue stands (songs end on their final section).
  assert(npshow::cue_at(s, 9999.0) == 5);

  // 6. The scan is stateless, so a backwards jump resolves correctly. Recognition
  //    re-anchors slightly behind constantly; a remembered cursor would stick.
  assert(npshow::cue_at(s, 100.0) == 4);
  assert(npshow::cue_at(s, 40.0) == 2);   // jumped back: must follow, not hold at 4

  // 7. Empty show: no cue, no crash.
  assert(npshow::cue_at(Show(), 12.0) == -1);

  // --- stale-anchor guard ---
  const uint32_t MAX = 30000;

  // 8. Fresh anchor is trusted; an old one is not.
  assert(!npshow::stale(100000, 99000, true, MAX));
  assert(npshow::stale(100000, 60000, true, MAX));

  // 9. No anchor at all is stale regardless of timestamps.
  assert(npshow::stale(100000, 99999, false, MAX));

  // 10. millis() rollover (~49.7 days) must not read as stale: unsigned wrap makes
  //     now-anchor the true small elapsed time, so this stays trusted.
  assert(!npshow::stale(500, (uint32_t)0xFFFFFF00, true, MAX));

  // --- playback: the decisions that actually break on hardware ---
  const uint8_t HOME = 3;   // Mountain

  // 11. A normal run fires each cue exactly once, in order, and says nothing between.
  {
    npshow::Player p;
    int fired = 0, applied[16];
    for (double t = 0.0; t < 130.0; t += 0.06) {          // the real ~60 ms tick
      int slot = npshow::tick(p, s, t, false, HOME);
      if (slot >= 0) applied[fired++] = slot;
    }
    assert(fired == 6);
    assert(applied[0] == 20 && applied[1] == 21 && applied[2] == 23);
    assert(applied[3] == 21 && applied[4] == 23 && applied[5] == 24);
  }

  // 12. Stale clock drops to home ONCE, not on every tick (which would restart the
  //     effect's animation ~16 times a second).
  {
    npshow::Player p;
    npshow::tick(p, s, 50.0, false, HOME);                // running the chorus look
    assert(npshow::tick(p, s, 50.0, true, HOME) == HOME); // recognition drops
    for (int i = 0; i < 100; i++)
      assert(npshow::tick(p, s, 50.0, true, HOME) == -1); // ...and stays quiet
  }

  // 13. When the clock comes back, the correct cue re-fires even though the position
  //     never left that section — otherwise the show would stay on the fallback look
  //     until the *next* section boundary, which can be half a minute away.
  {
    npshow::Player p;
    npshow::tick(p, s, 50.0, false, HOME);
    npshow::tick(p, s, 50.0, true, HOME);
    assert(npshow::tick(p, s, 50.0, false, HOME) == 23);
  }

  // 14. A show-less track never touches the panel, however stale the clock gets.
  {
    npshow::Player p;
    assert(npshow::tick(p, Show(), 10.0, false, HOME) == -1);
    assert(npshow::tick(p, Show(), 10.0, true, HOME) == -1);
  }

  // 15. Hysteresis: a clock sitting ON a boundary must NOT chatter. The chorus starts at
  //     37.76; re-anchoring either side of it every 2 s would otherwise restart the
  //     effect indefinitely.
  {
    npshow::Player p;
    npshow::tick(p, s, 20.0, false, HOME);                // settled in the verse (cue 1)
    assert(npshow::tick(p, s, 38.0, false, HOME) == -1);  // barely past: hold
    assert(npshow::tick(p, s, 37.0, false, HOME) == -1);  // wobbled back: still hold
    assert(npshow::tick(p, s, 38.9, false, HOME) == -1);  // still inside the margin
    assert(npshow::tick(p, s, 39.3, false, HOME) == 23);  // clearly in: now change
  }

  // 16. Hysteresis is symmetric — a genuine backwards move is followed once it clears
  //     the margin, so a real re-anchor is not ignored forever.
  {
    npshow::Player p;
    npshow::tick(p, s, 50.0, false, HOME);                // chorus (cue 2, starts 37.76)
    assert(npshow::tick(p, s, 37.0, false, HOME) == -1);  // inside the margin: hold
    assert(npshow::tick(p, s, 36.0, false, HOME) == 21);  // clearly back: follow
  }

  // 17. The first cue of a show is exempt from hysteresis: there is no previous look to
  //     hold, and holding would strand the panel on whatever preceded the song.
  {
    npshow::Player p;
    assert(npshow::tick(p, s, 0.25, false, HOME) == 20);  // fires immediately
  }

  // 18. Hysteresis must not defeat the stale-recovery re-fire (case 13): coming back from
  //     the fallback, lastCue is -1, so the current cue re-applies without waiting.
  {
    npshow::Player p;
    npshow::tick(p, s, 50.0, false, HOME);
    npshow::tick(p, s, 50.0, true, HOME);                 // stale -> home
    assert(npshow::tick(p, s, 50.0, false, HOME) == 23);  // recovers immediately
  }

  // --- anchor acceptance (bench-measured recognition jitter) ---

  { int r = 0;
    // 19. First anchor is always taken.
    assert(npshow::accept_anchor(50.0, 0.0, false, r));

    // 20. Forward corrections always land — drift correction must never be blocked.
    assert(npshow::accept_anchor(60.0, 50.0, true, r));
    assert(npshow::accept_anchor(200.0, 50.0, true, r));

    // 21. Small backwards wobble is ordinary and accepted.
    assert(npshow::accept_anchor(48.0, 50.0, true, r));

    // 22. A one-off backwards re-anchor in the jitter band is rejected, so the show does
    //     not rewind whole sections on a single bad poll.
    assert(!npshow::accept_anchor(33.0, 50.0, true, r));
  }

  // 23. A genuinely large jump back is a track change or seek, and IS followed —
  //     otherwise the clock would extrapolate forever against the wrong song.
  { int r = 0; assert(npshow::accept_anchor(5.0, 50.0, true, r)); }

  // 24. THE ONE THAT BIT ON THE BENCH: a persistent disagreement is not jitter. The cloud
  //     reporting ~14 s lower on every single poll means our extrapolation is wrong, so
  //     the second consecutive rejection must give in rather than free-run to the stale
  //     timeout and repeat forever.
  { int r = 0;
    assert(!npshow::accept_anchor(38.6, 52.6, true, r));   // first: assume jitter
    assert(npshow::accept_anchor(40.6, 54.7, true, r));    // second: believe it
  }

  // 25. The reject counter resets on acceptance, so isolated blips never accumulate into
  //     a spurious "persistent" verdict.
  { int r = 0;
    assert(!npshow::accept_anchor(33.0, 50.0, true, r));
    assert(npshow::accept_anchor(52.0, 50.0, true, r));    // a normal forward poll
    assert(!npshow::accept_anchor(33.0, 50.0, true, r));   // must be treated as fresh
  }

  printf("np_show: all checks passed\n");
  return 0;
}
