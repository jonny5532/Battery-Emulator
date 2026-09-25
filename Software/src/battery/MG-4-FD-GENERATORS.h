#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Decomposed generators for the MG4 contactor-closing FD frames
// (0x047, 0x08A, 0x313, 0x314).
//
// These are the runtime form of the standalone generators that used to live
// in mg4_dev/*cycle_opt.cpp. Each FD payload is a sequence of back-to-back
// 12-byte subfields with the layout:
//
//     [3-byte subaddr][len = 0x08][CRC-8][7 payload bytes]
//
// CRC-8: poly 0x1D, init 0x00, MSB-first, no reflection, no xorout, computed
// over the 7 payload bytes that follow the CRC.
//
// Dynamic fields are either modelled with the shared integer curve helpers
// below (precharge exponential, linear ramps) or run-length encoded from the
// original capture. Each build_xxx() call reproduces the same frame that the
// corresponding replay table entry used to hold, but from a fraction of the
// data.
// ---------------------------------------------------------------------------

namespace mg4_fd {

// Roll-length encoded field: `value` is held for `count` consecutive frames.
struct RleRun {
  uint16_t value;
  uint16_t count;
};

inline uint8_t crc8(const uint8_t* d) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 7; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

template <size_t N>
inline uint16_t rle_lookup(const RleRun (&runs)[N], int i) {
  for (size_t r = 0; r < N; r++) {
    if (i < (int)runs[r].count) {
      return runs[r].value;
    }
    i -= runs[r].count;
  }
  return 0;  // index past end of segment - should not happen
}

static const uint32_t CURVE_ONE = 1u << 16;  // 1.0 in Q16

// --- Delayed exponential precharge curve (0x047 VAL12 / 0x313 VAL16) -------
// The plateau this approaches is not fixed: the callers scale the live pack
// voltage (datalayer.battery.status.voltage_dV) by a per-frame factor.
static const uint32_t PRECHARGE_DELAY = 250;  // 10 ms frames before the ramp
static const uint32_t PRECHARGE_RATE = 66;    // ~0.258 of the gap per frame
static const uint32_t PRECHARGE_RATE_SHIFT = 8;
static const uint32_t PRECHARGE_RAMP_LEN = 50;  // tabulated ramp steps

// Normalised (0..CURVE_ONE) precharge shape at time t, in 0x047 10 ms frames.
inline uint32_t precharge_shape_q16(uint32_t t) {
  static uint16_t ramp[PRECHARGE_RAMP_LEN + 1];
  static bool built = false;
  if (!built) {
    uint32_t remaining = CURVE_ONE;
    for (uint32_t n = 0; n <= PRECHARGE_RAMP_LEN; n++) {
      remaining -= (remaining * PRECHARGE_RATE) >> PRECHARGE_RATE_SHIFT;
      ramp[n] = (uint16_t)(CURVE_ONE - remaining);
    }
    built = true;
  }
  if (t < PRECHARGE_DELAY) {
    return 0;
  }
  uint32_t n = t - PRECHARGE_DELAY;
  if (n > PRECHARGE_RAMP_LEN) {
    n = PRECHARGE_RAMP_LEN;
  }
  return ramp[n];
}

// --- Linear ramp -----------------------------------------------------------
// 0 for t <= start, straight line to CURVE_ONE at t == end, held afterwards.
inline uint32_t linear_shape_q16(uint32_t t, uint32_t start, uint32_t end) {
  if (t <= start) {
    return 0;
  }
  if (t >= end) {
    return CURVE_ONE;
  }
  return (uint32_t)(((uint64_t)(t - start) * CURVE_ONE) / (end - start));
}

// Map a normalised shape into [dc, scale] (rounded to nearest; scale >= dc).
inline uint16_t shape_value(uint32_t shape_q16, uint16_t dc, uint16_t scale) {
  uint32_t range = (uint32_t)(scale - dc);
  return (uint16_t)(dc + ((range * shape_q16 + CURVE_ONE / 2) >> 16));
}

// --- Step precharge curve (replaces delayed exponential above) ----------------
// Edge aligned to the old exponential's midway (50%) point: old shape is
// 29436 (< 32768) at t = 251 and 38743 (> 32768) at t = 252, so the step at
// t = 252 straddles 50% on the same frame. Time base is unchanged (0x047
// 10 ms frames), so 0x313 callers passing decimated t16 stay aligned.
static const uint32_t PRECHARGE_STEP_FRAME = 252;  // first frame at plateau

// Direct scaled step value: dc before the edge, scale at/after it.
inline uint16_t precharge_step_value(uint32_t t, uint16_t dc, uint16_t scale) {
  return (t < PRECHARGE_STEP_FRAME) ? dc : scale;
}

// Frame-segment lengths (number of frames in each generated segment).
static const int LEN_047 = 800;
static const int LEN_08A = 800;
static const int LEN_313 = 80;
static const int LEN_314 = 80;

// ===========================================================================
// 0x047 (24-byte FD payload, two 12-byte subfields)
// ===========================================================================
namespace gen047 {

static const uint16_t A_VAL12_DC = 45;        // idle / DC offset
static const uint32_t A_VAL12_SCALE_NUM = 2;  // frame value = 0.4 x voltage_dV
static const uint32_t A_VAL12_SCALE_DEN = 5;
static const uint16_t A_VAL12_FIELD_MAX = 0xFFF;  // 12-bit payload field saturation

// Subfield A: MOD value (0x00/0x01/0x02), run-length encoded
static const RleRun A_MOD_RLE[124] = {
    {0x01, 2}, {0x02, 3}, {0x01, 3}, {0x02, 1},   {0x01, 1}, {0x02, 4}, {0x01, 2}, {0x02, 1}, {0x01, 2}, {0x02, 1},
    {0x01, 1}, {0x02, 2}, {0x01, 1}, {0x02, 3},   {0x01, 2}, {0x02, 3}, {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 2},
    {0x01, 3}, {0x02, 1}, {0x01, 2}, {0x02, 2},   {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 1}, {0x01, 2}, {0x02, 1},
    {0x01, 5}, {0x02, 2}, {0x01, 5}, {0x02, 2},   {0x01, 6}, {0x02, 2}, {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 5},
    {0x01, 1}, {0x02, 4}, {0x01, 1}, {0x02, 1},   {0x01, 2}, {0x02, 5}, {0x01, 2}, {0x02, 6}, {0x01, 1}, {0x02, 4},
    {0x01, 4}, {0x02, 1}, {0x01, 3}, {0x02, 4},   {0x01, 1}, {0x02, 2}, {0x01, 1}, {0x02, 4}, {0x01, 1}, {0x02, 8},
    {0x01, 1}, {0x02, 2}, {0x01, 2}, {0x02, 5},   {0x01, 5}, {0x02, 1}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},
    {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},   {0x01, 3}, {0x02, 2}, {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 2},
    {0x01, 3}, {0x02, 1}, {0x01, 3}, {0x02, 1},   {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 3},
    {0x01, 1}, {0x02, 2}, {0x01, 1}, {0x02, 1},   {0x01, 8}, {0x02, 1}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},
    {0x01, 1}, {0x02, 3}, {0x01, 3}, {0x02, 2},   {0x01, 3}, {0x02, 3}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},
    {0x01, 2}, {0x02, 2}, {0x01, 2}, {0x02, 2},   {0x01, 2}, {0x02, 4}, {0x01, 3}, {0x02, 2}, {0x01, 1}, {0x02, 1},
    {0x01, 3}, {0x02, 2}, {0x01, 1}, {0x00, 544},
};

