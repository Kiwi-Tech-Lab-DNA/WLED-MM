#pragma once

#include "wled.h"
#include <WiFiClient.h>
#include "esp_heap_caps.h"
#include "room_mic_uploader_secrets.h"   // NP_BASE_URL + NP_DEVICE_TOKEN (gitignored)
#include "uart_link.h"                    // b2-13 inter-board UART1 frame codec (shared with Board 2)
#include <Wire.h>                         // b2-13 knobs: I2C bus for the Seesaw encoders
#include "Adafruit_seesaw.h"              // b2-13 knobs: 3x Adafruit #5880 rotary encoders

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

    // --- b2-13 knobs: 3 Seesaw rotary encoders (the toast source for Board 2) ---
    // I2C on Wire1 = SDA47/SCL48 (WLED's primary Wire is 42/41 — a separate bus, no collision).
    // Read inside the UART task (it already owns Serial1, so one writer emits the KNOB_EVENT).
    // value sent = the encoder's accumulated position; flags bit0 = the encoder push switch.
    // Index order matches the protocol knob id (0=mode,1=bright,2=gain). Bench-verified pins +
    // addresses from firmware/board1-smoketest/src/knob_test.cpp (turn+press, 2026-06-23).
    static constexpr int8_t   KNOB_SDA   = 47;
    static constexpr int8_t   KNOB_SCL   = 48;
    static constexpr uint8_t  KNOB_SW    = 24;   // seesaw pin of the encoder's push switch
    static constexpr uint32_t KNOB_POLL_MS = 20; // ~50 Hz: snappy without hammering the I2C bus
    static constexpr uint8_t  KNOB_N     = 3;
    // b2-13 local apply: target units moved per raw encoder count (bench-tunable). The #5880
    // reports a few counts per detent, so this scales a knob turn onto the 0..255 ranges.
    static constexpr int16_t  BRIGHT_STEP = 6;   // bri units per encoder count
    static constexpr int16_t  GAIN_STEP   = 6;   // viz-punch level units per encoder count
    static constexpr float    GAIN_MIN    = 0.1f; // npVizGain floor: never fully zero the bars (a flat,
                                                  // frozen equalizer reads as "hung", not "low gain")
    // The per-knob address / protocol-id / name tables (index-aligned to the protocol knob id:
    // 0=mode 0x37, 1=bright 0x36, 2=gain 0x38) live as local statics in knobsBegin/pollKnobs —
    // gnu++11 ODR-uses a static-constexpr array member, which needs an out-of-line def a
    // header-only usermod can't give. Scalars above are fine (used only as constant exprs).

    Adafruit_seesaw *knob[KNOB_N]  = {nullptr, nullptr, nullptr};
    int32_t  knobPos[KNOB_N]       = {0, 0, 0};   // raw encoder position (signed, for delta)
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

    static int16_t clampU8(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : (int16_t)v); }

    // The looks the mode knob cycles: Mountain (boot/home) + the six Keybeat GIFs. These slots
    // are uploaded to board flash by board1-config/provision-board1.sh (they live in flash, not
    // the repo — see appliance-reproducibility). Local static, returned by pointer: a
    // static-constexpr array MEMBER ODR-fails to link in a header-only usermod (gnu++11).
    static const uint8_t *presetSlots(uint8_t &count) {
      static const uint8_t SLOTS[] = {3, 10, 11, 12, 13, 14, 15};
      count = (uint8_t)(sizeof(SLOTS) / sizeof(SLOTS[0]));
      return SLOTS;
    }
    // Index of `slot` within presetSlots(), or -1 if it isn't one of ours.
    static int presetIndexOf(uint8_t slot) {
      uint8_t n; const uint8_t *s = presetSlots(n);
      for (uint8_t i = 0; i < n; i++) if (s[i] == slot) return i;
      return -1;
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

    // Wait out CADENCE_MS, but break early (returning true) the moment Board 2 asks for a
    // RE-CHECK. Polls in small steps with vTaskDelay so it both reacts fast and keeps core-0
    // IDLE fed (the watchdog trap that bit this task before).
    bool waitCadenceOrRecheck() {
      uint32_t waited = 0;
      while (waited < CADENCE_MS) {
        if (recheckRequest) { recheckRequest = false; return true; }
        vTaskDelay(pdMS_TO_TICKS(CADENCE_POLL_MS));
        waited += CADENCE_POLL_MS;
      }
      return false;
    }

    void run() {
      for (;;) {
        bool forced = waitCadenceOrRecheck();   // forced == a Board 2 RE-CHECK tap
        if (!ready) continue;
        if (!(WLED_CONNECTED)) {
          USER_PRINTLN(forced ? F("[ingest] RECHECK but wifi down — skip")
                              : F("[ingest] wifi down — skip"));
          continue;
        }

        double rms = captureWindow();
        if (rms < 0) { USER_PRINTLN(F("[ingest] tap not ready / window short — skip")); continue; }
        // A RE-CHECK is an explicit "listen now" from the user, so it bypasses the silence
        // gate; the periodic capture still honours it to avoid POSTing a quiet room.
        if (!forced && rms < RMS_GATE) {
          USER_PRINTF("[ingest] room below gate (RMS %.0f < %.0f) — skip\n", rms, RMS_GATE);
          continue;
        }
        if (forced) USER_PRINTF("[ingest] RECHECK -> forced ingest (RMS %.0f, gate bypassed) -> POST\n", rms);
        else        USER_PRINTF("[ingest] captured %u samples  RMS %.0f  -> POST\n", (unsigned)CAPTURE_SAMPLES, rms);
        postIngest();
      }
    }

    static void taskTrampoline(void *arg) { static_cast<RoomMicUploader *>(arg)->run(); }

    // b2-13 knobs: bring up Wire1 + attach whichever Seesaw encoders answer. Safe if some/all
    // are absent — a missing encoder is logged and simply never emits. Called once from the
    // UART task so all I2C lives on that one task (no cross-task Wire1 access).
    void knobsBegin() {
      static const uint8_t      ADDR[KNOB_N] = {0x37, 0x36, 0x38};
      static const char * const NAME[KNOB_N] = {"mode", "bright", "gain"};
      Wire1.begin(KNOB_SDA, KNOB_SCL);
      Wire1.setClock(100000);
      uint8_t found = 0;
      for (uint8_t i = 0; i < KNOB_N; i++) {
        Adafruit_seesaw *s = new Adafruit_seesaw(&Wire1);
        if (s && s->begin(ADDR[i])) {
          s->pinMode(KNOB_SW, INPUT_PULLUP);
          knob[i]        = s;
          knobPos[i]     = s->getEncoderPosition();
          knobPressed[i] = false;
          found++;
          USER_PRINTF("[knob] seesaw 0x%02X (%s) ready\n", ADDR[i], NAME[i]);
        } else {
          delete s;
          knob[i] = nullptr;
          USER_PRINTF("[knob] seesaw 0x%02X (%s) NOT found\n", ADDR[i], NAME[i]);
        }
      }
      USER_PRINTF("[knob] %u/%u encoder(s) ready on Wire1 (SDA%d/SCL%d)\n",
                  found, (unsigned)KNOB_N, (int)KNOB_SDA, (int)KNOB_SCL);
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
      static const char * const NAME[KNOB_N] = {"mode", "bright", "gain"};
      uint8_t presetN; const uint8_t *slots = presetSlots(presetN);

      for (uint8_t i = 0; i < KNOB_N; i++) {
        if (!knob[i]) continue;
        int32_t pos = knob[i]->getEncoderPosition();
        if (pos != knobPos[i]) {
          int32_t delta = pos - knobPos[i];
          knobPos[i] = pos;
          int16_t level;
          if (i == 0) {                  // mode: step one look per detent, wrapping
            int cur = presetIndexOf(currentPreset);       // re-sync to the LIVE preset first, so an
            if (cur >= 0) modeIdx = (uint8_t)cur;          // external change / press-home can't desync us
            modeIdx = (uint8_t)((modeIdx + presetN + (delta > 0 ? 1 : -1)) % presetN);
            pendingPreset = slots[modeIdx];               // applyPreset() runs in loop()
            level = modeIdx;                              // toast: 0-based mode number (not the raw slot id)
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
        bool pressed = !knob[i]->digitalRead(KNOB_SW);   // INPUT_PULLUP: pressed == LOW
        if (pressed && !knobPressed[i]) {
          int16_t level;
          if (i == 0) {                  // mode press = jump home (Mountain; clock face not ported yet)
            modeIdx = 0;
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
          if (dec.feed((uint8_t)Serial1.read())) {
            if (dec.type == UART_RECHECK) {
              recheckRequest = true;
              USER_PRINTLN(F("[uart] RECHECK in <- Board 2 (forcing ingest)"));
            } else if (dec.type == UART_KNOB_EVENT) {
              uint8_t k, fl; int16_t v;
              if (uart_parse_knob(dec, &k, &v, &fl))
                USER_PRINTF("[uart] knob in: id=%d value=%d flags=0x%02X (forwarded, no-op here)\n", k, v, fl);
            }
          }
        }
        uint32_t now = millis();
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

  public:
    void setup() override {
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
    }

    // Drain knob requests marshalled from the uartTask (core 0). bri+stateUpdated() and
    // applyPreset() expect the WLED main-loop context (core 1) — running them here avoids
    // stateUpdated() reentrancy against the render loop. Ingest/heartbeat stay on their tasks.
    void loop() override {
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
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};
