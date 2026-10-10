// uart_link.h — inter-board UART framing (slice b2-13).
//
// The wire is Board 2's UART1 (P4 JST: IO17=TX, IO18=RX, GND) crossed to Board 1's
// GPIO17/18 — see docs/issues/board2/b2-13-uart-board1.md and esp32-appliance-design.md §6.
//
// Frame:  [SYNC 0x7E][TYPE u8][LEN u8][PAYLOAD ...LEN][CRC8]
//   - CRC8 (poly 0x07, init 0x00) covers TYPE, LEN and PAYLOAD (not SYNC).
//   - No byte-stuffing: LEN delimits the payload, so a 0x7E inside it is just data. It only
//     matters after a lost byte, when the decoder may resync on a payload 0x7E — the CRC
//     rejects that false frame and the next real SYNC recovers.
//   - TYPE is never 0x7E, so the decoder can treat a 0x7E while waiting for TYPE as a
//     fresh SYNC (resync), not a type byte.
//
// Pure C++ (no Arduino deps) on purpose: this exact file can be copied into the Board 1
// (WLED-MM) usermod so both ends share one definition of the protocol.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define UART_SYNC          0x7E
#define UART_MAX_PAYLOAD   192    // CLOUD (2+96 + 2+80 = 180) is the largest
#define UART_MAX_FRAME     (UART_MAX_PAYLOAD + 4)  // SYNC+TYPE+LEN+payload+CRC

enum UartFrameType : uint8_t {
  UART_KNOB_EVENT = 0x01,  // B1->B2 : knob u8 (0=mode,1=bright,2=gain), value i16 LE, flags u8 (bit0=press)
  UART_RECHECK    = 0x02,  // B2->B1 : empty — Board 1 fires an immediate /ingest
  UART_PROVISION  = 0x03,  // B2->B1 : TLV [tag u8][len u8][bytes]: 1 = WiFi SSID (0..32; 0 = forget), 2 = PSK (0..63).
                           //          ACK (arg 0). nowplaying-project/docs/offline-mode.md §Wire
  UART_RESET      = 0x04,  // either : empty (b2-10, not built here)
  UART_HEARTBEAT  = 0x05,  // both   : uptime_s u32 LE [, flags u8 — B1: bit0 = WiFi connected, bit1 = ingest in flight]
                           //          (a 4-byte heartbeat = flags unknown)
  // Builder (nowplaying-project/docs/builder.md §Wire). All u8, no padding.
  UART_KNOB_GRAB     = 0x06,  // B2->B1 : flags u8 (bit0: 1=grab, 0=release). No ACK.
                              //          While grabbed Board 1 stops applying the mode knob; its
                              //          KNOB_EVENT value becomes the raw detent count (B2 diffs it).
  UART_PREVIEW       = 0x07,  // B2->B1 : seg u8, slot u8 — applyPreset(slot) (one segment). No ACK.
  UART_PRESET_SAVE   = 0x08,  // B2->B1 : slot u8, panel u8, strip u8, name[LEN-3] (no NUL; >32 truncated)
  UART_PRESET_DELETE = 0x09,  // B2->B1 : slot u8
  UART_CYCLE_SET     = 0x0A,  // B2->B1 : n u8, slots[n] u8 (n 1..40, slots 1..250)
  UART_ACK           = 0x0B,  // B1->B2 : type u8 (the frame being answered), arg u8 (its slot / n),
                              //          status u8 (0=ok, 1=rejected: bad slot/count/name)
  UART_AUDIO         = 0x0C,  // B1->B2 : 16 GEQ bins u8, volume u8, peak u8 — ~30 Hz, the Pi's offline
                              //          effect screen (nowplaying-project/docs/offline-mode.md). No ACK.
  UART_POWER         = 0x0D,  // B2->B1 : on u8 (0 = standby: WLED off -> panel + strip dark). No ACK.
  UART_WIFI          = 0x0E,  // B2->B1 : on u8 (0 = radio off, no AP, no reconnects). ACK (arg 0).
  UART_CLOUD         = 0x0F,  // B2->B1 : TLV: 1 = API base URL (1..96, "https://host/api"), 2 = device
                              //          token (1..80). RAM only — re-pushed on every B1 boot. ACK (arg 0).
};
#define UART_AUDIO_BINS 16
#define UART_HB_WIFI    0x01
#define UART_HB_INGEST  0x02   // an /ingest POST is in flight (Board 2 holds its RE-CHECK spinner until it clears)
#define UART_ACK_OK       0
#define UART_ACK_REJECTED 1