// Subfield B: 0x9F/A0 flag byte, run-length encoded
static const RleRun B_FLAG_RLE[92] = {
    {0x9F, 260}, {0xA0, 12}, {0x9F, 3}, {0xA0, 16}, {0x9F, 1}, {0xA0, 2},  {0x9F, 3}, {0xA0, 2},  {0x9F, 1}, {0xA0, 7},
    {0x9F, 1},   {0xA0, 16}, {0x9F, 1}, {0xA0, 6},  {0x9F, 1}, {0xA0, 35}, {0x9F, 2}, {0xA0, 22}, {0x9F, 4}, {0xA0, 4},
    {0x9F, 2},   {0xA0, 6},  {0x9F, 7}, {0xA0, 1},  {0x9F, 1}, {0xA0, 5},  {0x9F, 2}, {0xA0, 5},  {0x9F, 1}, {0xA0, 1},
    {0x9F, 3},   {0xA0, 3},  {0x9F, 2}, {0xA0, 1},  {0x9F, 1}, {0xA0, 1},  {0x9F, 3}, {0xA0, 40}, {0x9F, 2}, {0xA0, 3},
    {0x9F, 2},   {0xA0, 1},  {0x9F, 6}, {0xA0, 4},  {0x9F, 3}, {0xA0, 9},  {0x9F, 2}, {0xA0, 1},  {0x9F, 1}, {0xA0, 2},
    {0x9F, 2},   {0xA0, 6},  {0x9F, 3}, {0xA0, 11}, {0x9F, 1}, {0xA0, 11}, {0x9F, 1}, {0xA0, 1},  {0x9F, 1}, {0xA0, 1},
    {0x9F, 4},   {0xA0, 1},  {0x9F, 1}, {0xA0, 28}, {0x9F, 1}, {0xA0, 32}, {0x9F, 2}, {0xA0, 10}, {0x9F, 2}, {0xA0, 21},
    {0x9F, 1},   {0xA0, 43}, {0x9F, 1}, {0xA0, 43}, {0x9F, 1}, {0xA0, 1},  {0x9F, 2}, {0xA0, 4},  {0x9F, 1}, {0xA0, 3},
    {0x9F, 4},   {0xA0, 4},  {0x9F, 1}, {0xA0, 6},  {0x9F, 1}, {0xA0, 1},  {0x9F, 1}, {0xA0, 4},  {0x9F, 2}, {0xA0, 6},
    {0x9F, 1},   {0xA0, 9},
};

