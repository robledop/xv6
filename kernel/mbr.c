#include "defs.h"
#include "mbr.h"
#include "buf.h"

// MBR Partition Types
#define MBR_TYPE_EMPTY 0x00        // Empty or unused partition
#define MBR_TYPE_FAT12 0x01        // FAT12 filesystem
#define MBR_TYPE_FAT16_LT32M 0x04  // FAT16 filesystem with less than 32MB
#define MBR_TYPE_EXTENDED 0x05     // Extended partition
#define MBR_TYPE_FAT16_GT32M 0x06  // FAT16 filesystem with more than 32MB
#define MBR_TYPE_FAT32_CHS 0x0B    // FAT32 filesystem (CHS addressing)
#define MBR_TYPE_FAT32_LBA 0x0C    // FAT32 filesystem (LBA addressing)
#define MBR_TYPE_FAT16_LBA 0x0E    // FAT16 filesystem (LBA addressing)
#define MBR_TYPE_EXTENDED_LBA 0x0F // Extended partition using LBA
#define MBR_TYPE_LINUX 0x83        // Linux native partition


struct mbr mbr = {0};

void mbr_init_fs()
{
    for (int i = 0; i < 4; i++) {
        if (mbr.part[i].type == MBR_TYPE_EMPTY) {
            continue;
        }

        switch (mbr.part[i].type) {
        case MBR_TYPE_LINUX:
            cprintf("Linux partition found at MBR partition %d\n", i);
            // ext2_load_superblock(mbr.part[i].lba_start + 2); // EXT2 superblock is at offset 2
            break;
        case MBR_TYPE_FAT16_LBA:
            //vfs_insert_file_system(i, fat16_init());
            cprintf("FAT16 LBA partition found at MBR partition %d\n", i);
            break;
        default:
            cprintf("Unsupported partition type: %d\n", mbr.part[i].type);
            break;
        }
    }
}

/**
 * @brief Loads the MBR from the boot block.
 *
 * @return int
 */
void mbr_load()
{
    auto buf = bread(0, 0);
    memmove(&mbr, buf->data, sizeof(mbr));
    brelse(buf);
    if (mbr.signature != 0xAA55) {
        cprintf("Invalid MBR signature: 0x%X\n", mbr.signature);
        panic("Invalid MBR signature");
    }
    mbr_init_fs();
}

struct mbr *mbr_get()
{
    return &mbr;
}