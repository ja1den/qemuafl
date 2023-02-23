#include "qemu/osdep.h"
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include "qemu-common.h"
#include "qemuafl/ember.h"
#include "cpu.h"
#include "qemuafl/common.h"
#include "qemu/main-loop.h"

//#define DEBUG_MMIO
//#define DEBUG_LOADING
//#define DEBUG_CAUSE
//#define DEBUG_INT
//#define DEBUG_EXTRA

#define RUN_INTS
#define PERIPHERALINPUTPLAYBACK
#define FERMCOV
#define ALLOWBREAKSPEC
//#define VARIABLEINTERRUPTS
#define AFLREADBUFFERMIN 8
//#define RESTOREISRPOINTER

// FOR DMA BUF HARDCODE
extern bool dma_hardcode;
extern unsigned int passthrough_reg_count;
extern struct passthroughReg *passthrough_regs;


int interrupt_counter = 512;
uint8_t interrupt_gap = 64;
int should_disable_caching = 0;
bool disable_interrupts = false;

SysTickState* systick = NULL;
target_ulong non_excp_afl_loc = 0;
bool had_int = false;

bool subfork_run = false;
int subfork_offsets[MAX_SUBFORK] = {0};
int cur_subfork_id = -1;
int host_pipes[MAX_SUBFORK][2];
int response_pipes[MAX_SUBFORK][2];
uint8_t map_backup[MAP_SIZE];

static curRegVal* peripheralRegisters = NULL;

static int systick_counter = 0;
static int systick_runs = 0;
static int runs = 0;
static int cur_interrupt = -1;
static int exception_nums[257] = {0};
int enabled_ints[256] = {-1};

static char* aflBuffer = NULL;
int aflReadSize = -1;
int aflReadPos = 0;

extern const char* aflInput;
extern int forkserver_installed;

static void aflBuildInput(void){
    if(!forkserver_installed){
      afl_setup();
      afl_forkserver(first_cpu);
    }
    FILE* f = fopen(aflInput,"r");
#ifdef DEBUG_LOADING
    printf("Opening file for input: %s\n",aflInput);
#endif
    if(!f){
#ifdef DEBUG_CAUSE
        fprintf(stderr,"Unable to open aflInput\n");
#endif
        exit(1);
    }
    aflBuffer = malloc(afl_fuzz_size_max+1);
    aflReadSize = fread(aflBuffer,1,afl_fuzz_size_max,f);
#ifdef DEBUG_LOADING
    printf("Read %u bytes\n", aflReadSize);
#endif
    if(aflReadSize <= 0){
        fprintf(stderr,"Unable to read data from aflInput\n");
        exit(1);
    }
    if(aflReadSize < AFLREADBUFFERMIN) {
#ifdef DEBUG_CAUSE
        printf("Input was below minimum size\n");
#endif
        exit(1);
    }
#ifdef VARIABLEINTERRUPTS
    aflReadPos=1;
    interrupt_gap = aflBuffer[0];
#else
    aflReadPos=0;
#endif
    fclose(f);
}

