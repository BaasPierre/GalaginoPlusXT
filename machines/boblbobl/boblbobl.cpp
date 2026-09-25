#pragma GCC optimize("-O2")
#include "boblbobl.h"

static_assert(BOBLBOBL_MEM_END <= RAMSIZE, "RAMSIZE too low for boblbobl");
static_assert(BOBLBOBL_OBJECTRAM == BOBLBOBL_VIDEORAM + 0x1d00, "videoram/objectram must be contiguous (c000-dfff)");

#include <atomic>
#include <stdlib.h>
#include <stdio.h>

#if !BOBLBOBL_DBG_HARNESS
#include <xtensa/core-macros.h>
#define BB_CYCLES() ((uint32_t)XTHAL_GET_CCOUNT())   // CPU cycles (240 per us)
#else
#define BB_CYCLES() 0u
#endif
#if BOBLBOBL_PROFILE && !BOBLBOBL_DBG_HARNESS
#define BB_CCOUNT() ((uint32_t)XTHAL_GET_CCOUNT())
#define BB_PROF 1
#else
#define BB_CCOUNT() 0u
#define BB_PROF 0
#endif
// cycle totals; the render/audio ones are written on core 0, read on core 1
static uint32_t bb_prof_main, bb_prof_sub, bb_prof_audio, bb_prof_frame, bb_prof_frames;
static volatile uint32_t bb_prof_scan, bb_prof_blit, bb_prof_snd, bb_prof_snd_samples, bb_prof_screens, bb_prof_spans;
static uint32_t bb_prof_snd1, bb_prof_snd1_samples;   // sound rendered on core 1 (snd_render_ahead)
static volatile uint32_t bb_prof_snd_under;            // core 0 had nothing emulated to play (last sample repeated)

static void bb_build_tile_empty(void);

// boblbobl_z80.c: the shared Z80 core built with per-CPU 2KB page tables
// (a page = direct pointer to the bytes main_rd/sub_rd/audio_rd - or the
// _wr handlers - would use, 0 = call the machine). See that file.
extern "C" {
extern const unsigned char *bbz_rd_page[3][32];
extern unsigned char *bbz_wr_page[3][32];
extern const unsigned char *const *bbz_rd_cur;
extern unsigned char *const *bbz_wr_cur;
void bbz_Step(Z80 *R);
void bbz_Int(Z80 *R, unsigned short Vector);
void bbz_Reset(Z80 *R);
}

// The fixed ROM bytes (0000-7fff) of CPU c, through its page table
#define BB_ROM(c, a) (bbz_rd_page[c][(a) >> 11][(a) & 0x7ff])

boblbobl::~boblbobl()
{
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 4; k++)
      free(rom_ram[i][k]);
}

// The three fixed 32KB ROMs to internal RAM in 8KB pieces (the heap is
// fragmented - a 32KB block often does not exist), each piece falling back
// to flash on its own. The page tables point at the copies.
void boblbobl::roms_to_ram(void)
{
  // most-executed first: sound CPU, main CPU, sub CPU
  static const unsigned char *const src[3] = { boblbobl_audiocpu, boblbobl_maincpu, boblbobl_subcpu };
  static const unsigned char cpu_of[3] = { 2, 0, 1 };
  int in_ram[3] = { 0, 0, 0 };
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 4; k++)
    {
      if (!rom_ram[i][k])
      {
        rom_ram[i][k] = (unsigned char *)malloc(0x2000);
        if (rom_ram[i][k])
          memcpy(rom_ram[i][k], src[i] + k * 0x2000, 0x2000);
      }
      const unsigned char *base = rom_ram[i][k] ? rom_ram[i][k] : src[i] + k * 0x2000;
      in_ram[i] += rom_ram[i][k] != 0;
      for (int p = 0; p < 4; p++)
        bbz_rd_page[cpu_of[i]][k * 4 + p] = base + p * 0x800;
    }
  printf("boblbobl: ROMs in RAM (8KB pieces of 4): audio=%d main=%d sub=%d\n", in_ram[0], in_ram[1], in_ram[2]);
}

// Page tables of everything else (see boblbobl_z80.c). Reads and writes
// that have side effects or are I/O stay on the handlers (page 0):
//  main: 8000-bfff = the current ROM bank (bankswitch_w), c000-dfff video /
//        object RAM (writes only once game_started is set - main_wr sets it
//        on the first non-zero video RAM write), e000-f7ff shared RAM;
//        f800-ffff palette (recalc on write) / latches / I/O: handlers
//  sub:  e000-f7ff shared RAM, the rest handlers
//  audio: 8000-8fff RAM, the rest handlers (YM chips, latches)
void boblbobl::build_pages(void)
{
  for (int c = 0; c < 3; c++)
    for (int p = 16; p < 32; p++)
      bbz_rd_page[c][p] = 0;
  memset(bbz_wr_page, 0, sizeof(bbz_wr_page));
  set_bank_pages();
  for (int p = 0; p < 4; p++)
    bbz_rd_page[0][24 + p] = &memory[BOBLBOBL_VIDEORAM + p * 0x800];
  if (game_started)
    for (int p = 0; p < 4; p++)
      bbz_wr_page[0][24 + p] = &memory[BOBLBOBL_VIDEORAM + p * 0x800];
  for (int p = 0; p < 3; p++)
  {
    bbz_rd_page[0][28 + p] = bbz_rd_page[1][28 + p] = &memory[BOBLBOBL_SHARERAM + p * 0x800];
    bbz_wr_page[0][28 + p] = bbz_wr_page[1][28 + p] = &memory[BOBLBOBL_SHARERAM + p * 0x800];
  }
  for (int p = 0; p < 2; p++)
  {
    bbz_rd_page[2][16 + p] = &memory[BOBLBOBL_AUDIO_RAM + p * 0x800];
    bbz_wr_page[2][16 + p] = &memory[BOBLBOBL_AUDIO_RAM + p * 0x800];
  }
}

void IRAM_ATTR boblbobl::set_bank_pages(void)
{
  for (int p = 0; p < 8; p++)
    bbz_rd_page[0][16 + p] = m_bankptr + p * 0x800;
}

// current_cpu and the private core's page tables together
void IRAM_ATTR boblbobl::set_cpu(int c)
{
  current_cpu = (char)c;
  bbz_rd_cur = bbz_rd_page[c];
  bbz_wr_cur = bbz_wr_page[c];
}

void boblbobl::reset()
{
  machineBase::reset();
  roms_to_ram();
  bb_build_tile_empty();
  for (int b = 0; b < 36; b++)
    band_head[b] = band_tail[b] = SPAN_END;

  m_bank = 0;
  m_bankptr = boblbobl_maincpu + 0x10000;
  build_pages();
  m_flip = false;
  m_video_enable = false;
  sub_held_reset = true;
  coinBackup = 0;
  coinFrameCounter = 0;

  m_ic43_a = 0;
  m_ic43_b = 0;

  main_to_sound = 0;
  main_to_sound_pending = false;
  sound_to_main = 0;
  sound_to_main_pending = false;
  sound_nmi_enable = false;
  sound_nmi_line = false;
  main_irq_pending = false;
  aud_prev_idle = false;
  aud_prev_clk = 0;
  sub_irq_pending = false;

  ym_addr = 0;
  opl_addr = 0;
  snd_opn.construct();
  snd_ssg.construct();
  snd_opl.construct();
  snd_head = snd_tail = 0;
  snd_out_wr = snd_out_rd = 0;
  snd_last = 0;
  snd_lock.store(0);
  pace_init = false;
  snd_emu_clk_pub = 0;
  snd_evt_clk = 0;
  snd_evt_started = false;
  snd_ssg_cd = 0;
  snd_fm_cd = 0;
  ym_chips_reset();

  // MAME MACHINE_RESET(common): m_sreset_old starts CLEAR, so /SRESET is
  // pulsed (assert then clear). The sound CPU therefore runs from power-on;
  // it is NOT held until the main CPU writes 0xfa03.
  sreset_old = false;
  ym_chip_clocks = 0;   // ymfm m_total_clocks: counts from power-on, not reset by chip reset
  sound_sreset(true);
  sound_sreset(false);

  memset(pal_cpu, 0, sizeof(pal_cpu));
  memset(pal_shadow, 0, sizeof(pal_shadow));
  palette_front = 0;
  memset(vram_shadow, 0, sizeof(vram_shadow));
  vram_front = 0;
  span_count = 0;

  set_cpu(0);
}

void boblbobl::stop()
{
}

void IRAM_ATTR boblbobl::bankswitch_w(unsigned char data)
{
  m_bank = (data ^ 4) & 7;
  m_bankptr = boblbobl_maincpu + 0x10000 + m_bank * 0x4000;
  set_bank_pages();

  if (data & 0x10)
  {
    sub_held_reset = false;
  }
  else
  {
    sub_held_reset = true;
    bbz_Reset(&cpu[1]);
  }

  m_video_enable = (data & 0x40) != 0;

  m_flip = (data & 0x80) != 0;
}

unsigned char boblbobl::ic43_a_r(unsigned char offset)
{
  if (offset == 0)
    return (unsigned char)(m_ic43_a << 4);
  return (unsigned char)(random() & 0xff);
}