static const uint8_t BASE_A[12] = {0x00, 0x01, 0x27, 0x08, 0x00, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00};
static const uint8_t BASE_B[12] = {0x00, 0x01, 0x48, 0x08, 0x00, 0x00, 0x6A, 0x06, 0x00, 0xFF, 0xF0, 0xFF};

// Assemble the 24-byte 0x047 FD payload for frame index i. The 12-bit VAL12
// plateau tracks the live pack voltage (0.4 x voltage_dV at full precharge),
// so `voltage_dV` comes from datalayer.battery.status.voltage_dV.
inline void build(int i, uint16_t voltage_dV, uint8_t out[24]) {
  uint32_t target = ((uint32_t)voltage_dV * A_VAL12_SCALE_NUM) / A_VAL12_SCALE_DEN;
  if (target < A_VAL12_DC) {
    target = A_VAL12_DC;
  } else if (target > A_VAL12_FIELD_MAX) {
    target = A_VAL12_FIELD_MAX;
  }

  // Rolling counter: 0xF0..0xFE, starting at 0xFB on frame 0 (skips 0xFF)
  uint8_t cnt = (uint8_t)(0xF0 + ((i + 11) % 15));

  uint16_t val12 = precharge_step_value((uint32_t)i, A_VAL12_DC, (uint16_t)target);
  //uint16_t mod = rle_lookup(A_MOD_RLE, i);
  uint16_t mod = 0x01;
  //uint16_t flag = rle_lookup(B_FLAG_RLE, i);
  uint16_t flag = 0x9F;

  // Subfield A
  uint8_t a[12];
  memcpy(a, BASE_A, 12);
  a[5] = cnt;
  a[9] = (uint8_t)(val12 >> 4);                    // VAL12 high 8 bits
  a[10] = (uint8_t)(((val12 & 0xF) << 4) | 0x0C);  // VAL12 low 4 bits + static 0xC nibble
  a[11] = (uint8_t)mod;
  a[4] = crc8(&a[5]);

  // Subfield B
  uint8_t b[12];
  memcpy(b, BASE_B, 12);
  b[5] = cnt;
  b[8] = (uint8_t)flag;
  b[4] = crc8(&b[5]);

  memcpy(out, a, 12);
  memcpy(out + 12, b, 12);
}

}  // namespace gen047

