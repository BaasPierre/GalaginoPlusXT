#include "flstory.h"

// The FairyLand Story (Taito, 1985). See flstory.h for the general porting
// notes and source/mame/mame-master/src/mame/taito/flstory.cpp for ground
// truth (flstory_state/flstory_mcu_state, base_map/flstory_map, ROM_START
// (flstory)). v2026-09-19

static_assert(FLSTORY_MEM_END <= RAMSIZE, "RAMSIZE too low for flstory");

// MSM5232 envelope rate tables - defined further down (next to the rest of
// the MSM5232 model); forward-declared here because reset() initialises the
// voices from them and sits earlier in this file.
extern float flstory_msm_ar_tbl[8];
extern float flstory_msm_dr_tbl[16];
extern bool  flstory_msm_tables_built;
void flstory_msm_build_rate_tables(uint32_t chip_clock);

// ============================================================================
// m6805 core global callbacks (see src/cpus/m6805/m6805.h: "External
// callbacks - must be provided by the machine [...] or by the variant/glue
// layer sitting between the machine and this core, e.g. m68705p5.c
// intercepts these to serve its own ports/timer/EPROM before forwarding
// anything else to the machine"). flstory is the FIRST machine using the
// m6805 family, so nothing defines these yet anywhere else in the
// codebase - but m6805.c/m68705p5.c/taito68705_mcu.c are always compiled
// (PlatformIO globs every .c/.cpp under src/), so these externs must exist
// unconditionally in the final binary regardless of whether ENABLE_FLSTORY
// is defined, same as flstory.cpp itself is always compiled.
//
// MCU handshake never completing): these two functions used to
// be no-op stubs returning 0xFF unconditionally, on the theory that
// m68705p5_mem_read()/write() "fully decode the MCU's entire address space
// before falling through to this". That theory is WRONG for the actual
// call graph: m68705p5_mem_read/write are only invoked from the HOST
// side - flstory::rdZ80/wrZ80 call them (indirectly, via
// taito68705_data_r/w) when the MAIN Z80 touches the MCU's data port
// register (0xD000). They are NEVER in the call path of the MCU's OWN
// instruction execution: m6805_step()'s fetch8()/rd()/wr() (m6805.c) -
// used for every opcode/operand fetch, the RESET vector load in
// m6805_reset(), the interrupt-vector load AND the return-address/
// register push/pop in service_interrupt() - all go through THESE global
// externs directly, with no m68705p5 involvement at all. With them
// stubbed to return 0xFF, the MCU's reset vector read as 0xFFFF instead
// of the ROM's real 0x0570 entry point, and every subsequent instruction
// fetch read 0xFF (opcode 0xFF = "STX indexed", a real but meaningless
// opcode) instead of the MCU's actual firmware - i.e. the MCU was
// SILENTLY NEVER EXECUTING ITS REAL PROGRAM AT ALL, just free-running
// through an infinite stream of STX-indexed no-ops. This is why
// host_flag would go to 1 (the main CPU's write to 0xD000 still worked,
// since that side genuinely does go through m68705p5_mem_write) but
// mcu_flag never followed - the MCU never ran the code that would have
// set it. PC read 0x07FF right after
// taito68705_reset(), then incremented one byte at a time from address
// 0x0000 (a45-20.mcu's interrupt-vector-table region, not code)
// on every subsequent instruction - the unmistakable signature of
// fetch8() masking-and-wrapping through address 0 after starting from an
// unmasked 0xFFFF PC, never of the real "SEI;RSP;JMP $066F" reset code.
//
// Fix: forward to m68705p5_mem_read/write on the single active MCU
// instance (flstory::s_instance->m_bmcu_state.mcu), exactly fulfilling
// the "variant/glue layer intercepts these" design m6805.h's own header
// comment describes - taito68705_mcu_state wraps an m68705p5_state
// (.mcu), which is what m68705p5_mem_read/write decode against. This is
// safe under this project's documented "single currentMachine instance"
// model (m68705p5.h's own Integration model note) since only one flstory
// (and therefore only one MCU) is ever alive at a time - see flstory.h's
// s_instance for the exact same pattern this project uses elsewhere
// (emulation.cpp's currentMachine) applied at this class's own level,
// needed here because the MCU is not reachable through any machineBase
// virtual (run_frame() steps it directly - see that function's comment).
extern "C" IRAM_ATTR unsigned char m6805_read(m6805_state *s, uint16_t addr) {
  (void)s;
  return flstory::s_instance ? m68705p5_mem_read(&flstory::s_instance->m_bmcu_state.mcu, addr) : 0xff;
}
extern "C" IRAM_ATTR void m6805_write(m6805_state *s, uint16_t addr, uint8_t val) {
  (void)s;
  if (flstory::s_instance)
    m68705p5_mem_write(&flstory::s_instance->m_bmcu_state.mcu, addr, val);
}

flstory *flstory::s_instance = 0;

void flstory::reset() {
  machineBase::reset();

  m_gfxctrl = 0;
  // machine_reset() in flstory.cpp: "onna34ro doesn't set this up when
  // checking RAM/VRAM (available by keeping pressed service button at
  // startup) so we invert the logic here" - m_char_bank starts at 1 for
  // flstory (not 0, which would be onna34ro's default).
  m_char_bank = 1;
  m_palette_bank = 0;
  m_flip = false;

  memset(m_pal_base, 0, sizeof(m_pal_base));
  memset(m_pal_ext, 0, sizeof(m_pal_ext));
  memset(flstory_palette_cpu, 0, sizeof(flstory_palette_cpu));
  memset(flstory_palette_shadow, 0, sizeof(flstory_palette_shadow));
  palette_front = 0;

  memset(flstory_vram_shadow, 0, sizeof(flstory_vram_shadow));
  vram_front = 0;
  memset(spriteram_shadow, 0, sizeof(spriteram_shadow));
  spriteram_front = 0;
  memset(scrlram_shadow, 0, sizeof(scrlram_shadow));
  scrlram_front = 0;

  soundlatch = 0;
  soundlatch_pending = false;
  soundlatch2 = 0;
  soundlatch2_pending = false;
  // "BAD SOUND PCB" self-test
  // failure: this used to start true ("held in reset"), on the theory
  // that it works "same as gng's audio_running=false" - that precedent
  // doesn't actually apply here. gng's audio Z80 genuinely IS held off at
  // power-on by a real hardware latch bit (mainlatch bit1) that the main
  // CPU must explicitly set to start it - see gng::mainlatch_w. flstory's
  // audio Z80 has no such latch: MAME's machine config
  // (flstory_state::common(), Z80(config, m_audiocpu, ...)) declares it
  // as a plain Z80 device with no reset-line configuration at all, which
  // in MAME means it starts RUNNING like any other CPU device - only
  // snd_reset_w() (0xD403 bit0) can assert its reset line later, and only
  // if the game's own code ever chooses to. Starting audio_reset_held
  // TRUE here made run_frame() unconditionally skip StepZ80(&cpu[1]) (and
  // the periodic audio IRQ) from power-on, so the audio Z80 never ran,
  // never answered the main CPU's early sound self-test handshake
  // (soundlatch write @0xD400 + poll for a soundlatch2 reply @0xD401 bit1

  audio_reset_held = false;
  soundnmi_bit1 = false;

  snd_ctrl0 = 0;
  snd_ctrl1 = 0;
  // 0x88 = AY volume 8 of 15: the value the game itself writes right after
  // boot (measured in MAME at frame 185). Starting at 0 would mute the AY
  // until the game's first PORT A write; 15 would be louder than hardware.
  snd_ctrl2 = 0x88;
  snd_ctrl3 = 0;
  dac_value = 0x80;
  ay_addr = 0;
  // Per-voice init, mirroring MAME's init_voice() (msm5232.cpp). NOTE: a
  // plain memset() is NOT correct here - eg_sect must start at -1 (idle),
  // not 0 (which means "attack"), and the three rate fields must start at
  // their table-index-0 values or the envelope divides by zero / never
  // advances on the very first note.
  if (!flstory_msm_tables_built) flstory_msm_build_rate_tables(FLSTORY_MSM_CLOCK);
  for (int i = 0; i < 8; i++) {
    msm_ch_S &c = msm_ch[i];
    c.group_select = 0;
    c.gate = false;
    c.phase = 0;
    c.eg_sect = -1;
    c.eg_arm = false;
    c.eg = 0;
    c.eg_counter = 0;
    c.egvol = 0;
    c.ar_rate = flstory_msm_ar_tbl[0];
    c.dr_rate = flstory_msm_dr_tbl[0];
    // rr_rate is constant on real hardware - always the decay-2 (R53) path.
    c.rr_rate = flstory_msm_dr_tbl[0];
  }
  msm_group1_ctrl = 0;
  msm_group2_ctrl = 0;
  msm_noise_ctrl = 0;

  // main Z80 (current_cpu index 0) + audio Z80 (index 1), same convention
  // as every other dual-Z80 machine (see 1942.cpp/timeplt.cpp).
  ResetZ80(&cpu[0]);
  ResetZ80(&cpu[1]);
  current_cpu = 0;

  taito68705_reset(&m_bmcu_state, flstory_bmcu_mcu);
  m_mcu_cycle_credit = 0;
}

