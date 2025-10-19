#pragma once

#include "param.h"
#include "spinlock.h"
#include "file.h"

struct icache{
    struct spinlock lock;
    struct inode inode[NINODE];
};