static uint64_t aflRead(unsigned size){
    if(aflReadSize<0) aflBuildInput();
    if(subfork_run && cur_subfork_id >= 0 && cur_subfork_id < MAX_SUBFORK){
        if(aflReadPos + size > subfork_offsets[cur_subfork_id]){
            // Start subfork server
#ifdef DEBUG_SUBFORK
            printf("SUBFORK %d: Launching subfork server %d with divergence point %d\n", cur_subfork_id, cur_subfork_id, subfork_offsets[cur_subfork_id]);
            printf("SUBFORK %d Server State: offset: %d(%d)/%d, int counter %d, cur_int %d\n", cur_subfork_id, aflReadPos, size, aflReadSize, interrupt_counter, cur_interrupt);
#endif
            memcpy(map_backup,afl_area_ptr,MAP_SIZE);
            MEM_BARRIER();
            // Report ready
            if(write(response_pipes[cur_subfork_id][1], &aflReadPos, 4) != 4){ exit(6); }

            while(1){
                // Wait for go signal
                int signal = read(host_pipes[cur_subfork_id][0], &signal, 4);
                if(signal != 4){
#ifdef DEBUG_SUBFORK
                    printf("SUBFORK %d: Go signal read failed. Subfork kill? fd %d, returned %d, errno: %d\n", cur_subfork_id, host_pipes[cur_subfork_id][0], signal, errno);
#endif
                    exit(2);
                }

                pid_t child_pid = fork();
                if(child_pid < 0)
                    exit(4);
                if(!child_pid){ // Child
                    subfork_run = false;
                    close(host_pipes[cur_subfork_id][0]);
                    close(response_pipes[cur_subfork_id][1]);
                    memcpy(afl_area_ptr,map_backup,MAP_SIZE);
                    break;
                }
                // Parent
                if(write(response_pipes[cur_subfork_id][1], &child_pid, 4) != 4)
                    exit(5);
#ifdef DEBUG_SUBFORK
                printf("SUBFORK %d: New test created from subfork server %d, with PID %u\n", cur_subfork_id, cur_subfork_id, child_pid);
#endif
                int status;
                waitpid(child_pid, &status, 0);
                write(response_pipes[cur_subfork_id][1], &status, 4);
            }
#ifdef DEBUG_SUBFORK
            printf("SUBFORK %d: Child running with PID %d\n", cur_subfork_id, getpid());
#endif
            int temp = aflReadPos;
            aflBuildInput();
            aflReadPos = temp;
#ifdef DEBUG_SUBFORK
            printf("SUBFORK %d Inst State: offset: %d(%d)/%d, int counter %d, cur_int %d\n", cur_subfork_id, aflReadPos, size, aflReadSize, interrupt_counter, cur_interrupt);
#endif
        }
    }
    if(aflReadPos+size >= aflReadSize){
#ifdef DEBUG_CAUSE
        printf("Exhausted input buffer, exiting\n");
#endif
        handle_subfork_exit();
        exit(0);
    }
    uint64_t val = *(uint64_t*)(aflBuffer+aflReadPos);
    val = val << ((8-size)*8);
    val = val >> ((8-size)*8);
    aflReadPos+=size;
#ifdef DEBUG_MMIO
    printf("Input Read new input value: 0x%llx from input index %d\n",val,aflReadPos);
#endif
    return val;
}

static void new_reg(uint32_t memAddr, uint32_t val){
    curRegVal* newReg = malloc(sizeof(curRegVal));
    if(!newReg)
        exit(0); // OOM
    newReg->next = peripheralRegisters;
    newReg->value = val;
    newReg->memAddr = memAddr;
    newReg->accessMask = aflRead(sizeof(newReg->accessMask));
    newReg->maskBitsRemaining = sizeof(newReg->accessMask) * 8;
    peripheralRegisters = newReg;
}

void handle_subfork_exit(void){
  if(subfork_run && cur_subfork_id >= 0 && cur_subfork_id < MAX_SUBFORK){
    int status = -1;
    // Report failure
    if(write(response_pipes[cur_subfork_id][1], &status, 4) != 4){ exit(6); }
  }
/*  if(afl_fork_child){
    printf("Closing TSL \n");
    close((FORKSRV_FD - 1));
  }
*/
}