void flstory::stop() {
}

void flstory::start() {
  game_started = 1;
}

unsigned char flstory::snd_flag_r(void) {
  // snd_flag_r(): (soundlatch pending? 0:1) | (soundlatch2 pending? 2:0).
  // "pending" = written but not yet read by the OTHER side. Same formula
  // read from both CPUs' own memory maps (0xd401 on main, 0xda00 on audio).
  return (soundlatch_pending ? 0 : 1) | (soundlatch2_pending ? 2 : 0);
}

// soundnmi (INPUT_MERGER_ALL_HIGH, output_handler().set_inputline(m_audiocpu,
// INPUT_LINE_NMI)): re-derive the merged output level from its two inputs
// (bit0 mirrors soundlatch_pending via the generic_latch's own
// data_pending_callback, bit1 is soundnmi_bit1) every time either input
// could have changed, and pulse the audio Z80's NMI line on a RISING edge
// (0->1) of the merged output - matching MAME's own callback-per-write-that-
// changes-an-input semantics (the merger's output_handler fires whenever
// its computed output actually changes, and NMI on this Z80 core is taken
// once per IntZ80(...,INT_NMI) call, i.e. it is inherently edge-like here
// regardless of the merger's own level-vs-edge semantics upstream).
void flstory::update_soundnmi(void) {
  bool new_line = soundlatch_pending || soundnmi_bit1;
  if (new_line && !soundnmi_line) {
    // this function is called SYNCHRONOUSLY from the
    // MAIN CPU's own write handler (wrZ80 case 0xD400, the sound-latch-kick
    // write) as well as from the audio CPU's own read handler (rdZ80 case
    // 0xD800). Setting current_cpu=1 to correctly target the audio CPU's
    // NMI, then returning WITHOUT restoring it, left the caller's own
    // in-progress CPU context corrupted: e.g. the main CPU's very next
    // memory access (a RET popping its return address off its own stack)
    // would silently go through rdZ80()'s AUDIO-CPU memory map instead of
    // the main CPU's, reading back garbage (audio ROM padding, 0xFFFF) as
    // a "return address" - confirmed via a full-register instruction trace
    // the actual root cause of the main CPU's boot sequence spuriously restarting
    // from address 0 a second time, which is what made the MCU handshake
    // fail permanently (the MCU has moved on to idling by the second boot
    // pass, and its interrupt-driven command path legitimately does not
    // reply to that specific command from idle).
    // Save/restore current_cpu around the injection so this function is
    // side-effect-free from its caller's point of view, regardless of
    // which CPU context it's called from.
    int saved_cpu = current_cpu;
    current_cpu = 1;
    IntZ80(&cpu[1], INT_NMI);
    current_cpu = saved_cpu;
  }
  soundnmi_line = new_line;
}

// flstory_mcu_status_r() (0xD805, main-CPU side):
// MAME's REAL driver source (source/mame/mame-master/src/mame/taito/
// flstory.cpp, flstory_mcu_state::flstory_mcu_status_r()):
//   bit0 = (host_semaphore_r()==CLEAR_LINE) ? 1 : 0   // "1 = MCU ready to receive"
//   bit1 = (mcu_semaphore_r()!=CLEAR_LINE)  ? 1 : 0   // "1 = MCU has sent data"
// host_semaphore_r()/mcu_semaphore_r() return 1 when the respective m_flag
// is true and CLEAR_LINE is 0 (see taito68705.h/.cpp), so bit0 is 1 when
// host_flag is FALSE (inverted) and bit1 is 1 when mcu_flag is TRUE
// (direct) - i.e. taito68705_host_flag()/taito68705_mcu_flag() (this
// port's equivalent of host_semaphore_r()/mcu_semaphore_r(), see
// taito68705_mcu.h) need bit0 inverted and bit1 used directly, exactly as
// below. (A different MAME function, mcu_portc_r() in taito68705.cpp, has
// the OPPOSITE polarity on bit1 - but that's the MCU's own Port C
// self-read, a different register for a different consumer; it is not
// what the main CPU reads at 0xD805, and was mistakenly cited as this
// function's reference during a later research pass - re-verified against
// the real flstory_mcu_status_r() source above, this formula is correct.)
unsigned char flstory::flstory_mcu_status_r(void) {
  unsigned char host_flag = taito68705_host_flag(&m_bmcu_state);
  unsigned char mcu_flag  = taito68705_mcu_flag(&m_bmcu_state);
  return (host_flag ? 0x00 : 0x01) | (mcu_flag ? 0x02 : 0x00);
}

void flstory::flstory_gfxctrl_w(unsigned char data) {
  m_gfxctrl = data;
  m_flip = (~data) & 0x01;              // flip_screen_set(BIT(~data,0))
  unsigned char new_bank = (data & 0x10) >> 4;
  m_char_bank = new_bank;                // mark_all_dirty() has no shadow-vram
                                          // equivalent needed here - render_row()
                                          // always re-reads m_char_bank live.
  m_palette_bank = (data & 0x20) >> 5;
}

void flstory::flstory_scrlram_w(unsigned char offset, unsigned char data) {
  memory[FLSTORY_SCRLRAM + (offset & 0x1f)] = data;
}

// Palette RAM: 0x200-byte CPU window -> 512-entry palette. offset&0x100
// selects ext (high) vs base (low) byte file; (offset&0xff)+(palette_bank
// <<8) is the actual 0-511 entry. xBGR_444 = xxxxBBBBGGGGRRRR (verified
// against source/mame/mame-master/src/emu/emupal.h's xbgr_444_t enum), and
// palette_device::read_entry() combines the byte pair as
// `data = base_byte | (ext_byte << 8)` (see emupal.h) - i.e. R/G live in the
// LOW (base) byte, B in the HIGH (ext) byte.
void flstory::flstory_palette_w(unsigned short offset, unsigned char data) {
  unsigned short entry = (offset & 0xff) + ((unsigned short)m_palette_bank << 8);
  if (offset & 0x100)
    m_pal_ext[entry] = data;
  else
    m_pal_base[entry] = data;
  palette_recalc(entry);
}

unsigned char flstory::flstory_palette_r(unsigned short offset) {
  unsigned short entry = (offset & 0xff) + ((unsigned short)m_palette_bank << 8);
  return (offset & 0x100) ? m_pal_ext[entry] : m_pal_base[entry];
}

void flstory::palette_recalc(unsigned short entry) {
  unsigned char base = m_pal_base[entry];
  unsigned char ext  = m_pal_ext[entry];
  unsigned char r4 = base & 0x0f;
  unsigned char g4 = (base >> 4) & 0x0f;
  unsigned char b4 = ext & 0x0f;
  unsigned char r8 = (r4 << 4) | r4;
  unsigned char g8 = (g4 << 4) | g4;
  unsigned char b8 = (b4 << 4) | b4;
  unsigned short c = ((r8 & 0xf8) << 8) | ((g8 & 0xfc) << 3) | (b8 >> 3);
  // byte-swapped RGB565, matching this project's existing convention (see
  // gng::palette_write / tools/romconv/romextract.py's peplus_prom_to_rgb565).
  flstory_palette_cpu[entry] = (c >> 8) | (c << 8);
}