void boblbobl::ic43_a_w(unsigned char offset, unsigned char data)
{
  (void)data;
  int res = 0;
  switch (offset & 3)
  {
  case 0:
    if (~m_ic43_a & 8)
      res ^= 1;
    if (~m_ic43_a & 1)
      res ^= 2;
    if (~m_ic43_a & 1)
      res ^= 4;
    if (~m_ic43_a & 2)
      res ^= 4;
    if (~m_ic43_a & 4)
      res ^= 8;
    break;
  case 1:
    if (~m_ic43_a & 8)
      res ^= 1;
    if (~m_ic43_a & 2)
      res ^= 1;
    if (~m_ic43_a & 8)
      res ^= 2;
    if (~m_ic43_a & 1)
      res ^= 4;
    if (~m_ic43_a & 4)
      res ^= 8;
    break;
  case 2:
    if (~m_ic43_a & 4)
      res ^= 1;
    if (~m_ic43_a & 8)
      res ^= 2;
    if (~m_ic43_a & 2)
      res ^= 4;
    if (~m_ic43_a & 1)
      res ^= 8;
    if (~m_ic43_a & 4)
      res ^= 8;
    break;
  case 3:
    if (~m_ic43_a & 2)
      res ^= 1;
    if (~m_ic43_a & 4)
      res ^= 2;
    if (~m_ic43_a & 8)
      res ^= 2;
    if (~m_ic43_a & 8)
      res ^= 4;
    if (~m_ic43_a & 1)
      res ^= 8;
    break;
  }
  m_ic43_a = res;
}

void boblbobl::ic43_b_w(unsigned char offset, unsigned char data)
{
  static const int xorval[4] = {4, 1, 8, 2};
  m_ic43_b = ((data >> 4) ^ xorval[offset & 3]) & 0xff;
}

unsigned char boblbobl::ic43_b_r(unsigned char offset)
{
  if (offset == 0)
    return (unsigned char)(m_ic43_b << 4);
  return 0xff;
}

void IRAM_ATTR boblbobl::palette_recalc(unsigned short entry)
{
  unsigned char hi = memory[BOBLBOBL_PALETTERAM + entry * 2];
  unsigned char lo = memory[BOBLBOBL_PALETTERAM + entry * 2 + 1];

  unsigned char r4 = (hi >> 4) & 0x0f;
  unsigned char g4 = hi & 0x0f;
  unsigned char b4 = (lo >> 4) & 0x0f;

  unsigned char r8 = (r4 << 4) | r4;
  unsigned char g8 = (g4 << 4) | g4;
  unsigned char b8 = (b4 << 4) | b4;

  unsigned short c = ((r8 & 0xf8) << 8) | ((g8 & 0xfc) << 3) | (b8 >> 3);
  // Store byte-swapped: Video::write() DMAs the uint16_t frame_buffer as raw
  // little-endian bytes, but the ILI9341/ST7789 (COLMOD 0x55) takes the RGB565
  // high byte first on the wire.
  pal_cpu[entry] = (unsigned short)((c >> 8) | (c << 8));
}

void boblbobl::publish_palette(void)
{
  unsigned char back = palette_front ^ 1;
  memcpy(pal_shadow[back], pal_cpu, sizeof(pal_cpu));
  palette_front = back;
}

void boblbobl::publish_vram(void)
{
  unsigned char back = vram_front ^ 1;
  // c000-dcff videoram and dd00-dfff objectram, one consistent snapshot.
  memcpy(vram_shadow[back], &memory[BOBLBOBL_VIDEORAM], sizeof(vram_shadow[0]));
  vram_front = back;
}

// MAME: m_soundnmi is INPUT_MERGER_ALL_HIGH(in0 = NMI enable latch set by
// 0xb001 / cleared by 0xb002, in1 = main_to_sound data_pending). Its output
// drives INPUT_LINE_NMI, which the Z80 takes on the rising edge.
void IRAM_ATTR boblbobl::update_sound_nmi(void)
{
  bool line = sound_nmi_enable && main_to_sound_pending;
  if (line && !sound_nmi_line && !audio_held_reset)
  {
    int saved_cpu = current_cpu;
    set_cpu(2);
    bbz_Int(&cpu[2], INT_NMI);
    set_cpu(saved_cpu);
  }
  sound_nmi_line = line;
}

// MAME bublbobl_m.cpp common_sreset(), reached via bublbobl_soundcpu_reset_w().
void boblbobl::sound_sreset(bool assert_line)
{
  if (assert_line && !sreset_old)
  {
    ym_chips_reset();                 // ym2203 + ym3526 reset
    sound_to_main_pending = false;    // m_sound_to_main->acknowledge_w()
    sound_nmi_enable = false;         // m_soundnmi->in_w<0>(0)
  }
  if (assert_line)
  {
    audio_held_reset = true;          // audiocpu INPUT_LINE_RESET asserted
    bbz_Reset(&cpu[2]);
  }
  else
  {
    audio_held_reset = false;
  }
  sreset_old = assert_line;
  update_sound_nmi();
}

// ---- YM2203 / YM3526 timers (3rdparty/ymfm/src/ymfm_fm.ipp) ----
// update_timer() programs period * OPERATORS * clock_prescale chip clocks.
//   YM2203 (OPN): OPERATORS=12, prescale 6 -> x72
//     timer A = (1024 - {0x24,0x25[1:0]}) * 72, timer B = 16 * (256 - 0x26) * 72
//   YM3526 (OPL): OPERATORS=18, prescale 4 -> x72
//     timer A = (1024 - 0x02 * 4) * 72,        timer B = 16 * (256 - 0x03) * 72
// Both chips run at MAIN_XTAL/8 = 3MHz, the audio Z80's own clock, so the
// counters below are decremented by audio-CPU cycles.
static inline IRAM_ATTR int32_t opn_timer_period(const unsigned char *r, int tnum)
{
  if (tnum == 0)
    return (1024 - (((int32_t)r[0x24] << 2) | (r[0x25] & 3))) * 72;
  return 16 * (256 - (int32_t)r[0x26]) * 72;
}
static inline IRAM_ATTR int32_t opl_timer_period(const unsigned char *r, int tnum)
{
  if (tnum == 0)
    return (1024 - (int32_t)r[0x02] * 4) * 72;
  return 16 * (256 - (int32_t)r[0x03]) * 72;
}

void IRAM_ATTR boblbobl::opn_update_timer(int tnum, bool enable, int32_t delta_samples)
{
  if (enable && !opn_timer[tnum].running)
  {
    opn_timer[tnum].remain = opn_timer_period(ym_regs, tnum) + delta_samples * 72;
    opn_timer[tnum].running = true;
  }
  else if (!enable)
    opn_timer[tnum].running = false;
}

void IRAM_ATTR boblbobl::opl_update_timer(int tnum, bool enable, int32_t delta_samples)
{
  if (enable && !opl_timer[tnum].running)
  {
    opl_timer[tnum].remain = opl_timer_period(opl_regs, tnum) + delta_samples * 72;
    opl_timer[tnum].running = true;
  }
  else if (!enable)
    opl_timer[tnum].running = false;
}

// OPN REG_MODE 0x27: bit0/1 load A/B, bit2/3 enable A/B flag, bit4/5 reset
// A/B flag (ymfm engine_mode_write()). OPN has no irq_reset bit.
void IRAM_ATTR boblbobl::opn_mode_write(unsigned char data)
{
  ym_regs[0x27] = data;
  unsigned char reset_mask = 0;
  if (data & 0x20) reset_mask |= 0x02;
  if (data & 0x10) reset_mask |= 0x01;
  opn_status &= ~reset_mask;
  // "timer B gets a small negative adjustment because the *16 multiplier is
  // free-running": -(m_total_clocks & 15) FM samples (1 sample = 72 clocks).
  opn_update_timer(1, (data & 0x02) != 0, -(int32_t)((ym_chip_clocks / 72) & 15));
  opn_update_timer(0, (data & 0x01) != 0, 0);
}

// OPL REG_MODE 0x04: bit7 = IRQ reset (ymfm keeps the register's low bits and
// only ORs in 0x80, then clears status bits 0x78). Otherwise bit6/5 reset the
// A/B flags (and mask them: status_mask = reg & 0x78), bit0/1 load A/B.
// OPL timers always set their flag on expiry (enable_timer_x() == 1).
void IRAM_ATTR boblbobl::opl_mode_write(unsigned char data)
{
  if (data & 0x80)
  {
    opl_regs[0x04] |= 0x80;
    opl_status &= ~0x78;
    return;
  }
  opl_regs[0x04] = data;
  unsigned char reset_mask = 0;
  if (data & 0x20) reset_mask |= 0x20;
  if (data & 0x40) reset_mask |= 0x40;
  opl_status &= ~reset_mask;
  // "timer B gets a small negative adjustment because the *16 multiplier is
  // free-running": -(m_total_clocks & 15) FM samples (1 sample = 72 clocks).
  opl_update_timer(1, (data & 0x02) != 0, -(int32_t)((ym_chip_clocks / 72) & 15));
  opl_update_timer(0, (data & 0x01) != 0, 0);
}

void IRAM_ATTR boblbobl::ym_timers_advance(int32_t clocks)
{
  if (snd_sync && clocks > 0)
  {
    int32_t a = 0, b = 0, c = 0;
    int na = 0, nc = 0;
    snd_step_clocks((uint32_t)clocks, a, b, na, c, nc);
  }
  ym_chip_clocks += (uint32_t)clocks;
  for (int t = 0; t < 2; t++)
  {
    if (opn_timer[t].running)
    {
      opn_timer[t].remain -= clocks;
      while (opn_timer[t].remain <= 0)
      {
        // engine_timer_expired(): set the flag only if enabled, then reload
        if (t == 0 && (ym_regs[0x27] & 0x04)) opn_status |= 0x01;
        if (t == 1 && (ym_regs[0x27] & 0x08)) opn_status |= 0x02;
        opn_timer[t].remain += opn_timer_period(ym_regs, t);
      }
    }
    if (opl_timer[t].running)
    {
      opl_timer[t].remain -= clocks;
      while (opl_timer[t].remain <= 0)
      {
        opl_status |= (t == 0) ? 0x40 : 0x20;
        opl_timer[t].remain += opl_timer_period(opl_regs, t);
      }
    }
  }
}

