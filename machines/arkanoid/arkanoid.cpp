// -O2 for the emulation hot path, as boblbobl.cpp (the project builds -Os)
#pragma GCC optimize("-O2")

#include "arkanoid.h"

// Arkanoid (World, older). Every behaviour below cites the MAME source it was
// taken from - see arkanoid.h for the file list.

static_assert(ARKANOID_MEM_END <= RAMSIZE, "RAMSIZE too low for arkanoid");

// MAME timing constants - see "Scheduling" below for where each comes from.
#define ARK_ATTOS_PER_SECOND 1000000000000000000ULL
#define ARK_Z80_APC   166666666666ULL
#define ARK_Z80_SHIFT 7
#define ARK_Z80_DIV   (ARK_Z80_APC >> ARK_Z80_SHIFT)
#define ARK_MCU_APC   1333333333333ULL
#define ARK_MCU_SHIFT 10
#define ARK_MCU_DIV   (ARK_MCU_APC >> ARK_MCU_SHIFT)
#define ARK_QUANTUM   (ARK_ATTOS_PER_SECOND / 6000)
#define ARK_FRAME     (ARK_Z80_APC * 384 * 264)
#define ARK_SCAN      (ARK_FRAME / 264)
#define ARK_SND_PERIOD (ARK_ATTOS_PER_SECOND / 50)
static_assert(ARK_Z80_DIV < (1ULL << 31) && (ARK_Z80_APC >> (ARK_Z80_SHIFT - 1)) >= (1ULL << 31), "Z80 divshift");
static_assert(ARK_MCU_DIV < (1ULL << 31) && (ARK_MCU_APC >> (ARK_MCU_SHIFT - 1)) >= (1ULL << 31), "MCU divshift");

arkanoid *arkanoid::s_instance = 0;

// Program ROM as three 16KB pages and the MCU ROM, copied to internal RAM
// when the heap allows (each falls back to the flash array on its own):
// flash reads on the emulation core stall behind the render core's flash
// traffic (gfx). Pointers are file-static so the static helpers reach them.
static const unsigned char *ark_rom_page[3] = { arkanoid_rom, arkanoid_rom + 0x4000, arkanoid_rom + 0x8000 };
#define ARK_ROM(a) (ark_rom_page[(a) >> 14][(a) & 0x3fff])

void arkanoid::roms_to_ram(void)
{
  for (int i = 0; i < 3; i++)
  {
    if (!rom_ram[i])
    {
      rom_ram[i] = (unsigned char *)ownAlloc(malloc(0x4000));
      if (rom_ram[i])
        memcpy(rom_ram[i], arkanoid_rom + i * 0x4000, 0x4000);
    }
    ark_rom_page[i] = rom_ram[i] ? rom_ram[i] : arkanoid_rom + i * 0x4000;
  }
  if (!rom_ram[3])
  {
    rom_ram[3] = (unsigned char *)ownAlloc(malloc(0x800));
    if (rom_ram[3])
      memcpy(rom_ram[3], arkanoid_mcu, 0x800);
  }
  mcu_rom = rom_ram[3] ? rom_ram[3] : arkanoid_mcu;
  printf("arkanoid: ROM in RAM: %d%d%d mcu %d\n", rom_ram[0] != 0, rom_ram[1] != 0, rom_ram[2] != 0, rom_ram[3] != 0);
}

void arkanoid::reset()
{
  machineBase::reset();
  s_instance = this;
  roms_to_ram();

  // arkanoid_state::machine_reset()
  m_gfxbank = 0;
  m_palettebank = 0;
  m_paddle_select = 0;
  m_flipx = m_flipy = false;
  m_coin_enabled = true;    // bookkeeping_manager ctor: m_coinlockedout = 0 (not locked)
  in_keys = 0;

  // ay8910_reset_ym(): registers 0, m_active = false, latch 0
  ay_addr = 0;
  ay_active = false;

  z80_irq = false;
  ei_shadow = false;
  base = z80_t = mcu_t = frame_start = exec_base = exec_extra = 0;
  exec_pos = 0;
  snd_next = ARK_SND_PERIOD;
  time_seconds = 0;
  z80_r = z80_r2 = 0;
  z80_part_done = 0;
  z80_prev_pc = 0xffff;
  frame_timer = 0;
  sync_n = 0;
  abort_run = false;
  watchdog = 0;
  dial_accum = 0;
  dial_speed = 0;
  dial_dir = 0;
  dial_hold = 0;
  cheat_frame = 0;
  boot_done = false;

  mcu_power_on();

  memset(snap, 0, sizeof(snap));
  snap_front = 0;
  render_snap = 0;

  // run synchronised to the display from the first frame (the boot is short)
  game_started = 1;
}

// ============================================================================
// Main CPU memory map - arkanoid_map (arkanoid.cpp)
// ============================================================================

unsigned char IRAM_ATTR arkanoid::opZ80(unsigned short Addr)
{
  return Addr < 0xc000 ? ARK_ROM(Addr) : rdZ80(Addr);
}

unsigned char IRAM_ATTR arkanoid::rdZ80(unsigned short Addr)
{
  if (Addr < 0xc000)                       // map(0x0000, 0xbfff).rom()
    return ARK_ROM(Addr);

  switch (Addr >> 12)
  {
  case 0xc:                                // map(0xc000, 0xc7ff).ram().mirror(0x0800)
    return memory[ARKANOID_WORKRAM + (Addr & 0x07ff)];

  case 0xd:
    // I/O decoded by A4/A3 (74LS139@IC80 side 2); A0 for the AY
    switch (Addr & 0x18)
    {
    case 0x00:                             // map(0xd001, 0xd001).r(aysnd, data_r).mirror(0x0fe6)
      return (Addr & 1) ? ay_read() : 0x00;
    case 0x08:                             // d008 SYSTEM2 mirror 0fe3 / d00c SYSTEM mirror 0fe3
      return (Addr & 4) ? system_r() : 0xff;
    case 0x10:                             // d010 BUTTONS mirror 0fe7
    {
      // bit 0 P1 button 1, bit 2 P2 button 1 (cocktail), all active low;
      // 0x02/0x08/0xf0 IPT_UNKNOWN active low -> read 1
      unsigned char v = 0xff;
      if (in_keys & BUTTON_FIRE)
        v &= ~0x01;
      return v;
    }
    default:                               // d018 MCU data_r mirror 0fe7
      return mcu_data_r();
    }

  case 0xe:
    if (Addr < 0xe800)                     // videoram
      return memory[ARKANOID_VIDEORAM + (Addr & 0x07ff)];
    if (Addr < 0xe840)                     // spriteram
      return memory[ARKANOID_SPRITERAM + (Addr & 0x3f)];
    return memory[ARKANOID_EXTRARAM + (Addr - 0xe840)];

  default:                                 // map(0xf000, 0xffff).nopr(): unmap value 0
    return 0x00;
  }
}

