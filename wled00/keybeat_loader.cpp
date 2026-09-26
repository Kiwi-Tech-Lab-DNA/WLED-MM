#include "wled.h"

#ifdef WLED_ENABLE_GIF

#include "GifDecoder.h"

/*
 * nowplaying: "Keybeat 2D" effect support — an on-chip port of LedFx's Keybeat2d.
 *
 * Plays a GIF whose key frames are locked to the music's beat. Unlike the stock
 * Image effect (timer-paced, sequential), this needs RANDOM frame access (jump
 * back to an early key frame on loop), so on (re)load we PRE-DECODE every frame
 * into a PSRAM RGB565 buffer (compositing through a canvas so GIF frame-disposal
 * is honoured), then index that buffer directly during playback. No file IO or
 * GIF decoding happens on the render path.
 *
 * Beat sync: WLED's AudioReactive exposes only a samplePeak beat flag (no
 * continuous beat oscillator like LedFx). We reconstruct the oscillator from the
 * interval between detected peaks: each beat advances to the next key frame and
 * the measured interval drives a 0..1 phase that interpolates the in-between
 * frames toward the following key frame (matching LedFx's beat_incs model).
 *
 * Per-GIF beat data travels in a sidecar text file "/<name>.kb" uploaded next to
 * the GIF (segment params have nowhere to hold a long beat_frames string):
 *   line 1: beat_frames, space separated (may be empty => 1 frame per beat)
 *   line 2: "<ping_pong 0|1> <half_beat 0|1>"   (half_beat currently unused)
 *   line 3: frame count (lets us size the PSRAM buffer in one decode pass)
 */

static File kbFile;
static GifDecoder<320,320,12,true> kbDecoder;

bool kbSeekCallback(unsigned long position) { return kbFile.seek(position); }
unsigned long kbPositionCallback(void) { return kbFile.position(); }
int kbReadCallback(void) { return kbFile.read(); }
int kbReadBlockCallback(void *buffer, int numberOfBytes) { return kbFile.read((uint8_t*)buffer, numberOfBytes); }
int kbSizeCallback(void) { return kbFile.size(); }

// ---- module state (single active segment, like image_loader) ----
static Segment* kbSeg = nullptr;
static char kbName[WLED_MAX_SEGNAME_LEN+2] = "";
static bool kbFailed = false;

static uint16_t *kbBuf = nullptr;     // PSRAM: kbFrames * kbGW * kbGH, RGB565
static uint16_t *kbCanvas = nullptr;  // scratch composite, freed after capture
static int kbFrames = 0;              // number of decoded forward frames
static uint16_t kbGW = 0, kbGH = 0;   // native GIF size
static int kbCapIdx = 0;              // capture cursor (frame being decoded)

// timeline
#define KB_MAX_BEATS 64
static int   kbBeatFrames[KB_MAX_BEATS];
static float kbBeatIncs[KB_MAX_BEATS];
static int   kbNumBeats = 0;
static int   kbPlayCount = 0;         // logical frame count incl. ping-pong mirror
static bool  kbPingPong = false;

// playback — phase-locked tempo oscillator. Key frames step on the oscillator's
// PREDICTED beat; raw audio peaks only correct tempo/phase when they land near an
// expected beat, so off-beat onsets in dense music don't add extra steps.
static int   kbBeatIdx = 0;
static int   kbFrameC = 0;
static float kbPhase = 0.0f;            // 0..1 progress through the current beat
static unsigned long kbPeriod = 500;    // ms per beat (the locked tempo)
static unsigned long kbLastBeatT = 0;   // time of last phase-locked correction
static unsigned long kbLastPeakT = 0;   // time of last beat-plausible peak (interval reference)
static unsigned long kbLastActiveT = 0; // time of last raw peak of any kind (silence check)
static unsigned long kbLastT = 0;       // time of last render (for dt)
static uint8_t kbLastPeak = 0;
#define KB_MIN_BEAT 200UL               // ignore peaks < 200ms apart (>300bpm)
#define KB_MAX_BEAT 1500UL              // clamp musical period (>=40bpm)
#define KB_ACTIVE_MS 1600UL             // freeze if no peak within this window (silence)

