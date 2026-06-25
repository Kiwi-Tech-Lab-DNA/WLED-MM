Import("env")
import os
import json

def patch_audioreactive_lib(source, target, env):
    """Patch the AudioReactive library.json to exclude .cpp from compilation"""
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")
    library_json_path = os.path.join(libdeps_dir, "wled-audioreactive", "library.json")

    if os.path.exists(library_json_path):
        try:
            with open(library_json_path, 'r') as f:
                content = f.read()
                # Handle malformed JSON (missing opening brace)
                if not content.strip().startswith('{'):
                    content = '{' + content
                if not content.strip().endswith('}'):
                    content = content + '}'
                library_config = json.loads(content)

            # Add srcFilter to exclude all source files from library compilation
            if "build" not in library_config:
                library_config["build"] = {}

            if "srcFilter" not in library_config["build"]:
                library_config["build"]["srcFilter"] = ["-<*>"]

                with open(library_json_path, 'w') as f:
                    json.dump(library_config, f, indent=2)

                print("✓ Patched wled-audioreactive library.json to exclude .cpp from compilation")
        except Exception as e:
            print(f"Warning: Could not patch wled-audioreactive library.json: {e}")


# --- NP_PCM_TAP -------------------------------------------------------------------
# Inject a raw-PCM tap into the (auto-fetched) audioreactive lib so the nowplaying
# room_mic_uploader usermod can read mic audio. audioreactive owns the I2S mic, so a
# second usermod cannot capture independently; instead we copy each fresh batch of
# samples (int16-range floats in vReal, BEFORE the in-place FFT overwrites them) into
# a PSRAM ring. Single writer (the FFT task) + single reader (the usermod POST task) +
# a monotonic sample counter == lock-free, race-safe while the ring (12 s) stays
# comfortably larger than the capture window (10 s). The whole tap is guarded by
# USERMOD_ROOM_MIC_UPLOADER (only the board1 env defines it), so other WLED-MM builds
# are byte-for-byte unaffected. audio_reactive.h is #included exactly once
# (wled00/usermods_list.cpp), so the non-static globals have a single definition.
#
# This is a text patch against an auto-fetched dependency, so it is keyed on a marker
# string and is idempotent (PlatformIO re-runs pre: scripts every build).

NP_TAP_MARKER = "NP_PCM_TAP"

NP_TAP_GLOBALS = """// --- NP_PCM_TAP globals (nowplaying Board 1 room_mic_uploader) ---
#ifdef USERMOD_ROOM_MIC_UPLOADER
#include "esp_heap_caps.h"
uint32_t          npPcmRingLen = 12u * 22050u;  // ring length in samples, 12 s @ 22050 (> 10 s window); usermod externs this
int16_t*          npPcmRing    = nullptr;       // PSRAM ring; external linkage (header #included once)
volatile uint32_t npPcmWritten = 0;             // total samples ever written (monotonic)
#endif
"""

NP_TAP_GLOBALS_ANCHOR = "void FFTcode(void * parameter)\n{\n"

NP_TAP_COPY = """#ifdef USERMOD_ROOM_MIC_UPLOADER
    // NP_PCM_TAP: copy the freshest samples into the ingest ring before the FFT runs.
    // In sliding-window mode only the newest 50% (vReal+samplesFFT_2) is fresh each pass.
    {
      if (!npPcmRing) npPcmRing = (int16_t*) heap_caps_malloc(npPcmRingLen * sizeof(int16_t), MALLOC_CAP_SPIRAM);
      if (npPcmRing) {
      #ifdef FFT_USE_SLIDING_WINDOW
        const float* npSrc = usingOldSamples ? (vReal + samplesFFT_2) : vReal;
        const uint16_t npCnt = usingOldSamples ? samplesFFT_2 : samplesFFT;
      #else
        const float* npSrc = vReal;
        const uint16_t npCnt = samplesFFT;
      #endif
        uint32_t w = npPcmWritten;
        for (uint16_t i = 0; i < npCnt; i++) {
          float v = npSrc[i];
          int16_t s = (v >= 32767.0f) ? 32767 : (v <= -32768.0f) ? -32768 : (int16_t)v;
          npPcmRing[(w + i) % npPcmRingLen] = s;
        }
        npPcmWritten = w + npCnt;   // publish the new count AFTER the ring writes land
      }
    }
#endif
"""