void IRAM_ATTR arkanoid::wrZ80(unsigned short Addr, unsigned char Value)
{
  if (Addr < 0xc000)
    return;

  switch (Addr >> 12)
  {
  case 0xc:
    memory[ARKANOID_WORKRAM + (Addr & 0x07ff)] = Value;
    return;

  case 0xd:
    switch (Addr & 0x18)
    {
    case 0x00:                             // map(0xd000, 0xd001).w(aysnd, address_data_w).mirror(0x0fe6)
      if (Addr & 1)
      {
        // ay8910_write_ym(1, data)
        if (ay_active)
        {
          soundregs[ay_addr] = Value;
          if (ay_addr == 13)
            ay_r13_writes++;
#if ARKANOID_DBG_HARNESS
          if (dbg_ay_hook)
            dbg_ay_hook(ay_addr, Value);
#endif
        }
      }
      else
      {
        // ay8910_write_ym(0, data)
        ay_active = (Value >> 4) == 0;
        if (ay_active)
          ay_addr = Value & 0x0f;
      }
      return;
    case 0x08:                             // d008 mirror 0fe7
      d008_w(Value);
      return;
    case 0x10:                             // d010 watchdog reset_w mirror 0fe7
      watchdog = 0;
      return;
    default:                               // d018 MCU data_w mirror 0fe7
      mcu_data_w(Value);
      return;
    }

  case 0xe:
    if (Addr < 0xe800)
      memory[ARKANOID_VIDEORAM + (Addr & 0x07ff)] = Value;
    else if (Addr < 0xe840)
      memory[ARKANOID_SPRITERAM + (Addr & 0x3f)] = Value;
    else
      memory[ARKANOID_EXTRARAM + (Addr - 0xe840)] = Value;
    return;

  default:                                 // f000-ffff: nothing mapped for writes
    return;
  }
}

// arkanoid_v.cpp arkanoid_d008_w()
void IRAM_ATTR arkanoid::d008_w(unsigned char data)
{
  // bits 0 and 1 flip X and Y
  m_flipx = (data & 0x01) != 0;
  m_flipy = (data & 0x02) != 0;
  // bit 2 selects the input paddle
  m_paddle_select = data & 0x04;
  m_coin_enabled = (data & 0x08) != 0;
  // bit 3 is coin lockout (not emulated: no coin mech), bit 4 unknown
  // bit 5 gfx rom bank, bit 6 palette bank
  m_gfxbank = (data & 0x20) >> 5;
  m_palettebank = (data & 0x40) >> 6;
  // bit 7 resets the MCU and semaphore flipflops (active low)
  mcu_reset_w(!(data & 0x80));
}

// ay8910_read_ym(). Port A = "UNUSED" (0xff), port B = "DSW".
// YM2149: no register masking ("YM2149: no anomaly").
unsigned IRAM_ATTR char arkanoid::ay_read(void)
{
  if (!ay_active)
    return 0xff;
  if (ay_addr == 14)
    soundregs[14] = 0xff;
  else if (ay_addr == 15)
    soundregs[15] = ARKANOID_DSW;
  return soundregs[ay_addr];
}

// ============================================================================
// Inputs - INPUT_PORTS_START( arkanoid ), arkanoid_m.cpp
// ============================================================================

unsigned IRAM_ATTR char arkanoid::system_r(void)
{
  // bit 0 START1, bit 1 START2, bit 2 SERVICE1, bit 3 TILT: active low
  unsigned char v = 0x0f;
  if (in_keys & BUTTON_START)
    v &= ~0x01;
  // bit 4 COIN1, bit 5 COIN2: active high (lockout already applied)
  if (in_keys & BUTTON_COIN)
    v |= 0x10;
  // bits 6-7: arkanoid_semaphore_input_r(), active high custom:
  // bit 0 = host semaphore clear, bit 1 = MCU semaphore clear
  if (!m_host_flag)
    v |= 0x40;
  if (!m_mcu_flag)
    v |= 0x80;
  return v;
}

// input_mux_r(): P1 or P2 spinner by m_paddle_select. No second spinner here,
// so P2 reads what an idle IPT_DIAL reads (its default, 0x00).
unsigned IRAM_ATTR char arkanoid::input_mux_r(void)
{
  if (m_paddle_select)
    return 0x00;
  // analog_field::apply_sensitivity(): lround(accum * 30 / 100); accum is
  // counted in half counts here (keydelta 15 * 0.30 = 4.5 per frame)
  int v = dial_accum >= 0 ? (dial_accum + 1) / 2 : -((-dial_accum + 1) / 2);
  return (unsigned char)v;
}

// ============================================================================
// MC68705P5 + arkanoid_mcu_device_base (taito68705.cpp)
// ============================================================================

uint8_t IRAM_ATTR arkanoid::mcu_rd_hook(m6805_state *s, uint16_t addr)
{
  m68705p5_state *m = (m68705p5_state *)s;
  arkanoid *a = s_instance;
  // port_r<N>(): input-configured pins come from the read callbacks
  if (addr == M68705P5_REG_PORTB)
    m68705p5_port_input_w(m, M68705P5_PORT_B, a->input_mux_r());   // mcu_pb_r()
  else if (addr == M68705P5_REG_PORTC)
    // mcu_pc_r(): PC0 host flag (active high), PC1 MCU flag (active low)
    m68705p5_port_input_w(m, M68705P5_PORT_C,
      (a->m_host_flag ? 0x01 : 0x00) | (a->m_mcu_flag ? 0x00 : 0x02) | 0xfc);
#if ARKANOID_DBG_HARNESS
  if (a->dbg_mcu_r_hook && addr < 0x10)
  {
    uint8_t v = m68705p5_mem_read(m, addr);
    a->dbg_mcu_r_hook(addr, v, a->cur_time() + (uint64_t)m->cpu.cycles * ARK_MCU_APC);
    return v;
  }
#endif
  return m68705p5_mem_read(m, addr);
}

void IRAM_ATTR arkanoid::mcu_wr_hook(m6805_state *s, uint16_t addr, uint8_t val)
{
  m68705p5_state *m = (m68705p5_state *)s;
  arkanoid *a = s_instance;
#if ARKANOID_DBG_HARNESS
  if (a->dbg_mcu_w_hook && addr < 0x10)
    a->dbg_mcu_w_hook(addr, val, a->cur_time() + (uint64_t)m->cpu.cycles * ARK_MCU_APC);
#endif
  unsigned port;
  bool is_ddr;
  switch (addr)
  {
  case M68705P5_REG_PORTA: port = 0; is_ddr = false; break;
  case M68705P5_REG_PORTC: port = 2; is_ddr = false; break;
  case M68705P5_REG_DDRA:  port = 0; is_ddr = true; break;
  case M68705P5_REG_DDRC:  port = 2; is_ddr = true; break;
  default:
    m68705p5_mem_write(m, addr, val);   // port B has no write callback here
    return;
  }
  // m6805_hmos_device::port_latch_w / port_ddr_w: the port callback fires
  // when a latch bit that is an output changes, or when the DDR changes
  unsigned char old_latch = m->port_latch[port], old_ddr = m->port_ddr[port];
  m68705p5_mem_write(m, addr, val);
  bool fire = is_ddr ? (m->port_ddr[port] != old_ddr)
                     : (((old_latch ^ m->port_latch[port]) & m->port_ddr[port]) != 0);
  if (fire)
    a->mcu_port_cb(port);
}