static void fuzz_check_interrupts(void){
#ifdef RUN_INTS
  CPUState* cpu = first_cpu;
  if(!cpu || disable_interrupts)
    return;
  interrupt_counter--;
  if(!interrupt_counter){
    ARMCPU* acpu = ARM_CPU(cpu);
    cur_interrupt++;
    if(!aflBuffer) // Make sure we have defined the input source so we can set the interrupt gap
      aflBuildInput();
    interrupt_counter = (uint32_t)interrupt_gap;
    if(exception_nums[cur_interrupt]>0){
      runs++;
      should_disable_caching = 1;
      qemu_mutex_lock_iothread();
#ifdef DEBUG_INT
      printf("Running interrupt %d (offset %d)\n", exception_nums[cur_interrupt], cur_interrupt);
#endif
      if(exception_nums[cur_interrupt] == ARMV7M_EXCP_SYSTICK){
        systick_timer_tick((void*)systick);
      } else {
        armv7m_nvic_set_pending(acpu->env.nvic,exception_nums[cur_interrupt],false);
      }
      qemu_mutex_unlock_iothread();
    } else {
#ifdef RESTOREISRPOINTER
      if(exception_nums[0] <= 0 && cur_interrupt >= 0){ // All interrupts disabled, has the code just temporarily turned them off?
        cur_interrupt--; // Remove the increment we did earlier, and try trigger this same interrupt next time
      } else {
        cur_interrupt = -1;
      }
#else
      cur_interrupt = -1;
#endif
    }
  }
#endif
}

void ember_add_interrupt(int irq){
#ifdef DEBUG_INT
    printf("Attaching interrupt %d\n", irq);
#endif
    bool enabled = (enabled_ints[0] == -1);
    for(int i=0;i<257;i++){
        if(enabled_ints[i] == irq)
            enabled = true;
        if(enabled_ints[i] == 0)
            break;
    }
    if(!enabled)
        return;
    for(int j=0;j<257;j++){
        if(exception_nums[j]<=0 || exception_nums[j] == irq){
            exception_nums[j]=irq;
            break;
        }
    }
}

void ember_remove_interrupt(int irq){
#ifdef DEBUG_INT
    printf("Detaching interrupt %d\n", irq);
#endif
    for(int k=0;k<257;k++){
        if(exception_nums[k]==irq){
            for(int j=k;j<257;j++){
                if(exception_nums[j]<=0){
                    exception_nums[k]=exception_nums[j-1];
                    exception_nums[j-1]=0;
                    break;
                }
            }
            break;
        }
    }
}

void ember_handle_infinite_loops(void){
  // We are in an infinite loop, but this could be waiting for an interrupt.
  // But if block chaining is enabled, the interrupt will never trigger.
  // Cases where this is triggered may be an indicator that block chaining should be disabled.
#ifdef DEBUG_CAUSE
  printf("Exiting from infinite loop\n");
#endif
  handle_subfork_exit();
  exit(0);
}

int ember_manage_ints(void){
  had_int = false;
  CPUState* fcpu = first_cpu;
  if(fcpu){
    ARMCPU* cpu = ARM_CPU(fcpu);
    if(cpu->env.v7m.exception){
      if(!non_excp_afl_loc){
        non_excp_afl_loc = afl_prev_loc;
#ifdef FERMCOV
        afl_prev_loc = 0;
#endif
      }
    } else if(non_excp_afl_loc){
      should_disable_caching = 0;
      had_int = true;
#ifdef FERMCOV
      afl_prev_loc = non_excp_afl_loc;
      non_excp_afl_loc = 0;
      return 1;
#endif
    }
  }
 
  if(fcpu){
    ARMCPU* cpu = ARM_CPU(fcpu);
    if(!cpu->env.v7m.exception){
      should_disable_caching = 0;
      fuzz_check_interrupts();
    }
  }
  return 0;
}

