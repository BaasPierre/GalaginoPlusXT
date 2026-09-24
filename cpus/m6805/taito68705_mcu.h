/*
 * Taito MC68705 MCU handshake interface for Galagino
 *
 * Ported from MAME's taito68705_mcu_device_base / taito68705_mcu_device
 * (src/mame/shared/taito68705.h, taito68705.cpp - license:BSD-3-Clause,
 * copyright-holders:David Haywood, Vas Crabb), on top of the m68705p5
 * variant (m68705p5.h). This is the hookup used by flstory/onna34ro
 * (and many other Taito 68705-protected games - see the driver list in
 * taito68705.cpp's header comment); it is NOT the Arkanoid-style hookup
 * (latch control on port C instead of port B) - that's a different MAME
 * device (arkanoid_mcu_device_base) and is out of scope for this port.
 *
 * Protocol summary (see taito68705.cpp for the exact bit-level source
 * this was transcribed from):
 *   - Two independent one-byte latches, host->MCU and MCU->host, each
 *     with its own semaphore flag.
 *   - Host writes to the MCU's data port (taito68705_data_w): latches
 *     the byte, raises the "host flag" semaphore, and asserts the
 *     MCU's /INT line so it wakes up and reads the byte off its own
 *     Port A.
 *   - Host reads from the MCU's data port (taito68705_data_r): returns
 *     the MCU->host latch and clears the "MCU flag" semaphore.
 *   - The MCU's Port C bits 0/1 report the two semaphores back to
 *     itself (PC0 = host flag, PC1 = !mcu flag) so its firmware can
 *     poll them.
 *   - The MCU's Port B bits 1/2 are the control pair: a rising edge on
 *     PB1 clears the host flag (acknowledging it consumed the byte on
 *     its own Port A); PB2 driven low latches the MCU's current Port A
 *     output into the MCU->host latch and raises the MCU flag semaphore
 *     (telling the host a reply is ready).
 *
 * This project's single-active-machine-instance model (see m6805.h's
 * "Integration model" note in m68705p5.h) means the owning machine
 * calls into this glue directly - there is no devcb-style callback
 * indirection here either. The machine:
 *   - calls taito68705_data_w()/taito68705_data_r() from its own
 *     m6809_write/m6809_read (or whichever main-CPU family) memory map
 *     at the addresses the game wires the MCU's data port to;
 *   - calls taito68705_status_r() for the "MCU status" address some
 *     games expose (matches flstory_mcu_status_r() bit layout: bit 0 =
 *     host flag, bit 1 = mcu flag) so main-CPU code can poll readiness
 *     without doing a (possibly blocking) data read/write;
 *   - calls taito68705_reset_w() when the game's main CPU can hold the
 *     MCU in reset (some drivers tie this to a control latch bit;
 *     flstory does not use it - the MCU free-runs from power-on - but
 *     it's ported for completeness/other future Taito-hookup games);
 *   - drives the MCU exclusively through taito68705_step() (never
 *     m68705p5_step() directly) - it steps the MCU core one
 *     instruction at a time and, after each one, re-derives the
 *     handshake state from whatever the MCU's own program just wrote
 *     to Port B (latch_control's PB1/PB2 edge effects) and refreshes
 *     what Port C reads back. This project has no live devcb write-
 *     callback wake-up path - the MCU writes its ports through
 *     m68705p5_mem_write() same as any other register - so per-
 *     instruction polling after the fact is how this port observes
 *     the same edges MAME's mcu_portb_w()/mcu_pa_w() callbacks see
 *     immediately on the write.
 */

#ifndef TAITO68705_MCU_H
#define TAITO68705_MCU_H

#include <stdint.h>
#include "m68705p5.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct taito68705_mcu_state_S {
    m68705p5_state mcu;

    uint8_t latch_driven;  /* MCU's Port A is currently being driven by the host latch */
    uint8_t reset_asserted;
    uint8_t host_flag;     /* host has written a byte the MCU hasn't consumed yet */
    uint8_t mcu_flag;      /* MCU has written a byte the host hasn't consumed yet */
    uint8_t host_latch;    /* host -> MCU byte */
    uint8_t mcu_latch;     /* MCU -> host byte */

    uint8_t last_pb;       /* previous Port B output, to detect PB1/PB2 edges in sync() */
} taito68705_mcu_state;

/* Reset both the MCU core and the handshake state. `rom` is the MCU's
 * program dump (see m68705p5_reset() - same lifetime requirement). */
void taito68705_reset(taito68705_mcu_state *s, const uint8_t *rom);

/* Execute up to `count` MCU instructions, syncing the handshake latch
 * state after every one (see header note on why this must happen at
 * single-instruction granularity, same rationale as m68705p5_step()
 * driving the timer per-opcode instead of per-batch). Returns cycles
 * consumed. This is the ONLY function that should advance the MCU
 * once taito68705_reset() has been called - never call m68705p5_step()
 * directly on s->mcu, or handshake edges will be missed. */
IRAM_ATTR int taito68705_step(taito68705_mcu_state *s, int count);

/* Host-side interface - call from the main CPU's memory map. */
uint8_t taito68705_data_r(taito68705_mcu_state *s);
void taito68705_data_w(taito68705_mcu_state *s, uint8_t data);
/* bit 0: host flag (1 = MCU has NOT yet consumed the last host_data_w)
 * bit 1: mcu flag  (1 = MCU HAS written a byte the host hasn't read)
 * matches flstory_mcu_status_r() in flstory.cpp: ((host_semaphore==CLEAR)?0x01:0)
 * | ((mcu_semaphore!=CLEAR)?0x02:0) - i.e. host_semaphore_r()/mcu_semaphore_r()
 * as exposed by taito68705_mcu_device_base (host_flag()?1:0 / mcu_flag()?1:0
 * inverted per that driver's own status-register bit polarity - see
 * flstory_mcu_status_r's comment in flstory.cpp for the exact polarity
 * this project's flstory machine will need to reproduce at the call site). */
uint8_t taito68705_host_flag(const taito68705_mcu_state *s);
uint8_t taito68705_mcu_flag(const taito68705_mcu_state *s);

void taito68705_reset_w(taito68705_mcu_state *s, int asserted);

#ifdef __cplusplus
}
#endif

#endif /* TAITO68705_MCU_H */
