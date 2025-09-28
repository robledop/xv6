#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

/** @brief Start the non-boot (AP) processors. */
static void startothers(void);
/** @brief Common CPU setup code. */
static void mpmain(void) __attribute__((noreturn));
/** @brief Kernel page directory */
extern pde_t *kpgdir;
/** @brief First address after kernel loaded from ELF file */
extern char end[]; // first address after kernel loaded from ELF file

/**
 * @brief Bootstrap processor entry point.
 *
 * Allocates an initial stack, sets up essential subsystems, and
 * launches the first user process before transitioning into the scheduler.
 *
 * @return This function does not return; it hands control to the scheduler.
 */
int main(void)
{
    kinit1(end, P2V(4 * 1024 * 1024));          // phys page allocator
    kvmalloc();                                 // kernel page table
    mpinit();                                   // detect other processors
    lapicinit();                                // interrupt controller
    seginit();                                  // segment descriptors
    picinit();                                  // disable pic
    ioapicinit();                               // another interrupt controller
    consoleinit();                              // console hardware
    uartinit();                                 // serial port
    pinit();                                    // process table
    tvinit();                                   // trap vectors
    binit();                                    // buffer cache
    fileinit();                                 // file table
    ideinit();                                  // disk
    startothers();                              // start other processors
    kinit2(P2V(4 * 1024 * 1024), P2V(PHYSTOP)); // must come after startothers()
    user_init();                                // first user process
    mpmain();                                   // finish this processor's setup
}

/**
 * @brief Application processor entry point used by entryother.S.
 *
 * Switches to the kernel's page tables and continues CPU initialization
 * via ::mpmain.
 */
static void
mpenter(void)
{
    switch_kvm();
    seginit();
    lapicinit();
    mpmain();
}

/**
 * @brief Complete per-CPU initialization and enter the scheduler.
 */
static void mpmain(void)
{
    cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
    idtinit();                    // load idt register
    xchg(&(mycpu()->started), 1); // tell startothers() we're up
    scheduler();                  // start running processes
}

/** @brief Boot-time page directory referenced from assembly. */
pde_t entrypgdir[]; // For entry.S

/**
 * @brief Start all application processors (APs).
 *
 * Copies the AP bootstrap code to low memory, provides each processor with a
 * stack, entry point, and temporary page directory, then issues INIT/SIPI
 * sequences until every CPU reports as started.
 */
static void startothers(void)
{
    // This name depends on the path of the entryohter file. I moved it to the build folder
    extern uchar _binary_build_entryother_start[], _binary_build_entryother_size[];

    // Write entry code to unused memory at 0x7000.
    // The linker has placed the image of entryother.S in
    // _binary_entryother_start.
    uchar* code = P2V(0x7000);
    memmove(code, _binary_build_entryother_start, (uint)_binary_build_entryother_size);

    cprintf("%d cpu%s\n", ncpu, ncpu == 1 ? "" : "s");

    for (struct cpu* c = cpus; c < cpus + ncpu; c++)
    {
        if (c == mycpu()) // We've started already.
            continue;


        // Tell entryother.S what stack to use, where to enter, and what
        // pgdir to use. We cannot use kpgdir yet, because the AP processor
        // is running in low  memory, so we use entrypgdir for the APs too.
        char* stack = kalloc();
        *(void **)(code - 4) = stack + KSTACKSIZE;
        *(void (**)(void))(code - 8) = mpenter;
        *(int **)(code - 12) = (void *)V2P(entrypgdir);

        lapicstartap(c->apicid, V2P(code));

        // wait for cpu to finish mpmain()
        while (c->started == 0)
            ;
    }
}

/**
 * @brief Boot page table image used while bringing up processors.
 *
 * Page directories (and tables) must start on page boundaries, hence the
 * alignment attribute. The large-page bit (PTE_PS) allows identity mapping of
 * the first 4 MiB of physical memory.
 */
__attribute__((__aligned__(PGSIZE)))
pde_t entrypgdir[NPDENTRIES] = {
    // Map VA's [0, 4MB) to PA's [0, 4MB)
    [0] = (0) | PTE_P | PTE_W | PTE_PS,
    // Map VA's [KERNBASE, KERNBASE+4MB) to PA's [0, 4MB)
    [KERNBASE >>
        PDXSHIFT] = (0) | PTE_P | PTE_W | PTE_PS,
};