static inline uint8_t uart_crc8(uint8_t crc, uint8_t b) {
  crc ^= b;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  return crc;
}

// Encode a frame into `out` (must hold >= UART_MAX_FRAME). Returns total bytes, or 0 if
// the payload is too large / buffer too small.
static inline size_t uart_encode(uint8_t type, const uint8_t *payload, uint8_t len,
                                 uint8_t *out, size_t out_cap) {
  if (len > UART_MAX_PAYLOAD || (size_t)len + 4 > out_cap) return 0;
  uint8_t crc = uart_crc8(0, type);
  crc = uart_crc8(crc, len);
  size_t n = 0;
  out[n++] = UART_SYNC;
  out[n++] = type;
  out[n++] = len;
  for (uint8_t i = 0; i < len; i++) { out[n++] = payload[i]; crc = uart_crc8(crc, payload[i]); }
  out[n++] = crc;
  return n;
}

// Streaming decoder: feed received bytes one at a time. feed() returns true exactly when a
// CRC-valid frame completes, leaving `type`, `len`, `payload[]` set for that frame. Garbage
// (bad CRC, oversize LEN) silently drops the partial frame and resyncs on the next SYNC.
struct UartDecoder {
  uint8_t state = 0;   // 0=want SYNC, 1=want TYPE, 2=want LEN, 3=in PAYLOAD, 4=want CRC
  uint8_t type = 0, len = 0, idx = 0, crc = 0;
  uint8_t payload[UART_MAX_PAYLOAD];

  bool feed(uint8_t b) {
    switch (state) {
      case 0: if (b == UART_SYNC) state = 1; return false;
      case 1: // TYPE (a stray 0x7E here is a re-sync, not a type)
        if (b == UART_SYNC) return false;
        type = b; crc = uart_crc8(0, b); state = 2; return false;
      case 2: // LEN
        len = b;
        if (len > UART_MAX_PAYLOAD) { state = 0; return false; }
        crc = uart_crc8(crc, b); idx = 0; state = (len == 0) ? 4 : 3; return false;
      case 3: // PAYLOAD
        payload[idx++] = b; crc = uart_crc8(crc, b);
        if (idx >= len) state = 4; return false;
      case 4: { // CRC
        bool ok = (b == crc); state = 0; return ok;
      }
    }
    state = 0; return false;
  }
};

// --- typed payload helpers (little-endian) ----------------------------------
static inline size_t uart_make_recheck(uint8_t *out, size_t cap) {
  return uart_encode(UART_RECHECK, nullptr, 0, out, cap);
}
static inline size_t uart_make_heartbeat(uint32_t uptime_s, uint8_t *out, size_t cap) {
  uint8_t p[4] = { (uint8_t)uptime_s, (uint8_t)(uptime_s >> 8),
                   (uint8_t)(uptime_s >> 16), (uint8_t)(uptime_s >> 24) };
  return uart_encode(UART_HEARTBEAT, p, 4, out, cap);
}
static inline size_t uart_make_heartbeat_flags(uint32_t uptime_s, uint8_t flags, uint8_t *out, size_t cap) {
  uint8_t p[5] = { (uint8_t)uptime_s, (uint8_t)(uptime_s >> 8),
                   (uint8_t)(uptime_s >> 16), (uint8_t)(uptime_s >> 24), flags };
  return uart_encode(UART_HEARTBEAT, p, 5, out, cap);
}
static inline size_t uart_make_audio(const uint8_t *bins, uint8_t volume, uint8_t peak, uint8_t *out, size_t cap) {
  uint8_t p[UART_AUDIO_BINS + 2];
  for (int i = 0; i < UART_AUDIO_BINS; i++) p[i] = bins[i];
  p[UART_AUDIO_BINS] = volume; p[UART_AUDIO_BINS + 1] = peak;
  return uart_encode(UART_AUDIO, p, sizeof p, out, cap);
}
static inline size_t uart_make_knob(uint8_t knob, int16_t value, uint8_t flags,
                                    uint8_t *out, size_t cap) {
  uint8_t p[4] = { knob, (uint8_t)value, (uint8_t)((uint16_t)value >> 8), flags };
  return uart_encode(UART_KNOB_EVENT, p, 4, out, cap);
}
static inline bool uart_parse_knob(const UartDecoder &d, uint8_t *knob, int16_t *value, uint8_t *flags) {
  if (d.type != UART_KNOB_EVENT || d.len != 4) return false;
  *knob = d.payload[0];
  *value = (int16_t)((uint16_t)d.payload[1] | ((uint16_t)d.payload[2] << 8));
  *flags = d.payload[3];
  return true;
}
// Accepts 4 or more bytes (newer senders append flags); flags = the 5th byte, 0 if absent.
static inline bool uart_parse_heartbeat(const UartDecoder &d, uint32_t *uptime_s, uint8_t *flags = nullptr) {
  if (d.type != UART_HEARTBEAT || d.len < 4) return false;
  if (flags) *flags = d.len >= 5 ? d.payload[4] : 0;
  *uptime_s = (uint32_t)d.payload[0] | ((uint32_t)d.payload[1] << 8) |
              ((uint32_t)d.payload[2] << 16) | ((uint32_t)d.payload[3] << 24);
  return true;
}