// ymfm engine_check_interrupts(): irq = status & irq_mask(TIMERA|TIMERB) & ~status_mask
bool IRAM_ATTR boblbobl::opl_irq(void)
{
  return (opl_status & 0x60 & ~(opl_regs[0x04] & 0x78)) != 0;
}

// ym3526::read_status() = fm.status() | 0x06, fm.status() = m_status & ~status_mask,
// with STATUS_IRQ (0x80) mirroring the IRQ output.
unsigned char IRAM_ATTR boblbobl::opl_read_status(void)
{
  unsigned char st = opl_status & ~(opl_regs[0x04] & 0x78);
  if (opl_irq())
    st |= 0x80;
  return st | 0x06;
}

// soundirq = INPUT_MERGER_ANY_HIGH(ym2203 irq, ym3526 irq) -> audiocpu IRQ0.
bool IRAM_ATTR boblbobl::sound_irq_line(void)
{
  return (opn_status & 0x03) != 0 || opl_irq();
}

void boblbobl::ym_chips_reset(void)
{
  // ym2203::reset / ym3526::reset: registers cleared, timers stopped,
  // status cleared (fm_engine_base::reset -> set_reset_status(0, 0xff)).
  memset(ym_regs, 0, sizeof(ym_regs));
  memset(opl_regs, 0, sizeof(opl_regs));
  memset(opn_timer, 0, sizeof(opn_timer));
  memset(opl_timer, 0, sizeof(opl_timer));
  opn_status = 0;
  opl_status = 0;
  snd_push(2, 0, 0);
}

// ---- emulation -> audio register write transport -------------------------

void IRAM_ATTR boblbobl::snd_push(unsigned char chip, unsigned char reg, unsigned char val)
{
  if (snd_sync)
  {
    snd_apply(chip, reg, val);
    return;
  }
  uint16_t h = snd_head;
  uint16_t n = (uint16_t)((h + 1) & (SND_RING - 1));
  if (n == snd_tail)
  {
#if BOBLBOBL_DBG_HARNESS
    dbg_snd_drops++;
#endif
    return;                             // ring full (audio stalled): drop
  }
  snd_ring[h].clk = ym_chip_clocks;
  snd_ring[h].chip = chip;
  snd_ring[h].reg = reg;
  snd_ring[h].val = val;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  snd_head = n;
}

void IRAM_ATTR boblbobl::snd_chips_reset(void)
{
  snd_opn.reset();
  snd_ssg.reset();
  snd_opl.reset();
}

// ym2203::write_data: address < 0x10 goes to the SSG, the rest to FM.
void IRAM_ATTR boblbobl::snd_apply(unsigned char chip, unsigned char reg, unsigned char val)
{
  if (chip == 0)
  {
    if (reg < 0x10)
      snd_ssg.write(reg, val);
    else
      snd_opn.write(reg, val);
  }
  else if (chip == 1)
    snd_opl.write(reg, val);
  else
    snd_chips_reset();
}

void IRAM_ATTR boblbobl::snd_apply_due(uint32_t now)
{
  while (snd_tail != snd_head)
  {
    const snd_ev_S &e = snd_ring[snd_tail];
    if ((int32_t)(e.clk - now) > 0)
      break;
#if BOBLBOBL_DBG_HARNESS
    {
      int32_t late = (int32_t)(now - e.clk);
      if (late > dbg_snd_max_late) dbg_snd_max_late = late;
      dbg_snd_applied++;
    }
#endif
    snd_apply(e.chip, e.reg, e.val);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    snd_tail = (uint16_t)((snd_tail + 1) & (SND_RING - 1));
  }
}

// Advance the chips by `clocks` 3MHz clocks. MAME's ym2203 (fidelity max,
// prescale 6) clocks the SSG every 16 chip clocks and FM every 72; the ym3526
// generates one sample every 72 clocks. Ticks fall on clock positions in
// [start, start + clocks).
void IRAM_ATTR boblbobl::snd_step_clocks(uint32_t clocks, int32_t &fm_sum, int32_t &opl_sum, int &fm_n, int32_t &ssg_sum, int &ssg_n)
{
  uint32_t pos = 0;   // clocks into this call; event time of a tick = snd_evt_clk + pos
  while (clocks)
  {
    // Before emulation time 0 (the start-up latency) the chips are not
    // clocked, so chip tick N always falls on emulation clock 72 * N, as in
    // MAME (the envelope/LFO counters free-run from power-on).
    if (!snd_sync && (int32_t)(snd_evt_clk + pos) < 0)
    {
      uint32_t skip = (uint32_t)(-(int32_t)(snd_evt_clk + pos));
      if (skip > clocks)
        skip = clocks;
      clocks -= skip;
      pos += skip;
      continue;
    }
    if (snd_ssg_cd == 0)
    {
      if (!snd_sync)
        snd_apply_due(snd_evt_clk + pos);
#if BOBLBOBL_SND_FAST
      if (!snd_sync)
      {
        // SSG silent (all three amplitude registers 0): not clocked at all.
        // Otherwise clocked every 16 clocks, output taken once per sample.
        if (snd_ssg.regs[0x08] | snd_ssg.regs[0x09] | snd_ssg.regs[0x0a])
        {
          snd_ssg.clock();
          if (clocks <= 16)
          {
            snd_ssg.output();
            ssg_sum += snd_ssg.out[0] + snd_ssg.out[1] + snd_ssg.out[2];
            ssg_n++;
          }
        }
      }
      else
#endif
      {
        snd_ssg.clock();
        snd_ssg.output();
        ssg_sum += snd_ssg.out[0] + snd_ssg.out[1] + snd_ssg.out[2];
        ssg_n++;
      }
      snd_ssg_cd = 16;
#if BOBLBOBL_DBG_HARNESS
      if (dbg_fm_hook)
        dbg_fm_hook(1, snd_ssg.out[0], snd_ssg.out[1], snd_ssg.out[2]);
#endif
    }
    if (snd_fm_cd == 0)
    {
      if (!snd_sync)
        snd_apply_due(snd_evt_clk + pos);
      // fast mode: every FM tick is clocked (pitch and envelopes run at the
      // chip's own rate) but only the last tick of an output sample computes
      // the output.
      bool out = true;
#if BOBLBOBL_SND_FAST
      if (!snd_sync && clocks > 72)
        out = false;
#endif
      snd_opn.clock_fm(out);
      snd_opl.generate(out);
      if (out)
      {
        fm_sum += snd_opn.last_out;
        opl_sum += snd_opl.last_out;
        fm_n++;
      }
      snd_fm_cd = 72;
#if BOBLBOBL_DBG_HARNESS
      if (dbg_fm_hook)
        dbg_fm_hook(0, snd_opn.last_out, snd_opl.last_out, 0);
#endif
    }
    uint32_t step = clocks;
    if (snd_ssg_cd < step)
      step = snd_ssg_cd;
    if (snd_fm_cd < step)
      step = snd_fm_cd;
    snd_ssg_cd = (uint8_t)(snd_ssg_cd - step);
    snd_fm_cd = (uint8_t)(snd_fm_cd - step);
    clocks -= step;
    pos += step;
  }
}

unsigned char IRAM_ATTR boblbobl::main_rd(unsigned short addr)
{
  if (addr < 0x8000)
    return BB_ROM(0, addr);
  if (addr < 0xc000)
    return m_bankptr[addr - 0x8000];
  if (addr < 0xdd00)
    return memory[BOBLBOBL_VIDEORAM + (addr - 0xc000)];
  if (addr < 0xe000)
    return memory[BOBLBOBL_OBJECTRAM + (addr - 0xdd00)];
  if (addr < 0xf800)
    return memory[BOBLBOBL_SHARERAM + (addr - 0xe000)];
  if (addr < 0xfa00)
    return memory[BOBLBOBL_PALETTERAM + (addr - 0xf800)];

  if ((addr & 0xff83) == 0xfa00)
  {
    sound_to_main_pending = false;
    return sound_to_main;
  }
  if ((addr & 0xff83) == 0xfa01)
  {
    unsigned char ret = 0xfc;
    ret |= main_to_sound_pending ? 0x02 : 0x00;
    ret |= sound_to_main_pending ? 0x01 : 0x00;
    return ret;
  }

  if (addr >= 0xfc00 && addr < 0xfd00)
    return memory[BOBLBOBL_IC43RAM + (addr - 0xfc00)];
  if (addr >= 0xfd00 && addr < 0xfe00)
    return memory[BOBLBOBL_EXTRARAM + (addr - 0xfd00)];

  if (addr >= 0xfe00 && addr <= 0xfe03)
    return ic43_a_r((unsigned char)(addr - 0xfe00));
  if (addr >= 0xfe80 && addr <= 0xfe83)
    return ic43_b_r((unsigned char)(addr - 0xfe80));

  switch (addr)
  {
  case 0xff00:
    // DSW0 bit 3 is Demo Sounds (0x08 = On, MAME default)
    return (BOBLBOBL_DSW0_DEFAULT & ~0x08) |
           (input->demoSoundsOff() ? BOBLBOBL_DSW0_DEMO_SOUNDS_OFF : BOBLBOBL_DSW0_DEMO_SOUNDS_ON);
  case 0xff01:
    return BOBLBOBL_DSW1_DEFAULT;
  case 0xff02:
  {
    // MAME INPUT_PORTS_START(boblbobl) IN0: joystick/buttons/start active
    // LOW, COIN2 (0x04) and COIN1 (0x08) active HIGH with PORT_IMPULSE(1).
    // Idle value therefore has the two coin bits clear.
    unsigned char k = input->buttons_get();
    unsigned char r = 0xff & ~0x0c;
    if (k & BUTTON_LEFT)
      r &= ~0x01;
    if (k & BUTTON_RIGHT)
      r &= ~0x02;

    if ((k & BUTTON_COIN) && !coinBackup)
    {
      coinFrameCounter = 1; // PORT_IMPULSE(1)
      coinBackup = 1;
    }
    if (coinFrameCounter > 0)
      r |= 0x08;
    else if (!(k & BUTTON_COIN))
      coinBackup = 0;
    if (k & BUTTON_EXTRA)
      r &= ~0x10;
    if (k & BUTTON_FIRE)
      r &= ~0x20;
    if (k & BUTTON_START)
      r &= ~0x40;
    return r;
  }
  case 0xff03:
    return 0xff;   // IN1: player 2 / tilt / service, all active low, idle
  }
  return 0x00;     // unmapped (MAME reads 0x00)
}