// ===========================================================================
// 0x08A (48-byte FD payload, four 12-byte subfields)
// ===========================================================================
namespace gen08a {

// Subfield 1: VAL payload byte (0x2D-0x30), 102 runs over 800 frames
static const RleRun S118_VAL_RLE[102] = {
    {0x2D, 28}, {0x2E, 1},  {0x2D, 2},  {0x2E, 1},  {0x2D, 12}, {0x2E, 1},  {0x2D, 169}, {0x2E, 1},  {0x2D, 11},
    {0x2E, 2},  {0x2D, 26}, {0x2E, 1},  {0x2F, 5},  {0x30, 12}, {0x2F, 3},  {0x30, 16},  {0x2F, 1},  {0x30, 2},
    {0x2F, 3},  {0x30, 10}, {0x2F, 1},  {0x30, 16}, {0x2F, 1},  {0x30, 6},  {0x2F, 1},   {0x30, 35}, {0x2F, 2},
    {0x30, 22}, {0x2F, 4},  {0x30, 4},  {0x2F, 2},  {0x30, 6},  {0x2F, 7},  {0x30, 1},   {0x2F, 1},  {0x30, 5},
    {0x2F, 2},  {0x30, 5},  {0x2F, 1},  {0x30, 1},  {0x2F, 3},  {0x30, 3},  {0x2F, 2},   {0x30, 1},  {0x2F, 1},
    {0x30, 1},  {0x2F, 3},  {0x30, 40}, {0x2F, 2},  {0x30, 3},  {0x2F, 2},  {0x30, 1},   {0x2F, 6},  {0x30, 4},
    {0x2F, 3},  {0x30, 9},  {0x2F, 2},  {0x30, 1},  {0x2F, 1},  {0x30, 2},  {0x2F, 2},   {0x30, 6},  {0x2F, 3},
    {0x30, 11}, {0x2F, 1},  {0x30, 11}, {0x2F, 1},  {0x30, 1},  {0x2F, 1},  {0x30, 1},   {0x2F, 4},  {0x30, 1},
    {0x2F, 1},  {0x30, 28}, {0x2F, 1},  {0x30, 32}, {0x2F, 2},  {0x30, 10}, {0x2F, 2},   {0x30, 21}, {0x2F, 1},
    {0x30, 43}, {0x2F, 1},  {0x30, 43}, {0x2F, 1},  {0x30, 1},  {0x2F, 2},  {0x30, 4},   {0x2F, 1},  {0x30, 3},
    {0x2F, 4},  {0x30, 4},  {0x2F, 1},  {0x30, 6},  {0x2F, 1},  {0x30, 1},  {0x2F, 1},   {0x30, 4},  {0x2F, 2},
    {0x30, 6},  {0x2F, 1},  {0x30, 9},
};

// Subfield 2: MODE payload byte (0x00/0x01/0x21), 3 runs
static const RleRun S100_MODE_RLE[3] = {
    {0x00, 190},
    {0x01, 119},
    {0x21, 491},
};

// Subfield 2: FLAG payload byte (0x00/0x08), 3 runs
static const RleRun S100_FLAG_RLE[3] = {
    {0x00, 188},
    {0x08, 219},
    {0x00, 393},
};

// Subfield 2: LSB payload byte (0xFE/0xFF), flickers, 351 runs
static const RleRun S100_LSB_RLE[351] = {
    {0xFF, 2}, {0xFE, 1}, {0xFF, 2}, {0xFE, 7}, {0xFF, 3}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 2}, {0xFE, 2}, {0xFF, 2}, {0xFE, 1},  {0xFF, 2}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 2}, {0xFE, 5}, {0xFF, 2}, {0xFE, 4},  {0xFF, 2}, {0xFE, 6},  {0xFF, 2}, {0xFE, 4},
    {0xFF, 4}, {0xFE, 4}, {0xFF, 1}, {0xFE, 8}, {0xFF, 3}, {0xFE, 3},  {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 2}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1},  {0xFF, 3}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1}, {0xFF, 2}, {0xFE, 4},  {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 2}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 2}, {0xFE, 2}, {0xFF, 1}, {0xFE, 6},  {0xFF, 2}, {0xFE, 12}, {0xFF, 2}, {0xFE, 4},
    {0xFF, 1}, {0xFE, 7}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},  {0xFF, 2}, {0xFE, 8},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 9},  {0xFF, 1}, {0xFE, 6},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 3}, {0xFE, 4}, {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 4},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 3}, {0xFF, 5}, {0xFE, 2},  {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 2}, {0xFE, 4}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 3},
    {0xFF, 5}, {0xFE, 1}, {0xFF, 3}, {0xFE, 6}, {0xFF, 1}, {0xFE, 11}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 7}, {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 2}, {0xFE, 3}, {0xFF, 2}, {0xFE, 12}, {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},  {0xFF, 2}, {0xFE, 4},
    {0xFF, 2}, {0xFE, 2}, {0xFF, 3}, {0xFE, 1}, {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 5},  {0xFF, 1}, {0xFE, 2},  {0xFF, 2}, {0xFE, 3},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 2}, {0xFF, 3}, {0xFE, 2},  {0xFF, 1}, {0xFE, 3},  {0xFF, 2}, {0xFE, 9},
    {0xFF, 2}, {0xFE, 4}, {0xFF, 2}, {0xFE, 2}, {0xFF, 1}, {0xFE, 1},  {0xFF, 3}, {0xFE, 2},  {0xFF, 1}, {0xFE, 6},
    {0xFF, 2}, {0xFE, 2}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 2}, {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 1}, {0xFF, 3}, {0xFE, 6},  {0xFF, 2}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 1}, {0xFF, 2}, {0xFE, 1},  {0xFF, 2}, {0xFE, 1},  {0xFF, 3}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 2}, {0xFE, 1}, {0xFF, 3}, {0xFE, 1},  {0xFF, 2}, {0xFE, 4},  {0xFF, 2}, {0xFE, 3},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 6}, {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 3}, {0xFE, 5}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 3}, {0xFE, 2}, {0xFF, 1}, {0xFE, 3}, {0xFF, 2}, {0xFE, 11}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 5},
    {0xFF, 1}, {0xFE, 6}, {0xFF, 1}, {0xFE, 2}, {0xFF, 1}, {0xFE, 3},  {0xFF, 2}, {0xFE, 2},  {0xFF, 3}, {0xFE, 1},
    {0xFF, 2}, {0xFE, 1}, {0xFF, 2}, {0xFE, 3}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 2},  {0xFF, 3}, {0xFE, 4},
    {0xFF, 5}, {0xFE, 6}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 2},
};

