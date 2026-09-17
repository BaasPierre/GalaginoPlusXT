#include <math.h>

// ---------------------------------------------------------------------------
// Real ymfm tables, transcribed verbatim (see file header). Built once into
// integer form at gng::reset() time via ym_tables_init(); renderFmSample()'s
// hot path is pure integer table lookups, 24000x/sec on the audio task.
// ---------------------------------------------------------------------------

// abs_sin_attenuation: sin(phase), phase 0..2*PI mapped to 10 bits (0..1023),
// returned as a 4.8 fixed-point LOGARITHMIC attenuation (bigger = quieter).
// Table covers one quarter-wave (256 entries); the rest is derived by
// mirroring (ymfm_fm.ipp abs_sin_attenuation).
static const uint16_t s_sin_table[256] = {
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

// attenuation_to_volume: 5.8 fixed logarithmic attenuation -> 13-bit linear
// volume (ymfm_fm.ipp attenuation_to_volume). Mantissas pre-OR'd with the
// implicit leading bit and left-shifted by 2 (matches ymfm exactly).
#define YM_X(a) (((a) | 0x400) << 2)
static const uint16_t s_power_table[256] = {
  YM_X(0x3fa),YM_X(0x3f5),YM_X(0x3ef),YM_X(0x3ea),YM_X(0x3e4),YM_X(0x3df),YM_X(0x3da),YM_X(0x3d4),
  YM_X(0x3cf),YM_X(0x3c9),YM_X(0x3c4),YM_X(0x3bf),YM_X(0x3b9),YM_X(0x3b4),YM_X(0x3ae),YM_X(0x3a9),
  YM_X(0x3a4),YM_X(0x39f),YM_X(0x399),YM_X(0x394),YM_X(0x38f),YM_X(0x38a),YM_X(0x384),YM_X(0x37f),
  YM_X(0x37a),YM_X(0x375),YM_X(0x370),YM_X(0x36a),YM_X(0x365),YM_X(0x360),YM_X(0x35b),YM_X(0x356),
  YM_X(0x351),YM_X(0x34c),YM_X(0x347),YM_X(0x342),YM_X(0x33d),YM_X(0x338),YM_X(0x333),YM_X(0x32e),
  YM_X(0x329),YM_X(0x324),YM_X(0x31f),YM_X(0x31a),YM_X(0x315),YM_X(0x310),YM_X(0x30b),YM_X(0x306),
  YM_X(0x302),YM_X(0x2fd),YM_X(0x2f8),YM_X(0x2f3),YM_X(0x2ee),YM_X(0x2e9),YM_X(0x2e5),YM_X(0x2e0),
  YM_X(0x2db),YM_X(0x2d6),YM_X(0x2d2),YM_X(0x2cd),YM_X(0x2c8),YM_X(0x2c4),YM_X(0x2bf),YM_X(0x2ba),
  YM_X(0x2b5),YM_X(0x2b1),YM_X(0x2ac),YM_X(0x2a8),YM_X(0x2a3),YM_X(0x29e),YM_X(0x29a),YM_X(0x295),
  YM_X(0x291),YM_X(0x28c),YM_X(0x288),YM_X(0x283),YM_X(0x27f),YM_X(0x27a),YM_X(0x276),YM_X(0x271),
  YM_X(0x26d),YM_X(0x268),YM_X(0x264),YM_X(0x25f),YM_X(0x25b),YM_X(0x257),YM_X(0x252),YM_X(0x24e),
  YM_X(0x249),YM_X(0x245),YM_X(0x241),YM_X(0x23c),YM_X(0x238),YM_X(0x234),YM_X(0x230),YM_X(0x22b),
  YM_X(0x227),YM_X(0x223),YM_X(0x21e),YM_X(0x21a),YM_X(0x216),YM_X(0x212),YM_X(0x20e),YM_X(0x209),
  YM_X(0x205),YM_X(0x201),YM_X(0x1fd),YM_X(0x1f9),YM_X(0x1f5),YM_X(0x1f0),YM_X(0x1ec),YM_X(0x1e8),
  YM_X(0x1e4),YM_X(0x1e0),YM_X(0x1dc),YM_X(0x1d8),YM_X(0x1d4),YM_X(0x1d0),YM_X(0x1cc),YM_X(0x1c8),
  YM_X(0x1c4),YM_X(0x1c0),YM_X(0x1bc),YM_X(0x1b8),YM_X(0x1b4),YM_X(0x1b0),YM_X(0x1ac),YM_X(0x1a8),
  YM_X(0x1a4),YM_X(0x1a0),YM_X(0x19c),YM_X(0x199),YM_X(0x195),YM_X(0x191),YM_X(0x18d),YM_X(0x189),
  YM_X(0x185),YM_X(0x181),YM_X(0x17e),YM_X(0x17a),YM_X(0x176),YM_X(0x172),YM_X(0x16f),YM_X(0x16b),
  YM_X(0x167),YM_X(0x163),YM_X(0x160),YM_X(0x15c),YM_X(0x158),YM_X(0x154),YM_X(0x151),YM_X(0x14d),
  YM_X(0x149),YM_X(0x146),YM_X(0x142),YM_X(0x13e),YM_X(0x13b),YM_X(0x137),YM_X(0x134),YM_X(0x130),
  YM_X(0x12c),YM_X(0x129),YM_X(0x125),YM_X(0x122),YM_X(0x11e),YM_X(0x11b),YM_X(0x117),YM_X(0x114),
  YM_X(0x110),YM_X(0x10c),YM_X(0x109),YM_X(0x106),YM_X(0x102),YM_X(0x0ff),YM_X(0x0fb),YM_X(0x0f8),
  YM_X(0x0f4),YM_X(0x0f1),YM_X(0x0ed),YM_X(0x0ea),YM_X(0x0e7),YM_X(0x0e3),YM_X(0x0e0),YM_X(0x0dc),
  YM_X(0x0d9),YM_X(0x0d6),YM_X(0x0d2),YM_X(0x0cf),YM_X(0x0cc),YM_X(0x0c8),YM_X(0x0c5),YM_X(0x0c2),
  YM_X(0x0be),YM_X(0x0bb),YM_X(0x0b8),YM_X(0x0b5),YM_X(0x0b1),YM_X(0x0ae),YM_X(0x0ab),YM_X(0x0a8),
  YM_X(0x0a4),YM_X(0x0a1),YM_X(0x09e),YM_X(0x09b),YM_X(0x098),YM_X(0x094),YM_X(0x091),YM_X(0x08e),
  YM_X(0x08b),YM_X(0x088),YM_X(0x085),YM_X(0x082),YM_X(0x07e),YM_X(0x07b),YM_X(0x078),YM_X(0x075),
  YM_X(0x072),YM_X(0x06f),YM_X(0x06c),YM_X(0x069),YM_X(0x066),YM_X(0x063),YM_X(0x060),YM_X(0x05d),
  YM_X(0x05a),YM_X(0x057),YM_X(0x054),YM_X(0x051),YM_X(0x04e),YM_X(0x04b),YM_X(0x048),YM_X(0x045),
  YM_X(0x042),YM_X(0x03f),YM_X(0x03c),YM_X(0x039),YM_X(0x036),YM_X(0x033),YM_X(0x030),YM_X(0x02d),
  YM_X(0x02a),YM_X(0x028),YM_X(0x025),YM_X(0x022),YM_X(0x01f),YM_X(0x01c),YM_X(0x019),YM_X(0x016),
  YM_X(0x014),YM_X(0x011),YM_X(0x00e),YM_X(0x00b),YM_X(0x008),YM_X(0x006),YM_X(0x003),YM_X(0x000)
};
#undef YM_X

// attenuation_increment: 6-bit rate -> 4 increments (packed 4 bits each, one
// per env_counter substep 0..7 via 2 nibbles per bitfield call in the real
// code; ymfm packs 8 substeps into a 32-bit word, see attenuation_increment
// in ymfm_fm.ipp - transcribed verbatim below, same layout).
static const uint32_t s_increment_table[64] = {
  0x00000000, 0x00000000, 0x10101010, 0x10101010,  // 0-3    (0x00-0x03)
  0x10101010, 0x10101010, 0x11101110, 0x11101110,  // 4-7    (0x04-0x07)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 8-11   (0x08-0x0B)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 12-15  (0x0C-0x0F)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 16-19  (0x10-0x13)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 20-23  (0x14-0x17)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 24-27  (0x18-0x1B)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 28-31  (0x1C-0x1F)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 32-35  (0x20-0x23)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 36-39  (0x24-0x27)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 40-43  (0x28-0x2B)
  0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 44-47  (0x2C-0x2F)
  0x11111111, 0x21112111, 0x21212121, 0x22212221,  // 48-51  (0x30-0x33)
  0x22222222, 0x42224222, 0x42424242, 0x44424442,  // 52-55  (0x34-0x37)
  0x44444444, 0x84448444, 0x84848484, 0x88848884,  // 56-59  (0x38-0x3B)
  0x88888888, 0x88888888, 0x88888888, 0x88888888   // 60-63  (0x3C-0x3F)
};

static inline uint32_t ym_bitfield(uint32_t v, int start, int count) {
  return (v >> start) & ((1u << count) - 1u);
}
static inline uint32_t ym_attenuation_increment(uint32_t rate, uint32_t index) {
  return ym_bitfield(s_increment_table[rate], 4 * index, 4);
}

// abs_sin_attenuation, extended to a full 1024-entry table (10-bit phase
// index) so renderFmSample() is a single lookup instead of the mirroring
// branch every sample.
static uint16_t s_waveform[1024];       // full-cycle log-sine attenuation (4.8 fixed) + sign folded in via index range
static uint8_t  s_tables_ready = 0;

static void ym_tables_init() {
  if (s_tables_ready) return;
  for (int i = 0; i < 1024; i++) {
    int quarter = i & 0xff;
    if (i & 0x100) quarter = 0xff - quarter;   // mirror 2nd quarter of each half
    s_waveform[i] = s_sin_table[quarter];
  }
  s_tables_ready = 1;
}

// attenuation_to_volume - 5.8 fixed log attenuation -> 13-bit linear volume
// (ymfm_fm.ipp). Input already masked to 15 bits by the caller.
static inline uint32_t ym_attenuation_to_volume(uint32_t input) {
  return s_power_table[input & 0xff] >> (input >> 8);
}

// keycode + KSR-adjusted effective rate - ymfm_opn.cpp cache_operator_data(),
// unchanged from the previous version (this part was already correct).
static inline int ym_keycode(uint16_t fnum, uint8_t block) {
  uint32_t block_freq = ((uint32_t)(block & 7) << 11) | (fnum & 0x7ff);
  int kc = (int)((block_freq >> 10) & 0x0f) << 1;
  static const uint16_t magic = 0xfe80;
  kc |= (magic >> ((block_freq >> 7) & 0x0f)) & 1;
  return kc;
}
static inline int ym_effective_rate(int reg_rate_x, int ks, int keycode) {
  if (reg_rate_x == 0) return 0;
  int ksrval = keycode >> (ks ^ 3);
  int rate = reg_rate_x + ksrval;
  return rate > 63 ? 63 : rate;
}

// per-chip envelope clock counter (ymfm_fm.ipp fm_engine_base::m_env_counter),
// one per YM2203 (each chip clocks its own 3 channels independently).
static uint32_t s_env_counter[2];

// Standard Yamaha OPN algorithm connection table, transcribed from ymfm_fm.ipp
// s_algorithm_ops / output_4op() (only the first 8 OPN/OPM entries apply -
// OPL3's 8-11 are not used here). Encoding kept close to ymfm's ALGORITHM()
// macro: op2in (1 bit), op3in (3 bits), op4in (3 bits), plus which of
// opout[1]/[2]/[3] (=O1/O2/O3) also get added directly into the final sum.
struct algo_S {
  uint8_t op2in, op3in, op4in;   // index into opout[]: 0=none 1=O1 2=O2 3=O3 5=O1+O2 6=O1+O3 7=O2+O3
  uint8_t out1, out2, out3;      // 1 = include opout[1]/[2]/[3] (O1/O2/O3) in the final sum (O4 is always included)
};
static const algo_S s_algo[8] = {
  { 1, 2, 3,  0, 0, 0 },   // 0: O1->O2->O3->O4->out
  { 0, 5, 3,  0, 0, 0 },   // 1: (O1+O2)->O3->O4->out
  { 0, 2, 6,  0, 0, 0 },   // 2: (O1+(O2->O3))->O4->out
  { 1, 0, 7,  0, 0, 0 },   // 3: ((O1->O2)+O3)->O4->out
  { 1, 0, 3,  0, 1, 0 },   // 4: (O1->O2)+(O3->O4)->out (O2+O4)
  { 1, 1, 1,  0, 1, 1 },   // 5: O1->{O2,O3,O4} parallel ->out (O2+O3+O4)
  { 1, 0, 0,  0, 1, 1 },   // 6: (O1->O2)+O3+O4 ->out (O2+O3+O4)
  { 0, 0, 0,  1, 1, 1 },   // 7: O1+O2+O3+O4 ->out (all four)
};

// --- recompute the 10.10 phase step of all 4 ops of a channel -------------
// Ported from ymfm_opn.cpp compute_phase_step(): phase_step =
// (((fnum<<1) << block) >> 2) * multiple >> 1, where multiple = MUL*2 (or 1
// for MUL==0, i.e. x0.5). No detune (GnG's driver never sets it - reg 0x30
// bits 4-6 - so it is left at 0 here rather than porting the 32-entry
// detune table for an effect the game never uses).
void gng::ym_recalc_inc(int ch) {
  if (ch < 0 || ch >= 6) return;
  fm_ch_S &c = fm[ch];
  uint32_t block_freq = ((uint32_t)(c.block & 7) << 11) | (c.fnum & 0x7ff);
  uint32_t block = (block_freq >> 11) & 0x07;
  uint32_t fnum = (block_freq & 0x7ff) << 1;
  uint32_t base_step = (fnum << block) >> 2;
  base_step &= 0x1ffff;
  for (int o = 0; o < 4; o++) {
    uint32_t multiple = c.op[o].mul * 2;
    if (multiple == 0) multiple = 1;
    c.op[o].inc = (base_step * multiple) >> 1;   // 10.10 phase units per FM tick
  }
}

// Runs on the emulation core. Does NOT touch op[].phase/stage/env directly
// (see fm_ch_S::pending_key) - just latches the edge for renderFmSample()
// (audio core) to apply on its own next sample, avoiding a cross-core race
// where the audio core could observe a half-applied key-on.
void gng::ym_key(int ch, bool on) {
  fm_ch_S &c = fm[ch];
  if ((on && !c.keyon) || (!on && c.keyon)) {
    uint8_t slot = c.pending_head & 1;
    c.pending_key[slot] = on ? 1 : 0;
    c.pending_head++;
  }
  c.keyon = on;
}

// --- one YM2203 register write (chip 0 or 1) ----------------------------
// Also mirrors the SSG block into soundregs[] for audio.cpp's AY renderer.
void gng::ym_write(int chip, unsigned char reg, unsigned char val) {
  ym_regs[chip][reg] = val;

  // SSG (0x00-0x0f) == AY-3-8910 layout -> soundregs[chip*16 + reg]
  if (reg < 0x10) {
    soundregs[chip * 16 + reg] = val;
    return;
  }

#ifdef GNG_FM_LOG
  if (reg == 0x28 || (reg >= 0x40 && reg <= 0x8f) ||
      (reg >= 0xa0 && reg <= 0xa6) || (reg >= 0xb0 && reg <= 0xb2)) {
    static uint32_t n = 0;
    if (n++ < 4000)
      Serial.printf("YM%d r%02X=%02X\n", chip, reg, val);
  }
#endif

  const int chbase = chip * 3;            // fm[] index of this chip's ch 0

  // key on/off : reg 0x28, low 3 bits = channel (0..2, 4..6 for chip 2 on
  // OPNA - N/A here), bits 4-7 = op enable mask
  if (reg == 0x28) {
    int ch = val & 0x03;
    if (ch == 3) return;                  // 0x03 is not a valid channel
    ym_key(chbase + ch, (val & 0xf0) != 0);
    return;
  }

  // per-operator regs : channel = reg&3, slot = (reg>>2)&3
  // OPN hardware slot order is 1,3,2,4 -> slot index maps directly to our
  // op[0..3] (see fm_ch_S comment in gng.h).
  int ch  = reg & 0x03;
  int slot = (reg >> 2) & 0x03;
  if (ch != 3 && reg >= 0x30 && reg <= 0x9f) {
    int o = slot;
    fm_op_S &op = fm[chbase + ch].op[o];
    switch (reg & 0xf0) {
      case 0x30: op.mul = val & 0x0f; ym_recalc_inc(chbase + ch); break;   // DT/MUL (detune ignored, see ym_recalc_inc)
      case 0x40: op.tl  = val & 0x7f; break;                               // TL
      case 0x50: op.ar = val & 0x1f; op.ks = (val >> 6) & 0x03; break;     // KS/AR
      case 0x60: op.dr  = val & 0x1f; break;                               // AM/DR
      case 0x70: op.sr  = val & 0x1f; break;                               // SR
      case 0x80: op.rr  = (val & 0x0f) * 4 + 2; op.sl = (val >> 4) & 0x0f; break; // SL/RR (RR pre-scaled *4+2, matches ymfm_opn.cpp op_release_rate()*4+2)
      case 0x90: break;                                                     // SSG-EG (GnG's driver never sets EG_HAS_SSG per-op enable, not ported)
    }
    return;
  }

  // channel regs
  if (ch != 3 && reg >= 0xa0 && reg <= 0xa2) {           // F-number low
    fm[chbase + ch].fnum = (fm[chbase + ch].fnum & 0x700) | val;
    ym_recalc_inc(chbase + ch);
    return;
  }
  if (ch != 3 && reg >= 0xa4 && reg <= 0xa6) {           // F-number high + block
    fm[chbase + ch].fnum  = (fm[chbase + ch].fnum & 0x0ff) | ((val & 0x07) << 8);
    fm[chbase + ch].block = (val >> 3) & 0x07;
    ym_recalc_inc(chbase + ch);
    return;
  }
  if (ch != 3 && reg >= 0xb0 && reg <= 0xb2) {           // feedback / algorithm
    fm[chbase + ch].fb   = (val >> 3) & 0x07;
    fm[chbase + ch].algo =  val & 0x07;
    return;
  }
}

// Per-frame envelope stepping was removed when envelope clocking moved to
// per-FM-tick (renderFmSample, driven by env_counter - see ym_step_env())
// to avoid a once-per-frame audible jump. Kept as a no-op entry point since
// machineBase/gng.cpp call it once per 60 Hz frame.
void gng::ym_frame(void) {
}

#ifndef GNG_FM_VOL
#define GNG_FM_VOL 4
#endif

// --- clock one operator's envelope by one FM tick -------------------------
// Ported from ymfm_fm.ipp fm_operator::clock_envelope(), exactly including
// the OPN envelope clock divider: real hardware only advances env_counter's
// effective value every 3rd FM tick (EG_CLOCK_DIVIDER=3 in ymfm_opn.h), and
// even then only fires an actual attenuation step once every 1<<rate_shift
// ticks (the "11-bit fractional part must be zero" gate below) - so this
// is NOT a simple per-sample linear ramp like earlier versions of this file
// used; it reproduces the same coarse, rate-dependent stepping real OPN
// hardware has (audible as the characteristic "stepped" YM2203 envelope,
// not a perfectly smooth ramp).
// env_counter is a per-CHIP (not per-operator) counter that increases by 4
// every FM tick when EG_CLOCK_DIVIDER==1, or wraps through a %3 pattern
// when ==3 (ymfm_fm.ipp fm_engine_base::clock()) - reproduced in
// renderFmSample() via s_env_counter[2] (one per YM2203 chip).
static inline void ym_clock_envelope(gng::fm_op_S &op, uint32_t env_counter, int keycode) {
  // attack -> decay transition
  if (op.stage == 1 && op.env == 0)
    op.stage = 2;
  // decay -> sustain transition (before decay's own step, matches ymfm)
  if (op.stage == 2 && (uint32_t)op.env >= op.sl_units)
    op.stage = 3;

  int rate;
  switch (op.stage) {
    case 1: rate = ym_effective_rate(op.ar * 2, op.ks, keycode); break;
    case 2: rate = ym_effective_rate(op.dr * 2, op.ks, keycode); break;
    case 3: rate = ym_effective_rate(op.sr * 2, op.ks, keycode); break;
    case 4: rate = ym_effective_rate(op.rr,     op.ks, keycode); break;  // rr already *4+2
    default: return;                                                     // stage 0 = off
  }
  if (rate == 0)
    return;

  uint32_t rate_shift = rate >> 2;
  uint32_t counter = env_counter << rate_shift;

  // only clock when the low 11 bits are all zero (matches ymfm exactly)
  if (ym_bitfield(counter, 0, 11) != 0)
    return;

  uint32_t relevant_bits = ym_bitfield(counter, (rate_shift <= 11) ? 11 : rate_shift, 3);
  uint32_t increment = ym_attenuation_increment(rate, relevant_bits);

  if (op.stage == 1) {                       // attack: multiplicative approach to 0
    if (rate < 62)
      op.env += (int)(((~(uint32_t)op.env) * increment) >> 4);
  } else {                                   // decay/sustain/release: additive
    op.env += increment;
    if (op.env >= 0x400) op.env = 0x3ff;
  }
}

// one 10.10-phase operator: advances phase+envelope by one FM tick and
// returns its 14-bit signed output sample, given a phase-modulation input.
// modin is a MODULATION-DOMAIN value already in the same units as the top
// 10 bits of phase (i.e. the modulating operator's raw output right-shifted
// by 1, exactly ymfm's "opmod = opout[x] >> 1" - the caller passes that
// shift already applied, see the >>1 at each call site below).
// Ported from ymfm_fm.ipp fm_operator::compute_volume() / clock_phase().
static inline int32_t ym_op_output(gng::fm_op_S &op, uint32_t env_counter, int keycode, int32_t modin) {
  if (op.stage == 0)
    return 0;

  ym_clock_envelope(op, env_counter, keycode);

  uint32_t phase = (uint32_t)((int32_t)(op.phase >> 10) + modin) & 0x3ff;   // top 10 bits + modulation = waveform index
  op.phase = (op.phase + op.inc) & 0xfffff;                                 // 10.10 phase accumulator (20 bits)

  // EG_QUIET early-out (0x380 in ymfm units)
  if (op.env > 0x380)
    return 0;

  uint32_t sin_attenuation = s_waveform[phase];      // 4.8 fixed, sign folded via phase range (upper half already mirrored in table build)
  bool negative = (phase & 0x200) != 0;               // 2nd half of the cycle -> negative output
  uint32_t env_attenuation = (uint32_t)(op.env + op.tl * 8) << 2;  // env(0..1023,10bit)+TL*8 -> 4.6, shifted to 4.8
  uint32_t combined = (sin_attenuation + env_attenuation) & 0x7fff;
  int32_t result = (int32_t)ym_attenuation_to_volume(combined);
  return negative ? -result : result;
}

int gng::renderFmSample() {
  if (!s_tables_ready)
    return 0;

  int32_t mix = 0;

  for (int ch = 0; ch < 6; ch++) {
    fm_ch_S &c = fm[ch];

    // drain pending key-on/off edges latched by ym_key() (emulation core) -
    // see fm_ch_S::pending_key for why a 2-deep ring is needed here.
    while (c.pending_tail != c.pending_head) {
      uint8_t slot = c.pending_tail & 1;
      int8_t pk = c.pending_key[slot];
      c.pending_key[slot] = -1;
      c.pending_tail++;
      if (pk == 1) {
        int keycode0 = ym_keycode(c.fnum, c.block);
        for (int o = 0; o < 4; o++) {
          c.op[o].phase = 0;
          c.op[o].stage = 1;      // attack
          c.op[o].env   = 0x3ff;  // 1023 = max attenuation (silent), ramps toward 0
          // sustain level cached in "units" (10-bit envelope scale): 4-bit
          // SL register -> 5-bit (15 means 31) -> shifted to the 10-bit
          // envelope range, per ymfm_opn.cpp cache_operator_data().
          int sl5 = c.op[o].sl | (((c.op[o].sl + 1) & 0x10));
          c.op[o].sl_units = (uint32_t)sl5 << 5;
          // ymfm_fm.ipp start_attack(): "if the attack rate >= 62 then
          // immediately go to max attenuation [i.e. env=0, loudest]" - the
          // normal clock_envelope() increment path is disabled outright for
          // rate>=62 (see its "if (rate < 62)" guard), so without this an
          // instant-attack note would get stuck at env=0x3ff (silent)
          // forever instead of jumping straight to full volume.
          int ar_rate = ym_effective_rate(c.op[o].ar * 2, c.op[o].ks, keycode0);
          if (ar_rate >= 62)
            c.op[o].env = 0;
        }
        c.feedback[0] = c.feedback[1] = 0;
      } else if (pk == 0) {
        for (int o = 0; o < 4; o++)
          if (c.op[o].stage != 0)
            c.op[o].stage = 4;    // release
      }
    }

    fm_op_S &o1 = c.op[0];
    fm_op_S &o2 = c.op[2];
    fm_op_S &o3 = c.op[1];
    fm_op_S &o4 = c.op[3];

    int keycode = ym_keycode(c.fnum, c.block);

    // env_counter: per-chip counter incremented once per FM tick, gated by
    // OPN's EG_CLOCK_DIVIDER=3 (ymfm_fm.ipp fm_engine_base::clock()) - only
    // 2 of every 3 ticks actually advance it (by 4), matching real hardware's
    // coarser envelope clock relative to the phase clock.
    int chip = ch / 3;
    uint32_t &ec = s_env_counter[chip];
    if (ch % 3 == 0) {   // advance the shared per-chip counter once per chip per tick
      uint32_t nc = ec + 1;
      if ((nc & 0x03) == 3)
        ec = nc + (4 - 3);
      else
        ec = nc;
    }

    // feedback: op1 self-modulates from the average of its own last 2
    // output samples, shifted by (10-feedback) - ymfm_fm.ipp output_4op()
    // computes this as the same "opmod" that gets added directly to phase(),
    // so no further scaling is applied here.
    int32_t opmod1 = 0;
    if (c.fb != 0)
      opmod1 = (c.feedback[0] + c.feedback[1]) >> (10 - c.fb);

    int32_t v1 = ym_op_output(o1, ec, keycode, opmod1);
    c.feedback[0] = c.feedback[1];
    c.feedback[1] = (int16_t)v1;

    const algo_S &al = s_algo[c.algo & 0x07];

    // each modulation input is the selected running sum's raw output,
    // right-shifted by 1 - ymfm_fm.ipp output_4op()'s "opmod = opout[x] >> 1".
    int32_t opout1 = v1;
    int32_t opout2 = ym_op_output(o2, ec, keycode, (al.op2in ? opout1 : 0) >> 1);
    int32_t opout5 = opout1 + opout2;

    int32_t in3 = (al.op3in == 1) ? opout1 : (al.op3in == 2) ? opout2 : (al.op3in == 5) ? opout5 : 0;
    int32_t opout3 = ym_op_output(o3, ec, keycode, in3 >> 1);
    int32_t opout6 = opout1 + opout3;
    int32_t opout7 = opout2 + opout3;

    int32_t in4 = (al.op4in == 1) ? opout1 : (al.op4in == 3) ? opout3 :
                  (al.op4in == 6) ? opout6 : (al.op4in == 7) ? opout7 : 0;
    int32_t opout4 = ym_op_output(o4, ec, keycode, in4 >> 1);

    int32_t chan_out = opout4;   // O4 is always part of the sum
    if (al.out1) chan_out += opout1;
    if (al.out2) chan_out += opout2;
    if (al.out3) chan_out += opout3;

    // clamp per-channel to the real chip's 14-bit signed range before
    // summing channels together (ymfm_fm.ipp output_4op() clamps at each
    // partial-sum step; clamping once per channel here is equivalent since
    // all partial sums here are already-clamped operator outputs added at
    // most 4 times).
    if (chan_out > 8191) chan_out = 8191;
    if (chan_out < -8192) chan_out = -8192;

    mix += chan_out;
  }

  // YM2203-specific output stage (ymfm_opn.cpp ym2203::generate(): rshift=0,
  // clipmax=32767) - both chips summed with no per-chord division, matching
  // real hardware. Scaled down once here for this port's headroom.
  mix = (mix * GNG_FM_VOL) >> 7;

  // Soft-knee limiter: pass anything under GNG_FM_KNEE through unchanged,
  // asymptotically compress anything above it toward GNG_FM_CEIL instead of
  // hard-clamping (which flat-tops -> an audible click on every overshoot).
#define GNG_FM_KNEE 300
#define GNG_FM_CEIL 450
  {
    int m = mix, sign = 1;
    if (m < 0) { m = -m; sign = -1; }
    if (m > GNG_FM_KNEE) {
      int over = m - GNG_FM_KNEE;
      int span = GNG_FM_CEIL - GNG_FM_KNEE;
      m = GNG_FM_KNEE + (span * over) / (over + span);
    }
    mix = sign * m;
  }
  fm_last = mix;
  return mix;
}