unsigned char IRAM_ATTR flstory::rdZ80(unsigned short Addr) {
  if (current_cpu == 0) {
    // ---------------- main CPU ----------------
    if (Addr < 0xc000)
      return flstory_rom[Addr];

    if (Addr >= 0xc000 && Addr <= 0xcfff) {
      // 0xc000-0xc7ff videoram, .mirror(0x800) -> 0xc800-0xcfff aliases it
      return memory[FLSTORY_VIDEORAM + (Addr & 0x7ff)];
    }

    switch (Addr) {
      case 0xd000:
        return taito68705_data_r(&m_bmcu_state);
      case 0xd001: case 0xd002: case 0xd003:
        return 0xff; // watchdog / unknown-coin-lockout / snd_reset_w's nopr() side
      case 0xd400:
        soundlatch2_pending = false; // generic_latch read clears pending
        return soundlatch2;
      case 0xd401:
        return snd_flag_r();
      case 0xd800:
        return FLSTORY_SWA_DEFAULT;
      case 0xd801:
        // no demo-sounds DIP exists on flstory's SWA/SWB/SWC per
        // flstory_dipswitches.h (verified: no *_DEMO_SOUNDS_* macro there),
        // unlike gng's DSW1 - so input->demoSoundsOff() has nothing to OR in here.
        return FLSTORY_SWB_DEFAULT;
      case 0xd802:
        return FLSTORY_SWC_DEFAULT;
      case 0xd803: {
        // SYSTEM: start1(bit0)/start2(bit1)/service1(bit2)/tilt(bit3)/
        // coin1(bit4)/coin2(bit5), all active LOW; bits 6-7 are an I/O
        // self-check the main ROM performs directly after the sound test
        // (0x0220-0x0225 in the real ROM: `LD A,($D803) ; AND $C0 ; JP
        // NZ,$BE26` -> "BAD IO" message+hang if the result is NONZERO).
        //
        // BUG FIX (found 2026-09-19, the "BAD IO" screen appearing right
        // after fixing the sound-test hang): this used to return bits 6-7
        // as 1 (0xff with nothing else cleared), following MAME's
        // PORT_BIT(0x40/0x80, IP_ACTIVE_HIGH, IPT_UNKNOWN) source comment
        // "BAD IO if low" at face value - i.e. "must be high to pass".
        // That is backwards for what this ROM's own code actually checks:
        // traced byte-for-byte in a standalone disassembly,
        // the AND/JP NZ sequence takes the
        // FAILURE branch precisely when bits 6-7 are NONZERO - so the real
        // passing condition is both bits LOW (0), the opposite of the
        // comment's literal wording. (IP_ACTIVE_HIGH in MAME describes how
        // a PRESSED/asserted input reads, not what an unconnected/idle pin
        // defaults to - for an IPT_UNKNOWN placeholder bit with nothing
        // wired to it, MAME's own default idle level for an unused port
        // bit is 0, matching what this ROM actually expects; the comment
        // was likely about the metadata tag, not the emulated idle value.)
        // Confirmed via the harness: with bits 6-7 forced high the ROM
        // reliably took the "BAD IO" branch; with them low it passes.
        // This project has only one coin input, mapped to coin1; start
        // maps to start1 only (no 2-player-start support needed beyond
        // what other single-joystick machines here do).
        unsigned char k = input->buttons_get();
        unsigned char r = 0x3f; // bits 6-7 clear (see bug-fix note above), bits 0-5 idle-high
        if (k & BUTTON_START) r &= ~0x01;
        if (k & BUTTON_COIN)  r &= ~0x10;
        return r;
      }
      case 0xd804: {
        // P1: button1/button2/joystick-left/joystick-right (2-way, no up/down)
        unsigned char k = input->buttons_get();
        unsigned char r = 0xff;
        if (k & BUTTON_FIRE)  r &= ~0x01;
        if (k & BUTTON_EXTRA) r &= ~0x02;
        if (k & BUTTON_LEFT)  r &= ~0x04;
        if (k & BUTTON_RIGHT) r &= ~0x08;
        return r;
      }
      case 0xd805:
        return flstory_mcu_status_r();
      case 0xd806:
        return 0xff; // P2 (cocktail 2nd player) - not wired, upright-only support
    }

    if (Addr >= 0xdc00 && Addr <= 0xdc9f)
      return memory[FLSTORY_SPRITERAM + (Addr - 0xdc00)];
    if (Addr >= 0xdca0 && Addr <= 0xdcbf)
      return memory[FLSTORY_SCRLRAM + (Addr - 0xdca0)];
    if (Addr >= 0xdcc0 && Addr <= 0xdcff)
      return memory[FLSTORY_UNUSED_RAM + (Addr - 0xdcc0)];
    if (Addr >= 0xdd00 && Addr <= 0xdeff)
      return flstory_palette_r(Addr - 0xdd00);
    if (Addr >= 0xe000)
      return memory[FLSTORY_WORKRAM + (Addr & 0x7ff)]; // .mirror(0x1800): 0xe000-0xffff all alias the same 0x800 bytes

    return 0xff;
  } else {
    // ---------------- audio CPU ----------------
    // flstory_audiocpu[] is the full padded 0x10000 "audiocpu" ROM_REGION
    // (verified: FLSTORY_AUDIOCPU_SIZE == 0x10000 in flstory_audiocpu.h),
    // so indexing it directly for the whole 0x0000-0xbfff ROM window (and
    // the 0xe000-0xefff "diagnostics ROM" space) is always in-bounds; MAME's
    // own unbacked-region behaviour (reads as 0xff) falls out naturally
    // since romextract.py zero/0xff-pads regions it assembles - the ROM
    // itself is only 2*0x2000=0x4000 bytes at the front, the rest of the
    // 0xbfff window and the 0xe000-0xefff diagnostics window are whatever
    // romextract.py's pad byte is there.
    if (Addr < 0xc000)
      return flstory_audiocpu[Addr];
    if (Addr >= 0xc000 && Addr <= 0xc7ff)
      return memory[FLSTORY_AUDIO_RAM + (Addr - 0xc000)];
    if (Addr == 0xd800) {
      soundlatch_pending = false; // generic_latch read clears pending
      update_soundnmi();          // re-derive the merged NMI line (falling edge of bit0)
      return soundlatch;
    }
    if (Addr == 0xda00)
      return snd_flag_r();
    if (Addr == 0xde00)
      return 0xff; // DAC read: unknown/no-op per MAME's .nopr()
    if (Addr >= 0xe000 && Addr <= 0xefff)
      return flstory_audiocpu[Addr]; // diagnostics ROM space, see comment above
    return 0xff;
  }
}

void IRAM_ATTR flstory::wrZ80(unsigned short Addr, unsigned char Value) {
  if (current_cpu == 0) {
    // ---------------- main CPU ----------------
    if (Addr >= 0xc000 && Addr <= 0xcfff) {
      memory[FLSTORY_VIDEORAM + (Addr & 0x7ff)] = Value;
      return;
    }

    switch (Addr) {
      case 0xd000:
#if FLSTORY_DBG_HARNESS
        dbg_data_w_count++;
        dbg_last_data_w_value = Value;
#endif
        taito68705_data_w(&m_bmcu_state, Value);
        return;
      case 0xd001: case 0xd002:
        return; // watchdog nopw() / unknown noprw()
      case 0xd400:
        soundlatch = Value;
        soundlatch_pending = true;
        update_soundnmi(); // generic_latch's data_pending_callback -> soundnmi bit0 rising edge
        return;
      case 0xd403:
        // snd_reset_w: bit0 -> audio Z80's RESET line (1 = asserted/held,
        // 0 = clear/run free). A rising 0->1 edge does NOT need special
        // handling (real Z80 RESET is level-sensitive while asserted); a
        // falling 1->0 edge (release) must reset the audio CPU's registers/
        // PC to its reset vector exactly once - basic Z80 RESET semantics,
        // not specific to any one machine's own latch-write handler.
        {
          bool new_held = (Value & 0x01) != 0;
          if (audio_reset_held && !new_held) {
            ResetZ80(&cpu[1]);
          }
          audio_reset_held = new_held;
        }
        return;
    }

    if (Addr >= 0xdc00 && Addr <= 0xdc9f) {
      memory[FLSTORY_SPRITERAM + (Addr - 0xdc00)] = Value;
      return;
    }
    if (Addr >= 0xdca0 && Addr <= 0xdcbf) {
      flstory_scrlram_w((unsigned char)(Addr - 0xdca0), Value);
      return;
    }
    if (Addr >= 0xdcc0 && Addr <= 0xdcff) {
      memory[FLSTORY_UNUSED_RAM + (Addr - 0xdcc0)] = Value;
      return;
    }
    if (Addr >= 0xdd00 && Addr <= 0xdeff) {
      flstory_palette_w(Addr - 0xdd00, Value);
      return;
    }
    if (Addr == 0xdf03) {
      flstory_gfxctrl_w(Value);
      return;
    }
    if (Addr >= 0xe000) {
      memory[FLSTORY_WORKRAM + (Addr & 0x7ff)] = Value; // .mirror(0x1800)
      return;
    }
    // 0x0000-0xbfff is ROM: ignore writes (no bank/protection latch there
    // in flstory - unlike gng's banked M6809 map, flstory's Z80 program
    // space is flat, fixed ROM per base_map's map(0x0000,0xbfff).rom()).
  } else {
    // ---------------- audio CPU ----------------
    if (Addr >= 0xc000 && Addr <= 0xc7ff) {
      memory[FLSTORY_AUDIO_RAM + (Addr - 0xc000)] = Value;
      return;
    }
    if (Addr == 0xc800) {
      ay_addr = Value & 0x0f;
      return;
    }
    if (Addr == 0xc801) {
      // AY-8910 (YM2149) register write, latched by the previous 0xc800
      // address write. Mirrors into soundregs[] in the generic 16-byte-
      // per-chip layout audio.cpp's ay_render_buffer() expects (registers
      // 0-13; see MCH_FLSTORY dispatch added there) - same idiom as any
      // other single-AY machine's wrZ80 (e.g. see cm99.cpp/frogger.cpp).
      if (ay_addr < 14) {
        soundregs[ay_addr] = Value;
      } else if (ay_addr == 14) {
        // register 14 is the AY's PORT A, wired on this board to
        // sound_control_2_w() - the TA7630 volume for the WHOLE AY chip
        // (MAME: port_a_write_callback() -> set_device_volume(m_ay,
        // m_snd_ctrl2 >> 4)). Registers 14/15 were previously DISCARDED by
        // the `ay_addr < 14` test, so this volume control did not exist.
        //
        // That is what cuts the shoot effect on real hardware. Measured in
        // MAME across every shot in a session: the game sets the AY volume
        // to 15 as the effect starts, then writes 0x00 (mute) exactly 17
        // frames (~283ms) later, every time. The effect's frequency sweep
        // genuinely keeps running after that - it underflows past zero into
        // a ~30Hz rumble and loops forever - but it is INAUDIBLE on real
        // hardware because the chip is muted. Without this we played that
        // whole runaway sweep at full volume: the ~12s trailing sound.
        snd_ctrl2 = Value;
      } else {
        // register 15 = PORT B -> sound_control_3_w(), which in MAME only
        // latches m_snd_ctrl3 ("ta7630 bass / treble for AY?"); nothing
        // reads it back.
        snd_ctrl3 = Value;
      }
      return;
    }
    if (Addr >= 0xca00 && Addr <= 0xca0d) {
      msm5232_w((unsigned char)(Addr - 0xca00), Value);
      return;
    }
    if (Addr == 0xcc00) {
      sound_control_0_w(Value);
      return;
    }
    if (Addr == 0xce00) {
      sound_control_1_w(Value);
      return;
    }
    if (Addr == 0xd800) {
      soundlatch2 = Value;
      soundlatch2_pending = true;
      return;
    }
    if (Addr == 0xda00) {
      // input_merger_device::in_set<1> - manually raise soundnmi bit1.
      soundnmi_bit1 = true;
      return;
    }
    if (Addr == 0xdc00) {
      // input_merger_device::in_clear<1> - manually clear soundnmi bit1.
      soundnmi_bit1 = false;
      return;
    }
    if (Addr == 0xde00) {
      dac_value = Value; // DAC_8BIT_R2R, unsigned 0-255 centered at 0x80
      return;
    }
    // 0x0000-0xbfff / 0xe000-0xefff are ROM: ignore writes.
  }
}

