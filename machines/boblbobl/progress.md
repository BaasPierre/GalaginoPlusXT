------------------------------------------------------------------------------

Agent notes from here


2026-09-23 session - status: video and sound verified against MAME (harness), hardware test pending
=====================================================================================================

Everything below was checked against the MAME driver (taito/bublbobl.cpp, bublbobl_v.cpp,
bublbobl_m.cpp), MAME 0.288 itself (C:\mame, lua dumps) and MAME's ymfm sound core.
No comments from earlier agents were trusted.

ROM data - no change needed
  romszip/boblbobl vs the generated headers: bb3/bb5/bb4, a78-08.37, a78-07.46, a71-25.41 byte-exact,
  all 16384 gfx tiles identical to a fresh MAME charlayout decode (ROMREGION_INVERT included).
  One gfx header is correct for this board: MAME has a single GFXDECODE entry (gfx1) used for both
  playfield and sprites, so there is nothing to split into tile/sprite/char files like 1942 or gng.

Video - fixed, pixel-exact
  1. Mirror: the port mapped MAME x straight onto framebuffer rows (a transpose = mirror image).
     Now fb_row = 16 + 255 - x, fb_col = y - 16 (a real 90 degree rotation, flip DIP Off = MAME default).
  2. Repeating tiles ("CORPCORPCORP"): videoram was read with goffs & 0x1cff, which drops address
     bits 8-9. MAME indexes videoram unmasked; the shadow now holds c000-dfff as one block.
  3. Colours: palette entries must be byte-swapped RGB565 (Video::write DMAs little-endian words,
     the panel wants the high byte first). Palette format itself (RGBx_444 big endian) was right.
  4. Backdrop: MAME fills with pen 255, not black.
  Proof: harness "truth" mode renders MAME RAM dumps; 0 mismatched pixels vs MAME snapshots on
  5 frames, including 2 with Flip Screen On (debug/boblbobl_truth/compare.py).
  BOBLBOBL_ROW_OFFSET (16) centres the 256 game lines in the 288-line framebuffer; TFT_X/Y_OFFSET
  centre that framebuffer on the panel. They are independent and both correct.
  TFT_VFLIP: with it defined, set BOBLBOBL_DSW0_FLIP_SCREEN_SELECTED to _ON; the flip path is MAME's
  own (verified above). Left at Off / TFT_VFLIP undefined, matching the current config.h.

CPU / board logic - fixed
  - Coins: IN0 bits 2/3 are ACTIVE_HIGH in MAME; the port idled them high (coin always inserted).
  - Demo Sounds DIP override never cleared bit 3.
  - Unmapped reads return 0x00 on all three CPUs (checked in MAME); the port returned 0xff. The sound
    program probes 0xe000 for a diagnostic ROM.
  - main/sub vblank IRQ is irq0_line_hold at line 240: now held until accepted (was lost under DI).
  - Sound CPU ran 6 instructions per slice; now cycle-budgeted at 3MHz. It runs from power-on
    (MAME pulses /SRESET at reset), not from the first 0xfa03 write.
  - Slice budgets used integer division (101100/50400 cycles per frame instead of 101376/50688).

Sound - rewritten, bit-exact vs MAME's ymfm
  Root cause of "no sound": writes to 0x9001/0xa001 (chip data ports) were decoded with
  (addr & 0xf001) == 0x9000, so no register write ever reached either chip. Also no YM timers,
  status or IRQ existed, and the old FM models (boblbobl_ym.inc / boblbobl_opl.inc, deleted) differed
  from ymfm (OPN envelope clocked every sample, no detune, no fnum latch, no OPL vibrato, SSG an
  octave low through the shared AY renderer with linear volume).
  Now:
  - boblbobl.cpp: YM2203/YM3526 timers, status, IRQ (ymfm rules incl. timer B phase), sound NMI
    edge logic, /SRESET, 0x9000/0xa000 mirrors.
  - boblbobl_fm.h/.inc: transcription of ymfm for YM2203 FM + SSG and YM3526 (plain C++11).
  - Register writes go emulation core -> audio core through a timestamped ring and are replayed
    at their chip-clock time, two frames behind (about 34ms). 125 chip clocks per 24kHz sample.
  - Mix = MAME's: ym2203 0.25 (FM + 3 SSG), ym3526 0.50. BOBLBOBL_SND_GAIN (256 = MAME) scales it.
  - audio.cpp: boblbobl no longer uses the AY path; renderFmSample() is the whole mix.
  Proof (harness):
  - "ymlog": sound-latch commands identical to MAME's log; register writes identical up to a
    one-frame main-CPU skew in the attract demo.
  - "fmcmp 4000": port chips vs real ymfm on the game's own writes, 2.8M FM + 2.8M OPL samples,
    0 mismatches (through gameplay music).
  - "fmfuzz 400000": random registers (SSG, SSG-EG, multi-freq, OPL rhythm/LFO/KSL): 0 mismatches.
  - "wav24 4000": the device path (ring + 24kHz renderer), every chip tick equal to ymfm.
    Listen: boblbobl_harness/fm_port_24k.wav (device path) vs fm_ref_ymfm.wav (ymfm reference).
  The game never writes the SSG, so SSG silence is correct.

