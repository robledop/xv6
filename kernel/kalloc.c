// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void* vstart, void* vend);
/** @brief First address after kernel loaded from ELF file */
extern char end[]; // first address after kernel loaded from ELF file
// defined by the kernel linker script in kernel.ld

/** @brief Linked list node for free pages */
struct run
{
    struct run* next;
};

/** @brief Kernel memory allocator state */
struct
{
    struct spinlock lock;
    int use_lock;
    struct run* freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.

/** @brief Initialize kernel memory allocator phase 1 */
void kinit1(void* vstart, void* vend)
{
    initlock(&kmem.lock, "kmem");
    kmem.use_lock = 0;
    freerange(vstart, vend);
}

/** @brief Initialize kernel memory allocator phase 2 */
void kinit2(void* vstart, void* vend)
{
    freerange(vstart, vend);
    kmem.use_lock = 1;
}

/** @brief Free a range of memory */
void freerange(void* vstart, void* vend)
{
    int count = 0;
    char* p = (char*)PGROUNDUP((uint)vstart);
    for (; p + PGSIZE <= (char*)vend; p += PGSIZE)
    {
        kfree(p);
        count++;
    }

    cprintf("freerange: freed %d pages, start: %p, end: %p\n", count, vstart, vend);
}

/** @brief Free the page of physical memory pointed at by v,
 * which normally should have been returned by a
 * call to kalloc().  (The exception is when
 * initializing the allocator; see kinit)
 */
void kfree(char* v)
{
    if ((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
        panic("kfree");

    // Fill with junk to catch dangling refs.
    memset(v, 1, PGSIZE);

    if (kmem.use_lock)
        acquire(&kmem.lock);
    struct run* r = (struct run*)v;
    r->next = kmem.freelist; // The current head of the free list becomes the next of this page
    kmem.freelist = r; // This page becomes the head of the free list
    if (kmem.use_lock)
        release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
/** @brief Allocate one 4096-byte page of physical memory */
char* kalloc(void)
{
    if (kmem.use_lock)
        acquire(&kmem.lock);
    struct run* r = kmem.freelist; // Gets the first free page
    if (r)
        kmem.freelist = r->next; // The next free page becomes the head of the list
    if (kmem.use_lock)
        release(&kmem.lock);
    return (char*)r; // Returns the first free page (or 0 if none)
}