unsigned char flstory::opZ80(unsigned short Addr) {
  if (current_cpu == 0)
    return (Addr < 0xc000) ? flstory_rom[Addr] : 0xff;
  else
    return (Addr < 0xc000) ? flstory_audiocpu[Addr] :
           (Addr >= 0xe000 && Addr <= 0xefff) ? flstory_audiocpu[Addr] : 0xff;
}

void flstory::outZ80(unsigned short Port, unsigned char Value) { (void)Port; (void)Value; }
unsigned char flstory::inZ80(unsigned short Port) { (void)Port; return 0xff; }

// ============================================================================
// run_frame() - paces the main Z80, the audio Z80 and the MCU against each
// other for one 60Hz video frame. The per-slice "step both CPUs a fixed
// instruction count, then check whether an IRQ is due" structure is
// corroborated across several dual-CPU machines in this codebase (gng.cpp's
// M6809+Z80 interleave, 1942.cpp's and timeplt.cpp's own dual-Z80
// interleaves), not gng-specific; the accumulator used for the audio IRQ
// and the MCU stepping ratio below is this port's own addition, needed
// because neither of those two rates is a small integer multiple of
// FLSTORY_SLICES the way the other machines' interrupt rates are.
// ============================================================================
void flstory::run_frame(void) {
  // Audio Z80 periodic IRQ: real hardware generates it from a discrete-logic
  // counter dividing the raw (undivided) 8MHz board clock by 0x10000, i.e.
  // 8000000/65536 ~= 122.07 Hz - NOT an integer multiple of the 60Hz video
  // rate (~2.034 IRQs/frame), so a plain "fire every N-th slice" counter
  // would drift the phase relationship between video frames and audio IRQs
  // over time. Paced instead with a fixed-point accumulator: for every main-
  // CPU slice we credit FLSTORY_AUDIO_IRQ_HZ "IRQ ticks" and debit
  // FLSTORY_VIDEO_HZ*FLSTORY_SLICES once per slice; an IRQ fires whenever
  // the accumulator crosses FLSTORY_VIDEO_HZ*FLSTORY_SLICES (i.e. one full
  // "tick" has accumulated), giving the exact long-run average rate without
  // per-frame rounding drift.
  static long audio_irq_acc = 0;

  // MCU HARD CAP (2026-09-20, "game is very slow" investigation - see
  // notes.txt): deliberately caps real MCU instructions/frame BELOW the
  // real MAME-matched value - trades some handshake-timing accuracy for
  // speed. Tested incrementally (6000, then 3500) with the sound self-
  // test confirmed still passing on real hardware at both levels -
  // unlike SLICES cuts or audio cycle-accuracy changes, which both broke
  // the self-test, capped-but-not-fully-accurate MCU stepping has been
  // safe so far. 3500 is the last confirmed-working value.
  const int MAX_MCU_STEPS_PER_FRAME = 3500;
  int mcu_steps_this_frame = 0;

  // MAIN CPU pacing: see flstory.h's
  // FLSTORY_MAIN_CYCLES_PER_FRAME comment and task_plain_6.txt): paced by
  // REAL Z80 cycles per frame now, not a fixed instruction count. Z80.c's
  // StepZ80()/Codes.h already maintain a real per-instruction T-state
  // count internally via cpu[0].ICount - reading the before/after delta
  // around each StepZ80() call recovers it with no core changes. The
  // FLSTORY_MAIN_CYCLES_PER_FRAME budget is distributed evenly across the
  // FLSTORY_SLICES slices (so the audio-IRQ/MCU accumulators below, which
  // are keyed to "per slice", keep their existing granularity) rather than
  // spent in one big burst per frame.
  //
  // SPEED FIX - see
  // FLSTORY_MAIN_CYCLES_PER_FRAME's own comment in flstory.h
  // : FLSTORY_MAIN_CYCLES_PER_FRAME itself is now scaled down
  // (FLSTORY_MAIN_CYCLE_SCALE_NUM/DEN) rather than capped by a separate
  // instruction count here - an earlier attempt at an instruction cap on
  // this loop was found to be the wrong lever (measured in the native
  // harness: this game never actually needs more than ~9,000-14,000
  // instructions to exhaust even the FULL real-cycle budget, so an
  // instruction cap sized off the ~30,000+ "real hardware need" reference
  // figure would never bind) - the real fix is simply asking StepZ80() to
  // do less real-cycle-equivalent work per frame, which costs
  // proportionally less wall-clock time regardless of the instruction
  // count that takes. See flstory.h for the exact math behind the scale
  // factor chosen.
  static long main_cycle_debt = 0; // carries any per-slice rounding remainder forward
  const long main_cycles_per_slice = (long)FLSTORY_MAIN_CYCLES_PER_FRAME / FLSTORY_SLICES;

  for (int i = 0; i < FLSTORY_SLICES; i++) {
    current_cpu = 0;
    long slice_cycle_budget = main_cycles_per_slice + main_cycle_debt;
    long main_cycles_this_slice = 0;

    while (slice_cycle_budget > 0) {
      int icount_before = cpu[0].ICount;
      StepZ80(&cpu[0]);
      long consumed = (long)(icount_before - cpu[0].ICount);
      if (consumed <= 0) consumed = 1; // defensive: never spin forever on a 0/negative delta
      slice_cycle_budget -= consumed;
      main_cycles_this_slice += consumed;
    }
    // Carry the (possibly negative) leftover into the next slice so the
    // per-frame total still averages out to FLSTORY_MAIN_CYCLES_PER_FRAME
    // exactly over time, instead of drifting from per-slice rounding.
    main_cycle_debt = slice_cycle_budget;

    if (!audio_reset_held) {
      current_cpu = 1;
      for (int s = 0; s < FLSTORY_AUDIO_STEP; s++)
        StepZ80(&cpu[1]);
    }

    // audio Z80 periodic IRQ pacing
    audio_irq_acc += (long)FLSTORY_AUDIO_IRQ_HZ;
    if (audio_irq_acc >= (long)FLSTORY_VIDEO_HZ * FLSTORY_SLICES) {
      audio_irq_acc -= (long)FLSTORY_VIDEO_HZ * FLSTORY_SLICES;
      if (!audio_reset_held) {
        current_cpu = 1;
        IntZ80(&cpu[1], INT_IRQ);
      }
    }

    // 64-bit multiply: see flstory.h's m_mcu_cycle_credit comment - this
    // OVERFLOWS a 32-bit long once a single slice's main_cycles_this_slice
    // gets large enough (i.e. once FLSTORY_SLICES drops low enough that
    // each slice covers more real cycles), silently corrupting MCU pacing
    // rather than erroring - do not narrow this back to `long`.
    m_mcu_cycle_credit += (long long)main_cycles_this_slice * (long long)FLSTORY_MCU_CLOCK;
    while (m_mcu_cycle_credit >= (long long)FLSTORY_MAIN_CLOCK && mcu_steps_this_frame < MAX_MCU_STEPS_PER_FRAME) {
      int mcu_cycles_consumed = taito68705_step(&m_bmcu_state, 1);
      mcu_steps_this_frame++;
      if (mcu_cycles_consumed <= 0) mcu_cycles_consumed = 1; // defensive: never spin forever on a 0/negative return
      m_mcu_cycle_credit -= (long long)mcu_cycles_consumed * (long long)FLSTORY_MAIN_CLOCK;
    }
  }

  // vblank IRQ on the main CPU, once per frame (irq0_line_hold in MAME).
  current_cpu = 0;
  IntZ80(&cpu[0], INT_IRQ);

  publish_palette();
  publish_vram();

  // publish spriteram/scrlram shadows once per frame, using this codebase's
  // general front/back shadow-buffer mechanism (see machineBase.h's usage
  // pattern and this file's own palette/vram shadowing above) rather than
  // publishing on every individual write: flstory's spriteram/scrlram are
  // plain RAM the main CPU can write at any time (no dedicated "commit
  // sprite buffer" side-effect register on this hardware, unlike some other
  // machines - e.g. gng's own spriteram has one, see gng::m6809_write's
  // 0x3c00 case - which flstory's real base_map simply does not have), so a
  // per-frame snapshot is the natural place to publish here instead.
  {
    unsigned char back = spriteram_front ^ 1;
    memcpy(spriteram_shadow[back], &memory[FLSTORY_SPRITERAM], sizeof(spriteram_shadow[0]));
    spriteram_front = back;
  }
  {
    unsigned char back = scrlram_front ^ 1;
    memcpy(scrlram_shadow[back], &memory[FLSTORY_SCRLRAM], sizeof(scrlram_shadow[0]));
    scrlram_front = back;
  }
}