// port_cb_w<N>(): port A is open drain (callback value latch | ~ddr), goes to
// mcu_pa_w() which only records it - pa_value() recomputes the same thing
// from the port registers. Port C is push-pull: the callback gets the latch.
void IRAM_ATTR arkanoid::mcu_port_cb(unsigned port)
{
  if (port == 2)
    mcu_pc_w(mcu.port_latch[M68705P5_PORT_C]);
}

// taito68705_mcu_device_base::pa_value():
//   m_pa_output & (m_latch_driven ? m_host_latch : 0xff)
unsigned IRAM_ATTR char arkanoid::mcu_pa_value(void)
{
  unsigned char pa_out = mcu.port_latch[0] | (unsigned char)~mcu.port_ddr[0];
  return pa_out & (m_latch_driven ? m_host_latch : 0xff);
}

// arkanoid_mcu_device_base::mcu_pc_w() -> latch_control(data, m_pc_output, 2, 3)
void IRAM_ATTR arkanoid::mcu_pc_w(unsigned char data)
{
  unsigned char old_pa_value = mcu_pa_value();

  // rising edge on PC2 clears the host semaphore flag
  if (data & 0x04)
  {
    m_latch_driven = false;
    m68705p5_port_input_w(&mcu, M68705P5_PORT_A, 0xff);
    if (!(m_pc_output & 0x04))
    {
      m_host_flag = false;
      line_w(LINE_MCU_IRQ, false);
    }
  }
  else
  {
    m_latch_driven = true;
    m68705p5_port_input_w(&mcu, M68705P5_PORT_A, m_host_latch);
  }

  // PC3 sets the MCU semaphore when low
  if (!(data & 0x08))
  {
    if (!m_reset_input)
      m_mcu_flag = true;
    // data is latched on falling edge
    if (m_pc_output & 0x08)
      m_mcu_latch = old_pa_value;
  }

  m_pc_output = data;
}

unsigned IRAM_ATTR char arkanoid::mcu_data_r(void)
{
  // clear MCU semaphore flag and return data
  m_mcu_flag = false;
  return m_mcu_latch;
}

void IRAM_ATTR arkanoid::mcu_data_w(unsigned char data)
{
  // set host semaphore flag and latch data
  if (!m_reset_input)
    m_host_flag = true;
  m_host_latch = data;
  if (m_latch_driven)
    m68705p5_port_input_w(&mcu, M68705P5_PORT_A, data);
  line_w(LINE_MCU_IRQ, m_host_flag);
}

void IRAM_ATTR arkanoid::mcu_reset_w(bool assert_line)
{
  m_reset_input = assert_line;
  if (assert_line)
  {
    m_host_flag = false;
    m_mcu_flag = false;
    line_w(LINE_MCU_IRQ, false);
  }
  line_w(LINE_MCU_RESET, assert_line);
}

// Power on: taito68705_mcu_device_base::device_start() + device_reset(),
// arkanoid_mcu_device_base::device_start(), m68705 constructor + reset.
void arkanoid::mcu_power_on(void)
{
  m_latch_driven = false;
  m_reset_input = false;
  m_host_flag = false;
  m_mcu_flag = false;
  m_host_latch = 0xff;
  m_mcu_latch = 0xff;
  m_pc_output = 0xff;
  mcu_in_reset = false;
  mcu_irq_state = false;
  // m_port_input/m_port_latch = 0xff, m_port_ddr = 0 (m6805_hmos_device ctor)
  m68705p5_reset_hooked(&mcu, mcu_rom, mcu_rd_hook, mcu_wr_hook);
}

// CPU reset from the host (m68705_device::device_reset()): only the CPU core,
// DDRs (port_ddr_w(0), which fires the port callbacks) and timer are reset;
// the port latches and inputs keep their values, and so does the interface.
void arkanoid::mcu_device_reset(void)
{
  unsigned char latch[3], input[3], ddr[3];
  memcpy(latch, mcu.port_latch, 3);
  memcpy(input, mcu.port_input, 3);
  memcpy(ddr, mcu.port_ddr, 3);
  m68705p5_reset_hooked(&mcu, mcu_rom, mcu_rd_hook, mcu_wr_hook);
  memcpy(mcu.port_latch, latch, 3);
  memcpy(mcu.port_input, input, 3);
  for (unsigned p = 0; p < 3; p++)
    if (ddr[p] != 0)
      mcu_port_cb(p);
}

// ============================================================================
// Scheduling - a model of MAME's device_scheduler for these two CPUs
// ============================================================================
//
// Times are MAME attoseconds, with MAME's own truncated per-cycle periods -
// the rounding decides whether a CPU gets one more instruction before a
// synced line change, so it has to be the same integer arithmetic:
//   Z80: HZ_TO_ATTOSECONDS(6MHz)          = 166666666666 as per cycle
//   MCU: HZ_TO_ATTOSECONDS(3MHz / 4)      = 1333333333333 as per cycle
//        (m6805_base_device::execute_clocks_to_cycles divides by 4)
//   cycles for a time delta: divu_64x32(delta >> divshift, divisor), where
//   divisor = per-cycle attoseconds shifted right until < 2^31
//   (device_execute_interface::interface_clock_changed).
// A CPU's local time only grows by ran * per-cycle attoseconds.
//
// device_scheduler::timeslice(): while the base time is before the first
// timer, target = min(base + quantum, first timer); each CPU in turn (Z80,
// then MCU) runs the whole cycles up to the target, overshooting by the rest
// of its last instruction; a CPU that ends before the target (its timeslice
// was aborted) pulls the target back to its own time. Then the timers that
// are due fire. Quantum = set_maximum_quantum(6000Hz) = HZ_TO_ATTOSECONDS(6000).
//
// set_input_line() (device_input::set_state_synced) does not change the line
// at once: it queues the change and calls synchronize(), a timer at the
// current time of the running CPU. Being the earliest timer it aborts that
// CPU's timeslice (timer init -> abort_timeslice), the other CPU catches up
// to that time, and only then is the line changed. The HOLD_LINE acknowledge
// of the Z80 IRQ is NOT synced (default_irq_callback -> execute_set_input).
//
// Timers other than those syncs, from screen.cpp: at power on the beam is at
// the start of VBLANK (device_start: m_vblank_start_time = 0), so a frame
// here runs from one VBLANK start to the next. Within it: scanline0 (line 0,
// 24 lines in), vblank end (line 16, 40 lines in; screen_vblank(0) is a no-op
// for a latched HOLD_LINE input) and at the end vblank begin (line 240:
// screen update, then screen_vblank(1) -> Z80 IRQ). MAME's frame N is the
// frame ending at N frame periods, same as run_frame() call N here.
// set_raw(): frame period = HZ_TO_ATTOSECONDS(6MHz) * 384 * 264, line =
// frame / 264.
//
// One more timer does nothing visible but splits timeslices, so it changes
// when each CPU sees the other's writes: sound_manager's stream update,
// m_update_timer->adjust(STREAMS_UPDATE_ATTOTIME, 0, STREAMS_UPDATE_ATTOTIME),
// periodic from time 0 at attotime::from_hz(50).

static const uint64_t ark_frame_timers[3] = { 24 * ARK_SCAN, 40 * ARK_SCAN, ARK_FRAME };

