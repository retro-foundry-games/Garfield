#pragma once
#include <stdint.h>

/* Dev only: big-endian access to the port's memory by ST address, translating the typed members. The port
   itself never goes through addresses. */
uint8_t guest_rd8(uint32_t addr);
uint16_t guest_rd16(uint32_t addr);
uint32_t guest_rd32(uint32_t addr);
void guest_wr8(uint32_t addr, uint8_t v);
void guest_wr16(uint32_t addr, uint16_t v);
void guest_wr32(uint32_t addr, uint32_t v);
