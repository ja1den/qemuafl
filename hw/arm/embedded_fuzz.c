#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "hw/arm/embedded_fuzz.h"
#include "hw/arm/boot.h"

/* Main SYSCLK frequency in Hz (168MHz) */
#define SYSCLK_FRQ 168000000ULL
extern target_ulong flash_size;

static void embedded_fuzz_init(MachineState *machine)
{
    DeviceState *dev;

    system_clock_scale = NANOSECONDS_PER_SECOND / SYSCLK_FRQ;

    dev = qdev_new(TYPE_EMBEDDED_FUZZ);
    qdev_prop_set_string(dev, "cpu-type", machine->cpu_type);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    armv7m_load_kernel(ARM_CPU(first_cpu),
                       machine->kernel_filename,
                       flash_size);
}

static void embedded_fuzz_machine_init(MachineClass *mc)
{
    mc->desc = "Embedded Fuzz Machine";
    mc->init = embedded_fuzz_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m4");
    mc->default_ram_size = 256 * 1024;
}

DEFINE_MACHINE("embedded_fuzz", embedded_fuzz_machine_init)