// Subfield 3: LEVEL payload byte (0x00/0x20/0x40), 3 runs
static const RleRun S153_LEVEL_RLE[3] = {
    {0x00, 307},
    {0x20, 30},
    {0x40, 463},
};

static const uint8_t BASE_S118[12] = {0x00, 0x01, 0x18, 0x08, 0x00, 0x00, 0x75, 0x00, 0x75, 0x30, 0x75, 0x30};
static const uint8_t BASE_S100[12] = {0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00};
static const uint8_t BASE_S153[12] = {0x00, 0x01, 0x53, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Rolling counter: 15 values cycling with wrap, starting at base+11 on frame 0.
inline uint8_t counter08a(uint8_t base, int i) {
  return (uint8_t)(base + ((i + 11) % 15));
}

// Assemble the 48-byte 0x08A FD payload for frame index i.
inline void build(int i, uint8_t out[48]) {
  uint8_t s1[12], s2[12], s3[12];

  // Subfield 1 (00 01 18)
  memcpy(s1, BASE_S118, 12);
  s1[5] = counter08a(0x30, i);
  //s1[7] = (uint8_t)rle_lookup(S118_VAL_RLE, i);
  s1[7] = 0x2F;
  s1[4] = crc8(&s1[5]);

  // Subfield 2 (00 01 00)
  memcpy(s2, BASE_S100, 12);
  s2[5] = counter08a(0x40, i);
  // I suspect S100_MODE_RLE is the contactor close request?
  s2[7] = (uint8_t)rle_lookup(S100_MODE_RLE, i);
  s2[8] = (uint8_t)rle_lookup(S100_FLAG_RLE, i);
  //s2[11] = (uint8_t)rle_lookup(S100_LSB_RLE, i);
  s2[11] = 0xFF;
  s2[4] = crc8(&s2[5]);

  // Subfield 3 (00 01 53) - CRC slot stays 0x00, as captured
  memcpy(s3, BASE_S153, 12);
  s3[6] = (uint8_t)rle_lookup(S153_LEVEL_RLE, i);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
  memcpy(out + 24, s3, 12);
  memset(out + 36, 0, 12);  // subfield 4: all-zero padding
}

}  // namespace gen08a

