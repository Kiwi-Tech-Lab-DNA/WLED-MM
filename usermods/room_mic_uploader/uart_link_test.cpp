// Host self-check for uart_link.h: encode -> decode round-trip of every frame type, CRC
// rejection, and the 128-byte payload cap. Build + run:
//   g++ -std=c++11 -o /tmp/uart_link_test uart_link_test.cpp && /tmp/uart_link_test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uart_link.h"

// Feed a whole encoded frame; returns true if exactly the last byte completes a frame.
static bool roundtrip(UartDecoder &d, const uint8_t *buf, size_t n) {
  bool done = false;
  for (size_t i = 0; i < n; i++) { bool r = d.feed(buf[i]); assert(!r || i == n - 1); done = r; }
  return done;
}

int main() {
  uint8_t buf[UART_MAX_FRAME];
  UartDecoder d;

  // existing types still round-trip
  uint8_t k; int16_t v; uint8_t fl; uint32_t up;
  assert(roundtrip(d, buf, uart_make_knob(0, -3, 1, buf, sizeof buf)));
  assert(uart_parse_knob(d, &k, &v, &fl) && k == 0 && v == -3 && fl == 1);
  assert(roundtrip(d, buf, uart_make_heartbeat(0x01020304, buf, sizeof buf)));
  assert(uart_parse_heartbeat(d, &up) && up == 0x01020304);
  assert(roundtrip(d, buf, uart_make_recheck(buf, sizeof buf)) && d.type == UART_RECHECK && d.len == 0);

  // KNOB_GRAB: [7E 06 01 flags crc]
  size_t n = uart_make_grab(1, buf, sizeof buf);
  assert(n == 5 && buf[1] == 0x06 && buf[2] == 1 && buf[3] == 1);
  assert(roundtrip(d, buf, n) && d.type == UART_KNOB_GRAB && d.payload[0] == 1);

  // PREVIEW: [7E 07 02 seg slot crc]
  n = uart_make_preview(1, 120, buf, sizeof buf);
  assert(n == 6 && buf[1] == 0x07 && buf[3] == 1 && buf[4] == 120);
  assert(roundtrip(d, buf, n) && d.type == UART_PREVIEW && d.payload[0] == 1 && d.payload[1] == 120);

  // PRESET_SAVE: [7E 08 LEN slot panel strip name... crc], name not NUL-terminated
  const char *nm = "Mountain / Pixels";
  n = uart_make_preset_save(170, 3, 120, nm, (uint8_t)strlen(nm), buf, sizeof buf);
  assert(n == 4 + 3 + strlen(nm) && buf[1] == 0x08 && buf[2] == 3 + strlen(nm));
  assert(buf[3] == 170 && buf[4] == 3 && buf[5] == 120 && memcmp(buf + 6, nm, strlen(nm)) == 0);
  uint8_t slot, panel, strip; char name[33];
  assert(roundtrip(d, buf, n));
  assert(uart_parse_preset_save(d, &slot, &panel, &strip, name, sizeof name));
  assert(slot == 170 && panel == 3 && strip == 120 && strcmp(name, nm) == 0);
  // empty name is legal (LEN == 3)
  assert(roundtrip(d, buf, uart_make_preset_save(171, 4, 121, "", 0, buf, sizeof buf)));
  assert(uart_parse_preset_save(d, &slot, &panel, &strip, name, sizeof name) && name[0] == '\0');
  // a name longer than the parser's buffer is truncated, never overrun
  char big[126]; memset(big, 'x', 125); big[125] = '\0';
  assert(uart_make_preset_save(172, 1, 1, big, 126, buf, sizeof buf) == 0);   // 3 + 126 > 128: refused
  n = uart_make_preset_save(172, 1, 1, big, 125, buf, sizeof buf);            // 3 + 125 == 128: the cap
  assert(n == 4 + 128 && roundtrip(d, buf, n));
  char small[8];
  assert(uart_parse_preset_save(d, &slot, &panel, &strip, small, sizeof small));
  assert(strlen(small) == 7 && small[0] == 'x');

  // PRESET_DELETE: [7E 09 01 slot crc]
  n = uart_make_preset_delete(170, buf, sizeof buf);
  assert(n == 5 && buf[1] == 0x09 && buf[3] == 170);
  assert(roundtrip(d, buf, n) && d.type == UART_PRESET_DELETE && d.payload[0] == 170);

  // CYCLE_SET: [7E 0A 1+n n slots... crc]
  uint8_t cyc[40]; for (int i = 0; i < 40; i++) cyc[i] = (uint8_t)(170 + i);
  n = uart_make_cycle_set(cyc, 40, buf, sizeof buf);
  assert(n == 4 + 41 && buf[1] == 0x0A && buf[2] == 41 && buf[3] == 40 && buf[4] == 170 && buf[43] == 209);
  const uint8_t *slots; uint8_t cn;
  assert(roundtrip(d, buf, n) && uart_parse_cycle_set(d, &slots, &cn) && cn == 40 && slots[39] == 209);
  assert(roundtrip(d, buf, uart_make_cycle_set(cyc, 1, buf, sizeof buf)) && uart_parse_cycle_set(d, &slots, &cn) && cn == 1);
  assert(uart_make_cycle_set(cyc, 128, buf, sizeof buf) == 0);   // 1 + 128 > 128
  // count byte disagreeing with LEN is rejected by the parser
  uint8_t bad[3] = { 5, 1, 2 };
  assert(roundtrip(d, buf, uart_encode(UART_CYCLE_SET, bad, 3, buf, sizeof buf)));
  assert(!uart_parse_cycle_set(d, &slots, &cn));

  // ACK: [7E 0B 03 type arg status crc]
  n = uart_make_ack(UART_PRESET_SAVE, 170, UART_ACK_OK, buf, sizeof buf);
  assert(n == 7 && buf[1] == 0x0B && buf[3] == 0x08 && buf[4] == 170 && buf[5] == 0);
  uint8_t at, aa, as;
  assert(roundtrip(d, buf, n) && uart_parse_ack(d, &at, &aa, &as) && at == 0x08 && aa == 170 && as == 0);

  // 128-byte cap: exactly 128 encodes, 129 does not; the decoder drops LEN > 128
  uint8_t full[129]; memset(full, 0xAB, sizeof full);
  assert(uart_encode(0x42, full, 128, buf, sizeof buf) == 132);
  assert(uart_encode(0x42, full, 129, buf, sizeof buf) == 0);
  assert(roundtrip(d, buf, 132) && d.len == 128 && d.payload[127] == 0xAB);
  uint8_t oversize[4] = { UART_SYNC, 0x42, 129, 0 };
  for (int i = 0; i < 4; i++) assert(!d.feed(oversize[i]));
  assert(d.state == 0);

  // CRC: a flipped payload byte fails, and the decoder resyncs on the next frame
  n = uart_make_preset_delete(170, buf, sizeof buf);
  buf[3] ^= 1;
  assert(!roundtrip(d, buf, n));
  assert(roundtrip(d, buf, uart_make_grab(0, buf, sizeof buf)) && d.type == UART_KNOB_GRAB);

  // heartbeat flags: 5-byte heartbeat parses, and a 4-byte one still does (flags 0)
  uint8_t hf = 0xFF;
  assert(roundtrip(d, buf, uart_make_heartbeat_flags(77, UART_HB_WIFI, buf, sizeof buf)) && d.len == 5);
  assert(uart_parse_heartbeat(d, &up, &hf) && up == 77 && hf == UART_HB_WIFI);
  assert(roundtrip(d, buf, uart_make_heartbeat(78, buf, sizeof buf)) && uart_parse_heartbeat(d, &up, &hf) && hf == 0);

  // AUDIO: 16 bins + volume + peak; 0x7E (the SYNC byte) inside the payload is just data
  uint8_t bins[16]; for (int i = 0; i < 16; i++) bins[i] = (uint8_t)(0x70 + i);   // includes 0x7E
  n = uart_make_audio(bins, 0x7E, 1, buf, sizeof buf);
  assert(n == 4 + 18 && buf[1] == 0x0C && buf[2] == 18);
  assert(roundtrip(d, buf, n) && d.type == UART_AUDIO && d.payload[14] == 0x7E && d.payload[16] == 0x7E && d.payload[17] == 1);

  // PROVISION: max SSID + max PSK (with '~' = 0x7E) round-trips; bad TLVs are rejected
  char ssid[33], psk[64];
  char s32[33], p63[64];
  for (int i = 0; i < 32; i++) s32[i] = (char)('a' + i % 26);
  for (int i = 0; i < 63; i++) p63[i] = i % 3 ? (char)('A' + i % 26) : '~';
  s32[32] = p63[63] = '\0';
  n = uart_make_provision(s32, 32, p63, 63, buf, sizeof buf);
  assert(n == 4 + 99 && roundtrip(d, buf, n));
  assert(uart_parse_provision(d, ssid, psk) && strcmp(ssid, s32) == 0 && strcmp(psk, p63) == 0);
  assert(roundtrip(d, buf, uart_make_provision("open", 4, "", 0, buf, sizeof buf)));   // open network
  assert(uart_parse_provision(d, ssid, psk) && strcmp(ssid, "open") == 0 && psk[0] == '\0');
  assert(uart_make_provision("", 0, "x", 1, buf, sizeof buf) == 0);                    // SSID required
  assert(uart_make_provision(s32, 33, "x", 1, buf, sizeof buf) == 0);                  // SSID > 32
  uint8_t runs[4] = { 1, 9, 'a', 'b' };                                               // TLV past LEN
  assert(roundtrip(d, buf, uart_encode(UART_PROVISION, runs, 4, buf, sizeof buf)) && !uart_parse_provision(d, ssid, psk));
  uint8_t nossid[3] = { 2, 1, 'x' };
  assert(roundtrip(d, buf, uart_encode(UART_PROVISION, nossid, 3, buf, sizeof buf)) && !uart_parse_provision(d, ssid, psk));

  printf("uart_link: all checks passed\n");
  return 0;
}
