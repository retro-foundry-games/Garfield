#include "guestmem.h"
#include "game.h"

/* Native byte that holds the big-endian byte at `addr`: multi-byte members are stored byte-swapped. */
static uint8_t *be_byte(uint32_t addr)
{
    addr &= 0xFFFFFF;  /* 24-bit address bus */
    unsigned lo = 0, hi = mem_field_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        const MemField *f = &mem_fields[mid];
        if (addr < f->addr) hi = mid;
        else if (addr >= f->addr + f->size * f->count) lo = mid + 1;
        else {
            uint32_t off = addr - f->addr, elem = off / f->size;
            return (uint8_t *)&mem + f->addr + elem * f->size + (f->size - 1 - off % f->size);
        }
    }
    return (uint8_t *)&mem + addr;
}

uint8_t guest_rd8(uint32_t a) { return *be_byte(a); }
uint16_t guest_rd16(uint32_t a) { return (uint16_t)(guest_rd8(a) << 8 | guest_rd8(a + 1)); }
uint32_t guest_rd32(uint32_t a)
{
    if (mem_is_pointer(a)) {   /* a pointer member holds a pointer into `mem`: the original's address is its offset */
        return mem_address_of_pointer(*(uint32_t *)((uint8_t *)&mem + (a & 0xFFFFFF)));
    }
    return (uint32_t)guest_rd16(a) << 16 | guest_rd16(a + 2);
}
void guest_wr8(uint32_t a, uint8_t v) { *be_byte(a) = v; }
void guest_wr16(uint32_t a, uint16_t v) { guest_wr8(a, v >> 8); guest_wr8(a + 1, (uint8_t)v); }
void guest_wr32(uint32_t a, uint32_t v)
{
    if (mem_is_pointer(a)) {
        *(uint32_t *)((uint8_t *)&mem + (a & 0xFFFFFF)) = mem_pointer_of_address(v);
        return;
    }
    guest_wr16(a, v >> 16);
    guest_wr16(a + 2, (uint16_t)v);
}