void IRAM_ATTR boblbobl::main_wr(unsigned short addr, unsigned char val)
{
  if (addr < 0x8000)
    return;
  if (addr < 0xc000)
    return;
  if (addr < 0xdd00)
  {
    memory[BOBLBOBL_VIDEORAM + (addr - 0xc000)] = val;
    if (!game_started && val != 0)
    {
      game_started = 1;
      // from now on c000-dfff writes have no side effect: direct pages
      for (int p = 0; p < 4; p++)
        bbz_wr_page[0][24 + p] = &memory[BOBLBOBL_VIDEORAM + p * 0x800];
    }
    return;
  }
  if (addr < 0xe000)
  {
    memory[BOBLBOBL_OBJECTRAM + (addr - 0xdd00)] = val;
    return;
  }
  if (addr < 0xf800)
  {
    memory[BOBLBOBL_SHARERAM + (addr - 0xe000)] = val;
    return;
  }
  if (addr < 0xfa00)
  {
    memory[BOBLBOBL_PALETTERAM + (addr - 0xf800)] = val;
    palette_recalc((addr - 0xf800) >> 1);
    return;
  }

  if ((addr & 0xff83) == 0xfa00)
  {
#if BOBLBOBL_DBG_HARNESS
    if (dbg_snd_hook)
      dbg_snd_hook('L', addr, val);
#endif
    main_to_sound = val;
    main_to_sound_pending = true;
    update_sound_nmi();
    return;
  }
  if ((addr & 0xff83) == 0xfa03)
  {
    // bublbobl_soundcpu_reset_w: common_sreset(data ? ASSERT_LINE : CLEAR_LINE)
#if BOBLBOBL_DBG_HARNESS
    if (dbg_snd_hook)
      dbg_snd_hook('R', addr, val);
#endif
    sound_sreset(val != 0);
    return;
  }
  if ((addr & 0xff80) == 0xfa80)
    return;
  if ((addr & 0xffc0) == 0xfb00)
  {
    // bublbobl_nmitrigger_w: pulse subcpu NMI (ignored while it is in reset)
    if (sub_held_reset)
      return;
    int saved_cpu = current_cpu;
    set_cpu(1);
    bbz_Int(&cpu[1], INT_NMI);
    set_cpu(saved_cpu);
    return;
  }
  if ((addr & 0xffc0) == 0xfb40)
  {
    bankswitch_w(val);
    return;
  }

  if (addr >= 0xfc00 && addr < 0xfd00)
  {
    memory[BOBLBOBL_IC43RAM + (addr - 0xfc00)] = val;
    return;
  }
  if (addr >= 0xfd00 && addr < 0xfe00)
  {
    memory[BOBLBOBL_EXTRARAM + (addr - 0xfd00)] = val;
    return;
  }

  if (addr >= 0xfe00 && addr <= 0xfe03)
  {
    ic43_a_w((unsigned char)(addr - 0xfe00), val);
    return;
  }
  if (addr >= 0xfe80 && addr <= 0xfe83)
  {
    ic43_b_w((unsigned char)(addr - 0xfe80), val);
    return;
  }
}

unsigned char IRAM_ATTR boblbobl::sub_rd(unsigned short addr)
{
  if (addr < 0x8000)
    return BB_ROM(1, addr);
  if (addr >= 0xe000 && addr < 0xf800)
    return memory[BOBLBOBL_SHARERAM + (addr - 0xe000)];
  return 0x00;     // unmapped (MAME reads 0x00)
}

void IRAM_ATTR boblbobl::sub_wr(unsigned short addr, unsigned char val)
{
  if (addr >= 0xe000 && addr < 0xf800)
  {
    memory[BOBLBOBL_SHARERAM + (addr - 0xe000)] = val;
    return;
  }
}

unsigned char IRAM_ATTR boblbobl::audio_rd(unsigned short addr)
{
  if (addr < 0x8000)
    return BB_ROM(2, addr);
  if (addr >= 0x8000 && addr < 0x9000)
    return memory[BOBLBOBL_AUDIO_RAM + (addr - 0x8000)];
  if ((addr & 0xf001) == 0x9000)
    return opn_status & 0x03;                       // ym2203::read_status (never busy here)
  if ((addr & 0xf001) == 0x9001)
  {
#if BOBLBOBL_DBG_HARNESS
    if (dbg_snd_hook)
      dbg_snd_hook('r', ym_addr, (ym_addr < 0x10) ? ym_regs[ym_addr] : 0);
#endif
    return (ym_addr < 0x10) ? ym_regs[ym_addr] : 0; // ym2203::read_data (SSG regs only)
  }
  if ((addr & 0xf001) == 0xa000)
    return opl_read_status();                        // ym3526::read_status
  if ((addr & 0xf001) == 0xa001)
    return 0xff;                                     // ym3526::read, A0=1: no data
  if ((addr & 0xf003) == 0xb000)
  {
    main_to_sound_pending = false;
    update_sound_nmi();
    return main_to_sound;
  }
  if ((addr & 0xf003) == 0xb001)
  {
    unsigned char ret = 0xfc;
    ret |= main_to_sound_pending ? 0x02 : 0x00;
    ret |= sound_to_main_pending ? 0x01 : 0x00;
    return ret;
  }
  // 0xb002/0xb003 reads, 0xc000-0xdfff and the empty "diagnostic ROM" space
  // at 0xe000-0xffff (probed by the sound program; the audiocpu ROM region
  // is 0x10000 with only 0x8000 loaded) all read 0x00 in MAME.
  return 0x00;
}

void IRAM_ATTR boblbobl::audio_wr(unsigned short addr, unsigned char val)
{
#if BOBLBOBL_DBG_HARNESS
  if (dbg_snd_hook && addr >= 0x9000 && addr < 0xb000)
    dbg_snd_hook('A', addr, val);
#endif
  if (addr >= 0x8000 && addr < 0x9000)
  {
    memory[BOBLBOBL_AUDIO_RAM + (addr - 0x8000)] = val;
    return;
  }
  if ((addr & 0xf000) == 0x9000)   // ym2203 0x9000-0x9001, mirror 0x0ffe
  {
    if (addr & 1)
      ym_write(ym_addr, val);
    else
      ym_addr = val;
    return;
  }
  if ((addr & 0xf000) == 0xa000)   // ym3526 0xa000-0xa001, mirror 0x0ffe
  {
    if (addr & 1)
      opl_write(opl_addr, val);
    else
      opl_addr = val;
    return;
  }
  if ((addr & 0xf003) == 0xb000)
  {
    sound_to_main = val;
    sound_to_main_pending = true;
    return;
  }
  if ((addr & 0xf003) == 0xb001)
  {
    sound_nmi_enable = true;
    update_sound_nmi();
    return;
  }
  if ((addr & 0xf003) == 0xb002)
  {
    sound_nmi_enable = false;
    update_sound_nmi();
    return;
  }
}

unsigned char IRAM_ATTR boblbobl::opZ80(unsigned short Addr)
{
  switch (current_cpu)
  {
  case 0:
    return (Addr < 0x8000) ? BB_ROM(0, Addr) : main_rd(Addr);
  case 1:
    return (Addr < 0x8000) ? BB_ROM(1, Addr) : sub_rd(Addr);
  default:
    return (Addr < 0x8000) ? BB_ROM(2, Addr) : audio_rd(Addr);
  }
}

unsigned char IRAM_ATTR boblbobl::rdZ80(unsigned short Addr)
{
  switch (current_cpu)
  {
  case 0:
    return main_rd(Addr);
  case 1:
    return sub_rd(Addr);
  default:
    return audio_rd(Addr);
  }
}

void IRAM_ATTR boblbobl::wrZ80(unsigned short Addr, unsigned char Value)
{
  switch (current_cpu)
  {
  case 0:
    main_wr(Addr, Value);
    return;
  case 1:
    sub_wr(Addr, Value);
    return;
  default:
    audio_wr(Addr, Value);
    return;
  }
}

void boblbobl::outZ80(unsigned short Port, unsigned char Value)
{
  (void)Port;
  (void)Value;
}
unsigned char boblbobl::inZ80(unsigned short Port)
{
  (void)Port;
  return 0xff;
}

// One video frame = 384 x 264 pixel clocks at 6MHz (MAME set_raw(MAIN_XTAL/4,
// 384, 0, 256, 264, 16, 240)) = 101376 main/sub cycles, 50688 audio cycles
// (MAIN_XTAL/8). The frame is run in BOBLBOBL_SLICES interleaved slices
// (MAME: set_maximum_quantum(6000Hz)).
//
// Interrupts, as MAME wires them:
//  - main + sub: irq0_line_hold at VBLANK start (line 240). The line stays
//    raised until the CPU accepts it, so it is not lost while DI is active.
//  - audio: IRQ0 = ym2203 irq OR ym3526 irq (level), NMI from the latch (edge,
//    see update_sound_nmi()).
// An interrupt is only accepted when IFF1 was already set before the
// instruction just executed, which keeps the Z80's one-instruction delay
// after EI (StepZ80() itself sets IFF1 right after EI).
#define BOBLBOBL_VBLANK_SLICE ((BOBLBOBL_SLICES * 240) / 264)