void flstory::prepare_frame(void) {
  scan_sprites();
}

void flstory::publish_palette(void) {
  unsigned char back = palette_front ^ 1;
  memcpy(flstory_palette_shadow[back], flstory_palette_cpu, sizeof(flstory_palette_cpu));
  palette_front = back;
}

void flstory::publish_vram(void) {
  unsigned char back = vram_front ^ 1;
  memcpy(flstory_vram_shadow[back], &memory[FLSTORY_VIDEORAM], sizeof(flstory_vram_shadow[0]));
  vram_front = back;
}

// ============================================================================
// Video rendering.

// generic gfx_8x8x4_planar layout instead). tile_number = code +
// ((attr&0xc0)<<2) + 0x400 + 0x800*char_bank, per get_tile_info().
static inline unsigned int flstory_tile_number(unsigned char code, unsigned char attr, unsigned char char_bank) {
  return (unsigned int)code + (((unsigned int)attr & 0xc0) << 2) + 0x400 + 0x800u * char_bank;
}

void IRAM_ATTR flstory::scan_sprites(void) {
  active_sprites = 0;
  const unsigned char *sr = spriteram_shadow[m_render_spriteram];
  bool flip = m_render_flip;

  for (int i = 0x1f; i >= 0 && active_sprites < 32; i--) {
    unsigned char pr = sr[0xa0 - 1 - i];
    unsigned int offs = (unsigned int)(pr & 0x1f) * 4;

    unsigned int code = sr[offs + 2] + (((unsigned int)sr[offs + 1] & 0x30) << 4);
    int sx = sr[offs + 3];
    int sy = sr[offs + 0];

    bool flip_x = (sr[offs + 1] & 0x40) != 0;
    bool flip_y = (sr[offs + 1] & 0x80) != 0;

    if (flip) {
      sx = (240 - sx) & 0xff;
      sy = sy - 1;
      flip_x = !flip_x;
      flip_y = !flip_y;
    } else {
      sy = 240 - sy - 1;
    }

    int wrap_count = (sx > 240) ? 2 : 1;
    for (int wrap = 0; wrap < wrap_count && active_sprites < 32; wrap++) {
      int mame_x = sx - (wrap ? 256 : 0);
      int mame_y = sy;

      struct sprite_S spr;
      spr.code = code & 0xff;
      spr.color_block = (code >> 8) & 0x03; // bits 8-9 of the 10-bit sprite code
      spr.color = sr[offs + 1] & 0x0f;
      spr.flip_x = flip_x ? 1 : 0;
      spr.flip_y = flip_y ? 1 : 0;
      spr.is_32x32 = 0;
      spr.priority = (pr & 0x80) ? 1 : 0; // GFX_PMASK_8 vs (GFX_PMASK_8|GFX_PMASK_4) - see render_row()'s priority note

      spr.y = FLSTORY_ROW_OFFSET + (FLSTORY_MAME_FULL_W - 1 - (mame_x + 15));
      spr.x = mame_y - FLSTORY_CLIP_TOP;

      if (spr.y + 16 <= FLSTORY_ROW_OFFSET ||
          spr.y >= FLSTORY_ROW_OFFSET + FLSTORY_MAME_FULL_W ||
          spr.x + 16 <= 0 || spr.x >= 224)
        continue;

      sprite[active_sprites++] = spr;
    }
  }
}

static inline bool flstory_pen_opaque(unsigned short transmask, unsigned char pen) {
  return ((transmask >> pen) & 1) == 0;
}

void IRAM_ATTR flstory::blit_bg_strip(short row) {
  int line0 = row * 8;
  const unsigned char *vram = flstory_vram_shadow[m_render_vram];
  const unsigned char *scrl = scrlram_shadow[m_render_scrlram];
  bool flip = m_render_flip;

   for (int r = 0; r < 8; r++) {
    int fb_line = line0 + r;
    // panel_row -> native mame_x, inverting the fixed formula from this
    // section's header comment (panel_row = FLSTORY_ROW_OFFSET + (FULL_W-1
    // -mame_x)  =>  mame_x = FULL_W-1 - (panel_row - FLSTORY_ROW_OFFSET)).
    // NOTE: this row-axis point-reflection is the FIXED ROT transform this
    // port applies to map MAME's landscape picture onto the panel's
    // portrait axis (see this section's header comment) - a real, disctinct
    // transform from flip_screen, and NOT touched by the flip-model fix
    // above (flip_screen is applied at the tile-index/attribute level
    // below, on top of whatever native coordinate this recovers).
    int panel_row = fb_line;
    if (panel_row < FLSTORY_ROW_OFFSET || panel_row >= FLSTORY_ROW_OFFSET + FLSTORY_MAME_FULL_W)
      continue;
    int mame_x = (FLSTORY_MAME_FULL_W - 1) - (panel_row - FLSTORY_ROW_OFFSET);

    unsigned short *ptr = frame_buffer + r * 224;

    // Background tilemap: 32x32 cells of 8x8 px = 256x256 native, addressed
    // directly (no separate scrollx register - flstory_scrlram_w is a
    // PER-COLUMN Y-scroll, i.e. rowscroll on a normally-oriented tilemap
    // becomes a column-indexed Y-scroll register bank via
    // tilemap->set_scroll_cols(32) + set_scrolly(col,data) in MAME). tcol
    // is the LOGICAL tilemap COLUMN that mame_x (native X axis) falls into -
    // reflected to a MEMORY column further below, only if flip is set.
    int world_x = mame_x & 0xff; // 256-wide tilemap
    int tcol = (world_x >> 3) & 0x1f;
    int px0 = world_x & 7;

    int scr_col = 0;
    while (scr_col <= 223) {
      int mame_y = scr_col + FLSTORY_CLIP_TOP;
      int world_y = (mame_y + scrl[tcol & 0x1f]) & 0xff;
      int logical_trow = (world_y >> 3) & 0x1f;
      int py0 = world_y & 7;

      // Reflect LOGICAL (tcol,logical_trow) to the MEMORY (col,row) actually
      // stored in videoram, per tilemap_t::mappings_update()'s
      // `logical_col = (cols-1)-logical_col` / same for row, only when flip
      int mcol = flip ? (31 - tcol) : tcol;
      int mrow = flip ? (31 - logical_trow) : logical_trow;

      unsigned short vidx = (unsigned short)(mrow * 32 + mcol) * 2; // videoram is code,attr pairs (tile_index*2); max index 1023*2+1=2047, fits the 0x800-byte videoram exactly
      unsigned char code = vram[vidx & 0x7ff];
      unsigned char attr = vram[(vidx + 1) & 0x7ff];

      unsigned char tile_category = (attr >> 5) & 1;
      unsigned short transmask_front = tile_category == 0 ? 0x3fff : 0x8000;
      unsigned short transmask_back = tile_category == 0 ? 0xc000 : 0x7fff;

      unsigned int tile_id = flstory_tile_number(code, attr, m_char_bank) & 0xfff;
      const unsigned short *colors = &flstory_palette_shadow[m_render_palette][(attr & 0x0f) << 4];
      // Tile's own attribute flip bits, XORed with the global flip_screen
      // state - see this function's header comment
      // (tilemap_t::realize_tile(): `flags = tileinfo.flags ^ attributes`).
      bool flip_x = ((attr & 0x08) != 0) ^ flip;
      bool flip_y = ((attr & 0x10) != 0) ^ flip;
      int tile_x = flip_x ? (7 - px0) : px0;

      const unsigned char (*tile_pens)[8] = flstory_tilemap[tile_id];
      unsigned char pri_front = tile_category == 0 ? 1 : 2;
      unsigned char pri_back  = tile_category == 0 ? 4 : 8;

      // Run length: this tile's remaining 8-pixel span (8-py0), clipped to
      // the panel's right edge - matches gng's own run-length clamp.
      int run = 8 - py0;
      if (run > 224 - scr_col) run = 224 - scr_col;

      for (int k = 0; k < run; k++) {
        int py = py0 + k;
        int tile_y = flip_y ? (7 - py) : py;
        unsigned char pen = tile_pens[tile_y][tile_x];
        // front drawn first, back drawn second and allowed to overwrite -
        // matches the original 4-pass call order exactly (see comment
        // above this function).
        //
        // wizard invisible inside death bubble
        // - a pixel only ever belongs to ONE category (tileinfo.category is
        // a property of the tile occupying that position), so only that
        // category's own front/back values are ever possible here; back
        // overwrites front's priority too if it draws opaquely, same as it
        // overwrites the color, and 0 (transparent, matching
        // screen.priority().fill(0,...) at the start of MAME's own frame)
        // if NEITHER pass was opaque here.
        unsigned char pri = 0;
        int col = scr_col + k;
        if (flstory_pen_opaque(transmask_front, pen)) {
          ptr[col] = colors[pen];
          pri = pri_front;
        }
        if (flstory_pen_opaque(transmask_back, pen)) {
          ptr[col] = colors[pen];
          pri = pri_back;
        }
        priority_buffer[r * 224 + col] = pri;
      }
      scr_col += run;
    }
  }
}