// Where MAME's Z80 touches the I/O area (d000-dfff) inside an instruction.
//
// MAME's Z80 (z80.lst via z80make.py) runs machine cycle by machine cycle:
// each memory access happens at the start of its machine cycle, then that
// cycle's time is charged, and the CPU returns to the scheduler as soon as
// m_icount <= 0 - also in the middle of an instruction. So an I/O access at
// cycle offset o of an instruction only happens in the current run when the
// run still has more than o cycles left; otherwise the CPU pauses at the
// first machine-cycle boundary that uses the run up, the other CPU catches
// up, and the access happens in the next run. Only the I/O area matters
// here (it is what the MCU sees); instructions that do not touch it keep
// running whole, as MAME's instruction boundaries stay the same either way.
//
// Machine cycles (z80.lst): opcode fetch "rop" = 2 + 2, memory read/write
// "rm"/"wm"/"arg" = 3 (m_mreq_cycles), internal "nomreq" = 1.
// opcodes z80_io_access() looks at (a quick filter, one bit per opcode)
static const DRAM_ATTR uint32_t z80_io_candidates[8] = { 0x04040404u, 0x04740000u, 0x40404040u, 0x40bf4040u, 0x40404040u, 0x40404040u, 0x00000800u, 0x00000000u };

static IRAM_ATTR bool z80_io_access(const Z80 &r, uint16_t pc, arkanoid::z80_io_S &io)
{
  if (pc >= 0xc000 - 2)
    return false;
  unsigned char op0 = ARK_ROM(pc);
  if (!((z80_io_candidates[op0 >> 5] >> (op0 & 31)) & 1))
    return false;
  // an instruction never crosses a page boundary here: pc < 0xc000 - 2 and
  // the operand bytes are read one by one
  unsigned char p[3] = { op0, ARK_ROM(pc + 1), ARK_ROM(pc + 2) };
  unsigned char op = p[0];
  uint16_t addr;
  static const unsigned char rw16[] = { 2, 2, 3, 3, 3 };        // LD A,(nn) / LD (nn),A
  static const unsigned char rw8[] = { 2, 2, 3 };               // LD r,(HL) ... / (BC) / (DE)
  static const unsigned char ldhln[] = { 2, 2, 3, 3 };          // LD (HL),n
  static const unsigned char incdec[] = { 2, 2, 3, 1, 3 };      // INC/DEC (HL)
  static const unsigned char cbbit[] = { 2, 2, 2, 2, 3, 1 };    // BIT b,(HL)
  static const unsigned char cbrmw[] = { 2, 2, 2, 2, 3, 1, 3 }; // RES/SET/shift (HL)
  const unsigned char *st;
  int n;
  if (op == 0x3a || op == 0x32)
  {
    addr = p[1] | (p[2] << 8); st = rw16; n = 5; io.offset = 10;
  }
  else if (op == 0x0a || op == 0x02)
  {
    addr = r.BC.W; st = rw8; n = 3; io.offset = 4;
  }
  else if (op == 0x1a || op == 0x12)
  {
    addr = r.DE.W; st = rw8; n = 3; io.offset = 4;
  }
  else if ((op >= 0x40 && op <= 0x7f && op != 0x76 && ((op & 7) == 6 || (op & 0xf8) == 0x70)) ||
           (op >= 0x80 && op <= 0xbf && (op & 7) == 6))
  {
    addr = r.HL.W; st = rw8; n = 3; io.offset = 4;
  }
  else if (op == 0x36)
  {
    addr = r.HL.W; st = ldhln; n = 4; io.offset = 7;
  }
  else if (op == 0x34 || op == 0x35)
  {
    addr = r.HL.W; st = incdec; n = 5; io.offset = 4;
  }
  else if (op == 0xcb && (p[1] & 7) == 6)
  {
    addr = r.HL.W;
    bool bit = (p[1] & 0xc0) == 0x40;
    st = bit ? cbbit : cbrmw; n = bit ? 6 : 7; io.offset = 8;
  }
  else
    return false;
  if ((addr & 0xf000) != 0xd000)
    return false;
  io.nsteps = (unsigned char)n;
  memcpy(io.steps, st, n);
  return true;
}

// Time of the instruction being executed (only worked out when needed):
// the running CPU's local time at the start of its run + cycles since.
uint64_t IRAM_ATTR arkanoid::cur_time(void) const
{
  return exec_base + (uint64_t)(int64_t)exec_pos * (exec_z80 ? ARK_Z80_APC : ARK_MCU_APC);
}

void IRAM_ATTR arkanoid::line_w(unsigned char line, bool state)
{
  // The sync timer expires at the running CPU's time at the access itself
  // (device_scheduler::time() = local time + cycles executed so far). The
  // 6805 charges an instruction's cycles after executing it, so its accesses
  // are at the instruction start; the Z80's are exec_io_offset cycles into
  // the instruction (z80_io_access()).
  uint64_t t = cur_time() + exec_extra;
  if (exec_z80)
    t += (uint64_t)exec_io_offset * ARK_Z80_APC;
  else if (line != LINE_NONE)
    t += (uint64_t)mcu.cpu.cycles * ARK_MCU_APC;   // after an interrupt entry in this step
  if (sync_n < (int)(sizeof(sync_q) / sizeof(sync_q[0])))
  {
    sync_q[sync_n].time = t;
    sync_q[sync_n].line = line;
    sync_q[sync_n].state = state;
    sync_n++;
  }
  // abort the running CPU's timeslice
  abort_run = true;
}

void IRAM_ATTR arkanoid::line_apply(unsigned char line, bool state)
{
  if (line == LINE_NONE)
    return;
  if (line == LINE_MCU_IRQ)
  {
    // m6805_hmos_device::execute_set_input(): only a change of the line
    // level latches an interrupt (m_irq_state, kept across MCU resets)
    if (state != mcu_irq_state)
    {
      mcu_irq_state = state;
      m68705p5_irq(&mcu, state ? 1 : 0);
    }
  }
  else
  {
    // device_execute_interface INPUT_LINE_RESET: asserting suspends the CPU,
    // clearing a previously asserted line resets and resumes it
    if (state)
      mcu_in_reset = true;
    else if (mcu_in_reset)
    {
      mcu_device_reset();
      mcu_in_reset = false;
    }
  }
}

// Refresh register. The local core only fakes LD A,R (R - ICount). MAME
// (z80.lst): every opcode fetch "rop" does R++ - the opcode, each DD/FD
// prefix and the byte after CB/ED; for DD/FD CB d op only DD and CB are
// opcode fetches. A halted CPU re-fetches (R++ each 4 cycles) and an
// interrupt acknowledge does m_r++. LD A,R returns (r & 0x7f) | r2 with S,
// Z and the undocumented 5/3 bits from the value, H = N = 0, P/V = IFF2 and
// C kept; LD R,A sets r = A, r2 = A & 0x80. (HAS_LDAIR_QUIRK is 0 for the
// plain Z80, z80.inc.) This ROM only runs code from ROM.
static IRAM_ATTR unsigned char z80_opcode_fetches(uint16_t pc)
{
  if (pc >= 0xc000 - 4)
    return 1;
  unsigned char b[5] = { ARK_ROM(pc), ARK_ROM(pc + 1), ARK_ROM(pc + 2), ARK_ROM(pc + 3), 0 };
  const unsigned char *p = b;
  unsigned char n = 0;
  while (*p == 0xdd || *p == 0xfd)
  {
    n++;
    p++;
    if (n > 3)
      return n;
  }
  if (n && *p == 0xcb)
    return n + 1;                  // DD/FD CB d op
  if (*p == 0xcb || *p == 0xed)
    return n + 2;
  return n + 1;
}