static inline IRAM_ATTR bool bb_irq_ok(bool iff_before, const Z80 &c)
{
  return iff_before && (c.IFF & IFF_1);
}

// Both game CPUs park in a jump-to-self loop with interrupts enabled and do
// all their work in interrupt handlers (main: 0x01ED "jr $", sub: 0x000A
// "jp $000A"; ~64% / ~89% of all executed instructions). Such a loop has no
// effect except burning cycles, so when no interrupt can be taken the rest of
// the slice is consumed in whole loop iterations at once: same cycle count,
// same ICount (the core derives R from it), same state.
static inline IRAM_ATTR int bb_idle_loop_cycles(int cpu_n, unsigned short pc)
{
  if (pc >= 0x7ffd)
    return 0;
  unsigned char b0 = BB_ROM(cpu_n, pc), b1 = BB_ROM(cpu_n, pc + 1);
  if (b0 == 0x18 && b1 == 0xfe)                                       // jr $
    return 12;
  if (b0 == 0xc3 && (b1 | (BB_ROM(cpu_n, pc + 2) << 8)) == pc)        // jp $
    return 10;
  return 0;
}

static inline IRAM_ATTR bool bb_skip_idle(Z80 &c, int cpu_n, bool irq_pending, long &budget)
{
  if (c.IFF & IFF_EI)
    return false;
  if (irq_pending && (c.IFF & IFF_1))
    return false;
  int cyc = bb_idle_loop_cycles(cpu_n, c.PC.W);
  if (!cyc)
    return false;
  long n = (budget + cyc - 1) / cyc;
  budget -= n * cyc;
  c.ICount -= (int)(n * cyc);
  return true;
}

// Sound CPU idle loop (a78-07.46): 016D di / call 02A3 / ei / call 024D /
// call 0268 / jr 016D. With the command queue empty (8F80 == 8F81), no reply
// queued (8FAB bit 0 clear) and the sound->main latch still full, one pass
// only re-reads the same flags, disables and re-enables the NMI latch and
// pushes/pops the same return addresses: it maps the machine state onto
// itself in exactly BB_AUDIO_IDLE_CYCLES cycles. Such passes are skipped in
// whole numbers when nothing can interrupt them (no main->sound command, IRQ
// line low, no YM timer expiring inside the skipped time). Only taken when
// the previous pass was itself an idle one (exactly one period ago), so the
// registers already hold the loop's steady values.
#define BB_AUDIO_IDLE_PC 0x016D
#define BB_AUDIO_IDLE_CYCLES 252

bool IRAM_ATTR boblbobl::audio_idle_state(void)
{
  const unsigned char *ram = &memory[BOBLBOBL_AUDIO_RAM];
  return ram[0xf80] == ram[0xf81] && !(ram[0xfab] & 1) &&
         sound_to_main_pending && !main_to_sound_pending && sound_nmi_enable &&
         !(cpu[2].IFF & IFF_EI) && !sound_irq_line();
}

// Whole idle passes that fit in `budget` without reaching a YM timer expiry.
long IRAM_ATTR boblbobl::audio_idle_passes(long budget)
{
  int32_t limit = 0x7fffffff;
  for (int t = 0; t < 2; t++)
  {
    if (opn_timer[t].running && opn_timer[t].remain < limit)
      limit = opn_timer[t].remain;
    if (opl_timer[t].running && opl_timer[t].remain < limit)
      limit = opl_timer[t].remain;
  }
  // whole passes inside the budget; the rest of the slice is stepped normally,
  // so the slice ends on exactly the same instruction as without the skip
  long by_budget = budget / BB_AUDIO_IDLE_CYCLES;
  long by_timer = (limit - 1) / BB_AUDIO_IDLE_CYCLES;   // expiry stays outside the skip
  return by_budget < by_timer ? by_budget : by_timer;
}

void IRAM_ATTR boblbobl::run_frame(void)
{
  static long main_cycle_debt = 0;
  static long sub_cycle_debt = 0;
  static long audio_cycle_debt = 0;
  const long frame_cycles = (long)BOBLBOBL_MAIN_CYCLES_PER_FRAME;   // 101376 at 6MHz
  const long frame_audio_cycles = frame_cycles / 2;                  // 50688 at 3MHz

#if BOBLBOBL_REAL_SPEED && !BOBLBOBL_DBG_HARNESS
  // BOBLBOBL_REAL_SPEED: emulate a frame only when real time has caught up
  // with the game's own 59.19Hz (16896 us). Then the emulation makes exactly
  // the 24000 samples a second the audio core plays. The credit is capped so
  // a stall (menu, flash) does not make the game race afterwards.
  {
    uint32_t now = micros();
    if (!pace_init)
    {
      pace_init = true;
      pace_last = now;
      pace_credit = 16896;
    }
    pace_credit += (int32_t)(now - pace_last);
    pace_last = now;
    if (pace_credit > 3 * 16896)
      pace_credit = 3 * 16896;
    if (pace_credit < 16896)
      return;                                  // the display shows the last frame again
    pace_credit -= 16896;
  }
#endif
  const uint32_t frame_t0 = BB_CYCLES();

  for (int i = 0; i < BOBLBOBL_SLICES; i++)
  {
    // exact per-frame totals, spread over the slices without truncation loss
    const long cycles_per_slice = frame_cycles * (i + 1) / BOBLBOBL_SLICES - frame_cycles * i / BOBLBOBL_SLICES;
    const long audio_cycles_per_slice = frame_audio_cycles * (i + 1) / BOBLBOBL_SLICES - frame_audio_cycles * i / BOBLBOBL_SLICES;
    if (i == BOBLBOBL_VBLANK_SLICE)
    {
      // screen_update runs at VBLANK start, before the vblank IRQ handlers.
      publish_palette();
      publish_vram();
      main_irq_pending = true;
      if (!sub_held_reset)
        sub_irq_pending = true;
    }

    // ---- main CPU
    uint32_t pc0 = BB_CCOUNT();
    set_cpu(0);
    long main_budget = cycles_per_slice + main_cycle_debt;
    while (main_budget > 0)
    {
      if (bb_skip_idle(cpu[0], 0, main_irq_pending, main_budget))
        break;
      bool iff = (cpu[0].IFF & IFF_1) != 0;
      int icount_before = cpu[0].ICount;
      bbz_Step(&cpu[0]);
#if BOBLBOBL_DBG_HARNESS
      dbg_steps[0]++;
      if (cpu[0].IFF & IFF_HALT) dbg_halt_steps[0]++;
      dbg_pc_hist[0][cpu[0].PC.W]++;
#endif
      long consumed = (long)(icount_before - cpu[0].ICount);
      if (consumed <= 0)
        consumed = 1;
      main_budget -= consumed;
      if (main_irq_pending && bb_irq_ok(iff, cpu[0]))
      {
        main_irq_pending = false;
        bbz_Int(&cpu[0], INT_IRQ);
        main_budget -= 13;
      }
#if BOBLBOBL_DBG_HARNESS
      if (dbg_trace_hook)
        dbg_trace_hook(cpu[0].PC.W);
#endif
    }
    main_cycle_debt = main_budget;
    uint32_t pc1 = BB_CCOUNT();
    bb_prof_main += pc1 - pc0;

    // ---- sub CPU
    if (!sub_held_reset)
    {
      set_cpu(1);
      long sub_budget = cycles_per_slice + sub_cycle_debt;
      while (sub_budget > 0)
      {
        if (bb_skip_idle(cpu[1], 1, sub_irq_pending, sub_budget))
          break;
        bool iff = (cpu[1].IFF & IFF_1) != 0;
        int icount_before = cpu[1].ICount;
        bbz_Step(&cpu[1]);
#if BOBLBOBL_DBG_HARNESS
        dbg_steps[1]++;
        if (cpu[1].IFF & IFF_HALT) dbg_halt_steps[1]++;
        dbg_pc_hist[1][cpu[1].PC.W]++;
#endif
        long consumed = (long)(icount_before - cpu[1].ICount);
        if (consumed <= 0)
          consumed = 1;
        sub_budget -= consumed;
        if (sub_irq_pending && bb_irq_ok(iff, cpu[1]))
        {
          sub_irq_pending = false;
          bbz_Int(&cpu[1], INT_IRQ);
          sub_budget -= 13;
        }
      }
      sub_cycle_debt = sub_budget;
    }
    else
    {
      sub_cycle_debt = 0;
      sub_irq_pending = false;
    }

    uint32_t pc2 = BB_CCOUNT();
    bb_prof_sub += pc2 - pc1;
    // ---- audio CPU + YM timers (same 3MHz clock)
    long audio_budget = audio_cycles_per_slice + audio_cycle_debt;
    if (!audio_held_reset)
    {
      set_cpu(2);
      while (audio_budget > 0)
      {
        if (cpu[2].PC.W == BB_AUDIO_IDLE_PC)
        {
          bool idle = audio_idle_state();
          if (idle && aud_prev_idle && ym_chip_clocks - aud_prev_clk == BB_AUDIO_IDLE_CYCLES)
          {
            long n = audio_idle_passes(audio_budget);
            if (n > 0)
            {
              long c = n * BB_AUDIO_IDLE_CYCLES;
              cpu[2].ICount -= (int)c;
              audio_budget -= c;
              ym_timers_advance((int32_t)c);
              aud_prev_clk = ym_chip_clocks;   // back at 016D, still idle
              continue;
            }
          }
          aud_prev_idle = idle;
          aud_prev_clk = ym_chip_clocks;
        }
        bool iff = (cpu[2].IFF & IFF_1) != 0;
        int icount_before = cpu[2].ICount;
        bbz_Step(&cpu[2]);
#if BOBLBOBL_DBG_HARNESS
        dbg_steps[2]++;
        if (cpu[2].IFF & IFF_HALT) dbg_halt_steps[2]++;
        dbg_pc_hist[2][cpu[2].PC.W]++;
#endif
        long consumed = (long)(icount_before - cpu[2].ICount);
        if (consumed <= 0)
          consumed = 1;
        if (bb_irq_ok(iff, cpu[2]) && sound_irq_line())
        {
          bbz_Int(&cpu[2], INT_IRQ);
          consumed += 13;
        }
        audio_budget -= consumed;
        ym_timers_advance((int32_t)consumed);
#if BOBLBOBL_DBG_HARNESS
        if (cpu[2].PC.W == 0x016D)
        {
          static uint32_t lastclk = 0;
          uint32_t d = ym_chip_clocks - lastclk;
          if (d < 4096) dbg_loop_hist[d]++;
          lastclk = ym_chip_clocks;
        }
#endif
      }
      audio_cycle_debt = audio_budget;
    }
    else
    {
      ym_timers_advance((int32_t)audio_budget);
      audio_cycle_debt = 0;
    }
    bb_prof_audio += BB_CCOUNT() - pc2;
  }

  set_cpu(0);
  snd_emu_clk_pub = ym_chip_clocks;
  snd_render_ahead(frame_t0);

#if BB_PROF
  {
    static uint32_t last = 0;
    uint32_t now = BB_CCOUNT();
    if (last)
      bb_prof_frame += now - last;
    last = now;
    if (++bb_prof_frames >= 60)
    {
      // 240 cycles per us
      uint32_t f = bb_prof_frames, sc = bb_prof_screens ? bb_prof_screens : 1, ss = bb_prof_snd_samples ? bb_prof_snd_samples : 1;
      uint32_t ss1 = bb_prof_snd1_samples ? bb_prof_snd1_samples : 1;
      printf("bb us/frame: main=%lu sub=%lu audiocpu=%lu snd(core1)=%lu (frame period %lu) | us/screen: scan=%lu blit=%lu spans=%lu screens=%lu | snd core0: %lu ns/sample, %lu samples | core1: %lu ns/sample, %lu samples | repeated %lu\n",
             (unsigned long)(bb_prof_main / 240 / f), (unsigned long)(bb_prof_sub / 240 / f), (unsigned long)(bb_prof_audio / 240 / f),
             (unsigned long)(bb_prof_snd1 / 240 / f), (unsigned long)(bb_prof_frame / 240 / f),
             (unsigned long)(bb_prof_scan / 240 / sc), (unsigned long)(bb_prof_blit / 240 / sc), (unsigned long)(bb_prof_spans / sc), (unsigned long)bb_prof_screens,
             (unsigned long)((uint64_t)bb_prof_snd * 1000 / 240 / ss), (unsigned long)bb_prof_snd_samples,
             (unsigned long)((uint64_t)bb_prof_snd1 * 1000 / 240 / ss1), (unsigned long)bb_prof_snd1_samples,
             (unsigned long)bb_prof_snd_under);
      bb_prof_main = bb_prof_sub = bb_prof_audio = bb_prof_frame = bb_prof_frames = 0;
      bb_prof_scan = bb_prof_blit = bb_prof_snd = bb_prof_snd_samples = bb_prof_screens = bb_prof_spans = 0;
      bb_prof_snd1 = bb_prof_snd1_samples = 0;
      bb_prof_snd_under = 0;
    }
  }
#endif

  if (coinFrameCounter > 0)
    coinFrameCounter--;
}

