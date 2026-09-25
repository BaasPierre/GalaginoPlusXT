# m6805 / m68705p5 core – change log

This core is shared: **flstory** uses it through `taito68705_mcu.c` and the global `m6805_read`/`m6805_write` hooks that `flstory.cpp` defines. **arkanoid** uses it through its own per-instance hooks. If a machine that uses this CPU regresses, start with the changes below.

## 2026-09-24: changes for the Arkanoid port

Every change is either opt-in or arithmetically identical to the old behaviour, so flstory was not supposed to change. That was not re-tested on hardware.

### 1. Per-instance memory hooks (`m6805.h`, `m6805.c`)
- `m6805_state` gained `rd_hook` / `wr_hook` (function pointers).
- `fetch8()`, `rd()` and `wr()` now do `s->rd_hook ? s->rd_hook(...) : m6805_read(...)` (and the same for writes).
- Why: `flstory.cpp` defines the global `m6805_read`/`m6805_write` unconditionally and forwards them to `flstory::s_instance`. When another machine runs, that is null, so every MCU fetch would read 0xFF.
- flstory path: `m68705p5_reset()` sets both hooks to NULL, so it still uses the globals.
- **Regression check:** the hooks must be set before `m6805_reset()`, because that reads the reset vector. `m6805_state` is a plain struct inside a `new`-constructed machine, so the fields are garbage until a reset. **Any code that calls `m6805_reset()` directly without going through `m68705p5_reset*()` must now set the hooks first.**

### 2. `m68705p5_reset_hooked()` (`m68705p5.h`, `m68705p5.c`)
- This is the new reset that installs the hooks; `m68705p5_reset()` now just calls it with NULL, NULL.
- The body is otherwise unchanged.

### 3. Timer sync counters (`m68705p5.h`, `m68705p5.c`)
- New fields `timer_line_syncs` (a counter) and `timer_sync_off` (the cycle offset in the current step; 0xff means none). Both are reset in `m68705p5_reset_hooked()`.
- They are incremented in `timer_tcr_w()` (every TCR write) and in `timer_update()` (when a timer interrupt is raised and not masked by TIM).
- The core never reads them. Only arkanoid uses them, to model MAME's `set_input_line(M6805_INT_TIMER)` syncs.
- **Regression check:** pure bookkeeping. If these fields changed behaviour, that would be a bug.

### 4. Interrupt-entry cycles burnt separately (`m68705p5_step()`)
- Before: `timer_update(s, c)` once per step, where `c` is the 11 cycles of interrupt entry plus the opcode's cycles.
- After: if an interrupt is taken in this step (same condition as `service_interrupt()`: a pending IRQ or timer interrupt and `CC.I` clear), it calls `timer_update(s, 11, 11)` and then `timer_update(s, c - 11, c)`. This matches MAME's `interrupt()` `burn_cycles(11)` followed by the opcode's burn.
- The split is arithmetically the same: prescaler and TDR end up identical and TIR is set in the same step. Only the recorded `timer_sync_off` differs.
- `timer_update()` gained a third parameter, `elapsed`, which is only used for `timer_sync_off`.
- **Regression check:** if timer interrupt timing changes for an MCU, compare against the old single call `timer_update(s, (unsigned)c)`.

Not changed: opcode behaviour, cycle tables, port logic, the PC-at-reset override, and `taito68705_mcu.c`.

## Verified against MAME (harness, 2026-09-24)
- All 256 opcode cycle counts plus the interrupt entry match MAME's `s_hmos_cycles`: 0 mismatches. The test was a throwaway program; see the next section to rebuild it.
- With arkanoid's MCU (a75__06), the MCU register-access stream was identical to MAME for 120M Z80 cycles.

## Known differences from MAME, still present (not fixed: flstory relies on current behaviour)
- **TCR in PGM mode (MOR bit 6 clear):** `timer_tcr_w()` forces TIE (bit 4) on and ignores the divisor and source bits. MAME's `tcr_w()` only forces TIE in MOR mode, and in PGM mode it takes the divisor and source from the TCR write. Both arkanoid (MOR=0x08) and flstory (MOR=0x00) are in PGM mode:
  - arkanoid writes TCR=0x47, i.e. divisor 7 on the internal clock. That equals the core's default, so it is harmless there, but the MCU reads TCR back as 0x57 where MAME gives 0x47.
  - For flstory, check whether its MCU writes a divisor other than 7. If it does, its timer rate differs from MAME.
  - The comment at `m68705p5.c` `timer_tcr_w()` ("TIMER_MOR games ... never rely on TCR") is stale for both games.
- **Edge-only IRQ latch:** MAME's `m6805_hmos_device::execute_set_input()` only latches a pending IRQ when the line level *changes*, and keeps `m_irq_state` across resets. `m6805_irq(s, 1)` latches on every assert. arkanoid handles this in its glue (`mcu_irq_state`); flstory does not.
- **Dummy operand reads:** CLR and read-modify-write instructions do a dummy operand read, which MAME's handlers don't. It has no side effects on RAM or timer registers, but it would on a port with a read callback.

## Stale comment elsewhere
- `taito68705_mcu.h` says the Arkanoid hookup is "out of scope for this port". It is now implemented inside `machines/arkanoid/arkanoid.cpp`, not in this glue file.

## Re-running the cycle check
The test was not kept. To rebuild it: step each opcode with `m6805_step()` from a zeroed state (the hooks must be NULL), then compare the cycles with MAME's `s_hmos_cycles` in `mame/mame-master/src/devices/cpu/m6805/m6805.cpp`. Compile with `/I` pointing to a folder containing an `esp_attr.h` that does `#define IRAM_ATTR`.