// Z80 StepZ80() leaves ICount at 0 after HALT and at 1 after an EI that
// enables interrupts (it never restores IBackup), so cycles are measured per
// instruction from a large start value and those two cases are charged the
// 4 cycles both opcodes take. The EI case also delays the IRQ check by one
// instruction, as on the real CPU.
int IRAM_ATTR arkanoid::z80_exec(int want)
{
  static const int START = 1 << 20;
  int budget = want;
  abort_run = false;
  exec_z80 = true;
  exec_base = z80_t;
  exec_extra = 0;
  current_cpu = 0;
  while (budget > 0 && !abort_run)
  {
    z80_io_S io;
    bool has_io;
    if (z80_part_done)
    {
      // finishing an instruction MAME paused inside (no IRQ can be taken)
      io = z80_part_io;
      has_io = true;
    }
    else if (z80_irq && (cpu[0].IFF & IFF_1) && !ei_shadow)
    {
      // screen_vblank -> set_inputline(m_maincpu, 0, HOLD_LINE): no vector
      // callback, data bus 0xff = RST 38h; HOLD_LINE drops on acknowledge.
      // z80.lst take_interrupt IM1: 2 + 5 + two 3-cycle stack writes = 13
      exec_pos = want - budget;
      exec_pc = cpu[0].PC.W;
      exec_io_offset = 0;
      IntZ80(&cpu[0], INT_IRQ);
      z80_r++;
      z80_irq = false;
      z80_prev_pc = 0xffff;
      budget -= 13;
      continue;
    }
    else
    {
      // Idle loops, skipped in whole iterations (see z80_idle_skip)
      if (ARKANOID_IDLE_SKIP && z80_idle_skip(want, budget))
        continue;
      has_io = z80_io_access(cpu[0], cpu[0].PC.W, io);
    }

    int done = z80_part_done;
    if (has_io && io.offset - done >= budget)
    {
      // MAME stops at the first machine-cycle boundary that uses up the
      // run, before the I/O access; the rest of the instruction runs later
      int k = 0, sum = 0;
      while (sum <= done)
        sum += io.steps[k++];            // first boundary after `done`
      int used = sum - done;
      while (used < budget)
      {
        sum += io.steps[k++];
        used = sum - done;
      }
      z80_part_io = io;
      z80_part_done = (unsigned char)sum;
      budget -= used;
      break;
    }

    exec_pos = want - budget - done;                 // instruction start
    exec_pc = cpu[0].PC.W;
    exec_io_offset = has_io ? io.offset : 0;
    // Block instructions (LDIR, CPIR, INIR, OTIR and the decrementing ones):
    // MAME's z80.lst runs one iteration per instruction pass and repeats it
    // with PC -= 2, so interrupts and timeslice ends fall between iterations.
    // The local core loops while ICount > 0; entering with 1 makes it do one
    // iteration (21 cycles, 16 for the last) and rewind PC the same way.
    unsigned char op = exec_pc < 0xc000 - 4 ? ARK_ROM(exec_pc) : 0x00;
    unsigned char op2 = 0;
    if (op == 0xdd || op == 0xfd || op == 0xcb || op == 0xed)
    {
      z80_r += z80_opcode_fetches(exec_pc);
      op2 = ARK_ROM(exec_pc + 1);
    }
    else
      z80_r++;
    bool ld_a_r = op == 0xed && op2 == 0x5f;
    bool ld_r_a = op == 0xed && op2 == 0x4f;
    int c;
    if (op == 0xed && (op2 & 0xf4) == 0xb0)
    {
      cpu[0].ICount = 1;
      StepZ80(&cpu[0]);
      c = 1 - cpu[0].ICount;
      ei_shadow = false;
    }
    else
    {
      cpu[0].ICount = START;
      StepZ80(&cpu[0]);
      int left = cpu[0].ICount;
      ei_shadow = (left == 1);
      c = (left <= 1) ? 4 : START - left;
    }
    if (ld_a_r)
    {
      unsigned char a = (z80_r & 0x7f) | z80_r2;
      cpu[0].AF.B.h = a;
      cpu[0].AF.B.l = (cpu[0].AF.B.l & C_FLAG) | (a & (S_FLAG | 0x28)) | (a ? 0 : Z_FLAG) |
                      ((cpu[0].IFF & IFF_2) ? P_FLAG : 0);
    }
    else if (ld_r_a)
    {
      z80_r = cpu[0].AF.B.h;
      z80_r2 = cpu[0].AF.B.h & 0x80;
    }
#if ARKANOID_DBG_HARNESS
    if (dbg_z80_pc_hook)
      dbg_z80_pc_hook(exec_pc, dbg_cycles(cur_time()));
    dbg_prefix[op == 0xdd ? 0 : op == 0xfd ? 1 : op == 0xcb ? 2 : op == 0xed ? 3 : 4]++;
    dbg_pc_count[exec_pc]++;
    dbg_z80_steps++;
    if (cpu[0].IFF & IFF_HALT)
      dbg_z80_halt_steps++;
#endif
    z80_part_done = 0;
    z80_prev_pc = exec_pc;
    budget -= c - done;
  }
  exec_z80 = false;
  return want - budget;
}

