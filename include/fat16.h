#pragma once

#include <stdint.h>


/** @brief Discriminates between FAT items representing files or directories. */
typedef unsigned int FAT_ITEM_TYPE;
#define FAT_ITEM_TYPE_DIRECTORY 0
#define FAT_ITEM_TYPE_FILE 1


/**
 * @struct fat_directory_entry
 * @brief Packed on-disk representation of a FAT directory entry.
 */
struct fat_directory_entry
{
    uint8_t name[8];
    uint8_t ext[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t first_cluster;
    uint32_t size;
} __attribute__((packed));


/**
 * @struct fat_directory
 * @brief Snapshot of a FAT directory loaded into memory.
 */
struct fat_directory
{
    struct fat_directory_entry* entries;
    int entry_count;
    int sector_position;
    uint32_t ending_sector_position;
    int pool_index;
};

/**
 * @struct fat_item
 * @brief Wrapper describing either a directory snapshot or a single entry.
 */
struct fat_item
{
    struct fat_directory_entry* item;
    struct fat_directory directory;
    FAT_ITEM_TYPE type;
    bool owns_item_entry;
};

struct fat_file_descriptor
{
    struct fat_item* item;
    uint32_t position;
    struct disk* disk;
};


struct file_system* fat16_init(void);
