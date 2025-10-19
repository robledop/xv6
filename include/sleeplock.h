#pragma once

#include "types.h"
#include "spinlock.h"

// Long-term locks for processes
struct sleeplock
{
    u32 locked; // Is the lock held?
    struct spinlock lk; // spinlock protecting this sleep lock

    // For debugging:
    char* name; // Name of lock.
    int pid; // Process holding lock
};