// ---- capture callbacks: composite each decoded frame into kbCanvas ----
static void kbScreenClear(void) {
  if (kbCanvas) memset(kbCanvas, 0, (size_t)kbGW * kbGH * sizeof(uint16_t));
}
static void kbDrawPixel(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
  if (!kbCanvas || x < 0 || y < 0 || x >= (int)kbGW || y >= (int)kbGH) return;
  kbCanvas[(size_t)y * kbGW + x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static void kbUpdateScreen(void) {
  // frame fully painted into canvas -> snapshot it
  if (kbBuf && kbCanvas && kbCapIdx < kbFrames) {
    memcpy(kbBuf + (size_t)kbCapIdx * kbGW * kbGH, kbCanvas, (size_t)kbGW * kbGH * sizeof(uint16_t));
    kbCapIdx++;
  }
}

void endKeybeatPlayback(Segment *seg) {
  if (!kbSeg || kbSeg != seg) return;
  if (kbBuf)    { p_free(kbBuf);    kbBuf = nullptr; }
  if (kbCanvas) { p_free(kbCanvas); kbCanvas = nullptr; }
  if (kbFile) kbFile.close();
  kbFrames = 0; kbGW = 0; kbGH = 0; kbNumBeats = 0; kbPlayCount = 0;
  kbFailed = false;
  kbSeg = nullptr;
  kbName[0] = '\0';
}

// parse a space/whitespace separated list of non-negative ints into out[], capped
static int kbParseInts(const char *s, int *out, int maxN) {
  int n = 0; const char *p = s;
  while (*p && n < maxN) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p < '0' || *p > '9') { if (*p) p++; continue; }
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    out[n++] = v;
  }
  return n;
}

// read "/<name>.kb": fills kbBeatFrames/kbNumBeats/kbPingPong, returns frame count hint (0 if none)
static int kbLoadSidecar(const char *name) {
  kbNumBeats = 0; kbPingPong = false;
  char path[WLED_MAX_SEGNAME_LEN + 8];
  path[0] = '/';
  strncpy(path + 1, name, WLED_MAX_SEGNAME_LEN);
  path[WLED_MAX_SEGNAME_LEN + 1] = '\0';
  strncat(path, ".kb", sizeof(path) - strlen(path) - 1);

  File f = WLED_FS.open(path, "r");
  if (!f) return 0;

  char buf[512];
  int frameHint = 0, line = 0;
  while (f.available() && line < 3) {
    int len = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
    buf[len] = '\0';
    if (line == 0)      kbNumBeats = kbParseInts(buf, kbBeatFrames, KB_MAX_BEATS);
    else if (line == 1) { int v[2]; int m = kbParseInts(buf, v, 2); if (m >= 1) kbPingPong = (v[0] != 0); }
    else if (line == 2) { int v; if (kbParseInts(buf, &v, 1) == 1) frameHint = v; }
    line++;
  }
  f.close();
  return frameHint;
}

