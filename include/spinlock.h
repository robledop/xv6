#pragma once

#include "types.h"

// Mutual exclusion lock.
struct spinlock
{
    u32 locked; // Is the lock held?

    // For debugging:
    char* name; // Name of lock.
    struct cpu* cpu; // The cpu holding the lock.
    u32 pcs[10]; // The call stack (an array of program counters)
    // that locked the lock.
};