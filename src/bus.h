#ifndef CV1K_BUS_H
#define CV1K_BUS_H

#include "cv1k_types.h"
#include "cv1k_config.h"

struct cv1k_machine;

struct cv1k_bus {
    struct cv1k_machine *machine;
};

void cv1k_bus_bind(struct cv1k_bus *bus, struct cv1k_machine *machine);
cv1k_u8 cv1k_bus_read8(struct cv1k_bus *bus, cv1k_u32 addr);
cv1k_u16 cv1k_bus_read16(struct cv1k_bus *bus, cv1k_u32 addr);
cv1k_u16 cv1k_bus_fetch16(struct cv1k_bus *bus, cv1k_u32 addr);
cv1k_u32 cv1k_bus_read32(struct cv1k_bus *bus, cv1k_u32 addr);
void cv1k_bus_write8(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u8 data);
void cv1k_bus_write16(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u16 data);
void cv1k_bus_write32(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data);
cv1k_u8 cv1k_bus_port_read(struct cv1k_bus *bus, int port_id);
void cv1k_bus_cache_op(struct cv1k_bus *bus, cv1k_u32 addr, int op);
void cv1k_bus_tmu_tick(struct cv1k_bus *bus);
cv1k_u32 cv1k_bus_cycles_until_event(struct cv1k_bus *bus);
void cv1k_bus_invalidate_icache_all(struct cv1k_bus *bus);
void cv1k_bus_ldtlb(struct cv1k_bus *bus);
void cv1k_bus_irq_ack(struct cv1k_bus *bus, cv1k_u32 level, cv1k_u32 event);
void cv1k_bus_exception_ack(struct cv1k_bus *bus, cv1k_u32 event, cv1k_u32 tra);
int cv1k_bus_mame_trapa_enabled(struct cv1k_bus *bus);

#define CV1K_CACHEOP_PREF 0
#define CV1K_CACHEOP_OCBI 1
#define CV1K_CACHEOP_OCBP 2
#define CV1K_CACHEOP_OCBWB 3

#endif