// ===========================================================================
// 0x313 (48-byte FD payload, four 12-byte subfields)
// ===========================================================================
namespace gen313 {

// Subfield 1 (00 04 02): VAL1 payload byte (0x52-0x54), 3 runs
// static const RleRun S1_VAL1_RLE[3] = {
//     {0x52, 9},
//     {0x53, 24},
//     {0x54, 47},
// };

// Subfield 1 (00 04 02): VAL2 payload byte (0x53-0x56), 4 runs
// static const RleRun S1_VAL2_RLE[4] = {
//     {0x53, 5},
//     {0x54, 21},
//     {0x55, 37},
//     {0x56, 17},
// };

// Subfield 1 (00 04 02): STAT payload byte (0x01/0x03/0x05), 3 runs
static const RleRun S1_STAT_RLE[3] = {
    {0x01, 10},
    {0x03, 23},
    {0x05, 47},
};

// Subfield 1 (00 04 02): FLAG payload byte (0xE3/0xE7), 2 runs
// static const RleRun S1_FLAG_RLE[2] = {
//     {0xE3, 33},
//     {0xE7, 47},
// };

// Subfield 2 (00 04 00): VALA and VALC share a linear ramp shape, each with
// its own maximum.
static const int S2_RAMP_START = 32;  // first frame off the idle value
static const int S2_RAMP_END = 78;    // frame the ramp reaches MAX
static const uint16_t S2_VALA_MAX = 77;
static const uint16_t S2_VALC_MAX = 90;

// Subfield 2 (00 04 00): VALB payload byte (0x75-0x79), 14 runs
// static const RleRun S2_VALB_RLE[14] = {
//     {0x78, 37}, {0x79, 1}, {0x78, 1}, {0x76, 2}, {0x78, 2}, {0x79, 1}, {0x78, 3},
//     {0x77, 9},  {0x76, 8}, {0x77, 1}, {0x76, 7}, {0x75, 1}, {0x76, 2}, {0x75, 5},
// };

// Subfield 2 (00 04 00): VALD payload byte (0x68/0x69), 2 runs
// static const RleRun S2_VALD_RLE[2] = {
//     {0x68, 1},
//     {0x69, 79},
// };

// Subfield 3 (00 04 01): VAL16 is the same delayed-exponential precharge
// curve as 0x047's A_VAL12, sampled every 10th 0x047 frame. Its plateau also
// tracks the live pack voltage, but 12.5x larger: the 0x313 value is
// 5 x voltage_dV at full precharge.
static const uint16_t S3_VAL16_DC = 562;   // 12.5 x 45, idle
static const uint32_t S3_VAL16_SCALE = 5;  // frame value = 5 x voltage_dV
static const uint16_t S3_VAL16_FIELD_MAX = 0xFFFF;
static const int S3_VAL16_SKIP = 2;  // 313 frames of capture offset

static const uint8_t BASE_S1[12] = {0x00, 0x04, 0x02, 0x08, 0x00, 0x00, 0x3D, 0x00, 0x00, 0xF2, 0x00, 0x00};
static const uint8_t BASE_S2[12] = {0x00, 0x04, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x80, 0x08, 0x00, 0x00};
static const uint8_t BASE_S3[12] = {0x00, 0x04, 0x01, 0x08, 0x00, 0x00, 0xFF, 0x4C, 0x00, 0x00, 0x00, 0x00};

// Rolling counter: 15 values cycling with wrap, starting at base+7 on frame 0.
inline uint8_t counter313(uint8_t base, int i) {
  return (uint8_t)(base + ((i + 7) % 15));
}

// Assemble the 48-byte 0x313 FD payload for frame index i. Like 0x047, the
// VAL16 plateau tracks the live pack voltage (5 x voltage_dV).
inline void build(int i, uint16_t voltage_dV, uint8_t out[48]) {
  uint8_t s1[12], s2[12], s3[12];

  // Subfield 1 (00 04 02)
  memcpy(s1, BASE_S1, 12);
  s1[5] = counter313(0xF0, i);
  s1[7] = 0x54;  //(uint8_t)rle_lookup(S1_VAL1_RLE, i);
  s1[8] = 0x55;  //(uint8_t)rle_lookup(S1_VAL2_RLE, i);
  s1[10] = (uint8_t)rle_lookup(S1_STAT_RLE, i);
  s1[11] = 0xE7;  //(uint8_t)rle_lookup(S1_FLAG_RLE, i);
  s1[4] = crc8(&s1[5]);

  // Subfield 2 (00 04 00) - CRC slot stays 0x00 and byte 5 stays 0x01, as captured
  uint32_t ramp = linear_shape_q16((uint32_t)i, (uint32_t)S2_RAMP_START, (uint32_t)S2_RAMP_END);
  uint16_t valc = shape_value(ramp, 0, S2_VALC_MAX);
  memcpy(s2, BASE_S2, 12);
  s2[6] = (uint8_t)shape_value(ramp, 0, S2_VALA_MAX);
  s2[7] = 0x78;                                   //(uint8_t)rle_lookup(S2_VALB_RLE, i);
  s2[8] = (uint8_t)(0x80 | (valc >> 4));          // static hi nibble 8 + VALC hi nibble
  s2[9] = (uint8_t)(((valc & 0xF) << 4) | 0x08);  // VALC lo nibble + static lo nibble 8
  s2[10] = 0x69;                                  //(uint8_t)rle_lookup(S2_VALD_RLE, i);

  // Subfield 3 (00 04 01)
  uint32_t target16 = (uint32_t)voltage_dV * S3_VAL16_SCALE;
  if (target16 < S3_VAL16_DC) {
    target16 = S3_VAL16_DC;
  } else if (target16 > S3_VAL16_FIELD_MAX) {
    target16 = S3_VAL16_FIELD_MAX;
  }
  uint32_t t16 = (i > S3_VAL16_SKIP) ? (uint32_t)(i - S3_VAL16_SKIP) * 10u : 0u;
  uint16_t val16 = precharge_step_value(t16, S3_VAL16_DC, (uint16_t)target16);
  memcpy(s3, BASE_S3, 12);
  s3[5] = counter313(0x30, i);
  s3[8] = (uint8_t)(val16 >> 8);    // VAL16 high byte
  s3[9] = (uint8_t)(val16 & 0xFF);  // VAL16 low byte
  s3[4] = crc8(&s3[5]);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
  memcpy(out + 24, s3, 12);
  memset(out + 36, 0, 12);  // subfield 4: all-zero padding
}

}  // namespace gen313