// The Z80 spends ~80% of its instructions in two polling loops:
//   03c5 ld a,($C435) / 03c8 and a / 03c9 jr z,$03C5   (13+4+12 = 29 cycles)
//        waits for the flag the vblank IRQ handler sets
//   035f ld a,($D00C) / 0362 bit 6,a / 0364 jr z,$035F (13+8+12 = 33 cycles)
//        waits for the MCU to take the host latch (boot ROM check)
// Called at the loop's first instruction when the previous one was the
// loop's JR (so A and F already hold what one more iteration would leave,
// and no EI shadow is pending), after the IRQ check: z80_irq only changes
// between runs (vblank_begin), so no IRQ is taken for the rest of this run.
// What the loop reads cannot change within the run either: c435 is only
// written by the Z80, and d00c (system_r) depends on inputs sampled at
// vblank and on the semaphore flags, which only the MCU (not running now)
// or a Z80 write change. So the iterations the normal path would run are
// charged here in bulk: one iteration runs whole when the budget covers the
// start of its last instruction (and, for d00c, the I/O access 10 cycles
// into the LD - z80_io_access); the partial one is left to the normal path.
// R: one per opcode fetch (z80_opcode_fetches). The flags after BIT 6 with bit 6
// clear do not depend on A (C kept, H, Z/PV set).
bool IRAM_ATTR arkanoid::z80_idle_skip(int want, int &budget)
{
  unsigned short pc = cpu[0].PC.W;
  int n, jr, fetches;
  if (pc == 0x03c5 && z80_prev_pc == 0x03c9)
  {
    jr = 4;
    fetches = 3;
    if (memory[ARKANOID_WORKRAM + 0x435] != 0 || budget <= 17)
      return false;
    n = (budget - 18) / 29 + 1;
    budget -= n * 29;
  }
  else if (pc == 0x035f && z80_prev_pc == 0x0364)
  {
    jr = 5;
    fetches = 4;                     // BIT is CB-prefixed: two opcode fetches
    unsigned char v = system_r();
    if ((v & 0x40) || budget <= 21)
      return false;
#if ARKANOID_DBG_HARNESS
    if (dbg_z80_skip_rd_hook)
      for (int i = 0, b = budget; b > 21; i++, b -= 33)
      {
        exec_pos = want - b;
        exec_io_offset = 10;
        dbg_z80_skip_rd_hook(0xd00c, v, dbg_io_now());
      }
#endif
    n = (budget - 22) / 33 + 1;
    budget -= n * 33;
    cpu[0].AF.B.h = v;
  }
  else
    return false;
  z80_r += fetches * n;
#if ARKANOID_DBG_HARNESS
  if (dbg_z80_pc_hook)
  {
    static const unsigned char off03c5[3] = { 0, 13, 17 }, off035f[3] = { 0, 13, 21 };
    const unsigned char *off = pc == 0x03c5 ? off03c5 : off035f;
    int per = pc == 0x03c5 ? 29 : 33;
    for (int i = 0; i < n; i++)
      for (int k = 0; k < 3; k++)
      {
        exec_pos = want - (budget + (n - i) * per) + off[k];
        dbg_z80_pc_hook(k == 0 ? pc : k == 1 ? pc + 3 : pc + jr, dbg_cycles(cur_time()));
      }
  }
  dbg_pc_count[pc] += n;
  dbg_pc_count[pc + 3] += n;
  dbg_pc_count[pc + jr] += n;
  dbg_z80_steps += 3 * n;
  dbg_prefix[pc == 0x035f ? 2 : 4] += n;
  dbg_prefix[4] += 2 * n;
#endif
  return true;
}

int IRAM_ATTR arkanoid::mcu_exec(int want)
{
  int budget = want;
  abort_run = false;
  exec_base = mcu_t;
  exec_extra = 0;
  while (budget > 0 && !abort_run)
  {
    int skipped = ARKANOID_IDLE_SKIP ? mcu_idle_skip(want, budget) : 0;
    if (skipped)
    {
      budget -= skipped;
      continue;
    }
    exec_pos = want - budget;
#if ARKANOID_DBG_HARNESS
    if (dbg_mcu_pc_hook)
      dbg_mcu_pc_hook(mcu.cpu.PC, cur_time());
#endif
    uint8_t syncs = mcu.timer_line_syncs;
    mcu.timer_sync_off = 0xff;
    budget -= m68705p5_step(&mcu, 1);
    // m6805_timer tcr_w()/update() -> set_input_line(M6805_INT_TIMER):
    // synced in MAME - a timer at that moment, and the end of this
    // timeslice. The core has already changed the line itself, which MAME
    // would also do before the MCU runs again, so the event carries nothing.
    if (mcu.timer_line_syncs != syncs)
    {
      exec_extra = (uint64_t)mcu.timer_sync_off * ARK_MCU_APC;
      line_w(LINE_NONE, false);
      exec_extra = 0;
    }
  }
  return want - budget;
}

// The MCU spends ~99% of its steps in its idle loop, waiting for the host
// IRQ or the timer interrupt (a75__06.ic14):
//   110 cli / 111 brn $111 / 113 bra $111
// BRN and BRA take 4 cycles (m6805.cpp s_hmos_cycles row 2) and touch no
// register or RAM. With no interrupt latched (the IRQ line only changes
// between runs, in line_apply) the only thing that can end the loop within
// this run is the timer, which MAME advances after every opcode
// (burn_cycles -> m6805_timer::update). So whole steps are charged here with
// the same per-step timer arithmetic as timer_update() in m68705p5.c,
// stopping BEFORE the step that would reach the zero crossing: that step
// (TIR, the interrupt, timer_line_syncs and the timeslice end it causes)
// runs through the normal path. Returns the cycles charged (0 = none).
int IRAM_ATTR arkanoid::mcu_idle_skip(int want, int budget)
{
  uint16_t pc = mcu.cpu.PC;
  if ((pc != 0x111 && pc != 0x113) || mcu.cpu.pending_interrupts != 0)
    return 0;
  int n = 0;
  if (mcu.timer_source_ext)
    n = (budget + 3) / 4;
  else
  {
    unsigned div = mcu.timer_divisor, mask = (1u << div) - 1;
    unsigned p = mcu.timer_prescale;
    unsigned tdr = mcu.timer_tdr;
    for (int b = budget; b > 0; b -= 4, n++)
    {
      unsigned prescale = (p & mask) + 4;
      unsigned dec = prescale >> div;
      if ((tdr ? tdr : 256u) <= dec)
        break;
      p = prescale & 0x7f;
      tdr -= dec;
    }
    mcu.timer_prescale = (uint8_t)p;
    mcu.timer_tdr = (uint8_t)tdr;
  }
  if (n == 0)
    return 0;
#if ARKANOID_DBG_HARNESS
  if (dbg_mcu_pc_hook)
    for (int i = 0; i < n; i++)
    {
      exec_pos = want - budget + 4 * i;
      dbg_mcu_pc_hook((i & 1) ? (pc ^ 0x111 ^ 0x113) : pc, cur_time());
    }
#endif
  if (n & 1)
    mcu.cpu.PC = pc ^ 0x111 ^ 0x113;
  mcu.cpu.cycles = 4;
  mcu.cpu.total_cycles += 4 * n;
  return 4 * n;
}

uint64_t IRAM_ATTR arkanoid::first_timer(void)
{
  uint64_t t = frame_start + ark_frame_timers[frame_timer];
  if (snd_next < t)
    t = snd_next;
  for (int i = 0; i < sync_n; i++)
    if (sync_q[i].time < t)
      t = sync_q[i].time;
  return t;
}