NP_TAP_COPY_ANCHOR = "    if (audioSource) audioSource->getSamples(vReal, samplesFFT);\n#endif\n"


def inject_pcm_tap(env):
    """Inject the room_mic_uploader raw-PCM tap into the fetched audio_reactive.h."""
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")
    header_path = os.path.join(libdeps_dir, "wled-audioreactive", "audio_reactive.h")
    if not os.path.exists(header_path):
        return
    try:
        with open(header_path, 'r') as f:
            src = f.read()

        if NP_TAP_MARKER in src:
            return  # already injected

        if NP_TAP_GLOBALS_ANCHOR not in src or NP_TAP_COPY_ANCHOR not in src:
            print("Warning: NP_PCM_TAP anchors not found in audio_reactive.h — "
                  "audioreactive upstream changed; tap NOT injected.")
            return

        src = src.replace(NP_TAP_GLOBALS_ANCHOR, NP_TAP_GLOBALS + NP_TAP_GLOBALS_ANCHOR, 1)
        src = src.replace(NP_TAP_COPY_ANCHOR, NP_TAP_COPY_ANCHOR + NP_TAP_COPY, 1)

        with open(header_path, 'w') as f:
            f.write(src)
        print("✓ Injected NP_PCM_TAP raw-PCM tap into wled-audioreactive/audio_reactive.h")
    except Exception as e:
        print(f"Warning: Could not inject NP_PCM_TAP into audio_reactive.h: {e}")


# --- NP keep-local-mic on HUB75 ---------------------------------------------------
# audioreactive forces the digital-mic type to 254 ("network receive only", UDP sound
# sync) on every HUB75 build — a conservative workaround for boards where HUB75 and the
# I2S mic contend for the I2S peripheral. The appliance's Board 1 is an ESP32-S3 that runs
# BOTH: HUB75 uses LCD_CAM, the mic uses I2S0, so they coexist (proven on hardware). Skip
# that override when room_mic_uploader is enabled so SR_DMTYPE stays the env's value
# (generic I2S) and the local mic feeds the tap. Other HUB75 builds are unchanged.

NP_MIC_ANCHOR = "#ifdef WLED_ENABLE_HUB75MATRIX\n#undef SR_DMTYPE\n#define SR_DMTYPE 254  // \"network receive only\"\n#endif"

NP_MIC_REPLACEMENT = ("// NP: keep local I2S mic on the appliance's HUB75 board (room_mic_uploader)\n"
                      "#if defined(WLED_ENABLE_HUB75MATRIX) && !defined(USERMOD_ROOM_MIC_UPLOADER)\n"
                      "#undef SR_DMTYPE\n#define SR_DMTYPE 254  // \"network receive only\"\n#endif")


def keep_local_mic_on_hub75(env):
    """Don't force UDP-receive-only on the appliance's HUB75 board — keep the local I2S mic."""
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")
    header_path = os.path.join(libdeps_dir, "wled-audioreactive", "audio_reactive.h")
    if not os.path.exists(header_path):
        return
    try:
        with open(header_path, 'r') as f:
            src = f.read()
        if "!defined(USERMOD_ROOM_MIC_UPLOADER)" in src:
            return  # already neutralized
        if NP_MIC_ANCHOR not in src:
            print("Warning: HUB75 mic-override anchor not found in audio_reactive.h — "
                  "audioreactive upstream changed; local-mic patch NOT applied.")
            return
        src = src.replace(NP_MIC_ANCHOR, NP_MIC_REPLACEMENT, 1)
        with open(header_path, 'w') as f:
            f.write(src)
        print("✓ Neutralized HUB75 mic-override (keep local I2S mic for room_mic_uploader)")
    except Exception as e:
        print(f"Warning: Could not apply keep-local-mic patch: {e}")


# Run immediately when script is loaded
patch_audioreactive_lib(None, None, env)
inject_pcm_tap(env)
keep_local_mic_on_hub75(env)