static inline unsigned char flstory_sprite_readbit(const unsigned char *data, unsigned int bitnum) {
  return (data[bitnum >> 3] & (0x80 >> (bitnum & 7))) ? 0 : 1;
}

// MAME's literal spritelayout xoffset/yoffset tables (source/mame/mame-
// master/src/mame/taito/flstory.cpp), transcribed verbatim rather than
// re-derived from a perceived pattern
static const unsigned int FLSTORY_SPRITE_XOFF[16] = {
  3, 2, 1, 0, 8 + 3, 8 + 2, 8 + 1, 8 + 0,
  16 * 8 + 3, 16 * 8 + 2, 16 * 8 + 1, 16 * 8 + 0,
  16 * 8 + 8 + 3, 16 * 8 + 8 + 2, 16 * 8 + 8 + 1, 16 * 8 + 8 + 0
};
static const unsigned int FLSTORY_SPRITE_YOFF[16] = {
  0 * 16, 1 * 16, 2 * 16, 3 * 16, 4 * 16, 5 * 16, 6 * 16, 7 * 16,
  16 * 16, 17 * 16, 18 * 16, 19 * 16, 20 * 16, 21 * 16, 22 * 16, 23 * 16
};

static unsigned char flstory_decode_sprite_pixel(unsigned int code, int x, int y) {
  // planeoffset (bits) = { half+0, half+4, 0, 4 }, half = region-bits/2 -
  // same half-split nibble-pair packing as charlayout (see
  // flstory_tilemap.h's own header comment for the evidence this packing
  // is correct), just decoded at 16x16 instead of 8x8.
  static const unsigned int FLSTORY_TILES_TOTAL_BITS = 0x20000u * 8u;
  static const unsigned int half = FLSTORY_TILES_TOTAL_BITS / 2;
  unsigned int planeoffset[4] = { half + 0, half + 4, 0, 4 };

  unsigned int xoff = FLSTORY_SPRITE_XOFF[x & 15];
  unsigned int yoff = FLSTORY_SPRITE_YOFF[y & 15];

  unsigned int charincrement = 64u * 8u; // bits/tile (spritelayout's own charincrement)
  unsigned int base = code * charincrement;

  unsigned char pen = 0;
  unsigned char planebit = 8;
  for (int plane = 0; plane < 4; plane++, planebit >>= 1) {
    unsigned int bitoff = base + planeoffset[plane] + yoff + xoff;
    if (flstory_sprite_readbit(flstory_tiles, bitoff))
      pen |= planebit;
  }
  return pen;
}

void IRAM_ATTR flstory::blit_sprite(short row, unsigned char s_idx) {
  const struct sprite_S *s = &sprite[s_idx];
  const int y_strip = row * 8;

  if (s->y + 16 <= y_strip || s->y >= y_strip + 8)
    return;

  unsigned int code = s->code | ((unsigned int)s->color_block << 8); // reassemble the 10-bit sprite code (sprite_S.code is only 8-bit)
  if (code >= 1024)
    return;

  const unsigned short *colors = &flstory_palette_shadow[m_render_palette][256 + ((unsigned int)s->color << 4)];

  int dy0 = (s->y < y_strip) ? (y_strip - s->y) : 0;
  int dy1 = (s->y + 16 > y_strip + 8) ? (y_strip + 8 - s->y) : 16;

  //HOW each axis's offset was
  // read, both now re-derived from scan_sprites()'s corrected spr.y anchor
  // (see that function's own BUG FIX comment) instead of guessed/kept as
  // they were:
  //   1. The row-direction (dy) sample needs a BASE inversion (15-dy, not
  //      dy) that has nothing to do with the sprite's own flip_x/flip_y
  //      attribute bits: panel_row is a POINT-REFLECTION of native X
  //      (panel_row = ROW_OFFSET+(FULL_W-1-mame_x)), so as dy increases
  //      (panel row increases), the corresponding native-X-within-sprite
  //      offset DECREASES, not increases. scan_sprites() now anchors spr.y
  //      to the sprite's FAR mame_x edge (mame_x+15) specifically so that
  //      dy=0 maps to native-X-offset 15 and dy=15 maps to offset 0 - i.e.
  //      tile_x's base value is (15-dy), not dy.
  //   2. Which flip attribute bit gates which axis was swapped: the
  //      row-direction/native-X sample (tile_x/dy) must be gated by the
  //      sprite's own FLIP_X bit (native-X-axis flip - matching
  //      blit_bg_strip()'s tile_x/flip_x pairing exactly, both being
  //      native-X-derived), and the column-direction/native-Y sample
  //      (tile_y/dx) by FLIP_Y - the previous code had this pairing
  //      backwards (flip_y gating the row/dy sample, flip_x gating the
  //      column/dx sample), which combined with bug 1 above to produce a
  //      sprite that looked correct in one flip state but upside-down and
  //      wrongly split across the two axes in the other - matching the
  //      reported "walking left is fine, walking right is upside-down and
  //      in 2 pieces" symptom (walking left/right toggles the ROM's own
  //      flip_x bit, which this bug's mispaired gating made control the
  //      WRONG sample axis).
  for (int dy = dy0; dy < dy1; dy++) {
    int abs_line = s->y + dy;
    if (abs_line < FLSTORY_ROW_OFFSET || abs_line >= FLSTORY_ROW_OFFSET + FLSTORY_MAME_FULL_W)
      continue;
    int fb_line = abs_line - y_strip;
    int tile_x = s->flip_x ? dy : (15 - dy);
    unsigned short *fb = &frame_buffer[fb_line * 224];

    unsigned char *pri_row = &priority_buffer[fb_line * 224];
    for (int dx = 0; dx < 16; dx++) {
      int scr_x = s->x + dx;
      if (scr_x < 0 || scr_x >= 224)
        continue;
      int tile_y = s->flip_y ? (15 - dx) : dx;
      unsigned char pen = flstory_tiles16[code][tile_y][tile_x];
      if (pen == 15)
        continue;
      unsigned char pixel_pri = pri_row[scr_x];
      bool occluded = (pixel_pri == 31) ||
                      (pixel_pri == 8) ||
                      (!s->priority && pixel_pri == 4);
      if (!occluded)
        fb[scr_x] = colors[pen];
      pri_row[scr_x] = 31;
    }
  }
}

void IRAM_ATTR flstory::render_row(short row) {
  int line0 = row * 8;
  if (row == 0) {
    m_render_vram = vram_front;
    m_render_palette = palette_front;
    m_render_spriteram = spriteram_front;
    m_render_scrlram = scrlram_front;
    m_render_flip = m_flip;

    scan_sprites();
  }

  if (line0 + 8 <= FLSTORY_ROW_OFFSET || line0 >= FLSTORY_ROW_OFFSET + FLSTORY_MAME_FULL_W) {
    for (unsigned char s = 0; s < active_sprites; s++)
      blit_sprite(row, s);
    return;
  }

  // blit_bg_strip() now does all 4 of screen_update_flstory()'s original
  // draw(cat0,LAYER1)/draw(cat1,LAYER1)/draw(cat0,LAYER0)/draw(cat1,LAYER0)
  // passes internally, in ONE per-pixel pass instead of 4 - see its own
  // header comment for the collapse fix and the verified draw-order
  // preservation.

  blit_bg_strip(row);
  for (unsigned char s = 0; s < active_sprites; s++)
    blit_sprite(row, s);
}
// ============================================================================
// Sound.
//
// AY-8910 (YM2149, 8MHz/4=2MHz): handled through this codebase's existing
// generic soundregs[]-driven SSG renderer (see src/emulation/audio.cpp's
// ay_render_buffer(); MCH_FLSTORY dispatch added there) - wrZ80() above
// already latches register writes into soundregs[0..13] directly (same
// idiom as any other single-AY machine, e.g. see cm99.cpp's wrZ80).
//
// MSM5232 + DAC: NO existing emulation anywhere in this codebase (first
// game to need either) - a compact from-scratch model, mixed via the
// renderFmSample() hook (machineBase::renderFmSample(), see machineBase.h -
// called once per 24kHz output sample from audio.cpp's mixing loop). This
// hook has TWO existing implementers to cross-check its contract against:
// gng.cpp (2xYM2203 FM channels) and radio.cpp (decoded stream PCM) - both
// agree on the same "pre-scale the return value to roughly +/-512" contract
// (see radio::renderFmSample()'s own comment for an independent derivation
// of that exact number from Audio::valueToBuffer()'s "*64" scaling), which
// is what this implementation follows; gng's own internal FM-synthesis
// approach (its YM2203 operator/envelope model) is unrelated and not used
// as a template here.
// ============================================================================

