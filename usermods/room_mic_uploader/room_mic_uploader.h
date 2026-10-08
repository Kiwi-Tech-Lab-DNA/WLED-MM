#pragma once

#include "wled.h"
#include <WiFiClient.h>
#include "esp_heap_caps.h"
#include "room_mic_uploader_secrets.h"   // NP_BASE_URL + NP_DEVICE_TOKEN (gitignored)
#include "uart_link.h"                    // b2-13 inter-board UART1 frame codec (shared with Board 2)
#include "np_lyrics.h"                    // P1: pure timing model + word font/color (host-tested)
#include "np_encoder.h"                   // b2-13 knobs: pure quadrature decode (host-tested)

// --- b2-13 knobs: 3 bare EC11 quadrature encoders (Seesaw #5880 dropped 2026-07-19) ---
// Bare EC11s replace the I2C Seesaws: production-bom.md cost lever #1 (~$15/unit), and
// dropping I2C frees GPIO 47/48 — exactly the pair three encoders need. Rig + wiring in
// nowplaying-project/docs/breadboard-bringup.md.
//
// The Seesaw counted detents IN HARDWARE, so poll rate never mattered. We decode quadrature
// ourselves now, and that decode runs in a PIN-CHANGE ISR rather than the uartTask poll: that
// task ticks at 10 ms while a brisk spin emits transitions far faster, so polling the raw pins
// would silently drop counts AND mis-read direction. The ISR keeps the same "accumulated
// position" contract pollKnobs() already consumed from getEncoderPosition(), so the consumer
// logic is untouched.
struct NpEncoder {
  int8_t           pinA, pinB;
  volatile int32_t pos;   // detents, signed — the getEncoderPosition() replacement
  volatile int8_t  sub;   // quadrature transitions since the last emitted detent
  volatile uint8_t prev;  // last (A<<1|B) state
};
// Index order matches the protocol knob id: 0=mode, 1=bright, 2=gain.
// Direction is set by the pin ORDER here: swap a pair to reverse that knob. History: the
// knob PCB (nowplaying/hardware/knob-board) needed B-first (2026-09-26); the knobs as wired
// in the guitar decode clockwise=down with that, so they're A-first again (2026-10-07).
static NpEncoder npEnc[3] = {
  { 47, 48, 0, 0, 0 },   // mode   — GPIO 47/48, the pair freed by dropping the Seesaws
  {  1, 12, 0, 0, 0 },   // bright — GPIO 1/12
  { 41, 42, 0, 0, 0 },   // gain   — GPIO 41/42
};

// CALIBRATION KNOB. Quadrature transitions per physical detent. EC11 batches vary (1, 2 or 4,
// depending where the detent sits in the cycle), so one click may otherwise step 4 modes.
// Tune against the encoders actually on the bench: if one click steps N looks, scale this by N.
// 4 = the common full-cycle-per-detent case.
static constexpr int8_t NP_ENC_DETENT = 4;

static void IRAM_ATTR npEncIsr(void *arg) {
  NpEncoder *e = (NpEncoder *)arg;
  uint8_t cur  = (uint8_t)((digitalRead(e->pinA) << 1) | digitalRead(e->pinB));
  uint8_t prev = e->prev;                // volatile -> locals for npEncStep's in/out params
  int8_t  sub  = e->sub;
  int8_t  d    = npEncStep(cur, &prev, &sub, NP_ENC_DETENT);
  e->prev = prev;
  e->sub  = sub;
  e->pos += d;
}

// room_mic_uploader — Board 1 cloud-ingest as a WLED-MM usermod.
//
// Closes the appliance loop while the LEDs keep rendering: audioreactive owns the I2S
// mic, so this usermod CANNOT capture audio itself (a second i2s_read would steal the
// FFT task's samples). Instead the build injects a raw-PCM tap into audioreactive
// (pio-scripts/patch_audioreactive.py, guarded by -D USERMOD_ROOM_MIC_UPLOADER): the FFT
// task copies each fresh batch of mic samples into a PSRAM ring (npPcmRing) and publishes
// a monotonic sample counter (npPcmWritten). This usermod's own FreeRTOS task reads the
// last ~10 s out of that ring, frames it as a 22050 Hz mono S16-LE WAV, RMS-gates a silent
// room, and multipart-POSTs it to POST /api/ingest with the shared device bearer token.
//
// Why 22050 Hz and no resampling: proven against the live cloud — a 22050 Hz WAV returns
// matched:true (songrec resamples internally). The WAV header byte layout is the exact one
// proven by firmware/board1-smoketest to pass the gate's mimes:wav sniff. A drifted header
// => 422 is the #1 failure mode; keep it byte-for-byte.
//
// Concurrency: single writer (FFT task) + single reader (this task) + a monotonic counter,
// no lock. Safe because the ring (12 s) is comfortably larger than the window (10 s): the
// writer can't lap the oldest sample we read during the few-ms copy. The blocking HTTP POST
// runs on this task (pinned to core 0, low priority) so it never stalls LED rendering, which
// runs in the main loop on core 1.

// --- audioreactive raw-PCM tap (defined by the injected patch in audio_reactive.h) -------
extern int16_t*          npPcmRing;      // PSRAM ring of int16 mic samples (22050 Hz mono)
extern volatile uint32_t npPcmWritten;   // total samples ever written (monotonic)
extern uint32_t          npPcmRingLen;   // ring length in samples (single source of truth)

// b2-13 gain knob: runtime viz-punch multiplier. The Mountain look auto-normalizes its own
// gain and ignores WLED's inputLevel, so the gain knob drives THIS instead. 1.0 = the unchanged
// look; >1 punchier, <1 softer. We DEFINE the single global here (this header is #included once,
// in usermods_list.cpp); the audioreactive patch (NP_VIZ_GAIN) only injects an `extern` + the
// multiply into the LFX output. That ownership split means a drifted patch anchor just makes the
// gain knob a no-op — it can never leave npVizGain undefined and break the link.
float                    npVizGain = 1.0f;

class RoomMicUploader : public Usermod {
  private:
    // capture / contract config
    static constexpr uint32_t SAMPLE_RATE     = 22050;        // audioreactive native rate on -S3
    static constexpr uint32_t CAPTURE_SECONDS = 10;           // contract allows 8-18 s
    static constexpr uint32_t CAPTURE_SAMPLES = SAMPLE_RATE * CAPTURE_SECONDS;
    static constexpr size_t   PCM_BYTES       = (size_t)CAPTURE_SAMPLES * 2;  // S16 = 2 B/sample
    static constexpr uint32_t CADENCE_MS      = 33000;        // re-ingest ~33 s (< 90 s freshness)

    // Silent-room gate (ADR-0002). The tap samples are audioreactive's gained/AGC'd floats
    // cast to int16, so this threshold is tuned to THAT scaling (not the smoketest's >>13).
    // Logged every capture so it can be tuned on the bench; start permissive.
    static constexpr double RMS_GATE = 120.0;

    // Silence->music onset — the "a new song started" trigger. While waiting out the cadence we
    // cheaply probe the tap RMS; on a silent->loud edge we arm an ingest ONSET_SETTLE_MS later.
    // The delay matters: ingesting at the instant of onset frames mostly the preceding silence
    // (a ~10 s window with <1 s of music) and never recognizes — we wait for the window to fill
    // with the NEW song first. This also catches the next track after any gap between songs.
    static constexpr uint32_t ONSET_PROBE_MS  = 400;                          // RMS probe period while waiting
    static constexpr uint32_t ONSET_SETTLE_MS = (CAPTURE_SECONDS + 1) * 1000; // window-fill delay before the onset ingest
    static constexpr uint32_t ONSET_SAMPLES   = SAMPLE_RATE / 5;              // ~0.2 s probe window

    static constexpr const char *BOUNDARY = "----board1ingestBoundary7e3a";

    // --- b2-13 inter-board UART link (the direct Board 1 <-> Board 2 wire, NOT the cloud) ---
    // P4 JST on Board 2 is IO17=TX / IO18=RX, wired straight to Board 1's GPIO17/18 — so the
    // roles cross: Board 1 RX=17 (hears Board 2's TX), TX=18 (Board 2 hears it on its RX=18).
    // We emit a ~1 Hz HEARTBEAT (Board 2's "ear up" liveness, EAR_TIMEOUT_MS=3000 there) and,
    // on an inbound RECHECK, fire an immediate gate-bypassing ingest. Codec in uart_link.h.
    static constexpr int8_t   UART_RX_PIN   = 17;
    static constexpr int8_t   UART_TX_PIN   = 18;
    static constexpr uint32_t UART_BAUD     = 115200;
    static constexpr uint32_t HEARTBEAT_MS  = 1000;
    // Poll the cadence wait in small steps so a Board 2 RE-CHECK tap fires within ~this long
    // instead of waiting out the full CADENCE_MS.
    static constexpr uint32_t CADENCE_POLL_MS = 100;

