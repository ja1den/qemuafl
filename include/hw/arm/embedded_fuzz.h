#ifndef HW_ARM_EMBEDDED_FUZZ_H
#define HW_ARM_EMBEDDED_FUZZ_H

#include "hw/arm/armv7m.h"
#include "qom/object.h"

#define TYPE_EMBEDDED_FUZZ "embedded-fuzz"
OBJECT_DECLARE_SIMPLE_TYPE(EmbeddedFuzzState, EMBEDDED_FUZZ)

struct EmbeddedFuzzState {
    SysBusDevice parent_obj;
    char *cpu_type;

    ARMv7MState armv7m;

    MemoryRegion sram;
    MemoryRegion sram2;
    MemoryRegion sram3;
    MemoryRegion flash;
    MemoryRegion flash_alias;
    MemoryRegion mmio;
    MemoryRegion mmio_user;
};

#endif