// --- Persistent high scores -------------------------------------------------
const hiscore_region_S *flstory::hiscoreRegions(unsigned char *count) {
  static const hiscore_region_S regions[] = {
    { 0xe74e, 0x23, 0x00, 0x44 },
  };
  *count = sizeof(regions) / sizeof(regions[0]);
  return regions;
}

void flstory::sound_control_0_w(unsigned char data) {
  // TA7630 volume group1 (MSM5232 channels 0-3). Real TA7630 volume law is
  // a nonlinear (roughly -2dB/step) attenuation table; approximated here as
  // a simple linear 0-15 gain scale (data>>4) applied per-sample in
  // renderFmSample().
  // TODO(sound): TA7630 is a nonlinear attenuator - MAME's own driver
  // comment says "TA7630 emulation needs filter support" even in the real
  // MAME core, so a linear approximation here is consistent with this
  // project's existing level of fidelity elsewhere, not a special-case
  // shortcut invented for flstory alone.
  snd_ctrl0 = data;
}

void flstory::sound_control_1_w(unsigned char data) {
  // TA7630 volume group2 (MSM5232 channels 4-7). See sound_control_0_w()'s
  snd_ctrl1 = data;
}

// MSM5232 register write (0xca00-0xca0d on the audio CPU, 14 registers -
// see source/mame/mame-master/src/devices/sound/msm5232.cpp::write() for
// the authoritative register map this mirrors):
//   0x00-0x07: per-channel pitch (bit7 = gate/key-on, bits0-6 = pitch code
//              indexing the chip's internal 88-entry ROM table - see
//              MSM5232_ROM[] in msm5232.cpp, re-derived numerically below
//              rather than copied byte-for-byte - see
//              flstory_msm_freq_8prime()'s own comment)
//   0x08/0x09: group1/group2 attack rate (bits 0-2)
//   0x0a/0x0b: group1/group2 decay rate (bits 0-3)
//   0x0c/0x0d: group1/group2 output-enable control (bits 0-3 = 16'/8'/4'/2'
//              footage enables for that group's 4 channels; bit4 = ARM,
//              i.e. decay-vs-release envelope mode)
//
// All four rate registers and the ARM bit are now honoured - see
// msm_eg_advance() below and the envelope note in flstory.h. They were
// previously discarded, which was root-caused as the "dragged-out note"
// bug (the game drives rate selects 0/1 attack and 4/5 decay, and never
// sets ARM).

// --- MSM5232 envelope rate tables ---------------------------------------
// Direct port of MAME's init_tables() (msm5232.cpp). The chip's envelope is
// an external capacitor charged/discharged through one of three on-die
// resistors; the rate registers select a duty-cycle divider on top of that.
// Resulting units are seconds (ohms * farads), exactly as MAME uses them.
//
// flstory wires all 8 capacitors to 1.0uF (verified on a real PCB - see the
// driver's set_capacitors() call in mame/src/mame/taito/flstory.cpp).
static const double FLSTORY_MSM_R51 =    870.0;  // attack resistance
static const double FLSTORY_MSM_R52 =  17400.0;  // decay 1 resistance
static const double FLSTORY_MSM_R53 = 101000.0;  // decay 2 / release resistance
static const double FLSTORY_MSM_CAP = 1.0e-6;    // 1.0uF on every channel

float flstory_msm_ar_tbl[8];
float flstory_msm_dr_tbl[16];
bool  flstory_msm_tables_built = false;

void flstory_msm_build_rate_tables(uint32_t chip_clock) {
  // MAME calibrates the duty-cycle divider against a 2119040Hz reference
  // clock; keep that scaling so the timings track the real chip's.
  const double clockscale = (double)chip_clock / 2119040.0;
  for (int i = 0; i < 8; i++) {
    // "bit 1 is ignored if bit 2 is set" - MAME's own comment.
    const int rcp_duty_cycle = 1 << ((i & 4) ? (i & ~2) : i);
    // Stored as RECIPROCALS (1/rate). The envelope's hot path runs up to 8
    // times per 24kHz sample; the ESP32's FPU has no divide instruction at
    // all, so a literal `/ rate` there compiles to a software-emulated
    // divide costing microseconds each - the exact failure mode that caused
    // the earlier "audio pileup" stall documented above
    // flstory_msm_freq_8prime(). Pre-inverting here turns the whole per-
    // sample envelope step into plain multiplies.
    flstory_msm_ar_tbl[i]     = (float)(1.0 / ((rcp_duty_cycle / clockscale) * FLSTORY_MSM_R51 * FLSTORY_MSM_CAP));
    flstory_msm_dr_tbl[i]     = (float)(1.0 / ((rcp_duty_cycle / clockscale) * FLSTORY_MSM_R52 * FLSTORY_MSM_CAP));
    flstory_msm_dr_tbl[i + 8] = (float)(1.0 / ((rcp_duty_cycle / clockscale) * FLSTORY_MSM_R53 * FLSTORY_MSM_CAP));
  }
  flstory_msm_tables_built = true;
}

// Apply a rate-register write to all 4 channels of one group (MAME's
// cases 0x08-0x0b, which each loop over that group's 4 voices).
void flstory::msm_set_group_rates(int group, bool attack, unsigned char data) {
  if (!flstory_msm_tables_built) flstory_msm_build_rate_tables(FLSTORY_MSM_CLOCK);
  const int base = group ? 4 : 0;
  for (int i = 0; i < 4; i++) {
    if (attack) msm_ch[base + i].ar_rate = flstory_msm_ar_tbl[data & 0x07];
    else        msm_ch[base + i].dr_rate = flstory_msm_dr_tbl[data & 0x0f];
  }
}

void flstory::msm5232_w(unsigned char reg, unsigned char data) {
#if FLSTORY_DBG_HARNESS
  if (dbg_msm_write_hook) dbg_msm_write_hook(reg, data);
#endif
  if (!flstory_msm_tables_built) flstory_msm_build_rate_tables(FLSTORY_MSM_CLOCK);

  if (reg < 0x08) {
    msm_ch_S &ch = msm_ch[reg];
    ch.gate = (data & 0x80) != 0;
    if (data & 0x80) {
      // Key ON. NOTE: the pitch code is latched ONLY here, inside the
      // key-on branch - exactly as MAME does it. The previous version
      // assigned it unconditionally, so a key-off write (0x00) silently
      // clobbered the pitch to 0 (= 16.3Hz in our table), which made every
      // release play as a sub-audio rumble instead of the real note.
      // Pitch codes >= 0xd8 select the chip's noise mode; this board does
      // not route the noise output anywhere (see the machine config), so
      // treat those as "no tone" rather than modelling noise.
      ch.group_select = data & 0x7f;
      ch.eg_sect = 0;  // -> attack
    } else {
      // Key OFF: ARM selects which discharge path the envelope takes.
      // ARM=0 -> release (slow, R53). ARM=1 -> decay (faster, R52).
      ch.eg_sect = ch.eg_arm ? 1 : 2;
    }
    return;
  }

  switch (reg) {
    case 0x08: msm_set_group_rates(0, true,  data); break; // group1 attack
    case 0x09: msm_set_group_rates(1, true,  data); break; // group2 attack
    case 0x0a: msm_set_group_rates(0, false, data); break; // group1 decay
    case 0x0b: msm_set_group_rates(1, false, data); break; // group2 decay
    case 0x0c:
    case 0x0d: {
      const int group = (reg == 0x0d) ? 1 : 0;
      if (group) msm_group2_ctrl = data; else msm_group1_ctrl = data;
      // Latch ARM for this group's 4 voices. Writing ARM=1 also RETRIGGERS
      // the attack on any voice currently in decay (MAME msm5232.cpp's
      // "if ((data & 0x10) && eg_sect == 1) eg_sect = 0").
      for (int i = 0; i < 4; i++) {
        msm_ch_S &c = msm_ch[group * 4 + i];
        if ((data & 0x10) && c.eg_sect == 1) c.eg_sect = 0;
        c.eg_arm = (data & 0x10) != 0;
      }
      break;
    }
    default: break;
  }
}

// --- MSM5232 envelope generator -----------------------------------------
// Port of MAME's EG_voices_advance() (msm5232.cpp), run once per output
// sample per voice. MAME runs this at its own stream rate; we run it at the
// 24kHz output sample rate, which is what SAMPLE_RATE below refers to.
//
// The counter/step arithmetic is MAME's own: each sample the counter is
// reduced by (distance-to-target / rate), and whenever it goes negative the
// envelope level moves by the number of whole sample-periods that elapsed.
// That yields the RC-style exponential approach of the real capacitor.
#define FLSTORY_MSM_VMAX 32768
#define FLSTORY_MSM_VMIN 0