    // --- b2-13 knobs: 3 bare EC11 encoders (the toast source for Board 2) ---
    // Pins + the quadrature ISR live at file scope above (npEnc / npEncIsr). Read inside the
    // UART task (it already owns Serial1, so one writer emits the KNOB_EVENT). value sent = the
    // encoder's accumulated position; flags bit0 = the push switch. Index order matches the
    // protocol knob id (0=mode,1=bright,2=gain).
    static constexpr uint32_t KNOB_POLL_MS = 20; // ~50 Hz: only DRAINS the ISR counter, so this
                                                 // rate sets responsiveness, never accuracy
    static constexpr uint8_t  KNOB_N     = 3;
    // b2-13 local apply: target units moved per detent (bench-tunable). The ISR emits one count
    // per physical detent (see NP_ENC_DETENT), so this scales a click onto the 0..255 ranges.
    static constexpr int16_t  BRIGHT_STEP = 6;   // bri units per detent
    static constexpr int16_t  GAIN_STEP   = 6;   // viz-punch level units per detent
    static constexpr float    GAIN_MIN    = 0.1f; // npVizGain floor: never fully zero the bars (a flat,
                                                  // frozen equalizer reads as "hung", not "low gain")
    // The per-knob switch-pin / protocol-id / name tables (index-aligned to the protocol knob id)
    // live as local statics in knobsBegin/pollKnobs — gnu++11 ODR-uses a static-constexpr array
    // member, which needs an out-of-line def a header-only usermod can't give. Scalars above are
    // fine (used only as constant exprs).

    int32_t  knobPos[KNOB_N]       = {0, 0, 0};   // last drained ISR position (signed, for delta)
    bool     knobPressed[KNOB_N]   = {false, false, false};

    // b2-13 local knob apply: the knobs now DRIVE the Board-1 visualizer, not just the toast.
    // bright + mode read LIVE WLED state at turn time (bri / currentPreset) rather than a mirror,
    // so a preset load or an external change can't leave a stale target that snaps on the next
    // click. gainTarget is the one bit of retained state (npVizGain is core-0-owned, nothing
    // else moves it); seeded from npVizGain in knobsBegin.
    int16_t  gainTarget = 128;   // viz-punch level 0..255 -> npVizGain = gainTarget/128 (128 = unity)
    uint8_t  modeIdx    = 0;     // index into presetSlots(); re-synced from currentPreset each turn
    // bright + preset apply must run in the WLED main loop (stateUpdated/applyPreset expect it),
    // but pollKnobs runs in the uartTask (core 0). Marshal via these volatiles, drained in loop()
    // on core 1 — single producer / single consumer, last-write-wins (fine for a knob). gain is an
    // atomic float write so it applies inline. -1 == nothing pending.
    volatile int16_t pendingBri    = -1;   // 0..255 brightness to apply
    volatile int16_t pendingPreset = -1;   // preset slot to apply
    bool             knobGrabbed   = false; // KNOB_GRAB: Board 2 owns the mode knob (uartTask-only state)

    // The knob's mode-cycle order: a plain ordered list of preset slots, a RUNTIME value read from
    // /npcycle.json (the Pi pushes it over UART via CYCLE_SET, the web app via readFromJsonState)
    // instead of a compiled constant — reorder looks with no reflash. Plain fixed-cap member
    // array, NOT std::vector / NOT static constexpr: a static-constexpr array member ODR-fails to
    // link in this header-only usermod under gnu++11 (same trap the knob ADDR/NAME tables dodge).
    // Single writer (core 1: loadCycle/readFromJsonState/builder cmd) + single reader (core 0:
    // pollKnobs); cycleCount is published LAST so a reader never sees a count that outruns the
    // slots (the same lock-free discipline as pendingPreset). Default = the factory boot preset.
    static constexpr uint8_t CYCLE_MAX = 40;         // the built-preset range 170..209
    uint8_t          cycleSlots[CYCLE_MAX] = {170};
    volatile uint8_t cycleCount            = 1;      // WRITE LAST

    // Builder commands (B2->B1 over UART, nowplaying-project/docs/builder.md §Wire). The uartTask
    // (core 0) parses the frame into `cmd` and raises cmdPending; loop() (core 1) runs it —
    // applyPreset/savePreset/deletePreset and the FS writes all want the WLED main loop — and
    // answers through ack*, which the uartTask turns into the ACK frame (Serial1 stays
    // single-writer). One command in flight: the Pi waits for the ACK before the next mutating
    // frame, so a second one arriving early is dropped and the Pi's retry picks it up.
    struct BuilderCmd { uint8_t type, slot, panel, strip, n; char name[33]; uint8_t slots[CYCLE_MAX]; };
    BuilderCmd       cmd;                     // written by uartTask before cmdPending, read by loop()
    volatile bool    cmdPending = false;      // uartTask -> loop()
    uint8_t          cmdStep    = 0;          // loop()-only: PRESET_SAVE step sequencing
    volatile bool    ackPending = false;      // loop() -> uartTask (published LAST)
    uint8_t          ackType = 0, ackArg = 0, ackStatus = 0;
    uint32_t         lastPiHeartbeat = 0;     // uartTask-only: KNOB_GRAB auto-release timer
    // Link diagnostics, published in /json/state as "npuart": the only way to see the B2->B1
    // direction from the desk (Board 1's console is not on USB; the Pi's TX was never proven).
    volatile uint32_t uartRxBytes = 0, uartRxFrames = 0;
    static constexpr uint32_t GRAB_TIMEOUT_MS = 10000;   // a Pi crash can't leave the mode knob dead

