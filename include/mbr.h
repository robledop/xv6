#pragma once
#include "types.h"


struct mbr_partition_entry {
    u8 status;       /* 1 byte - bootable status */
    u8 chs_start[3]; /* 3 bytes - CHS start address */
    u8 type;         /* 1 byte - partition type */
    u8 chs_end[3];   /* 3 bytes - CHS end address */
    u32 lba_start;   /* 4 bytes - LBA start address */
    u32 num_sectors; /* 4 bytes - Number of sectors */
} __attribute__((packed));

struct mbr {
    u8 bootstrap[446];             /* 446 bytes - Bootstrap code */
    struct mbr_partition_entry part[4]; /* 64 bytes - Partition entries */
    u16 signature;                 /* 2 bytes - Signature */
} __attribute__((packed));


void mbr_load();
struct mbr *mbr_get();