void boblbobl::prepare_frame(void)
{
  // Nothing to do: render_row(0) latches the published shadows and scans.
}

// Tiles that are pen 15 (transparent) everywhere draw nothing - on this game
// 45-95% of all cells in a frame. One bit per tile code, built once from the
// ROM data. Skipping them does not change the picture.
static uint32_t bb_tile_empty[16384 / 32];
static bool bb_tile_empty_ready = false;

static void bb_build_tile_empty(void)
{
  if (bb_tile_empty_ready)
    return;
  for (int c = 0; c < 16384; c++)
  {
    const unsigned char *t = &boblbobl_gfx[c][0][0];
    bool empty = true;
    for (int i = 0; i < 64 && empty; i++)
      if (t[i] != 15)
        empty = false;
    if (empty)
      bb_tile_empty[c >> 5] |= 1u << (c & 31);
  }
  bb_tile_empty_ready = true;
}

#if BOBLBOBL_DBG_HARNESS
bool boblbobl::dbg_tile_empty(unsigned short code)
{
  return (bb_tile_empty[code >> 5] >> (code & 31)) & 1;
}
#endif

// Direct port of MAME bublbobl_v.cpp screen_update_bublbobl(). There is no
// tilemap on this hardware: every 8x8 cell (playfield and sprites) is drawn
// from gfx(0) by walking objectram (dd00-dfff) and fetching tile words from
// videoram (c000-dcff).
//
// MAME indexes m_videoram[goffs] with no mask. goffs can exceed 0x1cff, so the
// shadow holds c000-dfff as one block (videoram then objectram, contiguous as
// on the board) and goffs is only limited to that 8KB window.
//
// Screen mapping: the game is ROT0, visible area x 0-255, y 16-239 (MAME
// set_raw(..., 384, 0, 256, 264, 16, 240)). The 224x288 portrait
// frame_buffer is shown unmirrored, so the landscape image must be ROTATED
// onto it, not transposed:
//   fb_col = y - 16                      (MAME y 16..239 -> column 0..223)
//   fb_row = ROW_OFFSET + 255 - x        (MAME x 0..255  -> row 271..16)
// For an 8x8 cell at MAME (x,y), tile column px lands on
// fb_row = ROW_OFFSET + 255 - x - px, so the cell's top row is
// ROW_OFFSET + 248 - x and row dy inside the cell shows px = 7 - dy.
void IRAM_ATTR boblbobl::add_span(int band, short row_top, short col, unsigned short code, unsigned char color)
{
  if (span_count >= MAX_SPANS)
    return;
  unsigned short idx = (unsigned short)span_count++;
  span_S &sp = spans[idx];
  sp.fb_row_top = row_top;
  sp.fb_col = col;
  sp.code = code;
  sp.color = color;
  sp.next = SPAN_END;
  if (band_head[band] == SPAN_END)
    band_head[band] = idx;
  else
    spans[band_tail[band]].next = idx;
  band_tail[band] = idx;
}

void IRAM_ATTR boblbobl::scan_spans(void)
{
  span_count = 0;
  for (int b = 0; b < 36; b++)
    band_head[b] = band_tail[b] = SPAN_END;

  const unsigned char *vram = vram_shadow[m_render_vram];
  const unsigned char *sr = vram + (BOBLBOBL_OBJECTRAM - BOBLBOBL_VIDEORAM);
  const unsigned char *prom = m_proms;

  int sx = 0;
  for (int offs = 0; offs < 0x300; offs += 4)
  {
    if (sr[offs + 0] == 0 && sr[offs + 1] == 0 && sr[offs + 2] == 0 && sr[offs + 3] == 0)
      continue;

    int gfx_num = sr[offs + 1];
    int gfx_attr = sr[offs + 3];
    const unsigned char *prom_line = prom + 0x80 + ((gfx_num & 0xe0) >> 1);

    int gfx_offs = (gfx_num & 0x1f) * 0x80;
    if ((gfx_num & 0xa0) == 0xa0)
      gfx_offs |= 0x1000;

    int sy = -(int)sr[offs + 0];

    for (int yc = 0; yc < 32; yc++)
    {
      unsigned char pl = prom_line[yc / 2];
      if (pl & 0x08)
        continue; /* NEXT */

      if (!(pl & 0x04)) /* next column */
      {
        sx = sr[offs + 2];
        if (gfx_attr & 0x40)
          sx -= 256;
      }

      for (int xc = 0; xc < 2; xc++)
      {
        int goffs = gfx_offs + xc * 0x40 + (yc & 7) * 0x02 + (pl & 0x03) * 0x10;
        unsigned char v0 = vram[goffs & 0x1fff];
        unsigned char v1 = vram[(goffs + 1) & 0x1fff];
        unsigned short code = (v0 + 256 * (v1 & 0x03) + 1024 * (gfx_attr & 0x0f)) & 0x3fff;
        if ((bb_tile_empty[code >> 5] >> (code & 31)) & 1)
          continue;
        unsigned char color = (v1 & 0x3c) >> 2;
        bool flipx = (v1 & 0x40) != 0;
        bool flipy = (v1 & 0x80) != 0;
        int x = sx + xc * 8;
        int y = (sy + yc * 8) & 0xff;

        if (m_render_flip)
        {
          x = 248 - x;
          y = 248 - y;
          flipx = !flipx;
          flipy = !flipy;
        }

        // clip against MAME's visible area (x 0..255, y 16..239)
        if (x + 8 <= 0 || x >= 256 || y + 8 <= 16 || y >= 240)
          continue;

        short row_top = (short)(BOBLBOBL_ROW_OFFSET + 248 - x);
        unsigned char attr = color | (flipx ? 0x40 : 0) | (flipy ? 0x80 : 0);
        int b0 = row_top >> 3;
        int b1 = (row_top + 7) >> 3;
        add_span(b0, row_top, (short)(y - 16), code, attr);
        if (b1 != b0 && b1 < 36)
          add_span(b1, row_top, (short)(y - 16), code, attr);
      }
    }

    sx += 16;
  }
}

