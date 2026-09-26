// Bench harness: replay a stage show through the REAL cue engine (np_show.h) without a
// panel, so a show can be checked against the live recognition clock before flashing.
//
//   g++ -std=c++11 -o /tmp/np_show_replay np_show_replay.cpp
//   ./np_show_replay < script
//
// stdin is a tiny line protocol, so JSON parsing and HTTP stay in the driver script
// (tools/replay-show.py) and this file stays free of dependencies — the same reason
// np_show.h is pure in the first place.
//
//   CUE  <t_seconds> <preset_slot>     cues, in order (all CUEs before the first TICK)
//   HOME <slot>                        fallback look for a stale clock (default 3)
//   TICK <millis> <pos_seconds> <0|1>  one engine tick; trailing flag = clock stale
//
// Output: one line per preset change, i.e. exactly what the panel would be told to do.

#include "np_show.h"
#include <cstdio>
#include <cstring>

int main() {
  npshow::Show   show;
  npshow::Player player;
  uint8_t        home = 3;
  char           line[256];
  int            changes = 0;

  while (fgets(line, sizeof(line), stdin)) {
    if (!strncmp(line, "CUE", 3)) {
      double t; int p;
      if (sscanf(line + 3, "%lf %d", &t, &p) == 2) show.push_back(npshow::Cue{t, (uint8_t)p});
    } else if (!strncmp(line, "HOME", 4)) {
      int h;
      if (sscanf(line + 4, "%d", &h) == 1) home = (uint8_t)h;
    } else if (!strncmp(line, "TICK", 4)) {
      unsigned long ms; double pos; int st;
      if (sscanf(line + 4, "%lu %lf %d", &ms, &pos, &st) != 3) continue;
      int slot = npshow::tick(player, show, pos, st != 0, home);
      if (slot < 0) continue;
      printf("%8.1fs  preset %-3d %s\n", pos, slot,
             st ? "(STALE -> fallback)" : "");
      changes++;
    }
  }
  printf("-- %d preset change(s) from %u cue(s)\n", changes, (unsigned)show.size());
  return 0;
}
