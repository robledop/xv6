#pragma once

#include "types.h"
#include "sleeplock.h"
#include "fs.h"

struct buf
{
    int flags;
    u32 dev;
    u32 blockno;
    struct sleeplock lock;
    u32 refcnt;
    struct buf* prev; // LRU cache list
    struct buf* next;
    struct buf* qnext; // disk queue
    u8 data[BSIZE];
};

#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk