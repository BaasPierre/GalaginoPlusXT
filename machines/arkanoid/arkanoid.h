#ifndef ARKANOID_H
#define ARKANOID_H

// Arkanoid (World, older) - Taito 1986. MAME set `arkanoid`.
// Ground truth: source/mame/mame-master/src/mame/taito/arkanoid.cpp
// (arkanoid_map, INPUT_PORTS_START(arkanoid), arkanoid_state::arkanoid()),
// arkanoid_v.cpp (video), arkanoid_m.cpp (inputs), src/mame/shared/
// taito68705.cpp (ARKANOID_68705P5 MCU interface).
//
// Hardware: Z80 @ 6MHz, MC68705P5 @ 3MHz (750k cycles/s), YM2149 @ 3MHz with
// pin 26 low, one 32x32 tilemap of 8x8 tiles + 16 sprites of 8x16, 512
// PROM colours. Screen 384x264 @ 6MHz, visible x 0-255, y 16-239, ROT90.

#include "arkanoid_rom.h"
#include "arkanoid_mcu.h"
#include "arkanoid_gfx.h"
#include "arkanoid_palette.h"
#include "arkanoid_dipswitches.h"
#include "arkanoid_cheats.h"

// DSW as read by the game: arkanoid_dipswitches.h, unless overridden (the
// harness passes MAME's default 0xfe so the MAME comparison stays exact).
#ifndef ARKANOID_DSW
#define ARKANOID_DSW ARKANOID_DSW_DEFAULT
#endif
#include "arkanoid_logo.h"
#include "../machineBase.h"
#include "../../cpus/m6805/m68705p5.h"

// memory[] layout
#define ARKANOID_WORKRAM   0x0000   // c000-c7ff (mirrored at c800-cfff)
#define ARKANOID_VIDEORAM  0x0800   // e000-e7ff
#define ARKANOID_SPRITERAM 0x1000   // e800-e83f
#define ARKANOID_EXTRARAM  0x1040   // e840-efff
#define ARKANOID_MEM_END   0x1800

// m_screen->set_raw(12_MHz_XTAL/2, 384, 0, 256, 264, 16, 240)
#define ARKANOID_Z80_CYCLES_PER_FRAME (384 * 264)
// ---- Paddle feel (D-pad left/right stand in for the spinner) ---------------
// Units: half spinner counts per frame (the game reads an 8-bit spinner
// count; the paddle moves with the change per frame). Holding a direction
// moves DIAL_START a frame for the first DIAL_DELAY frames (so short taps stay
// precise), then DIAL_ACCEL more every frame, up to DIAL_MAX. Releasing or
// reversing starts again at DIAL_START.
// MAME's keyboard emulation of the spinner is 9 / 0 / 9 (keydelta 15 *
// sensitivity 30% = 4.5 counts a frame, no acceleration) - the harness builds
// with those values so it still matches MAME exactly.

//What to change if it doesn't feel right:

// Taps still too jumpy: lower START to 5 or 6.
// The paddle takes too long to get going: lower the delay to 3 or 4.
// Long moves are too slow for a fast ball: raise MAX to 26–30, or ACCEL to 3.
// Turn the hold off: set the delay to 0 (the behaviour you had before).

#ifndef ARKANOID_DIAL_START
#define ARKANOID_DIAL_START 7 //7
#endif
#ifndef ARKANOID_DIAL_ACCEL
#define ARKANOID_DIAL_ACCEL 2 //2
#endif
#ifndef ARKANOID_DIAL_MAX
#define ARKANOID_DIAL_MAX 12 //23
#endif
#ifndef ARKANOID_DIAL_DELAY
#define ARKANOID_DIAL_DELAY 5 //6 If 0 is selected it uses a default on all 3 values above
#endif

// Print the emulation time per frame over serial once a second (device only).
#ifndef ARKANOID_PROFILE
#define ARKANOID_PROFILE 1
#endif

// Skip the Z80 and MCU idle loops in whole iterations (arkanoid.cpp
// z80_idle_skip / mcu_idle_skip). Exact; 0 runs every instruction.
#ifndef ARKANOID_IDLE_SKIP
#define ARKANOID_IDLE_SKIP 1
#endif

// The 256 visible lines of the rotated screen, centred in the 288-line panel.
#define ARKANOID_ROW_OFFSET 16