void IRAM_ATTR arkanoid::run_frame(void)
{
#if ARKANOID_PROFILE && !ARKANOID_DBG_HARNESS
  uint32_t prof_t0 = micros();
#endif
  // Keep the 64-bit attosecond clock small: moving every time back by one
  // whole second is what MAME's attotime seconds field does.
  if (base >= ARK_ATTOS_PER_SECOND)
  {
    base -= ARK_ATTOS_PER_SECOND;
    z80_t -= ARK_ATTOS_PER_SECOND;
    mcu_t -= ARK_ATTOS_PER_SECOND;
    frame_start -= ARK_ATTOS_PER_SECOND;
    snd_next -= ARK_ATTOS_PER_SECOND;   // 1s is a whole number of periods
    time_seconds++;
    for (int i = 0; i < sync_n; i++)
      sync_q[i].time -= ARK_ATTOS_PER_SECOND;
  }

  frame_timer = 0;
  for (;;)
  {
    // ---- device_scheduler::timeslice()
    while (base < first_timer())
    {
      uint64_t target = base + ARK_QUANTUM;
      uint64_t ft = first_timer();
      if (ft < target)
        target = ft;

      if (target > z80_t && target - z80_t >= ARK_Z80_APC)
      {
        int want = (int)(((target - z80_t) >> ARK_Z80_SHIFT) / ARK_Z80_DIV);
#if ARKANOID_PROFILE && !ARKANOID_DBG_HARNESS
        uint32_t pz = micros();
        z80_t += (uint64_t)z80_exec(want) * ARK_Z80_APC;
        prof_z80 += micros() - pz;
#else
        z80_t += (uint64_t)z80_exec(want) * ARK_Z80_APC;
#endif
        if (z80_t < target)
          target = z80_t > base ? z80_t : base;
      }
      if (target > mcu_t && target - mcu_t >= ARK_MCU_APC)
      {
        int want = (int)(((target - mcu_t) >> ARK_MCU_SHIFT) / ARK_MCU_DIV);
        // a CPU held in reset is suspended with eatcycles: its time advances
#if ARKANOID_PROFILE && !ARKANOID_DBG_HARNESS
        uint32_t pm = micros();
        int ran = mcu_in_reset ? want : mcu_exec(want);
        prof_mcu += micros() - pm;
#else
        int ran = mcu_in_reset ? want : mcu_exec(want);
#endif
        mcu_t += (uint64_t)ran * ARK_MCU_APC;
        if (mcu_t < target)
          target = mcu_t > base ? mcu_t : base;
      }
      base = target;
#if ARKANOID_DBG_HARNESS
      if (dbg_slice_hook)
        dbg_slice_hook(base, z80_t, mcu_t);
#endif
    }

    // ---- execute_timers(): everything due, in time order
    bool frame_done = false;
    for (;;)
    {
      uint64_t ft = frame_start + ark_frame_timers[frame_timer];
      int si = -1;
      for (int i = 0; i < sync_n; i++)
        if (sync_q[i].time <= base && (si < 0 || sync_q[i].time < sync_q[si].time))
          si = i;
      if (snd_next <= base && snd_next <= ft && (si < 0 || snd_next <= sync_q[si].time))
      {
        snd_next += ARK_SND_PERIOD;      // sound_manager::update(): no CPU-visible effect
        continue;
      }
      if (si >= 0 && sync_q[si].time < ft)
      {
        sync_S e = sync_q[si];
        for (int i = si; i < sync_n - 1; i++)
          sync_q[i] = sync_q[i + 1];
        sync_n--;
        line_apply(e.line, e.state);
        continue;
      }
      if (ft > base)
        break;
      if (frame_timer == 2)
      {
        vblank_begin();
        frame_start += ARK_FRAME;
        frame_done = true;
        break;
      }
      frame_timer++;
    }
    if (frame_done)
      break;
  }

#if ARKANOID_PROFILE && !ARKANOID_DBG_HARNESS
  // Emulation cost per frame on the device. A frame is 16.9ms of game time;
  // printed once a second.
  static uint32_t prof_sum = 0, prof_max = 0, prof_n = 0;
  uint32_t dt = micros() - prof_t0;
  prof_sum += dt;
  if (dt > prof_max)
    prof_max = dt;
  if (++prof_n == 60)
  {
    printf("arkanoid: run_frame avg %lu us, max %lu us (z80 %lu us, mcu %lu us), budget 16896 us\n",
           (unsigned long)(prof_sum / 60), (unsigned long)prof_max,
           (unsigned long)(prof_z80 / 60), (unsigned long)(prof_mcu / 60));
    prof_sum = prof_max = prof_n = 0;
    prof_z80 = prof_mcu = 0;
  }
#endif
}

// screen_device::vblank_begin(): screen update, then screen_vblank(1)
void arkanoid::vblank_begin(void)
{
  publish_snapshot();

  // update_if_primary() -> video_manager::frame_update() ->
  // ioport_manager::frame_update(): every input is sampled here, once a
  // frame, and holds until the next frame.
  //  - ioport_field::frame_update(): a coin input reads as not pressed while
  //    the game holds its coin lockout (d008 bit 3 low, arkanoid_d008_w ->
  //    coin_lockout_w; option coin_lockout defaults to on)
  //  - analog_field::frame_update(): the dial moves once per frame while a
  //    key is held; IPT_DIAL increment = right. Speed: ARKANOID_DIAL_* in
  //    arkanoid.h (MAME's own keyboard rate is 9 half counts, constant).
  in_keys = input->buttons_get();
  if (!m_coin_enabled)
    in_keys &= ~BUTTON_COIN;
  int dir = ((in_keys & BUTTON_RIGHT) ? 1 : 0) - ((in_keys & BUTTON_LEFT) ? 1 : 0);
  if (dir == 0 || dir != dial_dir)
  {
    dial_speed = ARKANOID_DIAL_START;
    dial_hold = 0;
  }
  else if (++dial_hold < ARKANOID_DIAL_DELAY)
    dial_speed = ARKANOID_DIAL_START;
  else if (dial_speed + ARKANOID_DIAL_ACCEL <= ARKANOID_DIAL_MAX)
    dial_speed += ARKANOID_DIAL_ACCEL;
  else
    dial_speed = ARKANOID_DIAL_MAX;
  dial_dir = dir;
  dial_accum += dir * dial_speed;

  apply_cheats();

  z80_irq = true;

  // WATCHDOG_TIMER set_vblank_count("screen", 128) -> soft reset
  if (++watchdog >= 128)
  {
    printf("arkanoid: watchdog reset\n");
    watchdog = 0;
    ResetZ80(&cpu[0]);
    m_gfxbank = m_palettebank = m_paddle_select = 0;
    ay_active = false;
    ay_addr = 0;
    memset(soundregs, 0, sizeof(soundregs));
    z80_irq = false;
    ei_shadow = false;
    z80_part_done = 0;
    z80_prev_pc = 0xffff;
    boot_done = false;
    sync_n = 0;
    mcu_power_on();
  }
}

