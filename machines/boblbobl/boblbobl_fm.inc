// Implementation of boblbobl_fm.h - see that file's header. Every function
// here is a transcription of the ymfm function with the same name; comments
// name the ymfm source for each block. Included once, from boblbobl.cpp.
#include "boblbobl_fm.h"
#include <string.h>
#include <esp_attr.h>

// Hot path of the audio render (Audio::transmit, core 0): code in IRAM and
// lookup tables in DRAM so it does not fight the video renderer, which
// streams the 1MB tile table through the same flash cache.

namespace bbfm
{

// ---- ymfm.h helpers ------------------------------------------------------

#ifdef BB_FM_HOST_PROF
// host harness only: TSC time per stage of step_fast (see sndbench)
#include <intrin.h>
unsigned long long bbfm_prof[8];
#define BBFM_T0() unsigned long long bbfm_t = __rdtsc()
#define BBFM_T(k) do { unsigned long long bbfm_n = __rdtsc(); bbfm_prof[k] += bbfm_n - bbfm_t; bbfm_t = bbfm_n; } while (0)
#else
#define BBFM_T0() do { } while (0)
#define BBFM_T(k) do { } while (0)
#endif

static inline IRAM_ATTR uint32_t bitfield(uint32_t value, int start, int length = 1)
{
  return (value >> start) & ((1u << length) - 1);
}

static inline IRAM_ATTR int32_t clamp(int32_t value, int32_t minval, int32_t maxval)
{
  if (value < minval)
    return minval;
  if (value > maxval)
    return maxval;
  return value;
}

static inline IRAM_ATTR uint8_t count_leading_zeros(uint32_t value)
{
  if (value == 0)
    return 32;
#if defined(__GNUC__)
  return (uint8_t)__builtin_clz(value);
#else
  uint8_t n = 0;
  while (!(value & 0x80000000u))
  {
    value <<= 1;
    n++;
  }
  return n;
#endif
}

// ymfm.h roundtrip_fp(): 10.3 floating point DAC (YM3014) round trip
static inline IRAM_ATTR int16_t roundtrip_fp(int32_t value)
{
  if (value < -32768)
    return -32768;
  if (value > 32767)
    return 32767;
  int32_t scanvalue = value ^ (int32_t(value) >> 31);
  int exponent = 7 - count_leading_zeros((uint32_t)scanvalue << 17);
  if (exponent < 1)
    exponent = 1;
  exponent -= 1;
  int32_t mask = (1 << exponent) - 1;
  return (int16_t)(value & ~mask);
}

// ---- ymfm_fm.ipp tables ----------------------------------------------------

static const DRAM_ATTR uint16_t s_sin_table[256] =
{
  0x859,0x6c3,0x607,0x58b,0x52e,0x4e4,0x4a6,0x471,0x443,0x41a,0x3f5,0x3d3,0x3b5,0x398,0x37e,0x365,
  0x34e,0x339,0x324,0x311,0x2ff,0x2ed,0x2dc,0x2cd,0x2bd,0x2af,0x2a0,0x293,0x286,0x279,0x26d,0x261,
  0x256,0x24b,0x240,0x236,0x22c,0x222,0x218,0x20f,0x206,0x1fd,0x1f5,0x1ec,0x1e4,0x1dc,0x1d4,0x1cd,
  0x1c5,0x1be,0x1b7,0x1b0,0x1a9,0x1a2,0x19b,0x195,0x18f,0x188,0x182,0x17c,0x177,0x171,0x16b,0x166,
  0x160,0x15b,0x155,0x150,0x14b,0x146,0x141,0x13c,0x137,0x133,0x12e,0x129,0x125,0x121,0x11c,0x118,
  0x114,0x10f,0x10b,0x107,0x103,0x0ff,0x0fb,0x0f8,0x0f4,0x0f0,0x0ec,0x0e9,0x0e5,0x0e2,0x0de,0x0db,
  0x0d7,0x0d4,0x0d1,0x0cd,0x0ca,0x0c7,0x0c4,0x0c1,0x0be,0x0bb,0x0b8,0x0b5,0x0b2,0x0af,0x0ac,0x0a9,
  0x0a7,0x0a4,0x0a1,0x09f,0x09c,0x099,0x097,0x094,0x092,0x08f,0x08d,0x08a,0x088,0x086,0x083,0x081,
  0x07f,0x07d,0x07a,0x078,0x076,0x074,0x072,0x070,0x06e,0x06c,0x06a,0x068,0x066,0x064,0x062,0x060,
  0x05e,0x05c,0x05b,0x059,0x057,0x055,0x053,0x052,0x050,0x04e,0x04d,0x04b,0x04a,0x048,0x046,0x045,
  0x043,0x042,0x040,0x03f,0x03e,0x03c,0x03b,0x039,0x038,0x037,0x035,0x034,0x033,0x031,0x030,0x02f,
  0x02e,0x02d,0x02b,0x02a,0x029,0x028,0x027,0x026,0x025,0x024,0x023,0x022,0x021,0x020,0x01f,0x01e,
  0x01d,0x01c,0x01b,0x01a,0x019,0x018,0x017,0x017,0x016,0x015,0x014,0x014,0x013,0x012,0x011,0x011,
  0x010,0x00f,0x00f,0x00e,0x00d,0x00d,0x00c,0x00c,0x00b,0x00a,0x00a,0x009,0x009,0x008,0x008,0x007,
  0x007,0x007,0x006,0x006,0x005,0x005,0x005,0x004,0x004,0x004,0x003,0x003,0x003,0x002,0x002,0x002,
  0x002,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000
};

static inline IRAM_ATTR uint32_t abs_sin_attenuation(uint32_t input)
{
  if (bitfield(input, 8))
    input = ~input;
  return s_sin_table[input & 0xff];
}

#define BBX(a) ((((a) | 0x400) << 2))
static const DRAM_ATTR uint16_t s_power_table[256] =
{
  BBX(0x3fa),BBX(0x3f5),BBX(0x3ef),BBX(0x3ea),BBX(0x3e4),BBX(0x3df),BBX(0x3da),BBX(0x3d4),
  BBX(0x3cf),BBX(0x3c9),BBX(0x3c4),BBX(0x3bf),BBX(0x3b9),BBX(0x3b4),BBX(0x3ae),BBX(0x3a9),
  BBX(0x3a4),BBX(0x39f),BBX(0x399),BBX(0x394),BBX(0x38f),BBX(0x38a),BBX(0x384),BBX(0x37f),
  BBX(0x37a),BBX(0x375),BBX(0x370),BBX(0x36a),BBX(0x365),BBX(0x360),BBX(0x35b),BBX(0x356),
  BBX(0x351),BBX(0x34c),BBX(0x347),BBX(0x342),BBX(0x33d),BBX(0x338),BBX(0x333),BBX(0x32e),
  BBX(0x329),BBX(0x324),BBX(0x31f),BBX(0x31a),BBX(0x315),BBX(0x310),BBX(0x30b),BBX(0x306),
  BBX(0x302),BBX(0x2fd),BBX(0x2f8),BBX(0x2f3),BBX(0x2ee),BBX(0x2e9),BBX(0x2e5),BBX(0x2e0),
  BBX(0x2db),BBX(0x2d6),BBX(0x2d2),BBX(0x2cd),BBX(0x2c8),BBX(0x2c4),BBX(0x2bf),BBX(0x2ba),
  BBX(0x2b5),BBX(0x2b1),BBX(0x2ac),BBX(0x2a8),BBX(0x2a3),BBX(0x29e),BBX(0x29a),BBX(0x295),
  BBX(0x291),BBX(0x28c),BBX(0x288),BBX(0x283),BBX(0x27f),BBX(0x27a),BBX(0x276),BBX(0x271),
  BBX(0x26d),BBX(0x268),BBX(0x264),BBX(0x25f),BBX(0x25b),BBX(0x257),BBX(0x252),BBX(0x24e),
  BBX(0x249),BBX(0x245),BBX(0x241),BBX(0x23c),BBX(0x238),BBX(0x234),BBX(0x230),BBX(0x22b),
  BBX(0x227),BBX(0x223),BBX(0x21e),BBX(0x21a),BBX(0x216),BBX(0x212),BBX(0x20e),BBX(0x209),
  BBX(0x205),BBX(0x201),BBX(0x1fd),BBX(0x1f9),BBX(0x1f5),BBX(0x1f0),BBX(0x1ec),BBX(0x1e8),
  BBX(0x1e4),BBX(0x1e0),BBX(0x1dc),BBX(0x1d8),BBX(0x1d4),BBX(0x1d0),BBX(0x1cc),BBX(0x1c8),
  BBX(0x1c4),BBX(0x1c0),BBX(0x1bc),BBX(0x1b8),BBX(0x1b4),BBX(0x1b0),BBX(0x1ac),BBX(0x1a8),
  BBX(0x1a4),BBX(0x1a0),BBX(0x19c),BBX(0x199),BBX(0x195),BBX(0x191),BBX(0x18d),BBX(0x189),
  BBX(0x185),BBX(0x181),BBX(0x17e),BBX(0x17a),BBX(0x176),BBX(0x172),BBX(0x16f),BBX(0x16b),
  BBX(0x167),BBX(0x163),BBX(0x160),BBX(0x15c),BBX(0x158),BBX(0x154),BBX(0x151),BBX(0x14d),
  BBX(0x149),BBX(0x146),BBX(0x142),BBX(0x13e),BBX(0x13b),BBX(0x137),BBX(0x134),BBX(0x130),
  BBX(0x12c),BBX(0x129),BBX(0x125),BBX(0x122),BBX(0x11e),BBX(0x11b),BBX(0x117),BBX(0x114),
  BBX(0x110),BBX(0x10c),BBX(0x109),BBX(0x106),BBX(0x102),BBX(0x0ff),BBX(0x0fb),BBX(0x0f8),
  BBX(0x0f4),BBX(0x0f1),BBX(0x0ed),BBX(0x0ea),BBX(0x0e7),BBX(0x0e3),BBX(0x0e0),BBX(0x0dc),
  BBX(0x0d9),BBX(0x0d6),BBX(0x0d2),BBX(0x0cf),BBX(0x0cc),BBX(0x0c8),BBX(0x0c5),BBX(0x0c2),
  BBX(0x0be),BBX(0x0bb),BBX(0x0b8),BBX(0x0b5),BBX(0x0b1),BBX(0x0ae),BBX(0x0ab),BBX(0x0a8),
  BBX(0x0a4),BBX(0x0a1),BBX(0x09e),BBX(0x09b),BBX(0x098),BBX(0x094),BBX(0x091),BBX(0x08e),
  BBX(0x08b),BBX(0x088),BBX(0x085),BBX(0x082),BBX(0x07e),BBX(0x07b),BBX(0x078),BBX(0x075),
  BBX(0x072),BBX(0x06f),BBX(0x06c),BBX(0x069),BBX(0x066),BBX(0x063),BBX(0x060),BBX(0x05d),
  BBX(0x05a),BBX(0x057),BBX(0x054),BBX(0x051),BBX(0x04e),BBX(0x04b),BBX(0x048),BBX(0x045),
  BBX(0x042),BBX(0x03f),BBX(0x03c),BBX(0x039),BBX(0x036),BBX(0x033),BBX(0x030),BBX(0x02d),
  BBX(0x02a),BBX(0x028),BBX(0x025),BBX(0x022),BBX(0x01f),BBX(0x01c),BBX(0x019),BBX(0x016),
  BBX(0x014),BBX(0x011),BBX(0x00e),BBX(0x00b),BBX(0x008),BBX(0x006),BBX(0x003),BBX(0x000)
};
#undef BBX

static inline IRAM_ATTR uint32_t attenuation_to_volume(uint32_t input)
{
  return s_power_table[input & 0xff] >> (input >> 8);
}

static const DRAM_ATTR uint32_t s_increment_table[64] =
{
  0x00000000, 0x00000000, 0x10101010, 0x10101010,
  0x10101010, 0x10101010, 0x11101110, 0x11101110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x10101010, 0x10111010, 0x11101110, 0x11111110,
  0x11111111, 0x21112111, 0x21212121, 0x22212221,
  0x22222222, 0x42224222, 0x42424242, 0x44424442,
  0x44444444, 0x84448444, 0x84848484, 0x88848884,
  0x88888888, 0x88888888, 0x88888888, 0x88888888
};

static inline IRAM_ATTR uint32_t attenuation_increment(uint32_t rate, uint32_t index)
{
  return bitfield(s_increment_table[rate], 4 * index, 4);
}

static const DRAM_ATTR uint8_t s_detune_adjustment[32][4] =
{
  { 0,  0,  1,  2 },  { 0,  0,  1,  2 },  { 0,  0,  1,  2 },  { 0,  0,  1,  2 },
  { 0,  1,  2,  2 },  { 0,  1,  2,  3 },  { 0,  1,  2,  3 },  { 0,  1,  2,  3 },
  { 0,  1,  2,  4 },  { 0,  1,  3,  4 },  { 0,  1,  3,  4 },  { 0,  1,  3,  5 },
  { 0,  2,  4,  5 },  { 0,  2,  4,  6 },  { 0,  2,  4,  6 },  { 0,  2,  5,  7 },
  { 0,  2,  5,  8 },  { 0,  3,  6,  8 },  { 0,  3,  6,  9 },  { 0,  3,  7, 10 },
  { 0,  4,  8, 11 },  { 0,  4,  8, 12 },  { 0,  4,  9, 13 },  { 0,  5, 10, 14 },
  { 0,  5, 11, 16 },  { 0,  6, 12, 17 },  { 0,  6, 13, 19 },  { 0,  7, 14, 20 },
  { 0,  8, 16, 22 },  { 0,  8, 16, 22 },  { 0,  8, 16, 22 },  { 0,  8, 16, 22 }
};

static inline IRAM_ATTR int32_t detune_adjustment(uint32_t detune, uint32_t keycode)
{
  int32_t result = s_detune_adjustment[keycode][detune & 3];
  return bitfield(detune, 2) ? -result : result;
}

// fm_registers_base::effective_rate
static inline IRAM_ATTR uint32_t effective_rate(uint32_t rawrate, uint32_t ksr)
{
  if (rawrate == 0)
    return 0;
  uint32_t r = rawrate + ksr;
  return r > 63 ? 63 : r;
}

// ---- fm_operator (ymfm_fm.ipp), shared by both chips -----------------------
// HAS_SSG = RegisterType::EG_HAS_SSG (true for OPN, false for OPL).
// ssg_en/ssg_mode are op_ssg_eg_enable()/op_ssg_eg_mode() (always 0 on OPL).

template <bool HAS_SSG>
static inline IRAM_ATTR void op_start_attack(op_t &op, uint32_t ssg_en, uint32_t ssg_mode, bool is_restart)
{
  if (op.env_state == EG_ATTACK)
    return;
  op.env_state = EG_ATTACK;
  if (HAS_SSG && !is_restart)
    op.ssg_inverted = (uint8_t)(ssg_en & bitfield(ssg_mode, 2));
  if (!is_restart)
    op.phase = 0;
  if (op.eg_rate[EG_ATTACK] >= 62)
    op.env_attenuation = 0;
}

template <bool HAS_SSG>
static inline IRAM_ATTR void op_start_release(op_t &op)
{
  if (op.env_state >= EG_RELEASE)
    return;
  op.env_state = EG_RELEASE;
  if (HAS_SSG && op.ssg_inverted)
  {
    op.env_attenuation = (uint16_t)((0x200 - op.env_attenuation) & 0x3ff);
    op.ssg_inverted = 0;
  }
}

template <bool HAS_SSG>
static inline IRAM_ATTR void op_clock_keystate(op_t &op, uint32_t keystate, uint32_t ssg_en, uint32_t ssg_mode)
{
  if ((keystate ^ op.key_state) != 0)
  {
    op.key_state = (uint8_t)keystate;
    if (keystate != 0)
      op_start_attack<HAS_SSG>(op, ssg_en, ssg_mode, false);
    else
      op_start_release<HAS_SSG>(op);
  }
}

// fm_operator::clock_ssg_eg_state (OPN only)
static inline IRAM_ATTR void op_clock_ssg_eg_state(op_t &op, uint32_t ssg_en, uint32_t mode)
{
  if (!bitfield(op.env_attenuation, 9))
    return;
  if (bitfield(mode, 0))
  {
    op.ssg_inverted = (uint8_t)(bitfield(mode, 2) ^ bitfield(mode, 1));
    if (op.env_state != EG_ATTACK)
      op.env_attenuation = op.ssg_inverted ? 0x200 : 0x3ff;
  }
  else
  {
    op.ssg_inverted ^= (uint8_t)bitfield(mode, 1);
    if (op.env_state == EG_DECAY || op.env_state == EG_SUSTAIN)
      op_start_attack<true>(op, ssg_en, mode, true);
    if (bitfield(mode, 1) == 0)
      op.phase = 0;
  }
  if (op.env_state == EG_RELEASE)
    op.env_attenuation = 0x3ff;
}

// fm_operator::clock_envelope
template <bool HAS_SSG>
static inline IRAM_ATTR void op_clock_envelope(op_t &op, uint32_t env_counter, uint32_t ssg_en)
{
  if (op.env_state == EG_ATTACK && op.env_attenuation == 0)
    op.env_state = EG_DECAY;
  if (op.env_state == EG_DECAY && op.env_attenuation >= op.eg_sustain)
    op.env_state = EG_SUSTAIN;

  uint32_t rate = op.eg_rate[op.env_state];
  uint32_t rate_shift = rate >> 2;
  env_counter <<= rate_shift;
  if (bitfield(env_counter, 0, 11) != 0)
    return;

  uint32_t relevant_bits = bitfield(env_counter, (rate_shift <= 11) ? 11 : rate_shift, 3);
  uint32_t increment = attenuation_increment(rate, relevant_bits);

  if (op.env_state == EG_ATTACK)
  {
    // same expression and types as ymfm (uint16_t attenuation, uint32_t increment)
    if (rate < 62)
      op.env_attenuation += (~op.env_attenuation * increment) >> 4;
  }
  else
  {
    if (!(HAS_SSG && ssg_en))
      op.env_attenuation += increment;
    else if (op.env_attenuation < 0x200)
      op.env_attenuation += 4 * increment;
    if (op.env_attenuation >= 0x400)
      op.env_attenuation = 0x3ff;
  }
}

// fm_operator::envelope_attenuation (eg_shift is always 0 here)
template <bool HAS_SSG>
static inline IRAM_ATTR uint32_t op_envelope_attenuation(const op_t &op, uint32_t am_offset, uint32_t am_enable)
{
  uint32_t result = op.env_attenuation;
  if (HAS_SSG && op.ssg_inverted)
    result = (0x200 - result) & 0x3ff;
  if (am_enable)
    result += am_offset;
  result += op.total_level;
  return result < 0x3ff ? result : 0x3ff;
}

// fm_operator::compute_volume (single sine waveform: bit 15 of the ymfm
// waveform table entry is bitfield(index, 9))
template <bool HAS_SSG>
static inline IRAM_ATTR int32_t op_compute_volume(const op_t &op, uint32_t phase, uint32_t am_offset, uint32_t am_enable)
{
  if (op.env_attenuation > EG_QUIET)
    return 0;
  uint32_t index = phase & 0x3ff;
  uint32_t sin_attenuation = abs_sin_attenuation(index);
  uint32_t env_attenuation = op_envelope_attenuation<HAS_SSG>(op, am_offset, am_enable) << 2;
  int32_t result = (int32_t)attenuation_to_volume((sin_attenuation & 0x7fff) + env_attenuation);
  return bitfield(index, 9) ? -result : result;
}

static inline IRAM_ATTR void op_keyonoff(op_t &op, uint32_t on, uint32_t type)
{
  op.keyon_live = (uint8_t)((op.keyon_live & ~(1u << type)) | (bitfield(on, 0) << type));
}

static inline IRAM_ATTR void op_construct(op_t &op, uint16_t opoffs)
{
  memset(&op, 0, sizeof(op));
  op.opoffs = opoffs;
  op.phase = 0;
  op.env_attenuation = 0x3ff;
  op.env_state = EG_RELEASE;
}

// fm_operator::reset
static inline IRAM_ATTR void op_reset(op_t &op)
{
  op.phase = 0;
  op.env_attenuation = 0x3ff;
  op.env_state = EG_RELEASE;
  op.ssg_inverted = 0;
  op.key_state = 0;
  op.keyon_live = 0;
}

// ============================================================================
// YM2203 FM (opn_registers_base<false>)
// ============================================================================

// opn operator_map: channel -> operator numbers { O1, O2, O3, O4 }
static const DRAM_ATTR uint8_t s_opn_map[3][4] = { { 0, 6, 3, 9 }, { 1, 7, 4, 10 }, { 2, 8, 5, 11 } };

static inline IRAM_ATTR uint32_t opn_operator_offset(uint32_t opnum) { return opnum + opnum / 3; }

static inline IRAM_ATTR uint32_t opn_ch_block_freq(const uint8_t *r, uint32_t choffs)
{
  return (bitfield(r[0xa4 + choffs], 0, 6) << 8) | r[0xa0 + choffs];
}
static inline IRAM_ATTR uint32_t opn_multi_block_freq(const uint8_t *r, uint32_t num)
{
  return (bitfield(r[0xac + num], 0, 6) << 8) | r[0xa8 + num];
}

// opn_registers_base::compute_phase_step (no PM on YM2203)
static inline IRAM_ATTR uint32_t opn_compute_phase_step(const op_t &op)
{
  uint32_t fnum = bitfield(op.block_freq, 0, 11) << 1;
  uint32_t block = bitfield(op.block_freq, 11, 3);
  uint32_t phase_step = (fnum << block) >> 2;
  phase_step += op.detune;
  phase_step &= 0x1ffff;
  return (phase_step * op.multiple) >> 1;
}

// opn_registers_base::cache_operator_data
static IRAM_ATTR void opn_cache_operator_data(const uint8_t *r, uint32_t choffs, op_t &op)
{
  uint32_t opoffs = op.opoffs;
  uint32_t block_freq = op.block_freq = opn_ch_block_freq(r, choffs);

  // multi_freq(): 0x27 bits 6-7 != 0
  if (bitfield(r[0x27], 6, 2) != 0 && choffs == 2)
  {
    if (opoffs == 2)
      block_freq = op.block_freq = opn_multi_block_freq(r, 1);
    else if (opoffs == 10)
      block_freq = op.block_freq = opn_multi_block_freq(r, 2);
    else if (opoffs == 6)
      block_freq = op.block_freq = opn_multi_block_freq(r, 0);
  }

  uint32_t keycode = bitfield(block_freq, 10, 4) << 1;
  keycode |= bitfield(0xfe80, bitfield(block_freq, 7, 4));

  op.detune = detune_adjustment(bitfield(r[0x30 + opoffs], 4, 3), keycode);

  op.multiple = bitfield(r[0x30 + opoffs], 0, 4) * 2;
  if (op.multiple == 0)
    op.multiple = 1;

  op.phase_dynamic = false;
  op.phase_step = opn_compute_phase_step(op);

  op.total_level = bitfield(r[0x40 + opoffs], 0, 7) << 3;

  op.eg_sustain = bitfield(r[0x80 + opoffs], 4, 4);
  op.eg_sustain |= (op.eg_sustain + 1) & 0x10;
  op.eg_sustain <<= 5;

  uint32_t ksrval = keycode >> (bitfield(r[0x50 + opoffs], 6, 2) ^ 3);
  op.eg_rate[EG_ATTACK] = (uint8_t)effective_rate(bitfield(r[0x50 + opoffs], 0, 5) * 2, ksrval);
  op.eg_rate[EG_DECAY] = (uint8_t)effective_rate(bitfield(r[0x60 + opoffs], 0, 5) * 2, ksrval);
  op.eg_rate[EG_SUSTAIN] = (uint8_t)effective_rate(bitfield(r[0x70 + opoffs], 0, 5) * 2, ksrval);
  op.eg_rate[EG_RELEASE] = (uint8_t)effective_rate(bitfield(r[0x80 + opoffs], 0, 4) * 4 + 2, ksrval);
}

void opn_t::construct()
{
  memset(regs, 0, sizeof(regs));
  for (uint32_t n = 0; n < 12; n++)
    op_construct(op[n], (uint16_t)opn_operator_offset(n));
  for (uint32_t c = 0; c < 3; c++)
  {
    ch[c].feedback[0] = ch[c].feedback[1] = 0;
    ch[c].feedback_in = 0;
    ch[c].choffs = (uint8_t)c;
  }
  env_counter = 0;
  total_clocks = 0;
  active_channels = 7;
  modified_channels = 7;
  prepare_count = 0;
  last_out = 0;
}

// fm_engine_base::reset (status/timers live in boblbobl.cpp)
void opn_t::reset()
{
  memset(regs, 0, sizeof(regs));        // opn_registers_base::reset (not OPNA)
  regs[0x27] = 0;                       // write(REG_MODE, 0) -> engine_mode_write
  for (uint32_t c = 0; c < 3; c++)
    ch[c].feedback[0] = ch[c].feedback[1] = ch[c].feedback_in = 0;
  for (uint32_t n = 0; n < 12; n++)
    op_reset(op[n]);
}

// fm_engine_base::write + opn_registers_base::write
void IRAM_ATTR opn_t::write(uint8_t index, uint8_t data)
{
  if (index == 0x27)
  {
    // REG_MODE goes through ymfm_sync_mode_write -> engine_mode_write, which
    // marks all channels modified and stores the register
    modified_channels = 7;
    regs[0x27] = data;
    return;
  }

  modified_channels = 7;

  if ((index & 0xf0) == 0xa0)
  {
    if (bitfield(index, 0, 2) == 3)
      return;
    uint32_t latchindex = 0xb8 | bitfield(index, 3);
    if (bitfield(index, 2))
      regs[latchindex] = data & 0x3f;
    else
    {
      regs[index] = data;
      regs[index | 4] = regs[latchindex];
    }
    return;
  }
  else if ((index & 0xf8) == 0xb8)
    return;

  regs[index] = data;

  if (index == 0x28)
  {
    uint32_t channel = bitfield(data, 0, 2);
    if (channel == 3)
      return;
    uint32_t opmask = bitfield(data, 4, 4);
    for (uint32_t o = 0; o < 4; o++)
      op_keyonoff(op[s_opn_map[channel][o]], bitfield(opmask, o), KEYON_NORMAL);
  }
}

// fm_channel::output_4op (rshift 0, clipmax 32767 for ym2203)
static inline IRAM_ATTR int32_t opn_output_4op(opn_t &e, uint32_t c)
{
  const uint8_t *r = e.regs;
  ch_t &chn = e.ch[c];
  op_t &o1 = e.op[s_opn_map[c][0]];
  op_t &o2 = e.op[s_opn_map[c][1]];
  op_t &o3 = e.op[s_opn_map[c][2]];
  op_t &o4 = e.op[s_opn_map[c][3]];

  int32_t opmod = 0;
  uint32_t feedback = bitfield(r[0xb0 + c], 3, 3);
  if (feedback != 0)
    opmod = (chn.feedback[0] + chn.feedback[1]) >> (10 - feedback);

  int32_t op1value = chn.feedback_in = (int16_t)op_compute_volume<true>(o1, (o1.phase >> 10) + opmod, 0, 0);

  // ALGORITHM(op2in, op3in, op4in, op1out, op2out, op3out)
  static const DRAM_ATTR uint16_t s_algorithm_ops[8] =
  {
    (1 | (2 << 1) | (3 << 4)),
    (0 | (5 << 1) | (3 << 4)),
    (0 | (2 << 1) | (6 << 4)),
    (1 | (0 << 1) | (7 << 4)),
    (1 | (0 << 1) | (3 << 4) | (1 << 8)),
    (1 | (1 << 1) | (1 << 4) | (1 << 8) | (1 << 9)),
    (1 | (0 << 1) | (0 << 4) | (1 << 8) | (1 << 9)),
    (0 | (0 << 1) | (0 << 4) | (1 << 7) | (1 << 8) | (1 << 9)),
  };
  uint32_t algorithm_ops = s_algorithm_ops[bitfield(r[0xb0 + c], 0, 3)];

  int16_t opout[8];
  opout[0] = 0;
  opout[1] = (int16_t)op1value;

  opmod = opout[bitfield(algorithm_ops, 0, 1)] >> 1;
  opout[2] = (int16_t)op_compute_volume<true>(o2, (o2.phase >> 10) + opmod, 0, 0);
  opout[5] = (int16_t)(opout[1] + opout[2]);

  opmod = opout[bitfield(algorithm_ops, 1, 3)] >> 1;
  opout[3] = (int16_t)op_compute_volume<true>(o3, (o3.phase >> 10) + opmod, 0, 0);
  opout[6] = (int16_t)(opout[1] + opout[3]);
  opout[7] = (int16_t)(opout[2] + opout[3]);

  opmod = opout[bitfield(algorithm_ops, 4, 3)] >> 1;
  int32_t result = op_compute_volume<true>(o4, (o4.phase >> 10) + opmod, 0, 0);

  const int32_t clipmax = 32767, clipmin = -32768;
  if (bitfield(algorithm_ops, 7) != 0)
    result = clamp(result + opout[1], clipmin, clipmax);
  if (bitfield(algorithm_ops, 8) != 0)
    result = clamp(result + opout[2], clipmin, clipmax);
  if (bitfield(algorithm_ops, 9) != 0)
    result = clamp(result + opout[3], clipmin, clipmax);
  return result;
}

// ym2203::clock_fm: fm_engine_base::clock(ALL_CHANNELS), output(), roundtrip_fp
// fm_engine_base::clock(): "if something was modified, prepare; also prepare
// every 4k samples". `ticks` chip samples are being clocked (1 for ymfm).
void IRAM_ATTR opn_t::prepare_if_modified(uint32_t ticks)
{
  bool due = modified_channels != 0;
  if (!due)
  {
    due = prepare_count >= 4096;
    prepare_count += ticks;
  }
  if (due)
  {
    active_channels = 0;
    for (uint32_t c = 0; c < 3; c++)
    {
      uint32_t active_mask = 0;
      for (uint32_t o = 0; o < 4; o++)
      {
        op_t &p = op[s_opn_map[c][o]];
        // fm_operator::prepare
        opn_cache_operator_data(regs, c, p);
        uint32_t ssg_en = bitfield(regs[0x90 + p.opoffs], 3);
        uint32_t ssg_mode = bitfield(regs[0x90 + p.opoffs], 0, 3);
        op_clock_keystate<true>(p, (uint32_t)(p.keyon_live != 0), ssg_en, ssg_mode);
        p.keyon_live &= ~(1 << KEYON_CSM);
        if (p.env_state != EG_RELEASE || p.env_attenuation < EG_QUIET)
          active_mask |= 1 << o;
      }
      if (active_mask != 0)
        active_channels |= 1 << c;
    }
    modified_channels = prepare_count = 0;
  }
}

void IRAM_ATTR opn_t::clock_fm(bool out)
{
  total_clocks++;
  prepare_if_modified(1);

  // EG_CLOCK_DIVIDER = 3
  if (bitfield(++env_counter, 0, 2) == 3)
    env_counter += 4 - 3;

  // clock_noise_and_lfo(): no LFO on YM2203 -> lfo_raw_pm = 0

  for (uint32_t c = 0; c < 3; c++)
  {
    // fm_channel::clock
    ch[c].feedback[0] = ch[c].feedback[1];
    ch[c].feedback[1] = ch[c].feedback_in;
    for (uint32_t o = 0; o < 4; o++)
    {
      op_t &p = op[s_opn_map[c][o]];
      // fm_operator::clock
      uint32_t ssg_en = bitfield(regs[0x90 + p.opoffs], 3);
      // Silent operator (released to 0x3ff, no SSG-EG): clock_envelope cannot
      // change it and its phase is never observed (compute_volume returns 0
      // above EG_QUIET, and the next key-on resets phase to 0), so skip it.
      if (!ssg_en && p.env_state == EG_RELEASE && p.env_attenuation == 0x3ff)
      {
        p.ssg_inverted = 0;
        continue;
      }
      if (ssg_en)
        op_clock_ssg_eg_state(p, ssg_en, bitfield(regs[0x90 + p.opoffs], 0, 3));
      else
        p.ssg_inverted = 0;
      if (bitfield(env_counter, 0, 2) == 0)
        op_clock_envelope<true>(p, env_counter >> 2, ssg_en);
      p.phase += p.phase_step;
    }
  }

  // out == false (BOBLBOBL_SND_FAST, not ymfm): the output stage - and with it
  // the operator-1 feedback update - is skipped for this sample.
  if (!out)
    return;
  int32_t sum = 0;
  for (uint32_t c = 0; c < 3; c++)
    if (bitfield(active_channels, c))
      sum += opn_output_4op(*this, c);
  last_out = roundtrip_fp(sum);
}

// BOBLBOBL_SND_FAST: advance n chip samples, compute the output once.
// Phase: n * phase_step (exactly where ymfm's phase is after n samples).
// Envelope/SSG-EG: clocked on every chip sample it is due (env_counter runs
// at the chip rate), so ADSR timing is ymfm's. Only the operator feedback and
// the output are taken once per call instead of per chip sample.
void IRAM_ATTR opn_t::step_fast(uint32_t n)
{
  BBFM_T0();
  total_clocks += (uint8_t)n;
  prepare_if_modified(n);
  BBFM_T(0);

  for (uint32_t t = 0; t < n; t++)
  {
    if (bitfield(++env_counter, 0, 2) == 3)
      env_counter += 4 - 3;
    bool env_due = bitfield(env_counter, 0, 2) == 0;
    for (uint32_t k = 0; k < 12; k++)
    {
      op_t &p = op[k];
      uint32_t ssg_en = bitfield(regs[0x90 + p.opoffs], 3);
      if (!ssg_en)
      {
        p.ssg_inverted = 0;
        if (!env_due)
          continue;
        if (p.env_state == EG_RELEASE && p.env_attenuation == 0x3ff)
          continue;
        if (p.env_state == EG_SUSTAIN && p.eg_rate[EG_SUSTAIN] == 0)
          continue;   // nothing can change (rate 0 never steps)
        op_clock_envelope<true>(p, env_counter >> 2, 0);
      }
      else
      {
        op_clock_ssg_eg_state(p, ssg_en, bitfield(regs[0x90 + p.opoffs], 0, 3));
        if (env_due)
          op_clock_envelope<true>(p, env_counter >> 2, ssg_en);
      }
    }
  }
  BBFM_T(1);

  for (uint32_t c = 0; c < 3; c++)
  {
    ch[c].feedback[0] = ch[c].feedback[1];
    ch[c].feedback[1] = ch[c].feedback_in;
  }
  for (uint32_t k = 0; k < 12; k++)
    op[k].phase += op[k].phase_step * n;

  int32_t sum = 0;
  for (uint32_t c = 0; c < 3; c++)
    if (bitfield(active_channels, c))
      sum += opn_output_4op(*this, c);
  last_out = roundtrip_fp(sum);
  BBFM_T(2);
}

// ============================================================================
// YM3526 (opl_registers_base<1>)
// ============================================================================

static const DRAM_ATTR uint8_t s_opl_map[9][2] =
{
  { 0, 3 }, { 1, 4 }, { 2, 5 }, { 6, 9 }, { 7, 10 }, { 8, 11 }, { 12, 15 }, { 13, 16 }, { 14, 17 }
};

static inline IRAM_ATTR uint32_t opl_operator_offset(uint32_t opnum) { return opnum + 2 * (opnum / 6); }

static inline IRAM_ATTR uint32_t opl_key_scale_atten(uint32_t block, uint32_t fnum_4msb)
{
  static const DRAM_ATTR uint8_t fnum_to_atten[16] = { 0,24,32,37,40,43,45,47,48,50,51,52,53,54,55,56 };
  int32_t result = fnum_to_atten[fnum_4msb] - 8 * (int32_t)(block ^ 7);
  return result < 0 ? 0 : (uint32_t)result;
}

// opl_compute_phase_step
static inline IRAM_ATTR uint32_t opl_compute_phase_step(uint32_t block_freq, uint32_t multiple, int32_t lfo_raw_pm)
{
  uint32_t fnum = bitfield(block_freq, 0, 10) << 2;
  fnum += (lfo_raw_pm * (int32_t)bitfield(block_freq, 7, 3)) >> 1;
  fnum &= 0xfff;
  uint32_t block = bitfield(block_freq, 10, 3);
  uint32_t phase_step = (fnum << block) >> 2;
  return (phase_step * multiple) >> 1;
}

// opl_registers_base::cache_operator_data (YM3526: waveform 0 only)
static IRAM_ATTR void opl_cache_operator_data(const uint8_t *r, uint32_t choffs, op_t &op)
{
  uint32_t opoffs = op.opoffs;
  uint32_t block_freq = op.block_freq = (bitfield(r[0xb0 + choffs], 0, 5) << 8) | r[0xa0 + choffs];

  uint32_t keycode = bitfield(block_freq, 10, 3) << 1;
  keycode |= bitfield(block_freq, 9 - bitfield(r[0x08], 6), 1);   // note_select()

  op.detune = 0;

  uint32_t multiple = bitfield(r[0x20 + opoffs], 0, 4);
  op.multiple = ((multiple & 0xe) | bitfield(0xc2aa, multiple)) * 2;
  if (op.multiple == 0)
    op.multiple = 1;

  if (bitfield(r[0x20 + opoffs], 6) == 0)   // op_lfo_pm_enable
  {
    op.phase_dynamic = false;
    op.phase_step = opl_compute_phase_step(op.block_freq, op.multiple, 0);
  }
  else
  {
    op.phase_dynamic = true;
    op.phase_step = 1;                      // PHASE_STEP_DYNAMIC
  }

  op.total_level = bitfield(r[0x40 + opoffs], 0, 6) << 3;

  uint32_t temp = bitfield(r[0x40 + opoffs], 6, 2);
  uint32_t ksl = bitfield(temp, 1) | (bitfield(temp, 0) << 1);   // op_ksl()
  if (ksl != 0)
    op.total_level += opl_key_scale_atten(bitfield(block_freq, 10, 3), bitfield(block_freq, 6, 4)) << ksl;

  op.eg_sustain = bitfield(r[0x80 + opoffs], 4, 4);
  op.eg_sustain |= (op.eg_sustain + 1) & 0x10;
  op.eg_sustain <<= 5;

  uint32_t ksrval = keycode >> (2 * (bitfield(r[0x20 + opoffs], 4) ^ 1));
  op.eg_rate[EG_ATTACK] = (uint8_t)effective_rate(bitfield(r[0x60 + opoffs], 4, 4) * 4, ksrval);
  op.eg_rate[EG_DECAY] = (uint8_t)effective_rate(bitfield(r[0x60 + opoffs], 0, 4) * 4, ksrval);
  op.eg_rate[EG_SUSTAIN] = bitfield(r[0x20 + opoffs], 5) ? 0 : (uint8_t)effective_rate(bitfield(r[0x80 + opoffs], 0, 4) * 4, ksrval);
  op.eg_rate[EG_RELEASE] = (uint8_t)effective_rate(bitfield(r[0x80 + opoffs], 0, 4) * 4, ksrval);
  op.eg_rate[EG_DEPRESS] = 0x3f;
}

void opl_t::construct()
{
  memset(regs, 0, sizeof(regs));
  for (uint32_t n = 0; n < 18; n++)
    op_construct(op[n], (uint16_t)opl_operator_offset(n));
  for (uint32_t c = 0; c < 9; c++)
  {
    ch[c].feedback[0] = ch[c].feedback[1] = 0;
    ch[c].feedback_in = 0;
    ch[c].choffs = (uint8_t)c;
  }
  env_counter = 0;
  total_clocks = 0;
  active_channels = 0x1ff;
  modified_channels = 0x1ff;
  prepare_count = 0;
  lfo_am_counter = 0;
  lfo_pm_counter = 0;
  noise_lfsr = 1;
  lfo_am = 0;
  last_out = 0;
}

void opl_t::reset()
{
  memset(regs, 0, sizeof(regs));        // opl_registers_base::reset
  regs[0x04] = 0;                       // write(REG_MODE, 0)
  for (uint32_t c = 0; c < 9; c++)
    ch[c].feedback[0] = ch[c].feedback[1] = ch[c].feedback_in = 0;
  for (uint32_t n = 0; n < 18; n++)
    op_reset(op[n]);
}

// fm_engine_base::write + opl_registers_base::write
void IRAM_ATTR opl_t::write(uint8_t index, uint8_t data)
{
  if (index == 0x04)
  {
    // REG_MODE via engine_mode_write (marks all channels modified):
    // "writes to the mode register with high bit set ignore the low bits"
    modified_channels = 0x1ff;
    if (bitfield(data, 7) != 0)
      regs[index] |= 0x80;
    else
      regs[index] = data;
    return;
  }

  modified_channels = 0x1ff;
  regs[index] = data;

  if (index == 0xbd)
  {
    // rhythm keyons (RHYTHM_CHANNEL)
    uint32_t opmask = bitfield(data, 5) ? bitfield(data, 0, 5) : 0;
    uint32_t s6 = bitfield(opmask, 4) ? 3 : 0;
    uint32_t s7 = bitfield(opmask, 0) | (bitfield(opmask, 3) << 1);
    uint32_t s8 = bitfield(opmask, 2) | (bitfield(opmask, 1) << 1);
    for (uint32_t o = 0; o < 2; o++)
    {
      op_keyonoff(op[s_opl_map[6][o]], bitfield(s6, o), KEYON_RHYTHM);
      op_keyonoff(op[s_opl_map[7][o]], bitfield(s7, o), KEYON_RHYTHM);
      op_keyonoff(op[s_opl_map[8][o]], bitfield(s8, o), KEYON_RHYTHM);
    }
    return;
  }

  if ((index & 0xf0) == 0xb0)
  {
    uint32_t channel = index & 0x0f;
    if (channel < 9)
    {
      uint32_t opmask = bitfield(data, 5) ? 15 : 0;
      for (uint32_t o = 0; o < 2; o++)
        op_keyonoff(op[s_opl_map[channel][o]], bitfield(opmask, o), KEYON_NORMAL);
    }
  }
}

// opl_clock_noise_and_lfo
static inline IRAM_ATTR int32_t opl_clock_noise_and_lfo(opl_t &e)
{
  e.noise_lfsr <<= 1;
  e.noise_lfsr |= bitfield(e.noise_lfsr, 23) ^ bitfield(e.noise_lfsr, 9) ^ bitfield(e.noise_lfsr, 8) ^ bitfield(e.noise_lfsr, 1);

  uint32_t am_depth = bitfield(e.regs[0xbd], 7);
  uint32_t pm_depth = bitfield(e.regs[0xbd], 6);

  uint32_t am_counter = e.lfo_am_counter++;
  if (am_counter >= 210 * 64 - 1)
    e.lfo_am_counter = 0;
  int shift = 9 - 2 * (int)am_depth;
  e.lfo_am = (uint8_t)(((am_counter < 105 * 64) ? am_counter : (210 * 64 + 63 - am_counter)) >> shift);

  uint32_t pm_counter = e.lfo_pm_counter++;
  static const DRAM_ATTR int8_t pm_scale[8] = { 8, 4, 0, -4, -8, -4, 0, 4 };
  return pm_scale[bitfield(pm_counter, 10, 3)] >> (pm_depth ^ 1);
}

// fm_channel::output_2op (rshift 1, clipmax 32767, MODULATOR_DELAY)
static inline IRAM_ATTR int32_t opl_output_2op(opl_t &e, uint32_t c)
{
  const uint8_t *r = e.regs;
  ch_t &chn = e.ch[c];
  op_t &o1 = e.op[s_opl_map[c][0]];
  op_t &o2 = e.op[s_opl_map[c][1]];
  uint32_t am_offset = e.lfo_am;

  int32_t opmod = 0;
  uint32_t feedback = bitfield(r[0xc0 + c], 1, 3);
  if (feedback != 0)
    opmod = (chn.feedback[0] + chn.feedback[1]) >> (10 - feedback);

  int32_t op1value = chn.feedback_in = (int16_t)op_compute_volume<false>(o1, (o1.phase >> 10) + opmod, am_offset, bitfield(r[0x20 + o1.opoffs], 7));
  (void)op1value;

  int32_t result;
  if (bitfield(r[0xc0 + c], 0) == 0)
  {
    opmod = chn.feedback[1] >> 1;
    result = op_compute_volume<false>(o2, (o2.phase >> 10) + opmod, am_offset, bitfield(r[0x20 + o2.opoffs], 7)) >> 1;
  }
  else
  {
    result = chn.feedback[1] >> 1;
    result += op_compute_volume<false>(o2, o2.phase >> 10, am_offset, bitfield(r[0x20 + o2.opoffs], 7)) >> 1;
    result = clamp(result, -32768, 32767);
  }
  return result;
}

// fm_channel::output_rhythm_ch6/7/8
static inline IRAM_ATTR int32_t opl_output_rhythm(opl_t &e, uint32_t c, uint32_t phase_select)
{
  const uint8_t *r = e.regs;
  ch_t &chn = e.ch[c];
  op_t &o1 = e.op[s_opl_map[c][0]];
  op_t &o2 = e.op[s_opl_map[c][1]];
  uint32_t am_offset = e.lfo_am;
  uint32_t am1 = bitfield(r[0x20 + o1.opoffs], 7);
  uint32_t am2 = bitfield(r[0x20 + o2.opoffs], 7);
  int32_t result;

  if (c == 6)
  {
    int32_t opmod = 0;
    uint32_t feedback = bitfield(r[0xc0 + c], 1, 3);
    if (feedback != 0)
      opmod = (chn.feedback[0] + chn.feedback[1]) >> (10 - feedback);
    int32_t opout1 = chn.feedback_in = (int16_t)op_compute_volume<false>(o1, (o1.phase >> 10) + opmod, am_offset, am1);
    opmod = bitfield(r[0xc0 + c], 0) ? 0 : (opout1 >> 1);
    result = op_compute_volume<false>(o2, (o2.phase >> 10) + opmod, am_offset, am2) >> 1;
  }
  else if (c == 7)
  {
    uint32_t noise_state = bitfield(e.noise_lfsr >> 23, 0);
    uint32_t phase = (phase_select << 9) | (0xd0 >> (2 * (noise_state ^ phase_select)));
    result = op_compute_volume<false>(o1, phase, am_offset, am1) >> 1;
    uint32_t op13phase = o1.phase >> 10;
    phase = (0x100 << bitfield(op13phase, 8)) ^ (noise_state << 8);
    result += op_compute_volume<false>(o2, phase, am_offset, am2) >> 1;
    result = clamp(result, -32768, 32767);
  }
  else
  {
    result = op_compute_volume<false>(o1, o1.phase >> 10, am_offset, am1) >> 1;
    uint32_t phase = 0x100 | (phase_select << 9);
    result += op_compute_volume<false>(o2, phase, am_offset, am2) >> 1;
    result = clamp(result, -32768, 32767);
  }
  return result * 2;
}

// ym3526::generate: fm_engine_base::clock(ALL_CHANNELS), output(rshift 1,
// clipmax 32767), roundtrip_fp
void IRAM_ATTR opl_t::prepare_if_modified(uint32_t ticks)
{
  bool due = modified_channels != 0;
  if (!due)
  {
    due = prepare_count >= 4096;
    prepare_count += ticks;
  }
  if (due)
  {
    active_channels = 0;
    for (uint32_t c = 0; c < 9; c++)
    {
      uint32_t active_mask = 0;
      for (uint32_t o = 0; o < 2; o++)
      {
        op_t &p = op[s_opl_map[c][o]];
        opl_cache_operator_data(regs, c, p);
        op_clock_keystate<false>(p, (uint32_t)(p.keyon_live != 0), 0, 0);
        p.keyon_live &= ~(1 << KEYON_CSM);
        if (p.env_state != EG_RELEASE || p.env_attenuation < EG_QUIET)
          active_mask |= 1 << o;
      }
      if (active_mask != 0)
        active_channels |= 1 << c;
    }
    modified_channels = prepare_count = 0;
  }
}

void IRAM_ATTR opl_t::generate(bool out)
{
  total_clocks++;
  prepare_if_modified(1);

  // EG_CLOCK_DIVIDER = 1
  env_counter += 4;

  int32_t lfo_raw_pm = opl_clock_noise_and_lfo(*this);

  for (uint32_t c = 0; c < 9; c++)
  {
    ch[c].feedback[0] = ch[c].feedback[1];
    ch[c].feedback[1] = ch[c].feedback_in;
    for (uint32_t o = 0; o < 2; o++)
    {
      op_t &p = op[s_opl_map[c][o]];
      p.ssg_inverted = 0;
      // Silent operator: see opn_t::clock_fm. Operators 13 and 17 are never
      // skipped: rhythm mode reads their phases directly (phase_select, snare),
      // also right after it is switched on.
      uint32_t opnum = s_opl_map[c][o];
      if (opnum != 13 && opnum != 17 && p.env_state == EG_RELEASE && p.env_attenuation == 0x3ff)
        continue;
      if (bitfield(env_counter, 0, 2) == 0)
        op_clock_envelope<false>(p, env_counter >> 2, 0);
      uint32_t phase_step = p.phase_step;
      if (p.phase_dynamic)
        phase_step = opl_compute_phase_step(p.block_freq, p.multiple, lfo_raw_pm);
      p.phase += phase_step;
    }
  }

  if (!out)   // see opn_t::clock_fm
    return;
  int32_t sum = 0;
  if (bitfield(regs[0xbd], 5))   // rhythm_enable()
  {
    uint32_t op13phase = op[13].phase >> 10;
    uint32_t op17phase = op[17].phase >> 10;
    uint32_t phase_select = (bitfield(op13phase, 2) ^ bitfield(op13phase, 7)) | bitfield(op13phase, 3) | (bitfield(op17phase, 5) ^ bitfield(op17phase, 3));
    for (uint32_t c = 0; c < 9; c++)
      if (bitfield(active_channels, c))
        sum += (c >= 6) ? opl_output_rhythm(*this, c, phase_select) : opl_output_2op(*this, c);
  }
  else
  {
    for (uint32_t c = 0; c < 9; c++)
      if (bitfield(active_channels, c))
        sum += opl_output_2op(*this, c);
  }
  last_out = roundtrip_fp(sum);
}

// BOBLBOBL_SND_FAST: see opn_t::step_fast. LFO/noise and envelopes advance
// every chip sample; phase-modulated (vibrato) operators accumulate their
// per-sample step, the rest add n * phase_step.
void IRAM_ATTR opl_t::step_fast(uint32_t n)
{
  BBFM_T0();
  total_clocks += (uint8_t)n;
  prepare_if_modified(n);
  BBFM_T(3);

  for (uint32_t t = 0; t < n; t++)
  {
    env_counter += 4;
    int32_t lfo_raw_pm = opl_clock_noise_and_lfo(*this);
    for (uint32_t k = 0; k < 18; k++)
    {
      op_t &p = op[k];
      if (p.phase_dynamic)
        p.phase += opl_compute_phase_step(p.block_freq, p.multiple, lfo_raw_pm);
      if (p.env_state == EG_RELEASE && p.env_attenuation == 0x3ff)
        continue;
      if (p.env_state == EG_SUSTAIN && p.eg_rate[EG_SUSTAIN] == 0)
        continue;
      op_clock_envelope<false>(p, env_counter >> 2, 0);
    }
  }
  BBFM_T(4);

  for (uint32_t c = 0; c < 9; c++)
  {
    ch[c].feedback[0] = ch[c].feedback[1];
    ch[c].feedback[1] = ch[c].feedback_in;
  }
  for (uint32_t k = 0; k < 18; k++)
  {
    op[k].ssg_inverted = 0;
    if (!op[k].phase_dynamic)
      op[k].phase += op[k].phase_step * n;
  }

  int32_t sum = 0;
  if (bitfield(regs[0xbd], 5))   // rhythm_enable()
  {
    uint32_t op13phase = op[13].phase >> 10;
    uint32_t op17phase = op[17].phase >> 10;
    uint32_t phase_select = (bitfield(op13phase, 2) ^ bitfield(op13phase, 7)) | bitfield(op13phase, 3) | (bitfield(op17phase, 5) ^ bitfield(op17phase, 3));
    for (uint32_t c = 0; c < 9; c++)
      if (bitfield(active_channels, c))
        sum += (c >= 6) ? opl_output_rhythm(*this, c, phase_select) : opl_output_2op(*this, c);
  }
  else
  {
    for (uint32_t c = 0; c < 9; c++)
      if (bitfield(active_channels, c))
        sum += opl_output_2op(*this, c);
  }
  last_out = roundtrip_fp(sum);
  BBFM_T(5);
}

// ============================================================================
// SSG (ssg_engine)
// ============================================================================

void ssg_t::construct()
{
  memset(regs, 0, sizeof(regs));
  for (int c = 0; c < 3; c++)
  {
    tone_count[c] = 0;
    tone_state[c] = 0;
    out[c] = 0;
  }
  envelope_count = 0;
  envelope_state = 0;
  noise_count = 0;
  noise_state = 1;
}

void ssg_t::reset()
{
  construct();
}

void IRAM_ATTR ssg_t::write(uint8_t reg, uint8_t data)
{
  regs[reg & 0x0f] = data;
  if ((reg & 0x0f) == 0x0d)
    envelope_state = 0;
}

void IRAM_ATTR ssg_t::clock()
{
  for (int chan = 0; chan < 3; chan++)
  {
    tone_count[chan]++;
    uint32_t period = regs[0x00 + 2 * chan] | (bitfield(regs[0x01 + 2 * chan], 0, 4) << 8);
    if (tone_count[chan] >= period)
    {
      tone_state[chan] ^= 1;
      tone_count[chan] = 0;
    }
  }

  noise_count++;
  if ((noise_count >> 1) >= bitfield(regs[0x06], 0, 5) && noise_count != 1)
  {
    noise_state ^= (bitfield(noise_state, 0) ^ bitfield(noise_state, 3)) << 17;
    noise_state >>= 1;
    noise_count = 0;
  }

  envelope_count++;
  if (envelope_count >= (uint32_t)(regs[0x0b] | (regs[0x0c] << 8)))
  {
    envelope_state++;
    envelope_count = 0;
  }
}

void IRAM_ATTR ssg_t::output()
{
  if ((regs[0x08] | regs[0x09] | regs[0x0a]) == 0)
  {
    out[0] = out[1] = out[2] = 0;
    return;
  }
  static const DRAM_ATTR int16_t s_amplitudes[32] =
  {
       0,   32,   78,  141,  178,  222,  262,  306,
     369,  441,  509,  585,  701,  836,  965, 1112,
    1334, 1595, 1853, 2146, 2576, 3081, 3576, 4135,
    5000, 6006, 7023, 8155, 9963,11976,14132,16382
  };

  uint32_t hold = bitfield(regs[0x0d], 0);
  uint32_t alternate = bitfield(regs[0x0d], 1);
  uint32_t attack = bitfield(regs[0x0d], 2);
  uint32_t cont = bitfield(regs[0x0d], 3);

  uint32_t envelope_volume;
  if ((hold | (cont ^ 1)) && envelope_state >= 32)
  {
    envelope_state = 32;
    envelope_volume = ((attack ^ alternate) & cont) ? 31 : 0;
  }
  else
  {
    uint32_t a = attack;
    if (alternate)
      a ^= bitfield(envelope_state, 5);
    envelope_volume = (envelope_state & 31) ^ (a ? 0 : 31);
  }

  for (int chan = 0; chan < 3; chan++)
  {
    uint32_t noise_on = bitfield(regs[0x07], 3 + chan) | noise_state;
    uint32_t tone_on = bitfield(regs[0x07], 0 + chan) | tone_state[chan];
    uint32_t volume;
    if ((noise_on & tone_on & 1) == 0)
      volume = 0;
    else if (bitfield(regs[0x08 + chan], 4))
      volume = envelope_volume;
    else
    {
      volume = bitfield(regs[0x08 + chan], 0, 4) * 2;
      if (volume != 0)
        volume |= 1;
    }
    out[chan] = s_amplitudes[volume];
  }
}

} // namespace bbfm