class arkanoid : public machineBase
{
public:
	arkanoid() { }
	~arkanoid() { if (s_instance == this) s_instance = 0; }

	signed char machineType() override { return MCH_ARKANOID; }

	void reset() override;

	unsigned char rdZ80(unsigned short Addr) override;
	void wrZ80(unsigned short Addr, unsigned char Value) override;
	unsigned char opZ80(unsigned short Addr) override;
	void outZ80(unsigned short Port, unsigned char Value) override { }
	unsigned char inZ80(unsigned short Port) override { return 0xff; }

	void run_frame(void) override;
	void prepare_frame(void) override { }
	void render_row(short row) override;
	unsigned char ayEnvelopeRestarts(unsigned char ay) override { return ay_r13_writes; }

	const unsigned short *logo(void) override;

	const char *hiscoreKey() override { return "arkanoid"; }
	const hiscore_region_S *hiscoreRegions(unsigned char *count) override;

#ifdef LED_PIN
	void menuLeds(CRGB *leds) override;
	void gameLeds(CRGB *leds) override;
#endif

	// MCU memory hooks (m68705p5_reset_hooked) - static, they find the machine
	// through s_instance (one active machine at a time, see machines.h).
	static uint8_t mcu_rd_hook(m6805_state *s, uint16_t addr);
	static void mcu_wr_hook(m6805_state *s, uint16_t addr, uint8_t val);

private:
	static arkanoid *s_instance;

	unsigned char *rom_ram[4] = { 0, 0, 0, 0 };   // program ROM pages + MCU ROM in RAM
	const unsigned char *mcu_rom = arkanoid_mcu;
	void roms_to_ram(void);
	uint32_t prof_z80 = 0, prof_mcu = 0;   // ARKANOID_PROFILE

	// ---- main CPU ---------------------------------------------------------
	bool z80_irq = false;          // vblank IRQ, HOLD_LINE: dropped when taken
	bool ei_shadow = false;        // last instruction was an enabling EI
	unsigned char z80_r = 0, z80_r2 = 0; // refresh register as MAME counts it
	int watchdog = 0;              // vblanks since the last d010 write

	// ---- scheduler (arkanoid.cpp "Scheduling"), times in Z80 cycles --------
	enum { LINE_MCU_IRQ, LINE_MCU_RESET, LINE_NONE };
	struct sync_S { uint64_t time; unsigned char line; bool state; };
	sync_S sync_q[16];
	int sync_n = 0;
	uint64_t base = 0, z80_t = 0, mcu_t = 0, frame_start = 0;
	uint64_t exec_base = 0, exec_extra = 0; // running CPU: local time at run start, extra for a sync
	int exec_pos = 0;                  // cycles from run start to the current instruction
	uint64_t cur_time(void) const;
	uint64_t snd_next = 0;         // next sound_manager update timer
	uint32_t time_seconds = 0;     // whole seconds taken off the clock above
	bool exec_z80 = false;         // the Z80 is the running CPU
	unsigned char exec_io_offset = 0; // cycle offset of its I/O access (z80_io_access)
public:
	// machine cycles of a Z80 instruction that touches d000-dfff (arkanoid.cpp)
	struct z80_io_S
	{
		unsigned char offset;     // cycle offset of the I/O access
		unsigned char nsteps;
		unsigned char steps[7];   // machine cycle lengths of the whole instruction
	};
private:
	z80_io_S z80_part_io;          // instruction MAME paused inside, before its I/O access
	unsigned char z80_part_done = 0; // cycles of it already run (0 = none)
	unsigned short exec_pc = 0;    // PC of the Z80 instruction being executed
	unsigned short z80_prev_pc = 0xffff; // last whole instruction run (0xffff after an IRQ entry)
	int frame_timer = 0;
	bool abort_run = false;
	int z80_exec(int want);
	bool z80_idle_skip(int want, int &budget);
	int mcu_idle_skip(int want, int budget);
	int mcu_exec(int want);
	uint64_t first_timer(void);
	void line_w(unsigned char line, bool state);
	void line_apply(unsigned char line, bool state);
	void vblank_begin(void);
	void apply_cheats(void);
	unsigned cheat_frame = 0;      // frame counter for timed cheats
	bool boot_done = false;        // passed the boot ROM/RAM checks (cheats wait for it)

