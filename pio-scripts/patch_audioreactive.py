Import("env")
import os
import json
import shutil

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


# --- NP_VIZ_GAIN ------------------------------------------------------------------
# The Mountain equalizer renders from the custom LFX_DSP path (postProcessFFTResults),
# which auto-normalizes each band against its own slow peak mean and so structurally
# IGNORES WLED's inputLevel/sampleGain. To give the Board-1 "gain" knob a visible effect
# we inject a runtime multiplier npVizGain (default 1.0 == unchanged look) into the LFX
# output, applied just before the existing [0,1] clamp so >1 pushes bars toward saturation
# (punchier) and <1 softens them. The knob (room_mic_uploader) drives npVizGain ~0..2x.
#
# Ownership: npVizGain is DEFINED in room_mic_uploader.h (always compiled). We only inject an
# `extern` declaration (so the LFX code can name it) + the multiply. That split is deliberate:
# if an upstream audioreactive bump drifts either anchor, the inject warns and skips, and the
# worst case is the gain knob becomes a no-op — the symbol is still defined, so the link never
# breaks. Guarded by USERMOD_ROOM_MIC_UPLOADER so other WLED-MM builds are byte-for-byte
# unaffected, and marker-keyed so it's idempotent. To REVERT: delete this block + the
# inject_viz_gain call below, and drop the usermod's npVizGain definition + write.

NP_VIZ_GAIN_MARKER = "NP_VIZ_GAIN"

NP_VIZ_GAIN_GLOBAL_ANCHOR = "#define LFX_OUT_GAIN        1.0f     // optional output trim\n"

NP_VIZ_GAIN_GLOBAL = """#define LFX_OUT_GAIN        1.0f     // optional output trim
#ifdef USERMOD_ROOM_MIC_UPLOADER
extern float npVizGain;   // NP_VIZ_GAIN: Board-1 gain knob; DEFINED in room_mic_uploader.h, multiplied into the LFX output below
#endif
"""

NP_VIZ_GAIN_MULT_ANCHOR = "      out *= LFX_OUT_GAIN;\n"

NP_VIZ_GAIN_MULT = """      out *= LFX_OUT_GAIN;
#ifdef USERMOD_ROOM_MIC_UPLOADER
      out *= npVizGain;   // NP_VIZ_GAIN: Board-1 gain knob (applied before the [0,1] clamp below)
#endif
"""


def inject_viz_gain(env):
    """Inject the npVizGain runtime multiplier into the LFX_DSP output path."""
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")
    header_path = os.path.join(libdeps_dir, "wled-audioreactive", "audio_reactive.h")
    if not os.path.exists(header_path):
        return
    try:
        with open(header_path, 'r') as f:
            src = f.read()

        if NP_VIZ_GAIN_MARKER in src:
            return  # already injected

        if NP_VIZ_GAIN_GLOBAL_ANCHOR not in src or NP_VIZ_GAIN_MULT_ANCHOR not in src:
            print("Warning: NP_VIZ_GAIN anchors not found in audio_reactive.h — "
                  "audioreactive/LFX_DSP changed; viz-gain NOT injected (gain knob becomes a "
                  "no-op, build still links since npVizGain is defined in the usermod).")
            return

        src = src.replace(NP_VIZ_GAIN_GLOBAL_ANCHOR, NP_VIZ_GAIN_GLOBAL, 1)
        src = src.replace(NP_VIZ_GAIN_MULT_ANCHOR, NP_VIZ_GAIN_MULT, 1)

        with open(header_path, 'w') as f:
            f.write(src)
        print("✓ Injected NP_VIZ_GAIN runtime multiplier into wled-audioreactive/audio_reactive.h")
    except Exception as e:
        print(f"Warning: Could not inject NP_VIZ_GAIN into audio_reactive.h: {e}")


# --- NP_VENDOR_LFX ----------------------------------------------------------------
# The Mountain LedFx-look DSP (LFX_DSP, postProcessFFTResults, every LFX_* macro) is a
# vendored hand-edit of the auto-fetched audioreactive lib — NOT upstream, NOT produced
# by any WLED-MM script. A `pio` clean / `pkg update` / rm silently restores the stock
# audio_reactive.h, reverting Mountain to the plain GEQ look AND leaving the three
# injectors below (esp. NP_VIZ_GAIN) with no LFX anchors to patch. This step closes that
# gap: it runs BEFORE the injectors and, on the appliance build only, drops the vendored
# source-of-truth in when the libdeps copy is stock.
#
# Gated on USERMOD_ROOM_MIC_UPLOADER (board1 only) because the vendored file defines
# LFX_DSP=1 unconditionally — copying it into any other env would change that env's look.
# It NEVER overwrites a file that already has LFX_DSP (vendored or hand-edited), so a local
# tweak to the libdeps copy survives until the next clean — back-port it to the vendored
# file. Source of truth (override with the NP_VENDORED_AUDIOREACTIVE env var):
#   <WLED-MM>/../nowplaying/firmware/board1/wled-mm/audio_reactive.h

def _build_defines(env):
    """Flatten this env's preprocessor defines to a set of names (CPPDEFINES + raw flags)."""
    names = set()
    for d in env.get("CPPDEFINES", []):
        names.add(str(d[0]) if isinstance(d, (list, tuple)) else str(d))
    # Belt-and-suspenders: CPPDEFINES isn't always populated from build_flags at pre-script
    # time, so also scan the raw flag string.
    flags = env.subst(" ".join(env.GetProjectOption("build_flags", []) or []))
    for tok in flags.replace("-D", " ").split():
        names.add(tok.split("=")[0])
    return names


def vendor_lfx_audioreactive(env):
    """Restore the vendored LedFx-look audio_reactive.h when the appliance build finds stock."""
    if "USERMOD_ROOM_MIC_UPLOADER" not in _build_defines(env):
        return  # not the appliance build — leave other envs' audioreactive untouched

    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")
    header_path = os.path.join(libdeps_dir, "wled-audioreactive", "audio_reactive.h")
    if not os.path.exists(header_path):
        return  # lib not fetched yet this pass; nothing to vendor over

    with open(header_path, 'r') as f:
        if "LFX_DSP" in f.read():
            return  # already the LedFx-look file (vendored or hand-edited) — don't clobber

    vendored = os.environ.get("NP_VENDORED_AUDIOREACTIVE") or os.path.join(
        env.subst("$PROJECT_DIR"), "..", "nowplaying", "firmware", "board1", "wled-mm",
        "audio_reactive.h")
    vendored = os.path.normpath(vendored)
    if not os.path.exists(vendored):
        raise FileNotFoundError(
            "NP: stock audio_reactive.h detected on the appliance build, but the vendored "
            "LedFx-look source is missing at\n"
            f"  {vendored}\n"
            "Point NP_VENDORED_AUDIOREACTIVE at it (see nowplaying/firmware/board1/wled-mm/"
            "README.md). Without it Board 1 builds the plain GEQ look, not Mountain.")

    shutil.copyfile(vendored, header_path)
    print(f"✓ Vendored LedFx-look audio_reactive.h over stock libdeps copy (from {vendored})")


# Run immediately when script is loaded
patch_audioreactive_lib(None, None, env)
vendor_lfx_audioreactive(env)   # must precede the injectors — they patch on top of the LFX file
inject_pcm_tap(env)
keep_local_mic_on_hub75(env)
inject_viz_gain(env)
