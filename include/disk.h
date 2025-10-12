#pragma once

#include <defs.h>
#include <stdint.h>

typedef unsigned int DISK_TYPE;

#define DISK_TYPE_PHYSICAL 0

struct disk {
    int id;
    DISK_TYPE type;
    uint16_t sector_size;
    struct file_system *fs;
    void *fs_private;
};


void disk_init(void);
struct disk *disk_get(int index);
void disk_sync_buffer(struct buf *b);
