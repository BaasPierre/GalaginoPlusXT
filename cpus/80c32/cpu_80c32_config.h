#ifndef CPU_80C32_CONFIG_H
#define CPU_80C32_CONFIG_H

#include <stdint.h>
#include "cpu_80c32.h"

#ifdef __cplusplus
extern "C" {
#endif

// SFR Address Constants
#define SFR_P0   0x80
#define SFR_TCON 0x88
#define SFR_P1   0x90
#define SFR_SCON 0x98
#define SFR_P3   0xB0

static inline unsigned char cb_read_direct(struct cpu_80c32 *aCPU, int aAddress) {
    uint8_t addr = (uint8_t)(aAddress & 0xFF);
    if (aCPU->sfr_r) {
        return aCPU->sfr_r(aCPU, addr);
    }
    return aCPU->ram[addr];
}

static inline void cb_write_direct(struct cpu_80c32 *aCPU, int aAddress, unsigned char aValue) {
    uint8_t addr = (uint8_t)(aAddress & 0xFF);
    if (aCPU->sfr_w) {
        aCPU->sfr_w(aCPU, addr, aValue);
    } else {
        aCPU->ram[addr] = aValue;
    }
}

#ifdef __cplusplus
}
#endif

#endif // CPU_80C32_CONFIG_H