void IRAM_ATTR boblbobl::blit_span(int y_strip, const span_S &sp)
{
  // rows of this cell inside the band and inside the 256-line game area
  int r0 = sp.fb_row_top, r1 = sp.fb_row_top + 8;
  if (r0 < y_strip) r0 = y_strip;
  if (r1 > y_strip + 8) r1 = y_strip + 8;
  if (r0 < BOBLBOBL_ROW_OFFSET) r0 = BOBLBOBL_ROW_OFFSET;
  if (r1 > BOBLBOBL_ROW_OFFSET + BOBLBOBL_MAME_FULL_W) r1 = BOBLBOBL_ROW_OFFSET + BOBLBOBL_MAME_FULL_W;
  // columns inside the 224-wide framebuffer
  int c0 = sp.fb_col < 0 ? 0 : sp.fb_col;
  int c1 = sp.fb_col + 8 > 224 ? 224 : sp.fb_col + 8;
  if (r0 >= r1 || c0 >= c1)
    return;

  const unsigned short *colors = &pal_shadow[m_render_palette][(sp.color & 0x0f) << 4];
  const unsigned char *tile = &boblbobl_gfx[sp.code][0][0];   // [y][x], transparent pen 15
  const bool flip_x = (sp.color & 0x40) != 0;
  const bool flip_y = (sp.color & 0x80) != 0;
  const int n = c1 - c0;
  const int dx0 = c0 - sp.fb_col;
  // cell column dx -> tile row tile_y = flip_y ? 7 - dx : dx
  const int ystep = flip_y ? -8 : 8;
  const int ystart = (flip_y ? 7 - dx0 : dx0) * 8;

  for (int r = r0; r < r1; r++)
  {
    int dy = r - sp.fb_row_top;
    int tile_x = flip_x ? dy : 7 - dy;          // px = 7 - dy, tile_x = flip_x ? 7 - px : px
    const unsigned char *src = tile + ystart + tile_x;
    unsigned short *fb = &frame_buffer[(r - y_strip) * 224 + c0];
    for (int k = 0; k < n; k++, src += ystep)
    {
      unsigned char pen = *src;
      if (pen != 15)
        fb[k] = colors[pen];
    }
  }
}

void IRAM_ATTR boblbobl::render_row(short row)
{
  int line0 = row * 8;

  uint32_t rt0 = BB_CCOUNT();
  if (row == 0)
  {
    m_render_vram = vram_front;
    m_render_palette = palette_front;
    m_render_flip = m_flip;
    m_render_video_enable = m_video_enable;
    scan_spans();
#if BB_PROF
    bb_prof_scan += BB_CCOUNT() - rt0;
    bb_prof_screens++;
    bb_prof_spans += span_count;
    rt0 = BB_CCOUNT();
#endif
  }

  // Outside the 256-line game area the panel stays black.
  if (line0 + 8 <= BOBLBOBL_ROW_OFFSET || line0 >= BOBLBOBL_ROW_OFFSET + BOBLBOBL_MAME_FULL_W)
  {
    memset(frame_buffer, 0, 224 * 8 * sizeof(unsigned short));
    return;
  }

  // MAME: bitmap.fill(255, cliprect) before the video-enable test, so the
  // backdrop is palette entry 255 whether or not the display is enabled.
  unsigned short bg = pal_shadow[m_render_palette][255];
  for (int r = 0; r < 8; r++)
  {
    int abs_row = line0 + r;
    unsigned short *fb = frame_buffer + r * 224;
    unsigned short v = (abs_row >= BOBLBOBL_ROW_OFFSET && abs_row < BOBLBOBL_ROW_OFFSET + BOBLBOBL_MAME_FULL_W) ? bg : 0;
    for (int c = 0; c < 224; c++)
      fb[c] = v;
  }

  if (!m_render_video_enable)
    return;

  for (unsigned short i = band_head[row]; i != SPAN_END; i = spans[i].next)
    blit_span(line0, spans[i]);
#if BB_PROF
  bb_prof_blit += BB_CCOUNT() - rt0;
#endif
}

// ---- high scores / LEDs -------------------------------------------------

// High score: MAME plugins/hiscore/hiscore.dat, entry shared by boblbobl /
// bublbobl / sboblbobl ("now saves all hiscore data and special item
// counters"), all in the main CPU's shared RAM. 47 regions, same order.
const hiscore_region_S *boblbobl::hiscoreRegions(unsigned char *count)
{
  static const hiscore_region_S regions[] = {
    { 0xe654, 0x23, 0x00, 0x48 },
    { 0xe67b, 0x03, 0x1f, 0x13 },
    { 0xe64c, 0x03, 0x00, 0x00 },
    { 0xe5df, 0x01, 0x00, 0x00 },
    { 0xe5e0, 0x01, 0x00, 0x00 },
    { 0xe5e4, 0x01, 0x00, 0x00 },
    { 0xe5e6, 0x01, 0x00, 0x00 },
    { 0xe5e1, 0x01, 0x00, 0x00 },
    { 0xe5e2, 0x01, 0x00, 0x00 },
    { 0xe5e3, 0x01, 0x00, 0x00 },
    { 0xe5e7, 0x01, 0x00, 0x00 },
    { 0xe5e8, 0x01, 0x00, 0x00 },
    { 0xe5e9, 0x01, 0x00, 0x00 },
    { 0xe5ea, 0x01, 0x00, 0x00 },
    { 0xe5eb, 0x01, 0x00, 0x00 },
    { 0xe5f6, 0x01, 0x00, 0x00 },
    { 0xe5f7, 0x01, 0x00, 0x00 },
    { 0xe5ee, 0x01, 0x00, 0x00 },
    { 0xe5ef, 0x01, 0x00, 0x00 },
    { 0xe5f0, 0x01, 0x00, 0x00 },
    { 0xe5ec, 0x01, 0x00, 0x00 },
    { 0xe5ed, 0x01, 0x00, 0x00 },
    { 0xe5f3, 0x01, 0x00, 0x00 },
    { 0xe5f4, 0x01, 0x00, 0x00 },
    { 0xe5d9, 0x01, 0x00, 0x00 },
    { 0xe5da, 0x01, 0x00, 0x00 },
    { 0xf457, 0x01, 0x00, 0x00 },
    { 0xf458, 0x01, 0x00, 0x00 },
    { 0xe601, 0x01, 0x00, 0x00 },
    { 0xe602, 0x01, 0x00, 0x00 },
    { 0xe600, 0x01, 0x00, 0x00 },
    { 0xe5ff, 0x01, 0x00, 0x00 },
    { 0xe5fd, 0x01, 0x00, 0x00 },
    { 0xe5fc, 0x01, 0x00, 0x00 },
    { 0xe5fb, 0x01, 0x00, 0x00 },
    { 0xe5fa, 0x01, 0x00, 0x00 },
    { 0xe5f9, 0x01, 0x00, 0x00 },
    { 0xe5f8, 0x01, 0x00, 0x00 },
    { 0xe5fe, 0x01, 0x00, 0x00 },
    { 0xe604, 0x01, 0x00, 0x00 },
    { 0xe605, 0x01, 0x00, 0x00 },
    { 0xe606, 0x01, 0x00, 0x00 },
    { 0xe607, 0x01, 0x00, 0x00 },
    { 0xe609, 0x01, 0x00, 0x00 },
    { 0xe60a, 0x01, 0x00, 0x00 },
    { 0xe611, 0x01, 0x00, 0x00 },
    { 0xe60b, 0x02, 0x00, 0x00 },
  };
  *count = sizeof(regions) / sizeof(regions[0]);
  return regions;
}

#ifdef LED_PIN
// Bub (green) and Bob (blue) on the strip; a cyan bubble floats up the strip
// and pops white at the top, then the next one starts at the bottom.
void boblbobl::gameLeds(CRGB *leds)
{
  static char sub_cnt = 0;
  static char bubble = 0;          // 0..NUM_LEDS-1 rising, NUM_LEDS = pop
  if (sub_cnt++ < 12)
    return;
  sub_cnt = 0;

  for (char c = 0; c < NUM_LEDS; c++)
    leds[c] = (c & 1) ? LED_BLUE : LED_GREEN;
  if (bubble < NUM_LEDS)
    leds[(int)bubble] = LED_CYAN;
  else
    leds[NUM_LEDS - 1] = LED_WHITE;  // pop
  bubble = (bubble + 1) % (NUM_LEDS + 1);
}