Still to do on hardware (cannot be checked on the PC)
  1. Orientation on the real panel with the photo orientation of b1/b2.jpeg (should now be upright).
  2. Audio CPU load: rendering both chips at native rate costs about half of the three-Z80
     emulation on the PC. If it crackles, the renderer is the place to look.
  3. Loudness: BOBLBOBL_SND_GAIN if MAME's level is too quiet on the speaker.
  4. Game speed vs 59.19Hz (harness can't measure ESP32 timing).
  ESP32 build: pio run -e release_cheapYellowDisplay succeeds (RAM 23.1%, flash 48.1%), no warnings.

Performance pass (after logs/capture.txt: run_frame 1-5Hz)
  The log showed two problems: emulation alone took ~34ms per frame (Cpu 345ms / 10 loops, the
  budget is 16.9ms), and the audio render on core 0 took 1-4 s per half-frame, starving video.
  Changes (all exact, re-verified: fmcmp/fmfuzz/wav24 bit-exact vs ymfm, 5 video frames pixel-exact):
  - Idle-loop skip: main parks in "jr $" at 0x01ED (64% of its instructions), sub in "jp $000A"
    (89%); both do all work in interrupt handlers. When no interrupt can be taken, the rest of the
    slice is consumed in whole loop iterations (cycles and ICount unchanged). Z80 steps per frame:
    main 8678 -> 3095, sub 10039 -> 1066, audio 4960 (unchanged).
  - boblbobl.cpp compiled with -O2 (pragma, as cm99.cpp does); project default is -Os.
  - Sound hot path in IRAM, its lookup tables in DRAM (video streams the 1MB tile table through the
    same flash cache on core 0). IRAM .text now 83KB.
  - Silent operators (released to 0x3ff) are not clocked; SSG output skipped when all volumes are 0.
    OPL operators 13/17 are always clocked (rhythm mode reads their phase).
  Next: flash and capture a new log with DEBUG_TIMING - run_frame should approach 59Hz and the
  per-half "audio=" figure must drop well below the half-frame time.

Performance pass 2 (log: run_frame ~17Hz, emulation 23ms/frame, audio ~700ms of every second on core 0)
  - BOBLBOBL_SLICES 300 -> 101: MAME runs this board with set_maximum_quantum(6000Hz) = 101 slices
    per frame. Re-verified: latch commands and register writes vs MAME unchanged, audio bit-exact.
  - Audio cost on device was ~100x the PC ratio: non-inlined helper functions of the sound code
    were still emitted to flash, where video (1MB tile table on the same core) evicts them from the
    cache. Every sound helper, the machine's memory handlers, run_frame and bankswitch_w are now
    IRAM (iram0.text 87KB). Only reset/init paths stay in flash.

Performance pass 3 (flstory lessons applied; target 50-59Hz is fine per user)
  - Renderer: fully transparent tiles (pen 15 everywhere; 45-95% of all cells each frame) are
    dropped at scan time (2KB bitmap built once at reset). Spans are linked per 8-line band, so
    each band only walks its own cells; the pixel loop pre-clips and steps through the tile.
    Spans per frame ~1800 -> 80-460 (gameplay ~1100). Still 0 mismatched pixels on all 5 MAME frames.
  - BOBLBOBL_SLICES 101 -> 50: harness ymlog vs MAME identical to 101 (25 shifts commands by a frame).
  - BOBLBOBL_PROFILE (boblbobl.h, default 1): once a second prints
      "bb us/frame: main= sub= audiocpu= (frame period) | us/screen: scan= blit= spans= screens= | snd: ns/sample"
    Send that capture next. If core 0 is still short, the remaining lever is cheaper FM audio
    (e.g. rendering at 24kHz instead of 41.7kHz) - not bit-exact any more, user decision.

Performance pass 4 (profile: main 11.5ms + sub 3.9ms + sound CPU 11.8ms per frame on core 1;
audio render 31us/sample = ~780ms of every second on core 0)
  Emulation core:
  - Sound CPU idle loop (016D..0178, 252 cycles, maps state onto itself) skipped in whole passes
    when no command/IRQ/timer can land inside. Proven exact: every YM write at the identical chip
    clock, identical audio. Only 4960 -> 3692 instr/frame: in play the sound CPU mostly does music.
  - Per instruction the Z80 core still touched flash: memory trampolines (OpZ80_INL/RdZ80/WrZ80,
    emulation.cpp), CB/ED prefix handlers, IntZ80 and all Z80 tables. SHARED change (all Z80
    machines, placement only): tables -> DRAM (Tables.h), those functions -> IRAM. CodesDD/FD
    (27KB) stay in flash (IRAM now 99KB of ~128KB).
  - The three fixed 32KB Z80 ROMs are copied to heap RAM at reset (each falls back to flash if
    malloc fails; boot prints "boblbobl: ROMs in RAM: audio= main= sub=").
  Core 0 audio:
  - BOBLBOBL_SND_FAST (boblbobl.h, default 1): chips still clocked at 41.7kHz (pitch/envelopes
    exact) but the output stage runs once per 24kHz sample; silent SSG not clocked. vs exact:
    loudness correlation 0.98, spectrum correlation 0.98 (median), ~10% louder (no 2-tap average).
    BOBLBOBL_SND_FAST 0 = bit-exact path (harness: build_exact.bat, fmcmp still 0 mismatches).
    Listen: boblbobl_harness/fm_exact_24k.wav vs fm_fast_24k.wav.
  I2S DAC question: switching the built-in DAC to an I2S DAC (PCM5102) does not change speed - both
  use the same I2S DMA; i2s_write is ~4ms/s. The cost is computing the samples.

Performance pass 5 (capture: 35Hz; core 1 now 16.8ms/frame - fine; core 0 wall = audio 25us/sample)
  BOBLBOBL_SND_FAST now steps the chips once per 24kHz output sample (opn_t/opl_t::step_fast):
  phase += n * phase_step (n = chip samples in the output sample, 1 or 2 - exact phase), envelopes
  and OPL LFO/noise still clocked every chip sample they are due, operator feedback and output once.
  Operators in sustain with rate 0 are not envelope-clocked (nothing can change).
  Native: 213 -> 159 ns/sample (-25%). vs exact: loudness corr 0.98, spectrum corr 0.97 median.
  Exact path (BOBLBOBL_SND_FAST 0) re-verified: fmcmp/fmfuzz 0 mismatches, device path identical.

Pass 5 result on hardware: 54-57Hz, picture and sound correct (user: "a win").
  Core 0 per second: SPI wait ~440ms, audio ~420ms (16.7us/sample), render ~145ms -> full.
  The 40MHz SPI link needs ~25.8ms per screen; 60Hz (30 screens/s half-rate) leaves little room.

Pass 6: BOBLBOBL_SND_HALFRATE (boblbobl.h, default 1, only with SND_FAST): mix computed for every
  second 24kHz sample (250 chip clocks), the one between linearly interpolated. Native 159 -> 118
  ns/sample. Sound vs exact: loudness corr 0.96, spectrum corr 0.80 median (treble loss).
  Listen: boblbobl_harness/fm_half_24k.wav vs fm_fast_24k.wav vs fm_exact_24k.wav.
  If it does not reach ~60Hz, or the treble loss is audible, set BOBLBOBL_SND_HALFRATE 0
  (the 54-57Hz pass-5 state).
  -> REVERTED (2026-09-23). On hardware the treble loss was audible, and in gameplay (bubbles) the
     game still dropped to 42-43Hz. Code removed; the machine is back on the pass-5 state.


=====================================================================================================
SPEED - HOW BOBLBOBL WENT FROM 1-5Hz TO ~55Hz (reference for future ports)
=====================================================================================================

Where we ended (pass 5, CYD board, ESP32 240MHz, 40MHz SPI display, built-in DAC):
  attract mode ~54-57Hz; gameplay with many bubbles lower (last capture 42-43Hz, taken with the
  reverted pass 6). Picture pixel-exact vs MAME, sound = MAME/ymfm stepped once per output sample.
  Both cores are close to full in gameplay:
    core 1 (emulation): main ~8ms + sound CPU ~7.8ms + sub ~1-2ms per frame (budget 16.9ms)
    core 0 (video+audio): sound render ~21us/sample (~500ms/s), SPI waits ~340ms/s, render ~120ms/s
  Next real step is hardware (ESP32-S3: more internal RAM for code/ROMs, faster cores; needs the
  PCM5102 I2S path because the S3 has no DAC). An 80MHz-capable panel would also remove most of
  the SPI wait (VIDEO_HALF_RATE is only used below 80MHz).

How to measure (always measure first - guesses were wrong several times):
  - emulation.h DEBUG_TIMING: "10-loops ... run_frame Hz" (game speed) and "half=... render= write=
    audio=" (core 0 per second).
  - boblbobl.h BOBLBOBL_PROFILE: "bb us/frame: main= sub= audiocpu= | us/screen: scan= blit= spans=
    | snd: ns/sample" (CPU cycle counter, very cheap). Set to 0 once finished.
  - Native harness: "prof" (instructions/frame per CPU + hottest PCs), "sndbench" (render ns/sample),
    "spanstat" (transparent cells). PC ratios did NOT transfer to the device when flash was involved.

Steps, in order of payoff (all verified in the harness before flashing):
  1. Correct cycle accounting first. Slice budgets used integer division (CPUs ran 0.3-0.6% slow);
     the sound CPU ran 6 instructions/slice instead of its 3MHz budget. Fix correctness, then speed.
  2. Skip idle loops exactly. Main ("jr $" at 0x01ED) and sub ("jp $000A") wait in a jump-to-self
     loop for their interrupt: 64% / 89% of all instructions. When no interrupt can be taken, burn
     the rest of the slice in whole loop iterations (cycles and ICount unchanged). The sound CPU's
     polling loop (016D, 252 cycles, maps state onto itself) is skipped the same way; proven exact
     by comparing every YM write with its chip clock before/after.
  3. Keep hot code and data out of flash. The video core streams the 1MB tile table through flash;
     anything the other paths fetch from flash then stalls (the device was ~100x slower than the PC
     for the sound render, 3-4x slower per Z80 instruction than galaga). Moved to IRAM: sound render
     and its helpers, the machine's memory handlers, run_frame, bankswitch, and in the SHARED Z80
     core IntZ80, CodesCB/ED and the OpZ80_INL/RdZ80/WrZ80 trampolines; Z80 tables and the sound
     lookup tables to DRAM (DRAM_ATTR). Check with xtensa-esp32-elf-nm that nothing hot is left at
     0x400Dxxxx. IRAM is now ~105KB of ~128KB - CodesDD/FD (27KB) did not fit.
  4. Z80 ROMs in RAM: the three fixed 32KB ROMs are malloc'd and copied at reset (each falls back to
     flash on its own; boot prints which).
  5. Don't draw what can't be seen. 45-95% of the cells in a frame are fully transparent tiles:
     dropped at scan time (2KB bitmap built once). Cells are linked per 8-line band so each band
     walks only its own cells; the pixel loop pre-clips and steps through the tile.
  6. Fewer slices: 300 -> 101 (MAME's own quantum) -> 50. Checked against MAME's sound-command and
     register logs in the harness; 25 started to shift commands by a frame, so 50.
  7. -O2 for boblbobl.cpp (#pragma, as cm99.cpp does; the project builds -Os).
  8. Sound render cost (BOBLBOBL_SND_FAST): the exact ymfm path clocks both chips at 41.7kHz and
     mixes every chip sample (~31us/sample on the device). Fast mode steps the chips once per 24kHz
     output sample (phase += n*step, envelopes/LFO still at the chip rate), mixes once, skips a
     silent SSG and operators that cannot change. ~17-21us/sample, sounds the same (loudness 0.98,
     spectrum 0.97 correlation vs exact). BOBLBOBL_SND_FAST 0 restores the bit-exact path.
  Tried and rejected: mixing only every second sample (pass 6) - audible treble loss, not enough gain.
  Not a factor: I2S DAC vs built-in DAC (same DMA, i2s_write ~4ms/s).
