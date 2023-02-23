#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "hw/arm/embedded_fuzz.h"
#include "hw/misc/unimp.h"
#include "qemuafl/ember.h"

extern target_ulong afl_entry_point;
extern uint32_t flash_size, flash_base, sram_base, sram2_base, sram3_base, sram_size, sram2_size, sram3_size, periph_size;
extern ram_addr_t ram_size;
extern long int flash_alias_base, periph_base;

static void embedded_fuzz_soc_initfn(Object *obj)
{
    EmbeddedFuzzState *s = EMBEDDED_FUZZ(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

}

static void embedded_fuzz_soc_realize(DeviceState *dev_soc, Error **errp)
{
    EmbeddedFuzzState *s = EMBEDDED_FUZZ(dev_soc);
    MemoryRegion *system_memory = get_system_memory();
    static const MemoryRegionOps mmioOps = {
      .read = ember_mmio_read,
      .write = ember_mmio_write,
      .endianness = DEVICE_NATIVE_ENDIAN,
    };
    DeviceState *armv7m;
    Error *err = NULL;

    memory_region_init_io(&s->mmio, NULL, &mmioOps, s, "Embedded.mmio", 0x20000000);
    memory_region_add_subregion(system_memory, 0x40000000, &s->mmio);

    if(periph_base >= 0){
        memory_region_init_io(&s->mmio_user, NULL, &mmioOps, s, "Embedded.mmio.userdefined", periph_size);
        memory_region_add_subregion(system_memory, periph_base, &s->mmio_user);
    }

    memory_region_init_rom(&s->flash, OBJECT(dev_soc), "Embedded.flash",
                           flash_size, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }

    if(flash_alias_base >= 0)
        memory_region_init_alias(&s->flash_alias, OBJECT(dev_soc), "Embedded.flash.alias", &s->flash, 0, flash_size);

    memory_region_add_subregion(system_memory, flash_base, &s->flash);

    if(flash_alias_base >= 0)
        memory_region_add_subregion(system_memory, (uint32_t)flash_alias_base, &s->flash_alias);

    uint32_t sram_set = ram_size;
    if(sram_size)
      sram_set = sram_size;

    memory_region_init_ram(&s->sram, NULL, "Embedded.sram", sram_set,
                           &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, sram_base, &s->sram);

    if(sram2_size){
        memory_region_init_ram(&s->sram2, NULL, "Embedded.sram2", sram2_size,
                               &err);
        if (err != NULL) {
            error_propagate(errp, err);
            return;
        }
        memory_region_add_subregion(system_memory, sram2_base, &s->sram2);
    }

    if(sram3_size){
        memory_region_init_ram(&s->sram3, NULL, "Embedded.sram3", sram3_size,
                               &err);
        if (err != NULL) {
            error_propagate(errp, err);
            return;
        }
        memory_region_add_subregion(system_memory, sram3_base, &s->sram3);
    }

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 240);
    qdev_prop_set_string(armv7m, "cpu-type", s->cpu_type);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(system_memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }
}

static Property embedded_fuzz_soc_properties[] = {
    DEFINE_PROP_STRING("cpu-type", EmbeddedFuzzState, cpu_type),
    DEFINE_PROP_END_OF_LIST(),
};

static void embedded_fuzz_soc_class_init(ObjectClass *oclass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oclass);

    dc->realize = embedded_fuzz_soc_realize;
    device_class_set_props(dc, embedded_fuzz_soc_properties);
}

static const TypeInfo embedded_fuzz_soc_info = {
    .name          = TYPE_EMBEDDED_FUZZ,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(EmbeddedFuzzState),
    .instance_init = embedded_fuzz_soc_initfn,
    .class_init    = embedded_fuzz_soc_class_init,
};

static void embedded_fuzz_soc_types(void)
{
    type_register_static(&embedded_fuzz_soc_info);
}

type_init(embedded_fuzz_soc_types)