void flstory::msm_eg_advance(msm_ch_S &c) {
  const int samplerate = 24000;
  switch (c.eg_sect) {
    case 0: // attack - capacitor charge
      if (c.eg < FLSTORY_MSM_VMAX && c.ar_rate > 0.0f) {
        c.eg_counter -= (int32_t)((float)(FLSTORY_MSM_VMAX - c.eg) * c.ar_rate);
        if (c.eg_counter <= 0) {
          int n = -c.eg_counter / samplerate + 1;
          c.eg_counter += n * samplerate;
          if ((c.eg += n) > FLSTORY_MSM_VMAX) c.eg = FLSTORY_MSM_VMAX;
        }
      }
      // THE FIX for the dragged-out note: with ARM=0 the envelope switches
      // to decay as soon as the cap reaches VT (the EG inversion voltage,
      // ~80% of max) - it does NOT wait for key-off. With ARM=1 it instead
      // holds at maximum until key-off.
      if (!c.eg_arm && c.eg >= FLSTORY_MSM_VMAX * 80 / 100)
        c.eg_sect = 1;
      c.egvol = (unsigned short)(c.eg / 16); // 32768/16 = 2048 max
      break;

    case 1: // decay - capacitor discharge through R52
      if (c.eg > FLSTORY_MSM_VMIN && c.dr_rate > 0.0f) {
        c.eg_counter -= (int32_t)((float)(c.eg - FLSTORY_MSM_VMIN) * c.dr_rate);
        if (c.eg_counter <= 0) {
          int n = -c.eg_counter / samplerate + 1;
          c.eg_counter += n * samplerate;
          if ((c.eg -= n) < FLSTORY_MSM_VMIN) c.eg = FLSTORY_MSM_VMIN;
        }
      } else {
        c.eg_sect = -1;
      }
      c.egvol = (unsigned short)(c.eg / 16);
      break;

    case 2: // release - capacitor discharge through R53 (always the slow path)
      if (c.eg > FLSTORY_MSM_VMIN && c.rr_rate > 0.0f) {
        c.eg_counter -= (int32_t)((float)(c.eg - FLSTORY_MSM_VMIN) * c.rr_rate);
        if (c.eg_counter <= 0) {
          int n = -c.eg_counter / samplerate + 1;
          c.eg_counter += n * samplerate;
          if ((c.eg -= n) < FLSTORY_MSM_VMIN) c.eg = FLSTORY_MSM_VMIN;
        }
      } else {
        c.eg_sect = -1;
      }
      c.egvol = (unsigned short)(c.eg / 16);
      break;

    default: // -1: idle, envelope has finished
      break;
  }
}

// --- MSM5232 pitch -> frequency -----------------------------------------
// Re-derives a real 8' (footage) frequency from a 7-bit pitch code using
// the SAME chip-internal counter/binary-divider structure MAME's
// MSM5232_ROM[] table encodes (programmable-counter value * 2^binary-
// divider-shift), instead of inventing an arbitrary equal-tempered table -
// this keeps the in-game melody's actual pitch RELATIONSHIPS (semitone
// spacing across the pitch-code range) faithful to real hardware,
// calibrated the same way MAME's own source comment calibrates the chip
// ("at 2119040 Hz clock, pitch data=0x21 gives exactly 440.0 Hz on the 8'
// output"). This is a compact re-encoding of that table's structure (12
// counter values repeated across 7 binary-divider octaves), not a copy of
// its 88 raw entries - see msm5232.cpp for the original if exact byte-for-
// byte fidelity is ever needed.
static float flstory_msm_freq_8prime_compute(unsigned char pitch7, uint32_t chip_clock) {
  pitch7 &= 0x7f;
  if (pitch7 > 0x57) pitch7 = 0x57;
  static const int counters[12] = { 478,451,426,402,379,358,338,319,301,284,268,253 };
  int idx;
  int bindiv;
  if (pitch7 == 0) {
    idx = 0; bindiv = 7; // table's own entry 0 (506,7) - approximated with the nearest regular entry, see rationale below
  } else if (pitch7 >= 0x55) {
    idx = 11; bindiv = 1; // entries 0x55-0x57 collapse to the lowest table octave (0x57's (13,7) special case
                           // is the chip's absolute floor pitch, rarely if ever used by real game music -
                           // not worth a separate branch at this fidelity level)
  } else {
    unsigned char p = (unsigned char)(pitch7 - 1); // entries 1..0x54 are 7 regular chromatic octaves of 12
    int octave = p / 12;
    idx = p % 12;
    bindiv = 7 - octave;
  }
  int counter = counters[idx];
  // TG_count_period ~ counter * 2^bindiv; a square-wave output cycle is
  // 2 such periods (high half + low half), so frequency = clock / (2*period).
  double period = (double)counter * (double)(1u << bindiv);
  double freq = (double)chip_clock / (2.0 * period);
  return (float)freq;
}

static float flstory_msm_freq_8prime(unsigned char pitch7, uint32_t chip_clock) {
  static float table[128];
  static bool table_built = false;
  static uint32_t table_clock = 0;
  if (!table_built || table_clock != chip_clock) {
    for (int i = 0; i < 128; i++)
      table[i] = flstory_msm_freq_8prime_compute((unsigned char)i, chip_clock);
    table_built = true;
    table_clock = chip_clock;
  }
  return table[pitch7 & 0x7f];
}

int flstory::renderFmSample() {
  // Called once per 24kHz output sample by audio.cpp's mixing loop (see
  // machineBase::renderFmSample()'s own comment) - must be pure/side-effect
  // -free besides reading already-latched register state (msm_ch[]/
  // snd_ctrl0/snd_ctrl1/dac_value), and must NOT step any CPU.
  const uint32_t SAMPLE_RATE = 24000;
  const uint32_t MSM_CLOCK = FLSTORY_MSM_CLOCK; // 8_MHz_XTAL/4, per common(machine_config&) in flstory.cpp

  int32_t mix = 0;

  for (int ch = 0; ch < 8; ch++) {
    msm_ch_S &c = msm_ch[ch];
    unsigned char group_ctrl = (ch < 4) ? msm_group1_ctrl : msm_group2_ctrl;

    // Advance this voice's envelope one output sample (MAME-equivalent
    // 4-state EG - see msm_eg_advance() above). This REPLACES the earlier
    // fixed attack/decay/release constants, whose lack of the ARM=0
    // auto-decay was the measured cause of the dragged-out notes.
    msm_eg_advance(c);

    // egvol==0 means the envelope has fully decayed - nothing to mix even
    // if the gate is still high (which is exactly the ARM=0 case the old
    // model got wrong).
    if (c.egvol == 0 || (group_ctrl & 0x0f) == 0)
      continue;

    float f8 = flstory_msm_freq_8prime((unsigned char)c.group_select, MSM_CLOCK);

    // One phase accumulator per channel, run at the 8' (fundamental) rate;
    // each enabled footage (16'/8'/4'/2') is read as a bit-shift of that
    // SAME accumulator rather than 4 independent counters - exactly
    // correct for octave-related square waves sharing one divider chain
    // (which is what real MSM5232 hardware actually does: one binary
    // counter chain, 4 output taps at different bit positions).
    uint32_t inc8 = (uint32_t)((f8 / (float)SAMPLE_RATE) * (float)(1u << 24));
    if (inc8 == 0) inc8 = 1;
    c.phase += inc8;

    int32_t ch_sum = 0;
    for (int fo = 0; fo < 4; fo++) {
      if (!(group_ctrl & (1 << fo)))
        continue;
      // fo: 0=16' (half the fundamental rate), 1=8' (fundamental),
      // 2=4' (2x), 3=2' (4x).
      uint32_t ph = (fo == 0) ? (c.phase >> 1) : (c.phase << (fo - 1));
      bool high = (ph & 0x00800000u) != 0; // bit 23 = square-wave half-cycle
      ch_sum += high ? 1 : -1;
    }

    // This channel's group TA7630 volume (linear approximation - see
    // sound_control_0_w()/1_w()'s TODO) and its envelope level.
    //
    // egvol is MAME's own 0..2048 envelope scale (eg/16). Shift it down to
    // the same ~0..127 range the previous `env >> 8` produced, so this
    // channel's contribution to the +/-512 budget is unchanged in level -
    // only its TIME ENVELOPE is now correct. (2048 >> 4 = 128.)
    unsigned char vol4 = (ch < 4) ? (snd_ctrl0 >> 4) : (snd_ctrl1 >> 4);
    int32_t ch_out = (ch_sum * (int32_t)vol4 * (int32_t)(c.egvol >> 4)) >> 7;
    mix += ch_out;
  }

  // DAC (8-bit R2R, unsigned 0-255 centered at 0x80) - read the latched
  // 0xde00 byte and contribute a centered signed value.
  int32_t dac = (int32_t)dac_value - 0x80;
  mix += dac * 2;

  // Final safety clamp: Audio::valueToBuffer() (audio.cpp) applies its own
  // "*64" scale to every machine's renderFmSample()/mix return value to
  // reach full 15-bit output range, so every implementer of this hook must
  // pre-scale to roughly +/-512 or that *64 overflows/wraps - this is a
  // genuinely shared contract (see radio::renderFmSample()'s own comment
  // for an independent confirmation of the exact same +/-512 convention,
  // not something specific to gng's implementation).
  if (mix > 512) mix = 512;
  if (mix < -512) mix = -512;

  return (int)mix;
}

const unsigned short *flstory::logo(void) {
  return flstory_logo;
}

#ifdef LED_PIN
void flstory::menuLeds(CRGB *leds) {
  memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}

void flstory::gameLeds(CRGB *leds) {
  memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
#endif