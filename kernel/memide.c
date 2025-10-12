// Fake IDE disk; stores blocks in memory.
// Useful for running kernel without scratch disk.

#include <types.h>
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

extern uchar _binary_fs_img_start[], _binary_fs_img_size[];

/** @brief Number of disk blocks exposed by the in-memory disk. */
static int disksize;
/** @brief Pointer to the start of the in-memory disk image. */
static uchar *memdisk;

/** @brief Initialize the memory-backed disk using the embedded fs image. */
void ideinit(void)
{
    memdisk  = _binary_fs_img_start;
    disksize = (uint)_binary_fs_img_size / BSIZE;
}

/** @brief Memory disk interrupt handler (no-op placeholder). */
void ideintr(void)
{
    // no-op
}

/**
 * @brief Synchronize a buffer with the memory disk backing store.
 *
 * @param b Buffer to read or write; must be locked on entry.
 */
void iderw(struct buf *b)
{
    uchar *p;

    if (!holdingsleep(&b->lock)) {
        panic("iderw: buf not locked");
    }
    if ((b->flags & (B_VALID | B_DIRTY)) == B_VALID)
        panic("iderw: nothing to do");
    if (b->dev != 1)
        panic("iderw: request not for disk 1");
    if (b->blockno >= disksize)
        panic("iderw: block out of range");

    p = memdisk + b->blockno * BSIZE;

    if (b->flags & B_DIRTY) {
        b->flags &= ~B_DIRTY;
        memmove(p, b->data, BSIZE);
    } else
        memmove(b->data, p, BSIZE);
    b->flags |= B_VALID;
}