// build kbPlayCount, ping-pong mirrored beats and kbBeatIncs (port of LedFx config_updated)
static void kbBuildTimeline(void) {
  int N = kbFrames;
  // clamp + sort-unique-ascending the beat list to valid indices
  int m = 0;
  for (int i = 0; i < kbNumBeats; i++) if (kbBeatFrames[i] >= 0 && kbBeatFrames[i] < N) kbBeatFrames[m++] = kbBeatFrames[i];
  kbNumBeats = m;
  for (int i = 1; i < kbNumBeats; i++) { int k = kbBeatFrames[i], j = i - 1; while (j >= 0 && kbBeatFrames[j] > k) { kbBeatFrames[j+1] = kbBeatFrames[j]; j--; } kbBeatFrames[j+1] = k; }

  if (kbPingPong && N >= 2) {
    kbPlayCount = 2 * N - 2;
    int orig = kbNumBeats;
    for (int b = orig - 1; b >= 0 && kbNumBeats < KB_MAX_BEATS; b--) {
      int v = kbBeatFrames[b];
      if (v != 0 && v != N - 1) kbBeatFrames[kbNumBeats++] = 2 * N - 2 - v; // mirrored index, appended ascending
    }
  } else {
    kbPlayCount = N;
  }

  if (kbNumBeats == 0) {
    kbBeatIncs[0] = 1.0f;
    kbBeatIdx = 0; kbFrameC = 0;
  } else {
    for (int b = 0; b < kbNumBeats; b++) {
      int frames;
      if (b == kbNumBeats - 1) frames = kbPlayCount - kbBeatFrames[b] + kbBeatFrames[0];
      else                     frames = kbBeatFrames[b + 1] - kbBeatFrames[b];
      if (frames < 1) frames = 1;
      kbBeatIncs[b] = 1.0f / (float)frames;
    }
    kbBeatIdx = 0; kbFrameC = kbBeatFrames[0];
  }
}

// (re)load the GIF named by the segment into the PSRAM frame buffer
static void kbLoad(Segment &seg) {
  endKeybeatPlayback(kbSeg);          // free anything previous
  kbSeg = &seg;
  strncpy(kbName, seg.name, WLED_MAX_SEGNAME_LEN);
  kbName[WLED_MAX_SEGNAME_LEN] = '\0';
  kbFailed = false;

  size_t nameLen = strlen(kbName);
  if (nameLen < 4 || strcmp(kbName + nameLen - 4, ".gif") != 0) { kbFailed = true; return; }

  int frameHint = kbLoadSidecar(kbName);

  // wait for LED output to be idle before heavy file IO (flicker fixer)
  unsigned t0 = millis();
  while (strip.isUpdating() && (millis() - t0 < 100)) delay(1);

  char gifPath[WLED_MAX_SEGNAME_LEN + 2];
  gifPath[0] = '/'; strcpy(gifPath + 1, kbName);
  if (kbFile) kbFile.close();
  kbFile = WLED_FS.open(gifPath, "r");
  if (!kbFile) { kbFailed = true; USER_PRINTF("Keybeat: GIF not found %s\n", gifPath); return; }

  kbDecoder.setScreenClearCallback(kbScreenClear);
  kbDecoder.setUpdateScreenCallback(kbUpdateScreen);
  kbDecoder.setDrawPixelCallback(kbDrawPixel);
  kbDecoder.setFileSeekCallback(kbSeekCallback);
  kbDecoder.setFilePositionCallback(kbPositionCallback);
  kbDecoder.setFileReadCallback(kbReadCallback);
  kbDecoder.setFileReadBlockCallback(kbReadBlockCallback);
  kbDecoder.setFileSizeCallback(kbSizeCallback);

#if __cpp_exceptions
  try { kbDecoder.alloc(); }
  catch (...) { kbFailed = true; errorFlag = ERR_NORAM_PX; kbFile.close(); USER_PRINTLN(F("Keybeat: decoder OOM")); return; }
#else
  kbDecoder.alloc();
#endif

  if (kbDecoder.startDecoding() < 0) { kbFailed = true; kbDecoder.dealloc(); kbFile.close(); USER_PRINTLN(F("Keybeat: startDecoding failed")); return; }
  kbDecoder.getSize(&kbGW, &kbGH);
  if (kbGW == 0 || kbGH == 0) { kbFailed = true; kbDecoder.dealloc(); kbFile.close(); return; }

  // determine frame count: prefer sidecar hint, else decode one cycle to count
  int N = frameHint;
  if (N <= 0) {
    int count = 0;
    while (count < 1000) { if (kbDecoder.decodeFrame(false) < 0) break; count++; if (kbDecoder.getCycleNumber() > 0) break; }
    N = (kbDecoder.getCycleNumber() > 0) ? kbDecoder.getFrameCount() : count;
    // restart decoding for the capture pass
    kbDecoder.dealloc(); kbFile.seek(0);
    if (kbDecoder.startDecoding() < 0) { kbFailed = true; kbFile.close(); return; }
    kbDecoder.getSize(&kbGW, &kbGH);
  }
  if (N <= 0) { kbFailed = true; kbDecoder.dealloc(); kbFile.close(); return; }
  if (N > 1000) N = 1000;

  size_t frameBytes = (size_t)kbGW * kbGH * sizeof(uint16_t);
  kbCanvas = (uint16_t*)p_malloc(frameBytes);
  kbBuf    = (uint16_t*)p_malloc(frameBytes * N);
  if (!kbCanvas || !kbBuf) { kbFailed = true; errorFlag = ERR_NORAM_PX; endKeybeatPlayback(kbSeg); USER_PRINTLN(F("Keybeat: frame buffer OOM")); return; }

  memset(kbCanvas, 0, frameBytes);
  kbFrames = N; kbCapIdx = 0;
  for (int i = 0; i < N; i++) {
    if (kbDecoder.decodeFrame(false) < 0) break;   // kbUpdateScreen snapshots into kbBuf[i]
  }
  kbFrames = kbCapIdx;                              // actual captured frames

  // done with the decoder/file — playback is pure PSRAM
  kbDecoder.dealloc();
  kbFile.close();
  if (kbCanvas) { p_free(kbCanvas); kbCanvas = nullptr; }

  if (kbFrames <= 0) { kbFailed = true; endKeybeatPlayback(kbSeg); return; }

  kbBuildTimeline();
  kbBeatIdx = 0;
  kbFrameC = (kbNumBeats > 0) ? kbBeatFrames[0] : 0;
  kbPhase = 0.0f;
  kbPeriod = 500;
  kbLastBeatT = millis();
  kbLastPeakT = 0;
  kbLastT = millis();
  kbLastPeak = 0;
  USER_PRINTF("Keybeat: loaded %s  %dx%d  %d frames  %d beats  pp=%d  play=%d\n",
              kbName, kbGW, kbGH, kbFrames, kbNumBeats, (int)kbPingPong, kbPlayCount);
}