// ===========================================================================
// 0x314 (24-byte FD payload, two 12-byte subfields)
// ===========================================================================
namespace gen314 {

// Subfield 1 (00 04 04): VAL payload byte, 5 runs
static const RleRun S1_VAL_RLE[5] = {
    {0x02, 27}, {0x2E, 1}, {0x4C, 1}, {0x56, 35}, {0x57, 16},
};

// Subfield 1 (00 04 04): HI payload byte (top 2 bits vary, low 6 static 0x08)
static const RleRun S1_HI_RLE[7] = {
    {0xC8, 27}, {0x08, 1}, {0xC8, 1}, {0x48, 2}, {0x88, 1}, {0xC8, 32}, {0x08, 16},
};

// Subfield 1 (00 04 04): FLAG payload byte (0x7F/0x80), 3 runs
// static const RleRun S1_FLAG_RLE[3] = {
//     {0x7F, 57},
//     {0x80, 7},
//     {0x7F, 16},
// };

// Subfield 1 (00 04 04): STAT payload byte (0x04/0xE0/0xE4), 4 runs
// static const RleRun S1_STAT_RLE[4] = {
//     {0xE0, 32},
//     {0xE4, 25},
//     {0x04, 7},
//     {0xE4, 16},
// };

// Subfield 2 (00 04 05): VAL ramps from idle to a peak, then holds.
static const int S2_VAL_RAMP_START = 44;  // first frame off the idle value
static const int S2_VAL_RAMP_END = 59;    // frame the ramp reaches MAX
static const uint16_t S2_VAL_MAX = 48;

// Subfield 2 (00 04 05): CNT2 payload byte (0x00-0x04), 5 runs
// static const RleRun S2_CNT2_RLE[5] = {
//     {0x00, 48}, {0x01, 3}, {0x02, 4}, {0x03, 2}, {0x04, 23},
// };

static const uint8_t BASE_S1[12] = {0x00, 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x56};
static const uint8_t BASE_S2[12] = {0x00, 0x04, 0x05, 0x08, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xE0, 0x41};

// Rolling counter: 15 values cycling with wrap, starting at base+7 on frame 0.
inline uint8_t counter314(uint8_t base, int i) {
  return (uint8_t)(base + ((i + 7) % 15));
}

// Assemble the 24-byte 0x314 FD payload for frame index i.
inline void build(int i, uint8_t out[24]) {
  uint8_t s1[12], s2[12];

  // Subfield 1 (00 04 04)
  memcpy(s1, BASE_S1, 12);
  s1[5] = counter314(0x40, i);
  s1[6] = (uint8_t)rle_lookup(S1_VAL_RLE, i);
  s1[7] = (uint8_t)rle_lookup(S1_HI_RLE, i);
  //s1[8] = (uint8_t)rle_lookup(S1_FLAG_RLE, i);
  s1[8] = 0x7F;
  s1[9] = 0xE4;  // (uint8_t)rle_lookup(S1_STAT_RLE, i);
  s1[4] = crc8(&s1[5]);

  // Subfield 2 (00 04 05)
  memcpy(s2, BASE_S2, 12);
  s2[5] = counter314(0x70, i);
  s2[6] = (uint8_t)shape_value(linear_shape_q16((uint32_t)i, (uint32_t)S2_VAL_RAMP_START, (uint32_t)S2_VAL_RAMP_END), 0,
                               S2_VAL_MAX);
  s2[9] = 0x04;  //(uint8_t)rle_lookup(S2_CNT2_RLE, i);
  s2[4] = crc8(&s2[5]);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
}

}  // namespace gen314

}  // namespace mg4_fd