void ember_handle_halt(void){
#ifdef RUN_INTS
  CPUState* cpu = first_cpu;
  if(!cpu || disable_interrupts){
    printf("Invalid call to handle halt! Skipping\n");
    return;
  }
  printf("Halt detected, running interrupt\n");
  ARMCPU* acpu = ARM_CPU(cpu);
  cur_interrupt++;
  // Pick valid interrupt is valid
  if(exception_nums[cur_interrupt]<=0){
    cur_interrupt = 0;
  }
  if(exception_nums[cur_interrupt]>0){
    runs++;
    should_disable_caching = 1;
    qemu_mutex_lock_iothread();
    if(exception_nums[cur_interrupt] == ARMV7M_EXCP_SYSTICK){
      systick_timer_tick((void*)systick);
    } else {
      armv7m_nvic_set_pending(acpu->env.nvic,exception_nums[cur_interrupt],false);
    }
    qemu_mutex_unlock_iothread();
  } else {
    // No interrupts enabled, exit
#ifdef DEBUG_CAUSE
    printf("Exiting from halt\n");
#endif
    handle_subfork_exit();
    exit(0);
  }
#endif
}

void ember_handle_delay(uint32_t flags){
#ifdef RUN_INTS
  CPUState* cpu = first_cpu;
  if(!cpu || !systick){
#ifdef DEBUG_EXTRA
    printf("Invalid call to handle delay! Skipping\n");
#endif
    return;
  }

  if(flags & (1 << 16)) // SysTick Control and Status Register (CSR) COUNTFLAG
    return;
  systick_counter++;
  if(systick_counter < 3)
    return;

  systick_counter = 0;

#ifdef ALLOWBREAKSPEC
  systick->control |= 1<<16;
  return;
#endif

#ifdef DEBUG_EXTRA
  ARMCPU* acpu = ARM_CPU(cpu);
  systick_runs++;
  printf("Delay loop detected at 0x%X, running interrupt %d\n",acpu->env.regs[15],systick_runs);
#endif
  for(int i=0;i<257;i++){
    if(exception_nums[i] == ARMV7M_EXCP_SYSTICK){
      should_disable_caching = 1;
      systick_timer_tick((void*)systick);
      return;
    }
  }
#endif
}

uint64_t ember_mmio_read(void* opaque, hwaddr addr, unsigned size){
#ifdef DEBUG_MMIO
  printf("MMIO read " TARGET_FMT_plx "\n", addr);
#endif
  // Peripheral region on ARM v6/7/8 -M CPUs
  if(size <= 8){
    for(int i=0; i < passthrough_reg_count; i++){
      if(passthrough_regs[i].addr == addr + 0x40000000){
        return passthrough_regs[i].val;
      }
    }
#ifdef PERIPHERALINPUTPLAYBACK
    curRegVal* reg = peripheralRegisters;
    while(reg != NULL && reg->memAddr != addr)
      reg = reg->next;
    if(!reg){
      new_reg((uint32_t)addr, 0);
      reg = peripheralRegisters;
    }
    uint32_t maskRes = reg->accessMask & 3;
    reg->accessMask = reg->accessMask >> 2;
    reg->maskBitsRemaining -= 2;
    if(reg->maskBitsRemaining == 0){
      reg->accessMask = aflRead(sizeof(reg->accessMask));
      reg->maskBitsRemaining = sizeof(reg->accessMask) * 8;
    }
    if(maskRes == 3)
      return reg->value;
#endif
    uint64_t in = aflRead(size);
#ifdef PERIPHERALINPUTPLAYBACK
    reg->value = (uint32_t)in;
#endif
    return in;
  }
  printf("Peripheral access had unsupported requested size of %u\n",size);
  return 0;
}


void ember_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size){
#ifdef DEBUG_MMIO
  printf("MMIO write " TARGET_FMT_plx " = 0x%"PRIx64"\n", addr, val);
#endif
  for(int i=0; i < passthrough_reg_count; i++){
    if(passthrough_regs[i].addr == addr + 0x40000000){
      passthrough_regs[i].val = (uint32_t)val;
      return;
    }
  }

#ifdef PERIPHERALINPUTPLAYBACK
  curRegVal* regList = peripheralRegisters;
  while(regList != NULL && regList->memAddr != (uint32_t)addr){
    regList = regList->next;
  }
  if(regList){
    regList->value = (uint32_t)val;
  } else {
    new_reg((uint32_t)addr, (uint32_t)val);
  }
#endif
}