void boblbobl::menuLeds(CRGB *leds)
{
  memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
#endif

const unsigned short *boblbobl::logo(void) {
  return boblbobl_logo;
}

// ---- emulation-side chip register writes -----------------------------

void IRAM_ATTR boblbobl::ym_write(unsigned char reg, unsigned char val)
{
  if (reg == 0x27)
    opn_mode_write(val);       // REG_MODE: timers / flags
  else
    ym_regs[reg] = val;
  snd_push(0, reg, val);
}

void IRAM_ATTR boblbobl::opl_write(unsigned char reg, unsigned char val)
{
  if (reg == 0x04)
    opl_mode_write(val);       // REG_MODE: timers / flags / IRQ reset
  else
    opl_regs[reg] = val;
  snd_push(1, reg, val);
}

// ---- audio render (Audio::transmit, 24kHz) --------------------------------
// 3MHz / 24kHz = 125 chip clocks per output sample. The event clock follows
// the emulation two frames behind and never runs ahead of it, so register
// writes are replayed with the spacing the sound CPU made them.
//
// Mix (bublbobl.cpp): ym2203 ALL_OUTPUTS 0.25 (FM + 3 SSG channels),
// ym3526 0.50, streams normalised by 32768 (ymfm_mame.h put_int). Here
// +/-512 is full scale (Audio::valueToBuffer), so
//   value = (fm + ssgA + ssgB + ssgC + 2 * opl) / 256 * GAIN / 256.
// BOBLBOBL_SND_FAST (boblbobl.h): cheaper render for the ESP32, see snd_step_clocks.
#define BOBLBOBL_SND_CLOCK 3000000
#define BOBLBOBL_SND_RATE 24000
#define BOBLBOBL_SND_CLOCKS_PER_SAMPLE (BOBLBOBL_SND_CLOCK / BOBLBOBL_SND_RATE)
static_assert(BOBLBOBL_SND_CLOCK % BOBLBOBL_SND_RATE == 0, "chip clock must divide by the output rate");
// Two frames of chip clocks: emulation publishes a whole frame at once, so the
// event clock needs one frame of slack beyond the frame being played.
#define BOBLBOBL_SND_LATENCY (2 * (BOBLBOBL_MAIN_CYCLES_PER_FRAME / 2))
#ifndef BOBLBOBL_SND_GAIN
#define BOBLBOBL_SND_GAIN 256    // 256 = MAME's own mix level
#endif

// Sound render sharing between the cores. The chip state and the event
// clock below may be advanced by either core, one sample at a time, under
// snd_lock:
//  - core 1 (emulation), at the end of run_frame: snd_render_ahead() renders
//    the samples that lie wholly inside emulated time into snd_out, while it
//    has time left in the frame (BOBLBOBL_SND_CORE1_US);
//  - core 0 (audio): renderFmSample() pops snd_out, and renders the next
//    sample itself only when snd_out is empty.
// Every sample is computed by the same code from the same state in the same
// order, so the output does not depend on which core made it.
// Core 1 has priority: while it renders (snd_core1_busy) core 0 does not
// compete for the next samples. Otherwise a starving core 0 (emulation
// behind real time) takes every sample as soon as a frame publishes it,
// core 1 idles, core 0 stays full, the frame ticks stay slow - a stable
// ~43Hz in bubble scenes (pass 7c capture).
// When the event clock has caught up with emulated time (the emulation is
// slower than real time) there is nothing emulated to play: the last sample
// is repeated. (The old renderer kept stepping the chips at full cost there
// - on the audio core, which then slowed the video, the frame ticks and so
// the emulation further: gameplay locked at 26Hz.)
int IRAM_ATTR boblbobl::renderFmSample()
{
  for (;;)
  {
    uint16_t r = snd_out_rd;
    if (r != snd_out_wr)
    {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int v = snd_out[r];
      snd_out_rd = (uint16_t)((r + 1) & (SND_OUT - 1));
      snd_last = (short)v;
      return v;
    }
    if (snd_core1_busy)
    {
      // core 1 is rendering its batch (it has priority - see above): repeat
      // the last sample rather than take the samples it is about to render
#if BB_PROF
      bb_prof_snd_under++;
#endif
      return snd_last;
    }
    if (!snd_lock.exchange(1, std::memory_order_acquire))
    {
      if (snd_out_rd != snd_out_wr)
      {
        snd_lock.store(0, std::memory_order_release);   // core 1 just pushed one
        continue;
      }
      if (snd_evt_started && (int32_t)(snd_emu_clk_pub - (snd_evt_clk + BOBLBOBL_SND_CLOCKS_PER_SAMPLE)) < 0)
      {
        snd_lock.store(0, std::memory_order_release);
#if BB_PROF
        bb_prof_snd_under++;
#endif
        return snd_last;
      }
#if BB_PROF
      uint32_t st0 = BB_CCOUNT();
#endif
      int v = snd_render_one();
#if BB_PROF
      bb_prof_snd += BB_CCOUNT() - st0;
      bb_prof_snd_samples++;
#endif
      snd_lock.store(0, std::memory_order_release);
      snd_last = (short)v;
      return v;
    }
    // core 1 is rendering one sample into snd_out: it is there in a moment.
    // Wait off the memory bus (the cycle counter is a register) so core 1's
    // render is not slowed by this loop.
#if !BOBLBOBL_DBG_HARNESS
    uint32_t c0 = BB_CYCLES();
    while ((uint32_t)(BB_CYCLES() - c0) < 480)
      ;
#endif
  }
}

// Core 1, end of run_frame (t0 = CPU cycle count at the start of the frame).
void IRAM_ATTR boblbobl::snd_render_ahead(uint32_t t0)
{
  if (snd_sync)
    return;
#if BOBLBOBL_DBG_HARNESS
  if (!dbg_snd_core1)
    return;
#endif
  const uint32_t span = BOBLBOBL_SND_CLOCKS_PER_SAMPLE;
  unsigned checks = 0;
  (void)checks;
  snd_core1_busy = true;
#if BOBLBOBL_DBG_HARNESS
  int limit = dbg_snd_core1 >= 2 ? dbg_snd_core1 : 1 << 30;   // stands in for the time limit
#endif
  for (;;)
  {
#if BOBLBOBL_DBG_HARNESS
    if (limit-- <= 0)
      break;
#endif
    uint16_t w = snd_out_wr;
    if ((uint16_t)((w + 1) & (SND_OUT - 1)) == snd_out_rd)
      break;                                            // snd_out full
    if (snd_evt_started && (int32_t)(snd_emu_clk_pub - (snd_evt_clk + span)) < 0)
      break;                                            // caught up with emulated time
#if !BOBLBOBL_DBG_HARNESS
    // Stop when the next frame tick is already waiting (the emulation task's
    // notification count, read without taking it) - core 1 then has no time
    // left and core 0 renders the rest - or at the BOBLBOBL_SND_CORE1_US cap.
    if (((++checks) & 7) == 1 && ulTaskNotifyValueClear(NULL, 0) != 0)
      break;
    if ((uint32_t)(BB_CYCLES() - t0) > (uint32_t)BOBLBOBL_SND_CORE1_US * 240)
      break;
#endif
    while (snd_lock.exchange(1, std::memory_order_acquire))
      ;                                                 // core 0 is rendering one sample
#if BB_PROF
    uint32_t st0 = BB_CCOUNT();
#endif
    int v = snd_render_one();
#if BB_PROF
    bb_prof_snd1 += BB_CCOUNT() - st0;
    bb_prof_snd1_samples++;
#endif
    snd_out[w] = (short)v;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    snd_out_wr = (uint16_t)((w + 1) & (SND_OUT - 1));
    snd_lock.store(0, std::memory_order_release);
  }
  snd_core1_busy = false;
  (void)t0;
}

// One output sample (the caller holds snd_lock).
int IRAM_ATTR boblbobl::snd_render_one()
{
  // snd_evt_clk is the emulation chip-clock time of the start of this output
  // sample. It is placed two frames behind the emulation once, then advances
  // at exactly the chip rate. It only holds when it catches up with the last
  // completed emulation frame (emulation slower than real time), and is
  // re-placed if it falls more than 4 frames behind (e.g. audio paused).
  uint32_t pub = snd_emu_clk_pub;
  int32_t behind = (int32_t)(pub - snd_evt_clk);
  if (!snd_evt_started || behind > (int32_t)(4 * BOBLBOBL_SND_LATENCY))
  {
    snd_evt_clk = pub - (uint32_t)BOBLBOBL_SND_LATENCY;
    snd_evt_started = true;
  }

  int32_t fm = 0, opl = 0, ssg = 0;
  int nfm = 0, nssg = 0;
#if BOBLBOBL_SND_FAST
  const uint32_t span = BOBLBOBL_SND_CLOCKS_PER_SAMPLE;
  if ((int32_t)(snd_evt_clk + span) > 0)
  {
    // One chip step per output sample: count the FM samples (every 72 clocks)
    // that fall in these 125 clocks, apply this sample's register writes, then
    // advance both chips by that many samples and take one output.
    uint32_t n = 0;
    uint32_t cd = snd_fm_cd;
    while (cd < span)
    {
      n++;
      cd += 72;
    }
    snd_fm_cd = (uint8_t)(cd - span);
    snd_apply_due(snd_evt_clk + span - 1);
    if (n)
    {
      snd_opn.step_fast(n);
      snd_opl.step_fast(n);
      fm = snd_opn.last_out;
      opl = snd_opl.last_out;
      nfm = 1;
    }
    // SSG: only clocked while one of its channels has a volume (never in this game)
    if (snd_ssg.regs[0x08] | snd_ssg.regs[0x09] | snd_ssg.regs[0x0a])
    {
      uint32_t c = snd_ssg_cd;
      while (c < span)
      {
        snd_ssg.clock();
        c += 16;
      }
      snd_ssg_cd = (uint8_t)(c - span);
      snd_ssg.output();
      ssg = snd_ssg.out[0] + snd_ssg.out[1] + snd_ssg.out[2];
      nssg = 1;
    }
  }
#else
  snd_step_clocks(BOBLBOBL_SND_CLOCKS_PER_SAMPLE, fm, opl, nfm, ssg, nssg);
#endif

  if ((int32_t)(pub - (snd_evt_clk + BOBLBOBL_SND_CLOCKS_PER_SAMPLE)) >= 0)
    snd_evt_clk += BOBLBOBL_SND_CLOCKS_PER_SAMPLE;
  if (nfm)
  {
    fm /= nfm;
    opl /= nfm;
  }
  if (nssg)
    ssg /= nssg;

  int32_t v = ((fm + ssg + 2 * opl) * BOBLBOBL_SND_GAIN) >> 16;
  if (v > 511)
    v = 511;
  if (v < -511)
    v = -511;
  return v;
}

#include "boblbobl_fm.inc"
