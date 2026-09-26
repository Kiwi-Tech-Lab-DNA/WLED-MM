#pragma once
#include <stdint.h>

// Pure quadrature decode for the Board-1 EC11 knobs.
//
// Deliberately free of Arduino/WLED deps so it can be host-tested (np_encoder_test.cpp) —
// same split as np_lyrics.h. The table is the bug-prone part: a single transposed entry gives
// a knob that counts in one direction only, which is miserable to diagnose on hardware.

// Gray-code transition table: index = (prev<<2)|cur, value = -1 / 0 / +1.
// 0 covers both "no change" and an illegal double-step (contact bounce); dropping those is
// what lets cheap mechanical encoders work with no debounce passives on the bench.
static const int8_t NP_QTAB[16] = { 0,-1, 1, 0,
                                    1, 0, 0,-1,
                                   -1, 0, 0, 1,
                                    0, 1,-1, 0 };

// Feed one (A<<1|B) sample. Advances *prev and *sub in place.
// Returns the change in DETENTS: -1, 0 or +1.
static inline int8_t npEncStep(uint8_t cur, uint8_t *prev, int8_t *sub, int8_t detent) {
  int8_t d = NP_QTAB[(((*prev) << 2) | cur) & 0x0F];
  *prev = cur;
  if (!d) return 0;
  *sub += d;
  if (*sub >=  detent) { *sub = 0; return  1; }
  if (*sub <= -detent) { *sub = 0; return -1; }
  return 0;
}
