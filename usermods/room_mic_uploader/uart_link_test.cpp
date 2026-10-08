// Host self-check for uart_link.h: encode -> decode round-trip of every frame type, CRC
// rejection, and the 64-byte payload cap. Build + run:
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
  char big[62]; memset(big, 'x', 61); big[61] = '\0';
  assert(uart_make_preset_save(172, 1, 1, big, 62, buf, sizeof buf) == 0);   // 3 + 62 > 64: refused
  n = uart_make_preset_save(172, 1, 1, big, 61, buf, sizeof buf);            // 3 + 61 == 64: the cap
  assert(n == 4 + 64 && roundtrip(d, buf, n));
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
  assert(uart_make_cycle_set(cyc, 64, buf, sizeof buf) == 0);   // 1 + 64 > 64
  // count byte disagreeing with LEN is rejected by the parser
  uint8_t bad[3] = { 5, 1, 2 };
  assert(roundtrip(d, buf, uart_encode(UART_CYCLE_SET, bad, 3, buf, sizeof buf)));
  assert(!uart_parse_cycle_set(d, &slots, &cn));

  // ACK: [7E 0B 03 type arg status crc]
  n = uart_make_ack(UART_PRESET_SAVE, 170, UART_ACK_OK, buf, sizeof buf);
  assert(n == 7 && buf[1] == 0x0B && buf[3] == 0x08 && buf[4] == 170 && buf[5] == 0);
  uint8_t at, aa, as;
  assert(roundtrip(d, buf, n) && uart_parse_ack(d, &at, &aa, &as) && at == 0x08 && aa == 170 && as == 0);

  // 64-byte cap: exactly 64 encodes, 65 does not; the decoder drops LEN > 64
  uint8_t full[65]; memset(full, 0xAB, sizeof full);
  assert(uart_encode(0x42, full, 64, buf, sizeof buf) == 68);
  assert(uart_encode(0x42, full, 65, buf, sizeof buf) == 0);
  assert(roundtrip(d, buf, 68) && d.len == 64 && d.payload[63] == 0xAB);
  uint8_t oversize[4] = { UART_SYNC, 0x42, 65, 0 };
  for (int i = 0; i < 4; i++) assert(!d.feed(oversize[i]));
  assert(d.state == 0);

  // CRC: a flipped payload byte fails, and the decoder resyncs on the next frame
  n = uart_make_preset_delete(170, buf, sizeof buf);
  buf[3] ^= 1;
  assert(!roundtrip(d, buf, n));
  assert(roundtrip(d, buf, uart_make_grab(0, buf, sizeof buf)) && d.type == UART_KNOB_GRAB);

  printf("uart_link: all checks passed\n");
  return 0;
}