// advance the beat clock and render the current frame to the segment
byte renderKeybeatToSegment(Segment &seg, uint8_t samplePeak, float volumeSmth) {
  if (!seg.name) return 1;
  if (kbSeg && kbSeg != &seg) {                    // only one segment at a time
    if (!seg.isActive()) return 2;
    if (kbFailed || !kbSeg->isActive()) endKeybeatPlayback(kbSeg);
    else return 2;
  }

  if (strncmp(kbName, seg.name, WLED_MAX_SEGNAME_LEN) != 0) kbLoad(seg);
  if (kbFailed || !kbBuf || kbFrames <= 0) return 255;

  unsigned long now = millis();
  unsigned long dt = now - kbLastT;
  if (dt > 200) dt = 200;               // clamp big gaps (effect was paused, etc.)
  kbLastT = now;
  (void)volumeSmth;

  float speedF = (float)seg.speed / 128.0f;  // Speed slider scales tempo (sx=128 -> 1.0x)
  if (speedF < 0.05f) speedF = 0.05f;

  bool rising = (samplePeak && !kbLastPeak);
  kbLastPeak = samplePeak;

  // ----- phase-lock to incoming peaks (tempo + phase correction only) -----
  if (rising) {
    kbLastActiveT = now;                        // any onset means music is playing
    unsigned long ivRaw = now - kbLastPeakT;
    if (ivRaw >= KB_MIN_BEAT) {                 // sub-200ms onsets (hats) must not move the
      kbLastPeakT = now;                        // beat reference, or every iv reads too short
      if (ivRaw <= KB_MAX_BEAT) {
        // fold subdivisions/multiples into the tracked octave: without this, onsets on
        // eighths pull the period toward half a beat and nothing ever pulls it back up
        unsigned long iv = ivRaw;
        while (iv < (kbPeriod * 3) / 4) iv *= 2;
        while (iv > (kbPeriod * 3) / 2) iv /= 2;
        // phase error: distance from the nearest predicted beat boundary (-0.5..0.5, 0 = on beat)
        float err = (kbPhase < 0.5f) ? kbPhase : (kbPhase - 1.0f);
        if (fabsf(err) < 0.30f) {                 // peak lands near a predicted beat -> tighten lock
          kbPhase -= err * 0.5f;                  // nudge phase toward the beat
          if (kbPhase < 0.0f) kbPhase += 1.0f;
          kbPeriod = (kbPeriod * 4 + iv) / 5;     // gentle tempo adapt, within the locked octave
          kbLastBeatT = now;
        } else if (now - kbLastBeatT > 2 * kbPeriod) {
          // lost lock for a while -> (re)acquire tempo from the raw interval, so a genuinely
          // different tempo can still be picked up (folding would pin us to the old one)
          kbPeriod = ivRaw; kbPhase = 0.0f; kbLastBeatT = now;
        }
        // otherwise: off-beat onset -> ignored for stepping
      }
    }
  }
  if (kbPeriod < KB_MIN_BEAT) kbPeriod = KB_MIN_BEAT;
  if (kbPeriod > KB_MAX_BEAT) kbPeriod = KB_MAX_BEAT;

  // ----- free-run the oscillator; step a key frame on each PREDICTED beat (wrap) -----
  bool active = seg.check1 || (kbLastActiveT != 0 && (now - kbLastActiveT) < KB_ACTIVE_MS);
  if (active) {
    kbPhase += (float)dt * speedF / (float)kbPeriod;
    while (kbPhase >= 1.0f) {
      kbPhase -= 1.0f;
      if (kbNumBeats == 0) kbFrameC = (kbFrameC + 1) % kbPlayCount;
      else                 kbBeatIdx = (kbBeatIdx + 1) % kbNumBeats;
    }
  }

  // ----- interpolate the in-between frame from the 0..1 phase -----
  if (kbNumBeats > 0) {
    int prog = (int)(kbPhase / kbBeatIncs[kbBeatIdx]); // = phase * (frames to next key)
    kbFrameC = (kbBeatFrames[kbBeatIdx] + prog) % kbPlayCount;
  }

  // ----- map ping-pong logical index to a real forward frame -----
  int actual = kbFrameC;
  if (kbPingPong && kbFrames >= 2 && actual >= kbFrames) actual = 2 * kbFrames - 2 - actual;
  if (actual < 0) actual = 0;
  if (actual >= kbFrames) actual = kbFrames - 1;

  // ----- blit kbBuf[actual] to the segment (nearest-neighbour for any seg size) -----
  const uint16_t *frame = kbBuf + (size_t)actual * kbGW * kbGH;
  unsigned cols = seg.virtualWidth();
  unsigned rows = seg.virtualHeight();
  for (unsigned sy = 0; sy < rows; sy++) {
    unsigned gy = (kbGH == rows) ? sy : (sy * kbGH / rows);
    if (gy >= kbGH) gy = kbGH - 1;
    for (unsigned sx = 0; sx < cols; sx++) {
      unsigned gx = (kbGW == cols) ? sx : (sx * kbGW / cols);
      if (gx >= kbGW) gx = kbGW - 1;
      uint16_t c = frame[gy * kbGW + gx];
      uint8_t r = (c >> 8) & 0xF8;
      uint8_t g = (c >> 3) & 0xFC;
      uint8_t b = (c << 3) & 0xF8;
      seg.setPixelColorXY((int)sx, (int)sy, RGBW32(gamma8(r), gamma8(g), gamma8(b), 0));
    }
  }
  return 0;
}

#endif // WLED_ENABLE_GIF