    static int16_t clampU8(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : (int16_t)v); }

    // The looks the mode knob cycles: default Mountain (boot/home) + the six Keybeat GIFs, but the
    // live order comes from cycleSlots/cycleCount (loaded from /npcycle.json). These preset slots
    // are uploaded to board flash by board1-config/provision-board1.sh (they live in flash, not the
    // repo — see appliance-reproducibility). cycleCount is clamped to >=1 so the pollKnobs wrap
    // math (modulo modeN) can never divide by zero even if a bad file somehow zeroed the count.
    const uint8_t *presetSlots(uint8_t &count) {
      count = cycleCount ? cycleCount : 1;
      return cycleSlots;
    }
    // Index of `slot` within presetSlots(), or -1 if it isn't one of ours.
    int presetIndexOf(uint8_t slot) {
      uint8_t n; const uint8_t *s = presetSlots(n);
      for (uint8_t i = 0; i < n; i++) if (s[i] == slot) return i;
      return -1;
    }

    // Validate + apply a slot list into the live cycle store. Rejects the WHOLE update on any bad
    // slot (out of 1..250) or an empty/oversize list, leaving the prior order intact — a malformed
    // write can never brick the knob. Publishes cycleCount LAST.
    bool applyCycle(const uint8_t *slots, uint8_t n) {
      if (n < 1 || n > CYCLE_MAX) return false;
      for (uint8_t i = 0; i < n; i++) if (slots[i] < 1 || slots[i] > 250) return false;
      memcpy(cycleSlots, slots, n);
      cycleCount = n;                              // publish LAST
      return true;
    }
    // {"cycle":[...]} (the /npcycle.json + web shape) -> applyCycle.
    bool applyCycleDoc(JsonObjectConst o) {
      if (o.isNull()) return false;
      JsonArrayConst arr = o["cycle"];
      if (arr.isNull()) return false;
      uint8_t tmp[CYCLE_MAX], n = 0;
      for (JsonVariantConst v : arr) {
        if (n >= CYCLE_MAX) return false;
        int s = v | -1;
        if (s < 1 || s > 255) return false;
        tmp[n++] = (uint8_t)s;
      }
      return applyCycle(tmp, n);
    }

    // Persist the live cycle store to /npcycle.json on LittleFS (so it survives reboot).
    void persistCycle() {
      File f = WLED_FS.open("/npcycle.json", "w");
      if (!f) return;
      StaticJsonDocument<512> doc;
      JsonArray arr = doc.createNestedArray("cycle");
      for (uint8_t i = 0; i < cycleCount; i++) arr.add(cycleSlots[i]);
      serializeJson(doc, f);
      f.close();
    }

    // Load the cycle order from /npcycle.json at boot. Missing/invalid file => keep the compiled
    // default (the real safety net). Runs on core 1 in setup() before the knob task starts.
    void loadCycle() {
      File f = WLED_FS.open("/npcycle.json", "r");
      if (!f) return;
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (err) return;
      if (applyCycleDoc(doc.as<JsonObjectConst>()))
        USER_PRINTF("[cycle] loaded %u slot(s) from /npcycle.json\n", (unsigned)cycleCount);
    }

    // one PSRAM buffer holding the whole multipart body: [preamble][44B WAV][PCM][epilogue]
    uint8_t *body = nullptr;
    size_t   preambleLen = 0, epilogueLen = 0, headerOff = 0, pcmOff = 0, bodyTotal = 0;

    TaskHandle_t ingestTask = nullptr;
    volatile bool ready = false;   // body allocated + tap expected; task may run

    // b2-13: the UART task owns Serial1; it sets recheckRequest on an inbound RECHECK and the
    // ingest task consumes it. One volatile bool, single writer + single reader — no lock
    // (same loose hand-off as Board 2's g_recheck_request).
    TaskHandle_t  uartTask       = nullptr;
    volatile bool recheckRequest = false;
    // silence->music edge tracker for the onset trigger (ingest-task-local, no cross-task access)
    bool roomWasSilent = true;

    // canonical 44-byte WAV header (RIFF/WAVE/fmt /data, no extra chunks) — byte-exact
    static void writeWavHeader(uint8_t *p, uint32_t pcmLen) {
      const uint32_t sr = SAMPLE_RATE, ch = 1, bits = 16;
      const uint32_t byteRate = sr * ch * bits / 8;
      const uint16_t blockAlign = ch * bits / 8;
      auto u32 = [&](size_t o, uint32_t v) { p[o]=v; p[o+1]=v>>8; p[o+2]=v>>16; p[o+3]=v>>24; };
      auto u16 = [&](size_t o, uint16_t v) { p[o]=v; p[o+1]=v>>8; };
      memcpy(p,    "RIFF", 4); u32(4, 36 + pcmLen);
      memcpy(p+8,  "WAVE", 4);
      memcpy(p+12, "fmt ", 4); u32(16, 16); u16(20, 1); u16(22, ch);
      u32(24, sr); u32(28, byteRate); u16(32, blockAlign); u16(34, bits);
      memcpy(p+36, "data", 4); u32(40, pcmLen);
    }

    bool allocBody() {
      char preamble[256];
      preambleLen = snprintf(preamble, sizeof(preamble),
          "--%s\r\n"
          "Content-Disposition: form-data; name=\"file\"; filename=\"board1.wav\"\r\n"
          "Content-Type: audio/x-wav\r\n\r\n",
          BOUNDARY);
      char epilogue[64];
      epilogueLen = snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n", BOUNDARY);

      headerOff = preambleLen;
      pcmOff    = preambleLen + 44;
      bodyTotal = pcmOff + PCM_BYTES + epilogueLen;

      body = (uint8_t *)heap_caps_malloc(bodyTotal, MALLOC_CAP_SPIRAM);
      if (!body) body = (uint8_t *)malloc(bodyTotal);   // DRAM fallback (unlikely to fit)
      if (!body) { USER_PRINTLN(F("[ingest] OOM: multipart body")); return false; }

      memcpy(body, preamble, preambleLen);
      memcpy(body + pcmOff + PCM_BYTES, epilogue, epilogueLen);
      return true;
    }

    // Copy the most recent CAPTURE_SAMPLES out of the tap ring into the WAV PCM region and
    // return the window RMS (for the silence gate). Returns -1 if the tap isn't ready or
    // there isn't a full window of audio yet.
    double captureWindow() {
      if (!npPcmRing) return -1.0;
      uint32_t total = npPcmWritten;                 // volatile snapshot
      if (total < CAPTURE_SAMPLES) return -1.0;      // not enough audio buffered yet

      int16_t *out = (int16_t *)(body + pcmOff);
      uint32_t startIdx = (total - CAPTURE_SAMPLES) % npPcmRingLen;
      uint32_t first = npPcmRingLen - startIdx;
      if (first > CAPTURE_SAMPLES) first = CAPTURE_SAMPLES;
      memcpy(out, npPcmRing + startIdx, (size_t)first * 2);
      if (first < CAPTURE_SAMPLES)
        memcpy(out + first, npPcmRing, (size_t)(CAPTURE_SAMPLES - first) * 2);

      double sumSq = 0;
      for (uint32_t i = 0; i < CAPTURE_SAMPLES; i++) {
        sumSq += (double)out[i] * (double)out[i];
        if ((i & 0xFFFF) == 0) vTaskDelay(1);   // yield periodically (core-0 IDLE / watchdog)
      }
      return sqrt(sumSq / CAPTURE_SAMPLES);
    }

    // Cheap RMS over just the last ONSET_SAMPLES of the tap ring — light enough to call every
    // poll while waiting (captureWindow() copies the whole 10 s window + writes `body`, far too
    // heavy for that). A loose read of a ring the writer may be touching: fine for a gate estimate.
    double quickRms() {
      if (!npPcmRing) return -1.0;
      uint32_t total = npPcmWritten;                 // volatile snapshot
      if (total < ONSET_SAMPLES) return -1.0;
      uint32_t startIdx = (total - ONSET_SAMPLES) % npPcmRingLen;
      double sumSq = 0;
      for (uint32_t i = 0; i < ONSET_SAMPLES; i++) {
        int16_t s = npPcmRing[(startIdx + i) % npPcmRingLen];
        sumSq += (double)s * (double)s;
      }
      return sqrt(sumSq / ONSET_SAMPLES);
    }

    // Multipart POST over a raw WiFiClient — WLED's build has no Arduino HTTPClient. The
    // whole body (preamble + WAV header + PCM + epilogue) is already assembled in `body`;
    // we just write the request line + headers + body, then read the status + JSON.
    void postIngest() {
      writeWavHeader(body + headerOff, PCM_BYTES);

      WiFiClient client;
      client.setTimeout(4000);   // bound connect/socket blocks under the 5 s task watchdog
      if (!client.connect(NP_HOST, NP_PORT)) {
        USER_PRINTF("[ingest] connect %s:%d failed\n", NP_HOST, (int)NP_PORT);
        return;
      }

      // request line + headers
      client.printf("POST %s HTTP/1.1\r\n", NP_INGEST_PATH);
      client.printf("Host: %s:%d\r\n", NP_HOST, (int)NP_PORT);
      client.printf("Authorization: Bearer %s\r\n", NP_DEVICE_TOKEN);
      client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", BOUNDARY);
      client.printf("Content-Length: %u\r\n", (unsigned)bodyTotal);
      client.print(F("Connection: close\r\n\r\n"));

      // body, chunked with a yield every chunk. The ~441 KB upload takes seconds; this task
      // is pinned to core 0 above the IDLE task, so without yielding it starves core-0 IDLE
      // and the task watchdog reboots the board mid-POST. vTaskDelay() lets IDLE run.
      size_t sent = 0;
      while (sent < bodyTotal && client.connected()) {
        size_t n = bodyTotal - sent;
        if (n > 1460) n = 1460;
        sent += client.write(body + sent, n);
        vTaskDelay(1);   // feed core-0 IDLE / the task watchdog during the long upload
      }

      // response: read ONLY already-buffered bytes (returns immediately) and vTaskDelay
      // between polls. We must NOT use readStringUntil/timedRead here: those block inside
      // the Stream for up to the stream timeout, spinning on read()+yield(), and yield()
      // never drops to the priority-0 IDLE task — so a slow response (the cloud runs songrec
      // synchronously, several seconds) starves core-0 IDLE and the task watchdog reboots us.
      // The reply (status line + headers + small JSON) fits easily in this buffer.
      char resp[768];
      size_t rlen = 0;
      uint32_t t0 = millis();
      while (millis() - t0 < 15000) {
        int avail = client.available();
        if (avail > 0) {
          size_t room = sizeof(resp) - 1 - rlen;
          if (room > 0) {
            int n = client.read((uint8_t *)resp + rlen, avail < (int)room ? avail : (int)room);
            if (n > 0) { rlen += n; t0 = millis(); }   // extend budget while bytes flow
          } else {
            client.read();   // buffer full (have status+JSON already) — drain & discard
          }
        } else if (!client.connected()) {
          break;             // server closed and nothing buffered => done
        }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      client.stop();
      resp[rlen] = '\0';

      // status line = up to the first CR/LF; JSON body = after the blank line
      char *eol = strpbrk(resp, "\r\n");
      if (eol) *eol = '\0';
      const char *body = strstr(rlen ? (eol ? eol + 1 : resp) : "", "\r\n\r\n");
      body = body ? body + 4 : "";
      USER_PRINTF("[ingest] POST %s -> %s | %s\n", NP_INGEST_PATH, resp, body);
    }

    enum Trigger { TRIG_CADENCE, TRIG_RECHECK, TRIG_ONSET };

    // Wait until something says "ingest now": a Board 2 RE-CHECK, a silence->music ONSET (the
    // room was quiet and music just came up — armed on the edge, fired ONSET_SETTLE_MS later once
    // the capture window has filled with the new song), or the CADENCE_MS safety deadline. Polls
    // in small steps so it reacts fast and keeps core-0 IDLE fed (the watchdog trap that bit it).
    Trigger waitForTrigger() {
      uint32_t waited = 0, sinceProbe = 0;
      int32_t  onsetAt = -1;   // ms when a silence->music edge armed; ingest ONSET_SETTLE_MS later
      while (waited < CADENCE_MS) {
        if (recheckRequest) { recheckRequest = false; return TRIG_RECHECK; }
        if (onsetAt >= 0 && (int32_t)waited - onsetAt >= (int32_t)ONSET_SETTLE_MS) return TRIG_ONSET;
        vTaskDelay(pdMS_TO_TICKS(CADENCE_POLL_MS));
        waited += CADENCE_POLL_MS; sinceProbe += CADENCE_POLL_MS;
        if (sinceProbe >= ONSET_PROBE_MS) {
          sinceProbe = 0;
          double rms = quickRms();
          if (rms >= 0) {
            bool loud = rms >= RMS_GATE;
            if (loud && roomWasSilent && onsetAt < 0) onsetAt = (int32_t)waited;   // arm the onset ingest
            roomWasSilent = !loud;
          }
        }
      }
      return TRIG_CADENCE;
    }

    void run() {
      for (;;) {
        Trigger trig = waitForTrigger();
        bool forced = (trig == TRIG_RECHECK);   // a Board 2 RE-CHECK tap: bypass the gate
        if (!ready) continue;
        if (!(WLED_CONNECTED)) {
          USER_PRINTLN(forced ? F("[ingest] RECHECK but wifi down — skip")
                              : F("[ingest] wifi down — skip"));
          continue;
        }

        double rms = captureWindow();
        if (rms < 0) { USER_PRINTLN(F("[ingest] tap not ready / window short — skip")); continue; }
        // A RE-CHECK is an explicit "listen now" from the user, so it bypasses the silence gate.
        // CADENCE and ONSET still honour it to avoid POSTing a quiet room.
        if (!forced && rms < RMS_GATE) {
          USER_PRINTF("[ingest] room below gate (RMS %.0f < %.0f) — skip\n", rms, RMS_GATE);
          continue;
        }
        const char *why = forced ? "RECHECK (gate bypassed)"
                        : (trig == TRIG_ONSET ? "ONSET (music started)" : "cadence");
        USER_PRINTF("[ingest] %s  RMS %.0f  -> POST\n", why, rms);
        postIngest();
      }
    }

    static void taskTrampoline(void *arg) { static_cast<RoomMicUploader *>(arg)->run(); }

    // b2-13 knobs: arm the EC11 pins + their quadrature ISRs. Unlike the Seesaw version there
    // is nothing to probe for — a disconnected encoder simply never emits. Called once from the
    // UART task, which is also the only place the counters are drained.
    void knobsBegin() {
      static const int8_t       SW[KNOB_N]   = {-1, -1, -1};  // GPIO 2 is the LED strip now (see pollKnobs)
      static const char * const NAME[KNOB_N] = {"mode", "bright", "gain"};
      for (uint8_t i = 0; i < KNOB_N; i++) {
        NpEncoder &e = npEnc[i];
        if (e.pinA < 0 || e.pinB < 0) continue;   // -1 = encoder not wired (same convention as SW)
        pinMode(e.pinA, INPUT_PULLUP);   // EC11s carry no pull-ups; internal is fine over short
        pinMode(e.pinB, INPUT_PULLUP);   // wires. Enclosure-length runs want the carrier's 10k+100nF.
        e.prev = (uint8_t)((digitalRead(e.pinA) << 1) | digitalRead(e.pinB));
        e.pos  = 0;
        e.sub  = 0;
        // Pass the GPIO number straight in — do NOT wrap in digitalPinToInterrupt(). That macro
        // bounds on the variant's NUM_DIGITAL_PINS, which the esp32s3 variant sets to 48 even
        // though the S3 has 49 GPIOs (0..48). So GPIO 48 (mode knob's B) maps to NOT_AN_INTERRUPT
        // (-1), arrives as uint8_t 255, and indexes off the GPIO register block -> LoadProhibited
        // boot loop. On ESP32 the pin number IS the interrupt number, and the core's
        // __pinInterruptHandlers[] is sized SOC_GPIO_PIN_COUNT (49), so 48 is in range.
        attachInterruptArg(e.pinA, npEncIsr, &e, CHANGE);
        attachInterruptArg(e.pinB, npEncIsr, &e, CHANGE);
        if (SW[i] >= 0) pinMode(SW[i], INPUT_PULLUP);
        knobPos[i]     = 0;
        knobPressed[i] = false;
        USER_PRINTF("[knob] %s: EC11 A=IO%d B=IO%d sw=IO%d\n",
                    NAME[i], (int)e.pinA, (int)e.pinB, (int)SW[i]);
      }
      USER_PRINTF("[knob] %u EC11 encoder(s) armed, %d transitions/detent\n",
                  (unsigned)KNOB_N, (int)NP_ENC_DETENT);
      // Seed retained gain from npVizGain, and modeIdx from whatever preset actually booted
      // (not a hardcoded 0) so the first mode detent steps from the live look. bright reads
      // live `bri` at turn time, so it needs no seed.
      gainTarget = (int16_t)(npVizGain * 128.0f + 0.5f); // viz-punch unity (npVizGain 1.0 -> 128)
      int idx    = presetIndexOf(currentPreset);
      modeIdx    = (idx >= 0) ? (uint8_t)idx : 0;         // fall back to home if not one of ours
    }

    // Read every present encoder, APPLY it to the Board-1 visualizer locally, and emit a
    // KNOB_EVENT to Board 2 whose value is the real applied level (brightness / gain / preset
    // id). buf is the UART task's frame scratch. Runs in the uartTask (core 0); bright + preset
    // are marshalled to loop() (core 1) via pendingBri/pendingPreset, gain applies inline.
    void pollKnobs(uint8_t *buf, size_t cap) {
      static const uint8_t      ID[KNOB_N]   = {0, 1, 2};   // KNOB_EVENT knob u8
      static const int8_t       SW[KNOB_N]   = {-1, -1, -1}; // no pushes: GPIO 2 drives the glow strip
      static const char * const NAME[KNOB_N] = {"mode", "bright", "gain"};
      uint8_t presetN; const uint8_t *slots = presetSlots(presetN);

      for (uint8_t i = 0; i < KNOB_N; i++) {
        int32_t pos = npEnc[i].pos;      // drained from the ISR; one count per detent
        if (pos != knobPos[i]) {
          int32_t delta = pos - knobPos[i];
          knobPos[i] = pos;
          int16_t level;
          if (i == 0) {                  // mode: step one look per detent, wrapping
            if (knobGrabbed) {
              // Builder grab (KNOB_GRAB): Board 2 scrolls its picker with this knob, so skip the
              // local apply and stream the raw detent count — Board 2 diffs consecutive values.
              level = (int16_t)pos;
            } else {
              // Re-sync to the live preset first so an external change can't desync us.
              int cur = presetIndexOf(currentPreset); if (cur >= 0) modeIdx = (uint8_t)cur;
              modeIdx = (uint8_t)((modeIdx + presetN + (delta > 0 ? 1 : -1)) % presetN);
              lyricsActive  = false;                      // a turn always leaves the (web-only) lyrics mode
              pendingPreset = slots[modeIdx];             // applyPreset() runs in loop()
              level = modeIdx;                            // toast: 0-based mode number (not the raw slot id)
            }
          } else if (i == 1) {           // bright -> WLED brightness, nudged from LIVE bri (no stale mirror)
            int16_t newBri = clampU8(bri + delta * BRIGHT_STEP);
            pendingBri = newBri;                          // bri + stateUpdated() run in loop()
            level = newBri;
          } else {                       // gain -> LFX viz-punch (npVizGain); inputLevel is dead on the LFX_DSP path
            gainTarget = clampU8(gainTarget + delta * GAIN_STEP);
            float g = (float)gainTarget / 128.0f;         // 128 = unity, 255 ~= 2x punch
            npVizGain = g < GAIN_MIN ? GAIN_MIN : g;      // floored so the bars never freeze flat
            level = gainTarget;
          }
          size_t n = uart_make_knob(ID[i], level, 0x00, buf, cap);
          Serial1.write(buf, n);
          USER_PRINTF("[knob] %s turn level=%d (%+ld) -> apply + KNOB_EVENT\n",
                      NAME[i], (int)level, (long)delta);
        }
        // Only `mode` has a switch line today. All three EC11 switches are meant to share GPIO 2
        // via the carrier's resistor ladder (0R / 3.3k / 10k) — enabling bright+gain later is
        // then two threshold comparisons here, no board respin. See carrier-board-flux-prompt.md.
        // 2026-09-25: the pushes are given up. GPIO 2 (SW_LADDER) now drives a WS281x glow strip
        // as a second WLED bus (provision-board1.sh); the knob-board end of the brown wire is
        // pulled and carrier C7 removed. Keep SW all -1: knobsBegin runs after WLED's bus init,
        // so a pinMode() here would take the strip's data pin back as an input.
        if (SW[i] < 0) continue;
        bool pressed = !digitalRead(SW[i]);              // INPUT_PULLUP: pressed == LOW
        if (pressed && !knobPressed[i]) {
          int16_t level;
          if (i == 0) {                  // mode press = jump home (Mountain; clock face not ported yet)
            modeIdx = 0;
            lyricsActive = false;        // leave lyrics mode on press-home
            pendingPreset = slots[0];
            level = modeIdx;
          } else {                       // bright/gain press = no local action, just the toast cue
            level = (i == 1) ? bri : gainTarget;
          }
          size_t n = uart_make_knob(ID[i], level, 0x01, buf, cap);
          Serial1.write(buf, n);
          USER_PRINTF("[knob] %s PRESS -> KNOB_EVENT\n", NAME[i]);
        }
        knobPressed[i] = pressed;
      }
    }

    // KNOB_GRAB (uartTask-only). While grabbed, pollKnobs streams the mode knob instead of applying
    // it. Release — explicit, or GRAB_TIMEOUT_MS without a Pi heartbeat — re-applies the current
    // cycle preset so a PREVIEW leaves no trace.
    void setGrab(bool on) {
      if (on) { knobGrabbed = true; lastPiHeartbeat = millis(); USER_PRINTLN(F("[builder] knob grabbed")); return; }
      if (!knobGrabbed) return;
      knobGrabbed = false;
      uint8_t n; const uint8_t *slots = presetSlots(n);
      if (modeIdx >= n) modeIdx = 0;                 // the cycle may have shrunk meanwhile
      pendingPreset = slots[modeIdx];
      USER_PRINTLN(F("[builder] knob released -> cycle preset re-applied"));
    }

    // Parse a mutating builder frame into `cmd` for loop(). Malformed frames (bad length, slot
    // outside 1..250, count outside 1..CYCLE_MAX) are rejected right here with an ACK — nothing
    // to sequence. `buf` is the uartTask's frame scratch (we are on that task).
    void queueBuilderCmd(const UartDecoder &d, uint8_t *buf, size_t cap) {
      if (cmdPending) { USER_PRINTLN(F("[builder] busy, frame dropped")); return; }
      auto okSlot = [](uint8_t s) { return s >= 1 && s <= 250; };
      bool ok = false;
      cmd.type = d.type;
      if (d.type == UART_PRESET_SAVE) {
        ok = uart_parse_preset_save(d, &cmd.slot, &cmd.panel, &cmd.strip, cmd.name, sizeof cmd.name)
             && okSlot(cmd.slot) && okSlot(cmd.panel) && okSlot(cmd.strip);
      } else if (d.type == UART_PRESET_DELETE) {
        cmd.slot = d.payload[0];
        ok = d.len == 1 && okSlot(cmd.slot);
      } else if (d.type == UART_CYCLE_SET) {
        const uint8_t *s;
        ok = uart_parse_cycle_set(d, &s, &cmd.n) && cmd.n >= 1 && cmd.n <= CYCLE_MAX;
        if (ok) { memcpy(cmd.slots, s, cmd.n); for (uint8_t i = 0; i < cmd.n; i++) ok = ok && okSlot(s[i]); }
        cmd.slot = cmd.n;                              // ACK arg = n for CYCLE_SET
      }
      if (!ok) {
        size_t n = uart_make_ack(d.type, cmd.slot, UART_ACK_REJECTED, buf, cap);
        Serial1.write(buf, n);
        USER_PRINTF("[builder] rejected frame 0x%02X (len %u)\n", d.type, (unsigned)d.len);
        return;
      }
      cmdStep = 0;
      cmdPending = true;                               // publish LAST
    }

    // Run the queued builder command on the WLED main loop (core 1). PRESET_SAVE is a 3-step
    // sequence because applyPreset()/savePreset() are asynchronous — handlePresets() consumes one
    // request per loop iteration — so: apply panel -> wait -> apply strip -> wait -> savePreset ->
    // wait for the flash write -> ACK. A missing library slot (ERR_FS_PLOAD) rejects the save
    // rather than baking whatever was live into the preset.
    void runBuilderCmd() {
      if (!cmdPending || ackPending || presetsActionPending()) return;
      uint8_t status = UART_ACK_OK;
      if (cmd.type == UART_PRESET_SAVE) {
        bool loadFailed = (errorFlag == ERR_FS_PLOAD);
        if (cmdStep == 0) { if (loadFailed) errorFlag = ERR_NONE; applyPreset(cmd.panel, CALL_MODE_DIRECT_CHANGE); cmdStep = 1; return; }
        if (cmdStep == 1 && !loadFailed) { applyPreset(cmd.strip, CALL_MODE_DIRECT_CHANGE); cmdStep = 2; return; }
        if (cmdStep == 2 && !loadFailed) { savePreset(cmd.slot, cmd.name); cmdStep = 3; return; }
        if (loadFailed) status = UART_ACK_REJECTED;
        else currentPreset = cmd.slot;                 // the live look IS this preset now (knob re-sync)
        USER_PRINTF("[builder] save %u = panel %u + strip %u \"%s\" -> %s\n",
                    cmd.slot, cmd.panel, cmd.strip, cmd.name, status ? "REJECTED (slot missing)" : "ok");
      } else if (cmd.type == UART_PRESET_DELETE) {
        deletePreset(cmd.slot);
        USER_PRINTF("[builder] delete %u\n", cmd.slot);
      } else if (cmd.type == UART_CYCLE_SET) {
        if (applyCycle(cmd.slots, cmd.n)) {
          persistCycle();
          int idx = presetIndexOf(currentPreset);      // re-anchor the knob index to the live look
          if (idx >= 0) modeIdx = (uint8_t)idx;
          USER_PRINTF("[builder] cycle set: %u slot(s)\n", (unsigned)cycleCount);
        } else status = UART_ACK_REJECTED;
      }
      ackType = cmd.type; ackArg = cmd.slot; ackStatus = status;
      cmdPending = false;
      ackPending = true;                               // publish LAST
    }

    // b2-13 UART task: owns Serial1, emits the ~1 Hz HEARTBEAT and decodes inbound frames.
    // Lives on its OWN task (not the ingest task) so the heartbeat keeps flowing during the
    // multi-second blocking POST — otherwise Board 2 would see "ear down" on every ingest,
    // exactly when the ear is busiest. Non-blocking reads + tiny writes + vTaskDelay only.
    void runUart() {
      Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
      USER_PRINTF("[uart] link up: RX=IO%d TX=IO%d @ %u, heartbeat %u ms\n",
                  (int)UART_RX_PIN, (int)UART_TX_PIN, (unsigned)UART_BAUD, (unsigned)HEARTBEAT_MS);
      knobsBegin();   // b2-13: I2C encoders share this task so Serial1 stays single-writer
      UartDecoder dec;
      uint8_t buf[UART_MAX_FRAME];
      uint32_t last_hb = 0, last_knob = 0;
      for (;;) {
        while (Serial1.available()) {
          uartRxBytes++;
          if (dec.feed((uint8_t)Serial1.read())) {
            uartRxFrames++;
            if (dec.type == UART_RECHECK) {
              recheckRequest = true;
              USER_PRINTLN(F("[uart] RECHECK in <- Board 2 (forcing ingest)"));
            } else if (dec.type == UART_KNOB_EVENT) {
              uint8_t k, fl; int16_t v;
              if (uart_parse_knob(dec, &k, &v, &fl))
                USER_PRINTF("[uart] knob in: id=%d value=%d flags=0x%02X (forwarded, no-op here)\n", k, v, fl);
            } else if (dec.type == UART_HEARTBEAT) {
              lastPiHeartbeat = millis();              // feeds the KNOB_GRAB auto-release
            } else if (dec.type == UART_KNOB_GRAB && dec.len == 1) {
              setGrab(dec.payload[0] & 1);
            } else if (dec.type == UART_PREVIEW && dec.len == 2) {
              // Library presets are single-segment, so applyPreset() touches only that segment;
              // the seg byte is informational. No ACK.
              uint8_t slot = dec.payload[1];
              if (slot >= 1 && slot <= 250) pendingPreset = slot;
            } else if (dec.type == UART_PRESET_SAVE || dec.type == UART_PRESET_DELETE || dec.type == UART_CYCLE_SET) {
              queueBuilderCmd(dec, buf, sizeof buf);
            }
          }
        }
        uint32_t now = millis();
        if (ackPending) {
          size_t n = uart_make_ack(ackType, ackArg, ackStatus, buf, sizeof buf);
          Serial1.write(buf, n);
          ackPending = false;
        }
        if (knobGrabbed && now - lastPiHeartbeat > GRAB_TIMEOUT_MS) setGrab(false);
        if (now - last_knob >= KNOB_POLL_MS) {
          last_knob = now;
          pollKnobs(buf, sizeof buf);   // emits KNOB_EVENT on any turn/press
        }
        if (now - last_hb >= HEARTBEAT_MS) {
          last_hb = now;
          size_t n = uart_make_heartbeat(now / 1000, buf, sizeof buf);
          Serial1.write(buf, n);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }

    static void uartTrampoline(void *arg) { static_cast<RoomMicUploader *>(arg)->runUart(); }

    // ======================== Lyrics mode data path (P2) ========================
    // When the mode knob selects the LYRICS entry, this task polls GET /nowplaying,
    // fetches GET /lyrics?track_id= once per track, and keeps a re-anchored play
    // position so currentWord() (read per-frame by the P3 overlay draw on core 1)
    // can pick the word to flash. The timeline is double-buffered: the task builds
    // into the idle slot and publishes by flipping `liveSlot` (a single volatile
    // write), so a core-1 reader never iterates a vector mid-rebuild. Rebuilds only
    // happen on a track change (seconds apart) while a draw frame is microseconds —
    // two rebuilds would have to land inside one frame to race, which can't happen.
    static constexpr const char *NP_NOWPLAYING_PATH = "/api/nowplaying";
    static constexpr uint32_t LYRICS_POLL_MS = 2000;   // re-anchor cadence; words extrapolate between
    static constexpr size_t   LYRICS_NETCAP  = 16384;  // ponytail: response scratch ceiling; bump if a
                                                       // song's yrc exceeds ~16 KB (then the body truncates)
    // Timing lead (ms) added to the anchored position so words show ahead of where the raw
    // anchor would put them. Half the measured GET round-trip is added automatically (the
    // server computes position_s mid-round-trip; the board anchors at receipt → lag). This is
    // the BENCH CALIBRATION KNOB for the residual systematic offset (recognition-pipeline
    // position vs true playback): +ve = words earlier. Tune on hardware to taste.
    static constexpr int32_t  LYRICS_LEAD_MS = 0;

    TaskHandle_t  lyricsTask   = nullptr;
    volatile bool lyricsActive = false;   // set by the mode knob (uartTask); read here + by the draw

    char *netBuf = nullptr;               // PSRAM HTTP response scratch (reused per GET)

    // re-anchor model: playPos at the moment `anchorAt` millis() was taken. The estimate
    // is anchorPos + (millis()-anchorAt)/1000 — corrected to the server value every poll.
    volatile double   anchorPos = 0.0;
    volatile uint32_t anchorAt  = 0;
    volatile bool     haveAnchor = false;
    int               anchorRejects = 0;   // consecutive rejected re-anchors (see accept_anchor)

    std::string curTrack;                 // track_id the timeline was built for ("" = none)
    double      curDur     = -1.0;
    bool        lyricsFetched = false;    // fetched /lyrics for curTrack already (don't re-hammer)

    nplyrics::Timeline   slot[2];
    std::vector<double>  slotStarts[2];
    volatile int         liveSlot = -1;   // -1 = no timeline; else index into slot[]

    // --- play-clock anchor acceptance (was np_show.h; stage shows are gone, lyrics keep it) ---
    // Backwards re-anchors in the jitter band are IGNORED and the board keeps extrapolating its
    // own clock. Measured on the bench (2026-08-07): position_s is accurate to about +3 s right
    // after a recognition lands, but successive anchors can disagree by 17-25 s because the
    // cloud adds a FIXED 13 s recognition_lag_s fudge. Accepting those blindly drags lyrics
    // backwards mid-line. Forward jumps are always accepted; beyond ANCHOR_SEEK_S the move is a
    // real seek / track change. A disagreement that PERSISTS is not jitter: tolerate one odd
    // poll and believe the second (anchorRejects).
    static constexpr uint32_t ANCHOR_STALE_MS   = 30000;   // no re-anchor this long => clock is fiction
    static constexpr double   ANCHOR_JITTER_S   = 3.0;
    static constexpr double   ANCHOR_SEEK_S     = 30.0;
    static constexpr int      ANCHOR_MAX_REJECTS = 2;
    static bool anchorStale(uint32_t nowMs, uint32_t anchorAtMs, bool haveAnchor) {
      return !haveAnchor || (uint32_t)(nowMs - anchorAtMs) > ANCHOR_STALE_MS;
    }
    static bool acceptAnchor(double newPos, double curPos, bool haveCur, int &rejects) {
      if (!haveCur) { rejects = 0; return true; }
      double back = curPos - newPos;                   // >0 means the anchor moved backwards
      if (back <= ANCHOR_JITTER_S || back >= ANCHOR_SEEK_S) { rejects = 0; return true; }
      if (++rejects >= ANCHOR_MAX_REJECTS) { rejects = 0; return true; }
      return false;
    }

    static std::string urlencode(const std::string &s) {
      static const char *hex = "0123456789ABCDEF";
      std::string o;
      for (unsigned char c : s) {
        if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') o += (char)c;
        else { o += '%'; o += hex[c>>4]; o += hex[c&15]; }
      }
      return o;
    }

    // GET `path` with the shared bearer token into netBuf; return a pointer to the
    // (null-terminated, writable) response body for zero-copy JSON parse, or nullptr.
    // Same non-blocking read discipline as postIngest() so it can't starve core-0 IDLE.
    char *httpGet(const char *path) {
      if (!netBuf) return nullptr;
      WiFiClient client;
      client.setTimeout(4000);
      if (!client.connect(NP_HOST, NP_PORT)) { USER_PRINTF("[lyrics] connect %s:%d failed\n", NP_HOST, (int)NP_PORT); return nullptr; }
      client.printf("GET %s HTTP/1.1\r\n", path);
      client.printf("Host: %s:%d\r\n", NP_HOST, (int)NP_PORT);
      client.printf("Authorization: Bearer %s\r\n", NP_DEVICE_TOKEN);
      client.print(F("Connection: close\r\n\r\n"));

      // Read until the server closes AND the buffer is drained. The cloud sends
      // `Connection: close` with NO Content-Length, so the body is delimited only by
      // EOF — and /nowplaying (with the recent[] mosaic) spans several TCP segments.
      // Looping while (connected || available) avoids breaking in a mid-transfer gap
      // where available()==0 but more data is still in flight (that truncated the JSON
      // → IncompleteInput). t0 resets on every byte, so the 8 s cap only trips on a
      // genuinely stalled server (single-threaded artisan busy with an ingest).
      size_t rlen = 0;
      uint32_t t0 = millis();
      while ((client.connected() || client.available()) && millis() - t0 < 8000) {
        int avail = client.available();
        if (avail > 0) {
          size_t room = LYRICS_NETCAP - 1 - rlen;
          if (room == 0) { client.read(); t0 = millis(); }  // body > netCap: drain & discard (truncated)
          else {
            int n = client.read((uint8_t*)netBuf + rlen, avail < (int)room ? avail : (int)room);
            if (n > 0) { rlen += n; t0 = millis(); }
          }
        } else {
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      }
      client.stop();
      netBuf[rlen] = '\0';

      char *hdrEnd = strstr(netBuf, "\r\n\r\n");
      if (!hdrEnd) return nullptr;
      *hdrEnd = '\0';
      if (!strstr(netBuf, " 200 ")) {                       // non-200: log status line, skip
        USER_PRINTF("[lyrics] GET %s -> %s\n", path, netBuf);
        return nullptr;
      }
      return hdrEnd + 4;
    }

    // Build into the idle slot, then publish by flipping liveSlot (single write).
    void publishTimeline(nplyrics::Timeline tl) {
      int next = (liveSlot == 0) ? 1 : 0;
      slot[next] = std::move(tl);
      slotStarts[next] = nplyrics::starts_of(slot[next]);
      liveSlot = next;
    }

    void resetLyrics() {
      curTrack.clear(); curDur = -1.0; lyricsFetched = false;
      haveAnchor = false; liveSlot = -1;
    }

    void fetchLyrics(const std::string &trackId, double dur) {
      std::string path = "/api/lyrics?track_id=" + urlencode(trackId);
      char *body = httpGet(path.c_str());
      if (!body) return;
      // zero-copy parse: the lyrics string references netBuf, valid until the next GET.
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, body)) return;
      if (doc["pending"] | false) return;                   // worker still in flight → retry next poll
      lyricsFetched = true;                                  // present row (even null lyrics) → stop asking
      const char *lyr = doc["lyrics"] | "";
      if (!lyr || !*lyr) { publishTimeline(nplyrics::Timeline()); return; }   // track has no lyrics
      publishTimeline(nplyrics::build_timeline(lyr, dur > 0 ? dur : -1.0));
      USER_PRINTF("[lyrics] built timeline: %u words for track %s\n",
                  (unsigned)slot[liveSlot].size(), trackId.c_str());
    }

    void pollNowPlaying() {
      uint32_t tReq = millis();
      char *body = httpGet(NP_NOWPLAYING_PATH);
      uint32_t rtt = millis() - tReq;            // GET round-trip, for latency compensation
      if (!body) return;
      // /nowplaying's trailing recent[] mosaic is ~16 KB (63 tiles of long cover URLs) and
      // overflows netBuf → truncated JSON. Every field we need precedes it, so lop recent[]
      // off and re-close the object: a small, valid prefix that's robust to the mosaic growing.
      char *rec = strstr(body, ",\"recent\"");
      if (rec) { rec[0] = '}'; rec[1] = '\0'; }
      StaticJsonDocument<192> filter;
      filter["state"] = true; filter["track_id"] = true; filter["position_s"] = true;
      filter["duration_s"] = true; filter["lyrics_available"] = true;
      StaticJsonDocument<320> doc;
      if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;

      bool playing = strcmp(doc["state"] | "", "playing") == 0;
      std::string trackId = (const char*)(doc["track_id"] | "");
      if (!playing || trackId.empty()) { if (!curTrack.empty()) resetLyrics(); return; }

      double posS = doc["position_s"] | -1.0;
      double durS = doc["duration_s"] | -1.0;
      bool   lyrAvail = doc["lyrics_available"] | false;

      // re-anchor each poll, compensating GET latency: position_s was computed ~mid-round-trip,
      // so the true position at receipt is posS + rtt/2 (+ the bench LEAD for residual offset).
      //
      // ...but not blindly. The cloud's position_s carries a fixed recognition-latency fudge
      // (recognition_lag_s), so consecutive anchors can disagree by tens of seconds and jump
      // BACKWARDS — measured 17-25 s on the bench. Swallowing those rewinds a stage show by
      // whole sections and drags lyrics backwards mid-line, so mid-band backwards moves are
      // dropped and the local extrapolation carries on. See acceptAnchor().
      if (posS >= 0) {
        double cand = posS + (rtt / 2.0 + LYRICS_LEAD_MS) / 1000.0;
        double cur  = anchorPos + (double)(millis() - anchorAt) / 1000.0;
        bool   fresh = !anchorStale(millis(), anchorAt, haveAnchor);
        // A track change is not jitter: the new song's position bears no relation to the
        // old one's, so the jitter guard must not veto it.
        bool sameTrack = (trackId == curTrack);
        if (!sameTrack || acceptAnchor(cand, cur, haveAnchor && fresh, anchorRejects)) {
          anchorPos  = cand;
          anchorAt   = millis();
          haveAnchor = true;
        } else {
          USER_PRINTF("[np] ignored backwards re-anchor %.1fs -> %.1fs (jitter)\n", cur, cand);
        }
      }

      if (trackId != curTrack) {                            // new song: drop old words, refetch
        curTrack = trackId; curDur = durS; lyricsFetched = false;
        publishTimeline(nplyrics::Timeline());
      }
      if (lyrAvail && !lyricsFetched) fetchLyrics(trackId, durS);
    }

    // The word to flash right now, or nullptr in a gap / when inactive. Pure read of the
    // published slot + anchor; safe to call from the core-1 draw path (see publish note).
    const std::string *currentWord() {
      if (!lyricsActive || !haveAnchor) return nullptr;
      int s = liveSlot;
      if (s < 0) return nullptr;
      double pos = anchorPos + (double)(millis() - anchorAt) / 1000.0;
      return nplyrics::word_at(slot[s], pos, slotStarts[s]);
    }

    void runLyrics() {
      const std::string *lastLogged = nullptr;
      bool wasActive = false;
      for (;;) {
        if (lyricsActive && (WLED_CONNECTED)) {
          pollNowPlaying();
          // P2 verification hook: log word changes over serial (P3 replaces this with the
          // panel draw). Cheap and only while lyrics mode is the active look.
          uint32_t t0 = millis();
          while (lyricsActive && millis() - t0 < LYRICS_POLL_MS) {
            const std::string *w = currentWord();
            if (w != lastLogged && !(w && lastLogged && *w == *lastLogged)) {
              double pos = haveAnchor ? anchorPos + (double)(millis() - anchorAt) / 1000.0 : -1.0;
              USER_PRINTF("[lyrics] %.1fs  %s\n", pos, w ? w->c_str() : "(gap)");
              lastLogged = w;
            }
            vTaskDelay(pdMS_TO_TICKS(60));
          }
          wasActive = true;
        } else {
          if (wasActive) { resetLyrics(); lastLogged = nullptr; wasActive = false; }
          vTaskDelay(pdMS_TO_TICKS(200));
        }
      }
    }

    static void lyricsTrampoline(void *arg) { static_cast<RoomMicUploader *>(arg)->runLyrics(); }

  public:
    // Glow strip (GPIO 2, the LEDs after the 64x32 matrix) safety net. The strip's look lives in
    // the built presets (170..209, both segments) and the Pi re-pushes them on boot; nothing is
    // persisted here any more. If a seg-0-only web save ({"stop":0} on seg 1) still deletes the
    // strip segment, re-create it with a fixed fallback so the strip is never dark until the next
    // cycle preset restores the real look. ponytail: fixed Gravcenter, polled every 250 ms.
    void keepStripSegment() {
      const uint16_t first = Segment::maxWidth * Segment::maxHeight;  // matrix pixels come first
      const uint16_t total = strip.getLengthTotal();                   // 2048 + 206 on the appliance
      if (!strip.isMatrix || total <= first) return;                   // no strip configured
      for (uint8_t i = 0; i < strip.getSegmentsNum(); i++)
        if (strip.getSegment(i).isActive() && strip.getSegment(i).start >= first) return;
      static StaticJsonDocument<256> d; d.clear();
      JsonObject seg = d["seg"].to<JsonArray>().createNestedObject();
      seg["id"] = 1; seg["start"] = first; seg["stop"] = total; seg["startY"] = 0; seg["stopY"] = 1;
      seg["on"] = true; seg["fx"] = 156; seg["pal"] = 74; seg["sx"] = 128; seg["ix"] = 160; // Gravcenter
      auto keep = currentPreset;                                        // a seg edit marks the look "modified" (ps -1);
      deserializeState(d.as<JsonObject>(), CALL_MODE_NO_NOTIFY);        // the knob's modeIdx sync needs the real one
      currentPreset = keep;
      USER_PRINTLN(F("[strip] segment missing -> fallback Gravcenter"));
    }

    void setup() override {
      // Load the knob cycle order from /npcycle.json BEFORE the knob task starts (it reads
      // cycleSlots/cycleCount). Missing/invalid => the compiled default stands. FS is mounted
      // by now (usermod setup() runs late in WLED boot).
      loadCycle();

      // The inter-board UART link comes up first and independently of the (large) multipart
      // body: the heartbeat + RE-CHECK path must work even if the ingest buffer fails to alloc.
      xTaskCreatePinnedToCore(uartTrampoline, "npUart", 4096, this, 1, &uartTask, 0);

      if (!allocBody()) return;
      ready = true;
      // Pin to core 0 (protocol/FFT core) at low priority so the blocking POST never
      // competes with LED rendering in the main loop on core 1.
      xTaskCreatePinnedToCore(taskTrampoline, "npIngest", 12288, this, 1, &ingestTask, 0);
      USER_PRINTF("[ingest] room_mic_uploader up (body %u B, window %us @ %u Hz, cadence %ums)\n",
                   (unsigned)bodyTotal, (unsigned)CAPTURE_SECONDS, (unsigned)SAMPLE_RATE, (unsigned)CADENCE_MS);

      // Lyrics-mode data path (P2): its own task + PSRAM response scratch, so the /nowplaying
      // poll + /lyrics fetch never blocks ingest or the render loop. No buffer => lyrics no-op.
      netBuf = (char *)heap_caps_malloc(LYRICS_NETCAP, MALLOC_CAP_SPIRAM);
      if (!netBuf) netBuf = (char *)malloc(LYRICS_NETCAP);
      if (netBuf) xTaskCreatePinnedToCore(lyricsTrampoline, "npLyrics", 8192, this, 1, &lyricsTask, 0);
      else USER_PRINTLN(F("[lyrics] OOM: response buffer — lyrics mode disabled"));
    }

    // Drain knob requests marshalled from the uartTask (core 0). bri+stateUpdated() and
    // applyPreset() expect the WLED main-loop context (core 1) — running them here avoids
    // stateUpdated() reentrancy against the render loop. Ingest/heartbeat stay on their tasks.
    void loop() override {
      // [DEBUG-kb01] bright-channel raw levels: distinguishes wiring (levels never change while
      // turning) from firmware (levels change but no detent events). Remove after bring-up.
      static unsigned long kb01Last = 0;
      if (millis() - kb01Last > 1000) {
        kb01Last = millis();
        // Mapping probe: all free (non-HUB75/mic/PSRAM/USB) GPIOs, to find which pin a shorted
        // shield terminal actually lands on.
        USER_PRINTF("[DEBUG-kb01] IO0=%d IO1=%d IO3=%d IO12=%d IO17=%d IO18=%d IO41=%d IO42=%d\n",
                    digitalRead(0), digitalRead(1), digitalRead(3), digitalRead(12),
                    digitalRead(17), digitalRead(18), digitalRead(41), digitalRead(42));
      }
      // Preset FIRST, then brightness: applyPreset() restores the preset's own stored bri, so a
      // bright nudge that landed in the same poll must be applied AFTER it to win (otherwise the
      // user's brightness change silently vanishes under the preset's value).
      int16_t p = pendingPreset;
      if (p >= 0) {
        pendingPreset = -1;
        applyPreset((uint8_t)p, CALL_MODE_DIRECT_CHANGE);
      }
      int16_t b = pendingBri;
      if (b >= 0) {
        pendingBri = -1;
        bri = (uint8_t)b;
        if (bri) briLast = bri;
        stateUpdated(CALL_MODE_DIRECT_CHANGE);
      }
      runBuilderCmd();                                 // PRESET_SAVE / PRESET_DELETE / CYCLE_SET
      static unsigned long stripLast = 0;
      if (millis() - stripLast > 250) { stripLast = millis(); keepStripSegment(); }
    }

    // The web preset-manager POSTs {"npcycle":{"cycle":[...]}} to /json/state to set the knob
    // cycle order live. deserializeState() fans this out to every usermod (core 1, before it
    // applies ps/psave), so we validate + apply it into the live store AND persist it — reorder
    // takes effect immediately with no reboot and survives the next boot. Any other JSON state
    // (a preset load, a color change) has no "npcycle" key and we no-op.
    void readFromJsonState(JsonObject& root) override {
      // {"nplyrics":bool} — enter/leave lyrics mode from the web UI. Lyrics left the knob cycle
      // (builder.md); this toggle is the one remaining way in, kept so the overlay stays testable.
      // The home look runs underneath the words; leaving drops back home — unless the same POST
      // also picks a preset ({"ps":N,"nplyrics":false}): deserializeState applies ps after us, so
      // queuing home here would stomp it a tick later in loop().
      JsonVariantConst nl = root["nplyrics"];
      if (!nl.isNull()) {
        uint8_t presetN; const uint8_t *slots = presetSlots(presetN);
        lyricsActive = nl.as<bool>();
        if (root["ps"].isNull()) pendingPreset = slots[0];
        USER_PRINTF("[lyrics] web set active=%d\n", (int)lyricsActive);
      }

      JsonObjectConst np = root["npcycle"];
      if (np.isNull()) return;
      if (!applyCycleDoc(np)) { USER_PRINTLN(F("[cycle] rejected bad npcycle update")); return; }
      persistCycle();
      int idx = presetIndexOf(currentPreset);      // re-anchor the knob index to the live look
      if (idx >= 0) modeIdx = (uint8_t)idx;
      USER_PRINTF("[cycle] set live: %u slot(s)\n", (unsigned)cycleCount);
    }

    // Publish lyrics mode so the web UI can show the live state (and so the knob and the web
    // toggle agree). serializeState() only calls addToJsonState when !forPreset, so this can
    // never get baked into a saved preset and re-toggled on every preset load.
    void addToJsonState(JsonObject& root) override {
      root["nplyrics"] = (bool)lyricsActive;
      JsonObject u = root.createNestedObject("npuart");
      u["rx"] = (uint32_t)uartRxBytes; u["frames"] = (uint32_t)uartRxFrames; u["grab"] = knobGrabbed;
    }

    // P3: lyrics overlay. Called after effects, before show() — when a word is playing we
    // black out the panel and paint the word on top (covering the equalizer); in a gap or a
    // track with no synced lyrics we draw nothing, so the running effect shows through (the
    // no-lyrics spectrum fallback, for free — the in-gap Lyric Ribbon is P4). Ports np_lyrics
    // render(): same glyphs/layout/word_color, drawn with strip.setPixelColorXY. Global
    // brightness still folds in at show(), so the bright knob keeps working.
    void handleOverlayDraw() override {
      if (!lyricsActive || !strip.isMatrix) return;
      const std::string *wp = currentWord();
      if (!wp) return;
      const int W = Segment::maxWidth, H = Segment::maxHeight;
      if (W <= 0 || H <= 0) return;
      std::vector<const nplyrics::Glyph*> gs = nplyrics::glyphs(*wp);
      if (gs.empty()) return;   // word has no drawable glyphs → treat as a gap

      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) strip.setPixelColorXY(x, y, (uint32_t)0);   // cover the effect

      nplyrics::Layout L = nplyrics::layout(gs, W, H);
      nplyrics::RGB c = nplyrics::word_color(*wp);
      uint32_t col = RGBW32((uint8_t)(c.r*255.0f+0.5f), (uint8_t)(c.g*255.0f+0.5f),
                            (uint8_t)(c.b*255.0f+0.5f), 0);
      int cx = L.x0;
      for (size_t gi = 0; gi < gs.size(); gi++) {
        const nplyrics::Glyph *g = gs[gi];
        int gw = nplyrics::glyph_w(g);
        for (int ry = 0; ry < nplyrics::GLYPH_H; ry++) {
          const char *bits = g->rows[ry];
          for (int rx = 0; rx < gw; rx++) {
            if (bits[rx] != '1') continue;
            for (int dy = 0; dy < L.scale; dy++) {
              int py = L.y0 + ry*L.scale + dy;
              if (py < 0 || py >= H) continue;
              for (int dx = 0; dx < L.scale; dx++) {
                int px = cx + rx*L.scale + dx;
                if (px >= 0 && px < W) strip.setPixelColorXY(px, py, col);
              }
            }
          }
        }
        cx += (gw + nplyrics::CHAR_GAP) * L.scale;
      }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};