// --- builder frames (B2->B1 requests, B1->B2 ACK) ----------------------------
static inline size_t uart_make_grab(uint8_t flags, uint8_t *out, size_t cap) {
  return uart_encode(UART_KNOB_GRAB, &flags, 1, out, cap);
}
static inline size_t uart_make_preview(uint8_t seg, uint8_t slot, uint8_t *out, size_t cap) {
  uint8_t p[2] = { seg, slot };
  return uart_encode(UART_PREVIEW, p, 2, out, cap);
}
// name: up to UART_MAX_PAYLOAD-3 bytes, not NUL-terminated on the wire (LEN delimits it).
static inline size_t uart_make_preset_save(uint8_t slot, uint8_t panel, uint8_t strip,
                                           const char *name, uint8_t name_len,
                                           uint8_t *out, size_t cap) {
  if (name_len > UART_MAX_PAYLOAD - 3) return 0;
  uint8_t p[UART_MAX_PAYLOAD] = { slot, panel, strip };
  for (uint8_t i = 0; i < name_len; i++) p[3 + i] = (uint8_t)name[i];
  return uart_encode(UART_PRESET_SAVE, p, (uint8_t)(3 + name_len), out, cap);
}
static inline size_t uart_make_preset_delete(uint8_t slot, uint8_t *out, size_t cap) {
  return uart_encode(UART_PRESET_DELETE, &slot, 1, out, cap);
}
static inline size_t uart_make_cycle_set(const uint8_t *slots, uint8_t n, uint8_t *out, size_t cap) {
  if (n > UART_MAX_PAYLOAD - 1) return 0;
  uint8_t p[UART_MAX_PAYLOAD] = { n };
  for (uint8_t i = 0; i < n; i++) p[1 + i] = slots[i];
  return uart_encode(UART_CYCLE_SET, p, (uint8_t)(1 + n), out, cap);
}
static inline size_t uart_make_ack(uint8_t type, uint8_t arg, uint8_t status, uint8_t *out, size_t cap) {
  uint8_t p[3] = { type, arg, status };
  return uart_encode(UART_ACK, p, 3, out, cap);
}
// Copies the name out NUL-terminated into name[name_cap] (truncating if needed).
static inline bool uart_parse_preset_save(const UartDecoder &d, uint8_t *slot, uint8_t *panel,
                                          uint8_t *strip, char *name, size_t name_cap) {
  if (d.type != UART_PRESET_SAVE || d.len < 3 || name_cap == 0) return false;
  *slot = d.payload[0]; *panel = d.payload[1]; *strip = d.payload[2];
  size_t n = d.len - 3;
  if (n > name_cap - 1) n = name_cap - 1;
  for (size_t i = 0; i < n; i++) name[i] = (char)d.payload[3 + i];
  name[n] = '\0';
  return true;
}
// Returns a pointer into the decoder's payload (valid until the next feed()).
static inline bool uart_parse_cycle_set(const UartDecoder &d, const uint8_t **slots, uint8_t *n) {
  if (d.type != UART_CYCLE_SET || d.len < 1 || d.len != 1 + d.payload[0]) return false;
  *n = d.payload[0]; *slots = d.payload + 1;
  return true;
}
static inline bool uart_parse_ack(const UartDecoder &d, uint8_t *type, uint8_t *arg, uint8_t *status) {
  if (d.type != UART_ACK || d.len != 3) return false;
  *type = d.payload[0]; *arg = d.payload[1]; *status = d.payload[2];
  return true;
}

