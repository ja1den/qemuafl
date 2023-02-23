#ifndef EMBER_H
#define EMBER_H
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include "hw/timer/armv7m_systick.h"
#include "exec/hwaddr.h"

#ifndef ARMV7M_EXCP_SYSTICK
#define ARMV7M_EXCP_SYSTICK 15
#endif

//#define DEBUG_SUBFORK

void ember_add_interrupt(int irq);
void ember_remove_interrupt(int irq);
int ember_manage_ints(void);
void ember_handle_halt(void);
void ember_handle_delay(uint32_t flags);
void ember_handle_infinite_loops(void);
uint64_t ember_mmio_read(void* opaque, hwaddr addr, unsigned size);
void ember_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);
void systick_timer_tick(void* opaque);
void handle_subfork_exit(void);

extern uint8_t interrupt_gap;
extern int interrupt_counter;
extern SysTickState* systick;
extern bool had_int;
extern int should_disable_caching;
extern uint32_t afl_fuzz_size_max;
extern int aflReadSize;
extern bool disable_interrupts;

typedef struct curRegVal curRegVal;
struct curRegVal {
    curRegVal* next;
    uint32_t memAddr;
    uint32_t value;
    uint32_t accessMask;
    uint32_t maskBitsRemaining;
};

struct passthroughReg {
    uint32_t addr;
    uint32_t val;
};

#define MAX_SUBFORK 5
extern bool subfork_run;
extern int subfork_offsets[MAX_SUBFORK];
extern int cur_subfork_id;
extern int host_pipes[MAX_SUBFORK][2];
extern int response_pipes[MAX_SUBFORK][2];

#endif