	// ---- d008 latch (arkanoid_d008_w) --------------------------------------
	bool m_flipx = false, m_flipy = false;
	unsigned char m_paddle_select = 0;
	bool m_coin_enabled = false;   // d008 bit 3 (coin lockout off)
	unsigned char m_gfxbank = 0, m_palettebank = 0;
	void d008_w(unsigned char data);

	// ---- YM2149 ------------------------------------------------------------
	unsigned char ay_addr = 0;
	bool ay_active = true;
	volatile unsigned char ay_r13_writes = 0;   // ay8910 set_shape() on every R13 write
	unsigned char ay_read(void);

	// ---- inputs ------------------------------------------------------------
	int dial_accum = 0;            // half counts (MAME: keydelta 15 * sensitivity 30%)
	int dial_speed = 0;            // current half counts a frame (ARKANOID_DIAL_*)
	int dial_dir = 0;              // -1 left, 1 right, 0 none
	int dial_hold = 0;             // frames the current direction has been held
	unsigned char in_keys = 0;     // buttons sampled at the last frame update
	unsigned char input_mux_r(void);
	unsigned char system_r(void);

	// ---- MC68705P5 + arkanoid_mcu_device_base -------------------------------
	m68705p5_state mcu;
	bool mcu_in_reset = false;     // INPUT_LINE_RESET held (suspended)
	bool mcu_irq_state = false;    // IRQ line level as the MCU last saw it
	bool m_latch_driven = false;
	bool m_reset_input = false;
	bool m_host_flag = false;
	bool m_mcu_flag = false;
	unsigned char m_host_latch = 0xff;
	unsigned char m_mcu_latch = 0xff;
	unsigned char m_pc_output = 0xff;
	void mcu_power_on(void);
	void mcu_device_reset(void);
	unsigned char mcu_data_r(void);
	void mcu_data_w(unsigned char data);
	void mcu_reset_w(bool assert_line);
	unsigned char mcu_pa_value(void);
	void mcu_pc_w(unsigned char data);
	void mcu_port_cb(unsigned port);

	// ---- video -------------------------------------------------------------
	// state latched at vblank (screen_update time) for the render core
	struct snap_S {
		unsigned char vram[0x800];
		unsigned char spr[0x40];
		unsigned char gfxbank, palettebank;
		bool flipx, flipy;
	};
	snap_S snap[2];
	volatile unsigned char snap_front = 0;
	unsigned char render_snap = 0;
	void publish_snapshot(void);

#ifdef LED_PIN
	const CRGB menu_leds[7] = { LED_RED, LED_GREEN, LED_YELLOW, LED_YELLOW, LED_YELLOW, LED_GREEN, LED_RED };
#endif

#if ARKANOID_DBG_HARNESS
public:
	uint16_t dbg_pc() const { return cpu[0].PC.W; }
	uint16_t dbg_mcu_pc() const { return mcu.cpu.PC; }
	unsigned char dbg_mem(unsigned short a) const { return memory[a]; }
	bool dbg_mcu_reset() const { return mcu_in_reset; }
	uint64_t dbg_now() const { return cur_time(); }
	unsigned long dbg_z80_steps = 0, dbg_z80_halt_steps = 0;
	unsigned long dbg_prefix[5] = {0,0,0,0,0};
	unsigned long dbg_pc_count[65536];
	double dbg_io_now() const { return time_seconds * 6000000.0 + (cur_time() + (exec_z80 ? (uint64_t)exec_io_offset * 166666666666ULL : 0)) / 166666666666.6667; }
	double dbg_cycles(uint64_t t) const { return time_seconds * 6000000.0 + t / 166666666666.6667; }
	void (*dbg_ay_hook)(unsigned char reg, unsigned char val) = 0;
	void (*dbg_mcu_w_hook)(uint16_t addr, uint8_t val, uint64_t now) = 0;
	void (*dbg_mcu_r_hook)(uint16_t addr, uint8_t val, uint64_t now) = 0;
	void (*dbg_mcu_pc_hook)(uint16_t pc, uint64_t now) = 0;
	void (*dbg_z80_pc_hook)(uint16_t pc, double t) = 0;
	void (*dbg_z80_skip_rd_hook)(unsigned short addr, unsigned char val, double t) = 0; // reads of a skipped idle loop
	void (*dbg_slice_hook)(uint64_t base, uint64_t z80_t, uint64_t mcu_t) = 0;
#endif
};

#endif