// --- PROVISION (B2->B1): WiFi credentials as TLV ------------------------------
static inline size_t uart_make_provision(const char *ssid, uint8_t ssid_len, const char *psk, uint8_t psk_len,
                                         uint8_t *out, size_t cap) {
  if (ssid_len > 32 || psk_len > 63) return 0;
  uint8_t p[UART_MAX_PAYLOAD];
  uint8_t n = 0;
  p[n++] = 1; p[n++] = ssid_len;
  for (uint8_t i = 0; i < ssid_len; i++) p[n++] = (uint8_t)ssid[i];
  p[n++] = 2; p[n++] = psk_len;
  for (uint8_t i = 0; i < psk_len; i++) p[n++] = (uint8_t)psk[i];
  return uart_encode(UART_PROVISION, p, n, out, cap);
}
// ssid[33] / psk[64] come out NUL-terminated. Unknown tags are skipped; an SSID tag is required
// (empty = forget the network).
static inline bool uart_parse_provision(const UartDecoder &d, char ssid[33], char psk[64]) {
  if (d.type != UART_PROVISION) return false;
  ssid[0] = psk[0] = '\0';
  bool have = false;
  for (uint8_t i = 0; i + 2 <= d.len;) {
    uint8_t tag = d.payload[i], n = d.payload[i + 1];
    i += 2;
    if (i + n > d.len) return false;                       // TLV runs past LEN
    if (tag == 1 || tag == 2) {
      if (n > (tag == 1 ? 32 : 63)) return false;
      have |= tag == 1;
      char *dst = tag == 1 ? ssid : psk;
      for (uint8_t k = 0; k < n; k++) dst[k] = (char)d.payload[i + k];
      dst[n] = '\0';
    }
    i += n;
  }
  return have;
}

// --- CLOUD (B2->B1): the cloud API base + device token as TLV --------------------
// Both tags required and non-empty; base[97] / token[81] come out NUL-terminated.
static inline bool uart_parse_cloud(const UartDecoder &d, char base[97], char token[81]) {
  if (d.type != UART_CLOUD) return false;
  base[0] = token[0] = '\0';
  for (uint8_t i = 0; i + 2 <= d.len;) {
    uint8_t tag = d.payload[i], n = d.payload[i + 1];
    i += 2;
    if (i + n > d.len) return false;
    if (tag == 1 || tag == 2) {
      if (n > (tag == 1 ? 96 : 80)) return false;
      char *dst = tag == 1 ? base : token;
      for (uint8_t k = 0; k < n; k++) dst[k] = (char)d.payload[i + k];
      dst[n] = '\0';
    }
    i += n;
  }
  return base[0] && token[0];
}

// Split a CLOUD base URL "http[s]://host[:port][/path]" for the raw-socket client. Trailing '/'
// is dropped from path ("" = none). False on any other shape (no scheme, empty host, bad port).
static inline bool np_split_url(const char *url, bool *tls, char host[64], uint16_t *port, char path[33]) {
  const char *p;
  if (!strncmp(url, "https://", 8))     { *tls = true;  *port = 443; p = url + 8; }
  else if (!strncmp(url, "http://", 7)) { *tls = false; *port = 80;  p = url + 7; }
  else return false;
  size_t h = strcspn(p, ":/");
  if (h == 0 || h > 63) return false;
  memcpy(host, p, h); host[h] = '\0'; p += h;
  if (*p == ':') {
    char *e; long v = strtol(p + 1, &e, 10);
    if (e == p + 1 || v < 1 || v > 65535) return false;
    *port = (uint16_t)v; p = e;
  }
  if (*p && *p != '/') return false;
  size_t n = strlen(p);
  while (n && p[n - 1] == '/') n--;
  if (n > 32) return false;
  memcpy(path, p, n); path[n] = '\0';
  return true;
}
