// Host self-check for the EC11 quadrature decode.
//   g++ -std=c++11 -o /tmp/np_encoder_test np_encoder_test.cpp && /tmp/np_encoder_test
//
// Guards the one thing that is easy to get silently wrong and painful to debug on hardware:
// a transposed NP_QTAB entry, which yields a knob that counts in one direction only.

#include "np_encoder.h"
#include <cassert>
#include <cstdio>

// One full quadrature cycle, as (A<<1|B). CW is the direction NP_QTAB scores +1.
static const uint8_t CW[4]  = {0, 2, 3, 1};
static const uint8_t CCW[4] = {0, 1, 3, 2};

// Drive `turns` full cycles of `seq`, return net detents.
static int drive(const uint8_t *seq, int turns, int8_t detent) {
  uint8_t prev = seq[0];
  int8_t  sub  = 0;
  int     net  = 0;
  for (int t = 0; t < turns; t++)
    for (int i = 1; i <= 4; i++)              // 4 transitions per cycle, wrapping to seq[0]
      net += npEncStep(seq[i & 3], &prev, &sub, detent);
  return net;
}

int main() {
  // 1. One cycle = one detent, and the two directions are exact opposites.
  assert(drive(CW,  1, 4) ==  1);
  assert(drive(CCW, 1, 4) == -1);
  assert(drive(CW,  10, 4) ==  10);
  assert(drive(CCW, 10, 4) == -10);

  // 2. NP_ENC_DETENT actually scales — this is the calibration knob, so it must bite.
  assert(drive(CW, 12, 1) == 48);   // every transition is a detent
  assert(drive(CW, 12, 2) == 24);
  assert(drive(CW, 12, 4) == 12);

  // 3. A repeated sample (no edge) is inert — the ISR fires on CHANGE but bounce can re-fire.
  {
    uint8_t prev = 0; int8_t sub = 0;
    assert(npEncStep(0, &prev, &sub, 4) == 0);
    assert(npEncStep(0, &prev, &sub, 4) == 0);
    assert(sub == 0);
  }

  // 4. An illegal double-step (both lines changed at once = a missed sample) is dropped, not
  //    guessed at. Direction is genuinely ambiguous there; inventing one would drift the count.
  {
    uint8_t prev = 0; int8_t sub = 0;
    assert(npEncStep(3, &prev, &sub, 4) == 0);   // 00 -> 11
    assert(sub == 0);
  }

  // 5. Jitter inside one detent must not leak a count: rocking back and forth between two
  //    adjacent states nets zero no matter how long it goes on.
  {
    uint8_t prev = 0; int8_t sub = 0; int net = 0;
    for (int i = 0; i < 50; i++) {
      net += npEncStep(2, &prev, &sub, 4);
      net += npEncStep(0, &prev, &sub, 4);
    }
    assert(net == 0);
  }

  printf("np_encoder: all checks passed\n");
  return 0;
}
