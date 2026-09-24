/*
 * Taito MC68705 MCU handshake interface - see taito68705_mcu.h for
 * porting notes. Bit-level protocol (latch_control/mcu_portb_w/
 * mcu_portc_r/data_r/data_w/reset_w) ported from MAME's
 * taito68705_mcu_device_base/taito68705_mcu_device (src/mame/shared/
 * taito68705.cpp - license:BSD-3-Clause, copyright-holders:David
 * Haywood, Vas Crabb).
 */

#include "taito68705_mcu.h"

/* Port A "wire value" as the host's data latch would present it when
 * driving the MCU's Port A (taito68705_mcu_device_base::pa_value()):
 *   m_pa_output & (m_latch_driven ? m_host_latch : 0xff)
 * m_pa_output tracks what mcu_pa_w() last saw (i.e. the MCU's own Port
 * A OUTPUT drive - see m68705p5_port_output()); when the host isn't
 * actively driving the shared bus (latch_driven=false) that term is
 * 0xff (no additional pulldown), otherwise it's host_latch. */
static uint8_t pa_wire_value(const taito68705_mcu_state *s) {
    uint8_t pa_out = m68705p5_port_output(&s->mcu, M68705P5_PORT_A);
    return (uint8_t)(pa_out & (s->latch_driven ? s->host_latch : 0xff));
}

void taito68705_reset(taito68705_mcu_state *s, const uint8_t *rom) {
    s->latch_driven = 0;
    s->reset_asserted = 0;
    s->host_flag = 0;
    s->mcu_flag = 0;
    s->host_latch = 0xff;
    s->mcu_latch = 0xff;
    s->last_pb = 0xff;

    m68705p5_reset(&s->mcu, rom);
}

uint8_t taito68705_data_r(taito68705_mcu_state *s) {
    /* data_r(): clear MCU semaphore flag and return the latched byte */
    uint8_t result = s->mcu_latch;
    s->mcu_flag = 0;
    return result;
}

void taito68705_data_w(taito68705_mcu_state *s, uint8_t data) {
    /* data_w(): set host semaphore flag (unless the MCU is held in
       reset) and latch data; if the host latch is currently the thing
       driving the MCU's Port A input (latch_driven), update that
       input immediately (MAME: `if (m_latch_driven) m_mcu->pa_w(data)`);
       assert/clear the MCU's /INT line to match host_flag. */
    if (!s->reset_asserted)
        s->host_flag = 1;
    s->host_latch = data;
    if (s->latch_driven)
        m68705p5_port_input_w(&s->mcu, M68705P5_PORT_A, data);
    m68705p5_irq(&s->mcu, s->host_flag ? 1 : 0);
}

uint8_t taito68705_host_flag(const taito68705_mcu_state *s) {
    return s->host_flag;
}

uint8_t taito68705_mcu_flag(const taito68705_mcu_state *s) {
    return s->mcu_flag;
}

void taito68705_reset_w(taito68705_mcu_state *s, int asserted) {
    s->reset_asserted = asserted ? 1 : 0;
    if (asserted) {
        s->host_flag = 0;
        s->mcu_flag = 0;
        m68705p5_irq(&s->mcu, 0);
    }
    /* device_reset()-on-assert semantics (host_flag/mcu_flag cleared,
       semaphore callback cleared) are folded into taito68705_reset()
       for a full reset; a live reset-line assert here only clears the
       flags/IRQ, matching reset_w()'s own scope (it does NOT re-run
       device_reset() itself - that happens via the CPU's own
       INPUT_LINE_RESET handling, which this port doesn't model as a
       separate runtime "hold PC at reset vector" state machine since
       no flstory-family game pulses MCU reset independently of a full
       machine reset). */
}

/* ============================================================
 * MCU-side sync: observe Port B/C activity written by the MCU's own
 * program since the last call, apply latch_control()'s edge-triggered
 * effects, and refresh what Port C reads back.
 * ============================================================ */

static IRAM_ATTR void taito68705_mcu_sync_step(taito68705_mcu_state *s) {
    uint8_t pb = m68705p5_port_output(&s->mcu, M68705P5_PORT_B);
    uint8_t old_pb = s->last_pb;

    if (pb == old_pb)
        goto refresh_portc;

    {
        uint8_t old_pa = pa_wire_value(s);

        /* PB1 (bit 1): rising edge clears host flag / MCU's /INT */
        if (pb & 0x02) {
            s->latch_driven = 0;
            if (!(old_pb & 0x02)) {
                s->host_flag = 0;
                m68705p5_irq(&s->mcu, 0);
            }
        } else {
            s->latch_driven = 1;
        }
        /* Port A input always reflects the current latch_driven state
           right after PB1 is evaluated (matches MAME calling
           m_mcu->pa_w(0xff) / m_mcu->pa_w(m_host_latch) inline in
           latch_control() before touching PB2). */
        m68705p5_port_input_w(&s->mcu, M68705P5_PORT_A, s->latch_driven ? s->host_latch : 0xff);

        /* PB2 (bit 2): low sets MCU flag; falling edge latches the
           MCU's current Port A OUTPUT (sampled before this write) into
           mcu_latch. */
        if (!(pb & 0x04)) {
            if (!s->reset_asserted)
                s->mcu_flag = 1;
            if (old_pb & 0x04)
                s->mcu_latch = old_pa;
        }

        s->last_pb = pb;
    }

refresh_portc:
    /* mcu_portc_r(): PC0 = host flag, PC1 = !mcu flag, PC2-7 = 1 */
    m68705p5_port_input_w(&s->mcu, M68705P5_PORT_C,
        (uint8_t)((s->host_flag ? 0x01 : 0x00) | (s->mcu_flag ? 0x00 : 0x02) | 0xfc));
}

int taito68705_step(taito68705_mcu_state *s, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += m68705p5_step(&s->mcu, 1);
        taito68705_mcu_sync_step(s);
    }
    return total;
}