// arkanoid_cheats.h: MAME cheat "run" scripts, once a frame. Pokes into Z80
// RAM (c000-c7ff work RAM, e840-efff extra RAM), plus one program patch.
void arkanoid::apply_cheats(void)
{
#if ARKANOID_CHEATS
  cheat_frame++;

  // The boot streams the whole program ROM to the MCU as a checksum and
  // tests the RAM: a patched byte or a poke then shows BAD HARDWARE. Once
  // passed, the Z80 waits for each vblank in its main loop (03c5-03c9, the
  // idle loop) - that is where it is at vblank from then on.
  if (!boot_done && cpu[0].PC.W >= 0x03c5 && cpu[0].PC.W <= 0x03c9)
    boot_done = true;

#if ARKANOID_CHEAT_ONE_HIT_BRICKS
  // patch / restore every frame (2 bytes), so a watchdog reset never boots
  // with it in place; only possible with program page 1 (4000-7fff) in RAM
  if (rom_ram[1])
  {
    rom_ram[1][0x58c7 - 0x4000] = boot_done ? 0x18 : arkanoid_rom[0x58c7];
    rom_ram[1][0x5909 - 0x4000] = boot_done ? 0xc3 : arkanoid_rom[0x5909];
  }
#endif
  if (!boot_done)
    return;

#define C_RAM(a) memory[ARKANOID_WORKRAM + ((a) - 0xc000)]    // c000-c7ff
#define E_RAM(a) memory[ARKANOID_EXTRARAM + ((a) - 0xe840)]   // e840-efff
#if ARKANOID_CHEAT_KEEP_BALL
  if (C_RAM(0xc4a5) > 0xe4)
    C_RAM(0xc4a5) = 0xe2;
#endif
#if ARKANOID_CHEAT_DONT_DIE
  E_RAM(0xef62) = 0x00;
#endif
#if ARKANOID_CHEAT_INFINITE_LIVES
  E_RAM(0xed76) = 0x06;
#endif
#if ARKANOID_CHEAT_INFINITE_LIVES_P2
  E_RAM(0xed7b) = 0x06;
#endif
#if ARKANOID_CHEAT_INFINITE_CREDITS
  C_RAM(0xc432) = 0x09;
#endif
#if ARKANOID_CHEAT_BALL_SPEED
  C_RAM(0xc462) = ARKANOID_CHEAT_BALL_SPEED;
#endif
#if ARKANOID_CHEAT_WARP == 1
  C_RAM(0xc4ce) = 0x01;
#elif ARKANOID_CHEAT_WARP == 2
  C_RAM(0xc4ce) = 0x00;
#endif
#if ARKANOID_CHEAT_LAST_LEVEL
  E_RAM(0xed72) = 0x20;
#endif
#if ARKANOID_CHEAT_PILL_EVERY_5S
  if (cheat_frame % 300 == 0)
    C_RAM(0xc658) = ARKANOID_CHEAT_PILL_EVERY_5S;
#endif
#undef C_RAM
#undef E_RAM
#endif
}

// ============================================================================
// Video - arkanoid_v.cpp screen_update_arkanoid()
// ============================================================================

void arkanoid::publish_snapshot(void)
{
  unsigned char back = snap_front ^ 1;
  snap_S &s = snap[back];
  memcpy(s.vram, &memory[ARKANOID_VIDEORAM], sizeof(s.vram));
  memcpy(s.spr, &memory[ARKANOID_SPRITERAM], sizeof(s.spr));
  s.gfxbank = m_gfxbank;
  s.palettebank = m_palettebank;
  s.flipx = m_flipx;
  s.flipy = m_flipy;
  snap_front = back;
}

// Screen mapping. The game is ROT90 (ORIENTATION_SWAP_XY | FLIP_X): MAME
// pixel (x, y), x 0-255, y 16-239, is shown at rotated column 239 - y
// (0-223) and rotated row x (0-255). Rotated row x is panel line
// ARKANOID_ROW_OFFSET + x, so the 8-line strip `row` holds x = row*8 - 16 ..
// +7, i.e. exactly one tilemap column.
void IRAM_ATTR arkanoid::render_row(short row)
{
  if (row == 0)
    render_snap = snap_front;
  const snap_S &s = snap[render_snap];

  int x0 = row * 8 - ARKANOID_ROW_OFFSET;
  if (x0 < 0 || x0 >= 256)
    return;   // frame_buffer was cleared by the caller

  const unsigned short *pal = arkanoid_palette;

  // ---- tilemap: TILEMAP_SCAN_ROWS 32x32, opaque, flip mirrors the whole
  // 256x256 map (tilemap set_flip_all from flip_screen_x/y)
  for (int r = 0; r < 8; r++)
  {
    int x = x0 + r;
    int tx = s.flipx ? 255 - x : x;
    unsigned short *fb = frame_buffer + r * 224;
    for (int col = 0; col < 224; col++)
    {
      int y = 239 - col;
      int ty = s.flipy ? 255 - y : y;
      // get_bg_tile_info()
      int offs = ((ty >> 3) * 32 + (tx >> 3)) * 2;
      int code = s.vram[offs + 1] + ((s.vram[offs] & 0x07) << 8) + 2048 * s.gfxbank;
      int color = ((s.vram[offs] & 0xf8) >> 3) + 32 * s.palettebank;
      fb[col] = pal[color * 8 + arkanoid_gfx[code][ty & 7][tx & 7]];
    }
  }

  // ---- sprites: draw_sprites(), 16 x (two 8x8 cells), later ones on top,
  // transparent pen 0, clipped to the visible area (x 0-255, y 16-239)
  for (int offs = 0; offs < 0x40; offs += 4)
  {
    int sx = s.spr[offs];
    int sy = 248 - s.spr[offs + 1];
    if (s.flipx)
      sx = 248 - sx;
    if (s.flipy)
      sy = 248 - sy;
    if (sx + 8 <= x0 || sx >= x0 + 8)
      continue;

    int code = s.spr[offs + 3] + ((s.spr[offs + 2] & 0x03) << 8) + 1024 * s.gfxbank;
    int color = ((s.spr[offs + 2] & 0xf8) >> 3) + 32 * s.palettebank;
    const unsigned short *cpal = pal + color * 8;

    for (int cell = 0; cell < 2; cell++)
    {
      int tcode = 2 * code + cell;
      int cy = cell == 0 ? sy + (s.flipy ? 8 : -8) : sy;
      for (int i = 0; i < 8; i++)
      {
        int x = sx + i;
        if (x < x0 || x >= x0 + 8 || x > 255)
          continue;
        int px = s.flipx ? 7 - i : i;
        unsigned short *fb = frame_buffer + (x - x0) * 224;
        for (int j = 0; j < 8; j++)
        {
          int y = cy + j;
          if (y < 16 || y > 239)
            continue;
          int py = s.flipy ? 7 - j : j;
          unsigned char pen = arkanoid_gfx[tcode][py][px];
          if (pen)
            fb[239 - y] = cpal[pen];
        }
      }
    }
  }
}

// ============================================================================
// Menu, high scores, LEDs
// ============================================================================

const unsigned short *arkanoid::logo(void)
{
  return arkanoid_logo;
}

// MAME plugins/hiscore/hiscore.dat, entry shared by arkanoid & clones:
//   @:maincpu,program,ef79,23,00,52
//   @:maincpu,program,c4df,03,00,00
const hiscore_region_S *arkanoid::hiscoreRegions(unsigned char *count)
{
  static const hiscore_region_S regions[] = {
    { 0xef79, 0x23, 0x00, 0x52 },
    { 0xc4df, 0x03, 0x00, 0x00 },
  };
  *count = sizeof(regions) / sizeof(regions[0]);
  return regions;
}

#ifdef LED_PIN
// A row of coloured bricks with the energy ball bouncing across it.
void arkanoid::gameLeds(CRGB *leds)
{
  static const CRGB bricks[] = { LED_RED, LED_YELLOW, LED_BLUE, LED_MAGENTA, LED_GREEN, LED_CYAN };
  static char sub_cnt = 0;
  static char pos = 0;             // 0 .. 2*NUM_LEDS-3, back and forth
  if (sub_cnt++ < 8)
    return;
  sub_cnt = 0;

  char ball = (pos < NUM_LEDS) ? pos : ((2 * NUM_LEDS - 2) - pos);
  for (char c = 0; c < NUM_LEDS; c++)
    leds[c] = (c == ball) ? LED_WHITE : bricks[c % (sizeof(bricks) / sizeof(bricks[0]))];
  pos = (pos + 1) % (2 * NUM_LEDS - 2);
}

void arkanoid::menuLeds(CRGB *leds)
{
  memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
#endif
