/**
 * @file fat16.c
 * @brief FAT16 file system driver providing VFS integration and cluster-level helpers.
 */

#include <fat16.h>
#include <stdint.h>
#include <memory.h>

#define FAT16_SIGNATURE 0x29
#define FAT16_FAT_ENTRY_SIZE 0x02
#define FAT16_FAT_BAD_SECTOR 0xFFF7
#define FAT16_FREE 0x00
// End of cluster chain marker
#define FAT16_EOC 0xFFF8
#define FAT16_EOC2 0xFFFF

// For internal use
/** @brief Discriminates between FAT items representing files or directories. */
typedef unsigned int FAT_ITEM_TYPE;
#define FAT_ITEM_TYPE_DIRECTORY 0
#define FAT_ITEM_TYPE_FILE 1

// FAT Directory entry attributes
#define FAT_FILE_READ_ONLY 0x01
#define FAT_FILE_HIDDEN 0x02
#define FAT_FILE_SYSTEM 0x04
#define FAT_FILE_VOLUME_LABEL 0x08
#define FAT_FILE_SUBDIRECTORY 0x10
#define FAT_FILE_ARCHIVE 0x20
#define FAT_FILE_LONG_NAME 0x0F

/**
 * @brief Compile-time limits for the statically-allocated FAT16 data structures.
 */
#define FAT16_MAX_ROOT_ENTRIES 512U
#define FAT16_MAX_FAT_SECTORS 256U
#define FAT16_MAX_CLONED_DIRECTORIES 32U
#define FAT16_MAX_DIRECTORY_ENTRIES FAT16_MAX_ROOT_ENTRIES
#define FAT16_MAX_CLONED_ENTRIES 128U
#define FAT16_MAX_FAT_ITEMS 128U
#define FAT16_MAX_FILE_DESCRIPTORS 128U

/**
 * @struct fat_header_extended
 * @brief FAT BIOS Parameter Block extension fields stored in the boot sector.
 */
struct fat_header_extended {
    uint8_t drive_number;
    uint8_t win_nt_bit;
    uint8_t signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t system_id_string[8];
} __attribute__((packed));

/**
 * @struct fat_header
 * @brief FAT BIOS Parameter Block describing core geometry details.
 */
struct fat_header {
    uint8_t jmp[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_copies;
    uint16_t root_entries; // Max number of entries in the root directory
    uint16_t total_sectors;
    uint8_t media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_large;
} __attribute__((packed));

/**
 * @struct fat_h
 * @brief Combined FAT header as it appears on disk.
 */
struct fat_h {
    struct fat_header primary_header;
    union fat_h_e {
        struct fat_header_extended extended_header;
    } shared;
};

/**
 * @struct fat_private
 * @brief Per-mount FAT16 driver state cached by the VFS.
 */
struct fat_private {
    struct fat_h header;
    struct fat_directory root_directory;
    struct disk_stream cluster_read_stream;
    struct disk_stream cluster_write_stream;
    struct disk_stream fat_read_stream;
    struct disk_stream fat_write_stream;
    struct disk_stream directory_stream;
};

#define FAT_ENTRIES_PER_SECTOR (512 / sizeof(struct fat_directory_entry))

/** @brief Statically-allocated driver state shared across the filesystem. */
static struct file_system fat16_fs_storage;
static struct fat_private fat_private_storage;
static struct fat_directory_entry fat16_root_directory_entries[FAT16_MAX_ROOT_ENTRIES];

/** @brief Cached in-memory copy of the first FAT (single-drive configuration). */
static uint8_t fat_table[FAT16_MAX_FAT_SECTORS * 512U];
static size_t fat_table_length_bytes;
static struct fat_directory_entry fat16_directory_entries_pool[FAT16_MAX_CLONED_DIRECTORIES]
                                                              [FAT16_MAX_DIRECTORY_ENTRIES];
static bool fat16_directory_entries_used[FAT16_MAX_CLONED_DIRECTORIES];
static struct fat_directory_entry fat16_entry_pool[FAT16_MAX_CLONED_ENTRIES];
static bool fat16_entry_pool_used[FAT16_MAX_CLONED_ENTRIES];
static struct fat_item fat16_item_pool[FAT16_MAX_FAT_ITEMS];
static bool fat16_item_pool_used[FAT16_MAX_FAT_ITEMS];
static struct fat_file_descriptor fat16_fd_pool[FAT16_MAX_FILE_DESCRIPTORS];
static bool fat16_fd_pool_used[FAT16_MAX_FILE_DESCRIPTORS];

static void fat16_free_file_descriptor(struct fat_file_descriptor *descriptor);

static int fat16_acquire_directory_slot(void)
{
    for (int i = 0; i < (int)FAT16_MAX_CLONED_DIRECTORIES; i++) {
        if (!fat16_directory_entries_used[i]) {
            fat16_directory_entries_used[i] = true;
            memset(fat16_directory_entries_pool[i], 0, sizeof(fat16_directory_entries_pool[i]));
            return i;
        }
    }

    panic("FAT16 directory pool exhausted");
    return -1;
}

static void fat16_release_directory_slot(const int slot)
{
    if (slot < 0) {
        return;
    }

    ASSERT(slot < (int)FAT16_MAX_CLONED_DIRECTORIES, "Invalid directory pool slot");
    fat16_directory_entries_used[slot] = false;
    memset(fat16_directory_entries_pool[slot], 0, sizeof(fat16_directory_entries_pool[slot]));
}

static struct fat_directory_entry *fat16_entry_alloc(void)
{
    for (int i = 0; i < (int)FAT16_MAX_CLONED_ENTRIES; i++) {
        if (!fat16_entry_pool_used[i]) {
            fat16_entry_pool_used[i] = true;
            memset(&fat16_entry_pool[i], 0, sizeof(struct fat_directory_entry));
            return &fat16_entry_pool[i];
        }
    }

    panic("FAT16 directory entry pool exhausted");
    return nullptr;
}

static void fat16_entry_release(struct fat_directory_entry *entry)
{
    if (!entry) {
        return;
    }

    const ptrdiff_t idx = entry - fat16_entry_pool;
    ASSERT(idx >= 0 && idx < (ptrdiff_t)FAT16_MAX_CLONED_ENTRIES, "Invalid entry release");
    fat16_entry_pool_used[idx] = false;
    memset(entry, 0, sizeof(struct fat_directory_entry));
}

static struct fat_item *fat16_item_alloc(void)
{
    for (int i = 0; i < (int)FAT16_MAX_FAT_ITEMS; i++) {
        if (!fat16_item_pool_used[i]) {
            fat16_item_pool_used[i] = true;
            struct fat_item *item   = &fat16_item_pool[i];
            memset(item, 0, sizeof(*item));
            item->directory.pool_index = -1;
            item->owns_item_entry      = false;
            return item;
        }
    }

    panic("FAT16 item pool exhausted");
    return nullptr;
}

static void fat16_item_release(struct fat_item *item)
{
    if (!item) {
        return;
    }

    const ptrdiff_t idx = item - fat16_item_pool;
    ASSERT(idx >= 0 && idx < (ptrdiff_t)FAT16_MAX_FAT_ITEMS, "Invalid FAT item release");
    fat16_item_pool_used[idx] = false;
    memset(item, 0, sizeof(*item));
}

static struct fat_file_descriptor *fat16_fd_alloc(void)
{
    for (int i = 0; i < (int)FAT16_MAX_FILE_DESCRIPTORS; i++) {
        if (!fat16_fd_pool_used[i]) {
            fat16_fd_pool_used[i] = true;
            memset(&fat16_fd_pool[i], 0, sizeof(struct fat_file_descriptor));
            return &fat16_fd_pool[i];
        }
    }

    panic("FAT16 descriptor pool exhausted");
    return nullptr;
}

static void fat16_fd_release(struct fat_file_descriptor *descriptor)
{
    if (!descriptor) {
        return;
    }

    const ptrdiff_t idx = descriptor - fat16_fd_pool;
    ASSERT(idx >= 0 && idx < (ptrdiff_t)FAT16_MAX_FILE_DESCRIPTORS, "Invalid descriptor release");
    fat16_fd_pool_used[idx] = false;
    memset(descriptor, 0, sizeof(*descriptor));
}
/** @brief Guards concurrent FAT table loads. */
struct spinlock fat16_table_lock = {};
/** @brief Serializes callers updating individual FAT entries. */
struct spinlock fat16_set_entry_lock = {};
/** @brief Protects writes when flushing the FAT table back to disk. */
struct spinlock fat16_table_flush_lock = {};

/** @cond INTERNAL */
int fat16_resolve(struct disk *disk);
void *fat16_open(const struct path_root *path, FILE_MODE mode, enum INODE_TYPE *type_out, uint32_t *size_out);
int fat16_read(const void *descriptor, size_t size, off_t nmemb, char *out);
int fat16_write(const void *descriptor, const char *data, size_t size);
int fat16_seek(void *private, uint32_t offset, enum FILE_SEEK_MODE seek_mode);
int fat16_stat(void *descriptor, struct stat *stat);
int fat16_close(void *descriptor);
time_t fat_date_time_to_unix_time(uint16_t fat_date, uint16_t fat_time);
static int fat16_get_fat_entry(const struct disk *disk, int cluster);
int fat16_create_file(const char *path, void *data, int size);
int fat16_create_directory(const char *path);
int fat16_get_directory(const struct path_root *path_root, struct fat_directory *fat_directory);
int fat16_read_entry(struct file *descriptor, struct dir_entry *entry);
uint16_t fat16_allocate_new_entry(const struct disk *disk, uint16_t clusters_needed);
/** @endcond */

/** @brief VFS operations vector for FAT16 regular files. */
struct inode_operations fat16_file_inode_ops = {
    .open  = fat16_open,
    .read  = fat16_read,
    .write = fat16_write,
    .seek  = fat16_seek,
    .stat  = fat16_stat,
    .close = fat16_close,
};

/** @brief VFS operations vector for FAT16 directories. */
struct inode_operations fat16_directory_inode_ops = {
    .open       = fat16_open,
    .read       = fat16_read,
    .write      = fat16_write,
    .seek       = fat16_seek,
    .stat       = fat16_stat,
    .close      = fat16_close,
    .mkdir      = fat16_create_directory,
    .lookup     = memfs_lookup,
    .read_entry = fat16_read_entry,
};

/** @brief Global file system descriptor registered with the VFS. */
static struct file_system *fat16_fs = &fat16_fs_storage;

/**
 * @brief Allocate and initialize the FAT16 file system descriptor.
 *
 * Sets up synchronization primitives and registers the resolve handler used by the VFS.
 *
 * @return Pointer to the initialized file system descriptor.
 */
struct file_system *fat16_init()
{

    initlock(&fat16_table_lock, "fat16_table_lock");
    initlock(&fat16_set_entry_lock, "fat16_set_entry_lock");
    initlock(&fat16_table_flush_lock, "fat16_table_flush_lock");

    memset(fat16_fs, 0, sizeof(*fat16_fs));
    fat16_fs->type    = FS_TYPE_FAT16;
    fat16_fs->resolve = fat16_resolve;
    fat16_fs->ops     = &fat16_directory_inode_ops;

    strncpy(fat16_fs->name, "FAT16", 20);
    ASSERT(fat16_fs->resolve != nullptr, "Resolve function is null");

    return fat16_fs;
}

/**
 * @brief Check whether a directory reference corresponds to the volume root.
 *
 * @param directory Directory metadata under inspection.
 * @param fat_private FAT16 instance private data.
 * @return true when the directory matches the cached root directory.
 */
bool fat16_is_root_directory(const struct fat_directory *directory, const struct fat_private *fat_private)
{
    return directory->sector_position == fat_private->root_directory.sector_position;
}

/**
 * @brief Compute the first sector index that contains FAT table data.
 *
 * @param fat_private FAT16 instance private data.
 * @return Sector number where the first FAT copy begins.
 */
static uint16_t get_fat_start_sector(const struct fat_private *fat_private)
{
    return fat_private->header.primary_header.reserved_sectors;
}

/**
 * @brief Initialize per-mount FAT16 private state and backing streams.
 *
 * @param disk Disk device associated with the mount.
 * @param fat_private Instance storage to populate.
 */
static void fat16_init_private(const struct disk *disk, struct fat_private *fat_private)
{
    memset(&fat_private->header, 0, sizeof(struct fat_h));

    fat_private->cluster_read_stream  = disk_stream_create(disk->id);
    fat_private->cluster_write_stream = disk_stream_create(disk->id);
    fat_private->fat_read_stream      = disk_stream_create(disk->id);
    fat_private->fat_write_stream     = disk_stream_create(disk->id);
    fat_private->directory_stream     = disk_stream_create(disk->id);
}

/**
 * @brief Translate a cluster index into a sector offset on disk.
 *
 * @param fat_private FAT16 private state providing layout information.
 * @param cluster Cluster index from the FAT chain (>= 2).
 * @return Absolute sector number corresponding to the cluster start.
 */
static uint32_t fat16_cluster_to_sector(const struct fat_private *fat_private, const int cluster)
{
    return fat_private->root_directory.ending_sector_position +
        ((cluster - 2) * fat_private->header.primary_header.sectors_per_cluster);
}

/**
 * @brief Convert a sector number to its owning cluster index.
 *
 * @param fat_private FAT16 private state.
 * @param sector Sector number relative to the start of data region.
 * @return Cluster index containing the sector.
 */
static uint16_t fat16_sector_to_cluster(const struct fat_private *fat_private, const int sector)
{
    const int cluster_size = fat_private->header.primary_header.sectors_per_cluster;
    return sector / cluster_size;
}

/**
 * @brief Translate a sector number into a byte offset on disk.
 *
 * @param disk Target disk descriptor.
 * @param sector Sector number to convert.
 * @return Absolute byte address suitable for stream seeking.
 */
static int fat16_sector_to_absolute(const struct disk *disk, const int sector)
{
    return sector * disk->sector_size;
}

/**
 * @brief Load the primary FAT table from disk into memory.
 *
 * @param fat_private FAT16 private state containing layout metadata.
 * @return `ALL_OK` on success or a negative errno-style value on failure.
 */
int fat16_load_table(const struct fat_private *fat_private)
{
    const uint16_t first_fat_start_sector = fat_private->header.primary_header.reserved_sectors;
    const uint16_t sector_size            = fat_private->header.primary_header.bytes_per_sector;
    const uint16_t fat_sectors            = fat_private->header.primary_header.sectors_per_fat;

    const uint32_t required_bytes = (uint32_t)fat_sectors * sector_size;
    if (required_bytes > sizeof(fat_table)) {
        panic("FAT table exceeds static buffer\n");
        return -ENOMEM;
    }

    fat_table_length_bytes = required_bytes;

    acquire(&fat16_table_lock);

    for (uint16_t i = 0; i < fat_sectors; i++) {
        if (disk_read_sector(first_fat_start_sector + i, fat_table + (i * sector_size)) < 0) {
            panic("Failed to read FAT\n");
            return -EIO;
        }
    }

    release(&fat16_table_lock);
    return ALL_OK;
}

/**
 * @brief Persist the in-memory FAT table back to disk.
 *
 * @param fat_private FAT16 private state describing the target region.
 */
void fat16_flush_table(const struct fat_private *fat_private)
{
    ASSERT(fat_table_length_bytes > 0);

    const uint16_t start_sector = fat_private->header.primary_header.reserved_sectors;
    const uint16_t sector_size  = fat_private->header.primary_header.bytes_per_sector;
    const uint16_t fat_sectors  = fat_private->header.primary_header.sectors_per_fat;

    acquire(&fat16_table_flush_lock);

    // TODO: Flush all FATs, not just the first one

    for (uint16_t i = 0; i < fat_sectors; i++) {
        if (disk_write_sector(start_sector + i, fat_table + i * sector_size) < 0) {
            panic("Failed to write FAT table\n");
        }
    }

    release(&fat16_table_flush_lock);
}

/**
 * @brief Update a FAT chain entry and flush the change to disk.
 *
 * @param cluster Cluster index to update.
 * @param value New FAT value to store for the cluster.
 */
void fat16_set_fat_entry(const uint32_t cluster, const uint16_t value)
{
    const uint32_t fat_offset             = cluster * FAT16_FAT_ENTRY_SIZE;
    const struct disk *disk               = disk_get(0);
    const struct fat_private *fat_private = disk->fs_private;

    ASSERT(fat_table_length_bytes >= fat_offset + sizeof(uint16_t));

    acquire(&fat16_set_entry_lock);

    // Inefficient, but I don't care for now
    fat16_load_table(fat_private);
    *(uint16_t *)(fat_table + fat_offset) = value;
    fat16_flush_table(fat_private);

    release(&fat16_set_entry_lock);
}

/**
 * @brief Find and reserve the next free cluster in the FAT chain.
 *
 * @param disk Disk device to search.
 * @return Allocated cluster index or -1 if none are available.
 */
uint32_t fat16_get_free_cluster(const struct disk *disk)
{
    // First 2 (2 * FAT_ENTRY_SIZE) entries are reserved
    for (int i = 5; i < 65536; i++) {
        if (fat16_get_fat_entry(disk, i) == FAT16_FREE) {
            fat16_set_fat_entry(i, FAT16_EOC2);
            return i;
        }
    }

    return -1;
}

/**
 * @brief Count valid directory entries in a directory chain.
 *
 * @param disk Disk device hosting the directory.
 * @param directory_start_sector First sector of the directory region.
 * @return Number of valid entries or negative errno value on failure.
 */
int fat16_get_total_items_for_directory(const struct disk *disk, const uint32_t directory_start_sector)
{
    struct fat_directory_entry entry      = {};
    const struct fat_private *fat_private = disk->fs_private;

    int res   = 0;
    int count = 0;
    ASSERT(disk->sector_size > 0, "Invalid sector size");
    const uint32_t directory_start_pos = directory_start_sector * disk->sector_size;
    struct disk_stream stream          = fat_private->directory_stream;
    if (disk_stream_seek(&stream, directory_start_pos) != ALL_OK) {
        panic("Failed to seek to directory start");
        res = -EIO;
        goto out;
    }

    while (1) {
        if (disk_stream_read(&stream, &entry, sizeof(entry)) != ALL_OK) {
            panic("Failed to read directory entry");
            res = -EIO;
            goto out;
        }

        if (entry.name[0] == 0x00) {
            break;
        }

        if (entry.name[0] == 0xE5) {
            continue;
        }

        count++;
    }

    res = count;

out:
    return res;
}

/**
 * @brief Populate the cached root directory entries within the private state.
 *
 * @param disk Disk whose root directory should be loaded.
 * @return `ALL_OK` on success or negative errno value on failure.
 */
int fat16_load_root_directory(const struct disk *disk)
{
    ASSERT(disk->sector_size > 0, "Invalid sector size");

    int res = 0;

    struct fat_private *fat_private = disk->fs_private;
    struct fat_directory *directory = &fat_private->root_directory;

    const struct fat_header *primary_header = &fat_private->header.primary_header;
    const int root_dir_sector_pos =
        (primary_header->fat_copies * primary_header->sectors_per_fat) + primary_header->reserved_sectors;
    const int root_dir_entries   = fat_private->header.primary_header.root_entries;
    const uint32_t root_dir_size = root_dir_entries * sizeof(struct fat_directory_entry);

    const int total_items = fat16_get_total_items_for_directory(disk, root_dir_sector_pos);
    if (total_items < 0) {
        res = total_items;
        goto error_out;
    }

    if ((size_t)total_items > FAT16_MAX_ROOT_ENTRIES) {
        panic("Root directory exceeds static buffer\n");
        res = -ENOMEM;
        goto error_out;
    }

    struct fat_directory_entry *dir = fat_private->root_directory.entries;
    memset(dir, 0, sizeof(struct fat_directory_entry) * FAT16_MAX_ROOT_ENTRIES);

    struct disk_stream stream = fat_private->directory_stream;
    if (disk_stream_seek(&stream, fat16_sector_to_absolute(disk, root_dir_sector_pos)) != ALL_OK) {
        panic("Failed to seek to root directory\n");
        res = -EIO;
        goto error_out;
    }

    if (disk_stream_read(&stream, dir, root_dir_size) != ALL_OK) {
        panic("Failed to read root directory\n");
        res = -EIO;
        goto error_out;
    }

    directory->entries                = dir;
    directory->entry_count            = total_items;
    directory->sector_position        = root_dir_sector_pos;
    directory->ending_sector_position = root_dir_sector_pos +
        (int)((root_dir_size / disk->sector_size) ? (root_dir_size / disk->sector_size) - 1U : 0U);
    directory->pool_index = -1;

    return res;

error_out:
    return res;
}

/**
 * @brief Resolve a FAT16 volume and attach driver state to the disk.
 *
 * Reads the on-disk headers, validates the signature, and caches the root directory.
 *
 * @param disk Disk to initialize.
 * @return `ALL_OK` on success or a negative errno-compatible value.
 */
int fat16_resolve(struct disk *disk)
{
    int res = 0;

    struct fat_private *fat_private = &fat_private_storage;
    memset(fat_private, 0, sizeof(*fat_private));
    fat_private->root_directory.entries    = fat16_root_directory_entries;
    fat_private->root_directory.pool_index = -1;

    fat16_init_private(disk, fat_private);

    disk->fs_private = fat_private;
    disk->fs         = fat16_fs;

    struct disk_stream stream = disk_stream_create(disk->id);

    if (disk_stream_read(&stream, &fat_private->header, sizeof(fat_private->header)) != ALL_OK) {
        panic("Failed to read FAT16 header\n");
        res = -EIO;
        goto out;
    }

    if (fat_private->header.shared.extended_header.signature != 0x29) {
        warningf("Invalid FAT16 signature: %x\n", fat_private->header.shared.extended_header.signature);
        panic("File system not supported\n");

        res = -EFSNOTUS;
        goto out;
    }

    if (fat16_load_root_directory(disk) != ALL_OK) {
        panic("Failed to get root directory\n");
        res = -EIO;
        goto out;
    }

    // tests(disk);

    vfs_add_mount_point("/", disk->id, nullptr);

out:
    if (res < 0) {
        disk->fs_private = nullptr;
        disk->fs         = nullptr;
        memset(&fat_private_storage, 0, sizeof(fat_private_storage));
    }

    return res;
}

/**
 * @brief Copy a space-padded FAT string into a null-terminated buffer.
 *
 * @param out Pointer to the destination write cursor.
 * @param in Source FAT string (space padded).
 * @param size Maximum number of characters to copy.
 */
void fat16_get_null_terminated_string(char **out, const char *in, size_t size)
{
    size_t i = 0;
    while (*in != '\0' && *in != ' ') {
        **out = *in;
        *out += 1;
        in += 1;

        if (i >= size - 1) {
            break;
        }
        i++;
    }

    **out = 0x00;
}

/**
 * @brief Construct an 8.3 filename string from a directory entry.
 *
 * @param entry Directory entry to read.
 * @param out Destination buffer for the filename.
 * @param max_len Size of the destination buffer.
 */
void fat16_get_relative_filename(const struct fat_directory_entry *entry, char *out, const int max_len)
{
    memset(out, 0x00, max_len);
    char *out_tmp = out;
    fat16_get_null_terminated_string(&out_tmp, (const char *)entry->name, sizeof(entry->name));
    if (entry->ext[0] != 0x00 && entry->ext[0] != 0x20) {
        *out_tmp++ = '.';
        fat16_get_null_terminated_string(&out_tmp, (const char *)entry->ext, sizeof(entry->ext));
    }
}

/**
 * @brief Deep clone a FAT directory structure.
 *
 * @param directory Directory to duplicate.
 * @return Newly allocated directory copy or nullptr on failure.
 */
struct fat_directory fat16_clone_fat_directory(const struct fat_directory *directory)
{
    struct fat_directory new_directory = {
        .entries                = nullptr,
        .entry_count            = directory->entry_count,
        .sector_position        = directory->sector_position,
        .ending_sector_position = directory->ending_sector_position,
        .pool_index             = -1,
    };

    if (directory->entry_count <= 0) {
        return new_directory;
    }

    ASSERT(directory->entry_count <= (int)FAT16_MAX_DIRECTORY_ENTRIES, "Directory too large");
    const int slot           = fat16_acquire_directory_slot();
    new_directory.entries    = fat16_directory_entries_pool[slot];
    new_directory.pool_index = slot;
    memcpy(
        new_directory.entries, directory->entries, (size_t)directory->entry_count * sizeof(struct fat_directory_entry));

    return new_directory;
}

/**
 * @brief Clone a FAT directory entry into freshly allocated memory.
 *
 * @param entry Directory entry to copy.
 * @param size Size of the allocation to perform, must be >= entry size.
 * @return Pointer to the cloned entry or nullptr on failure.
 */
struct fat_directory_entry *fat16_clone_fat_directory_entry(const struct fat_directory_entry *entry, const size_t size)
{
    if (size < sizeof(struct fat_directory_entry)) {
        warningf("Invalid size for cloning directory entry\n");
        return nullptr;
    }

    struct fat_directory_entry *new_entry = fat16_entry_alloc();
    memcpy(new_entry, entry, sizeof(struct fat_directory_entry));

    return new_entry;
}


/**
 * @brief Read a FAT entry for a given cluster.
 *
 * @param disk Disk hosting the FAT.
 * @param cluster Cluster index to query.
 * @return FAT entry value or negative errno on failure.
 */
static int fat16_get_fat_entry(const struct disk *disk, const int cluster)
{
    uint16_t result = ALL_OK;

    const struct fat_private *fs_private = disk->fs_private;

    const uint32_t fat_offset = cluster * 2;
    const uint32_t fat_sector = fs_private->header.primary_header.reserved_sectors +
        (fat_offset / fs_private->header.primary_header.bytes_per_sector);
    const uint32_t fat_entry_offset = fat_offset % fs_private->header.primary_header.bytes_per_sector;

    uint8_t buffer[512];

    int res = disk_read_sector(fat_sector, buffer);
    result  = *(uint16_t *)&buffer[fat_entry_offset];
    if (res < 0) {
        warningf("Failed to read FAT table\n");
        goto out;
    }

    res = result;

out:
    return res;
}

/**
 * @brief Walk a cluster chain until reaching the cluster owning an offset.
 *
 * @param disk Target disk.
 * @param start_cluster First cluster of the file.
 * @param offset Byte offset into the file.
 * @param cache Optional lookup cache for FAT entries.
 * @return Cluster index satisfying the offset or negative errno on failure.
 */
static int fat16_get_cluster_for_offset(const struct disk *disk, const int start_cluster, const uint32_t offset,
                                        hash_table_t *cache)
{
    int res = ALL_OK;

    const struct fat_private *fs_private = disk->fs_private;
    const int size_of_cluster            = fs_private->header.primary_header.sectors_per_cluster * disk->sector_size;

    int cluster_to_use            = start_cluster;
    const uint32_t clusters_ahead = offset / size_of_cluster;

    for (uint32_t i = 0; i < clusters_ahead; i++) {
        int entry;
        auto cached_entry = (int)ht_get(cache, cluster_to_use);
        if (cached_entry) {
            entry = cached_entry;
        } else {
            entry = fat16_get_fat_entry(disk, cluster_to_use);
            ht_set(cache, cluster_to_use, (void *)entry);
        }

        // - 0xFFF8 to 0xFFFF: These values are reserved to mark the end of a cluster chain.
        // When you encounter any value in this range in the FAT (File Allocation Table), it
        // signifies that the current cluster is the last cluster of the file.
        // This means there are no more clusters to read for this file.
        if (entry >= 0xFFF8) {
            res = -FAT_EOC;
            goto out;
        }

        if (entry == FAT16_FAT_BAD_SECTOR) {
            res = -EIO;
            goto out;
        }

        if (entry >= 0xFFF0) {
            res = -EIO;
            goto out;
        }

        if (entry == FAT16_FREE) {
            res = -EIO;
            goto out;
        }

        cluster_to_use = entry;
    }

    res = cluster_to_use;

out:
    return res;
}

/**
 * @brief Recursively read file data spanning multiple clusters.
 *
 * @param disk Disk descriptor.
 * @param cluster First cluster of the file.
 * @param offset Byte offset within the file.
 * @param total Total number of bytes to read.
 * @param out Destination buffer.
 * @param cache FAT entry cache accelerated by the caller.
 * @return `ALL_OK` on success or negative errno code.
 */
static int fat16_read_internal(const struct disk *disk, const int cluster, const uint32_t offset, uint32_t total,
                               void *out, hash_table_t *cache)
{
    int res = 0;

    const struct fat_private *private    = disk->fs_private;
    struct disk_stream stream            = private->cluster_read_stream;
    const uint32_t size_of_cluster_bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
    const int cluster_to_use             = fat16_get_cluster_for_offset(disk, cluster, offset, cache);

    if (cluster_to_use == -FAT_EOC) {
        // We tried to read beyond the end of the file
        res = -FAT_EOC;
        goto out;
    }

    if (cluster_to_use < 0) {
        res = cluster_to_use;
        goto out;
    }

    const uint32_t offset_from_cluster = offset % size_of_cluster_bytes;

    const uint32_t starting_sector = fat16_cluster_to_sector(private, cluster_to_use);
    const uint32_t starting_pos    = (starting_sector * disk->sector_size) + offset_from_cluster;
    const uint32_t total_to_read   = total > size_of_cluster_bytes ? size_of_cluster_bytes : total;

    res = disk_stream_seek(&stream, starting_pos);
    if (res != ALL_OK) {
        goto out;
    }

    res = disk_stream_read(&stream, out, total_to_read);
    if (res != ALL_OK) {
        goto out;
    }

    total -= total_to_read;
    if (total > 0) {
        // We still have more to read
        res = fat16_read_internal(disk, cluster, offset + total_to_read, total, (char *)out + total_to_read, cache);
    }

out:
    return res;
}

/**
 * @brief Release memory held by a directory snapshot.
 *
 * @param directory Directory instance to destroy.
 */
void fat16_free_directory(struct fat_directory directory)
{
    if (directory.pool_index >= 0) {
        fat16_release_directory_slot(directory.pool_index);
    }
}

/**
 * @brief Release a fat_item wrapper and any associated resources.
 *
 * @param item Item to destroy.
 */
void fat16_fat_item_free(struct fat_item *item)
{
    if (!item) {
        return;
    }

    if (item->type == FAT_ITEM_TYPE_DIRECTORY) {
        fat16_free_directory(item->directory);
    } else if (item->type == FAT_ITEM_TYPE_FILE) {
        item->directory = (struct fat_directory){};
    }

    if (item->owns_item_entry && item->item) {
        fat16_entry_release(item->item);
    }

    item->item            = nullptr;
    item->owns_item_entry = false;

    fat16_item_release(item);
}

/**
 * @brief Load an on-disk directory into memory.
 *
 * @param disk Backing disk device.
 * @param entry Directory entry describing the subdirectory.
 * @return Allocated directory snapshot or nullptr on failure.
 */
struct fat_directory fat16_load_fat_directory(const struct disk *disk, const struct fat_directory_entry *entry)
{
    int res = 0;

    struct fat_directory directory = {
        .entries     = nullptr,
        .entry_count = 0,
        .pool_index  = -1,
    };
    const struct fat_private *fat_private = disk->fs_private;
    if (!(entry->attributes & FAT_FILE_SUBDIRECTORY)) {
        warningf("Invalid directory entry\n");
        res = -EINVARG;
        goto out;
    }

    const int cluster             = entry->first_cluster;
    const uint32_t cluster_sector = fat16_cluster_to_sector(fat_private, cluster);
    const int total_items         = fat16_get_total_items_for_directory(disk, cluster_sector);
    directory.entry_count         = total_items;
    const size_t directory_size   = (size_t)directory.entry_count * sizeof(struct fat_directory_entry);
    if (directory_size == 0) {
        goto out;
    }

    if (directory.entry_count > (int)FAT16_MAX_DIRECTORY_ENTRIES) {
        panic("Directory too large for FAT16 pool\n");
        res = -ENOMEM;
        goto out;
    }

    const int slot       = fat16_acquire_directory_slot();
    directory.entries    = fat16_directory_entries_pool[slot];
    directory.pool_index = slot;

    hash_table_t *cache = ht_create();
    res                 = fat16_read_internal(disk, cluster, 0x00, (int)directory_size, directory.entries, cache);
    ht_destroy(cache);
    if (res != ALL_OK) {
        warningf("Failed to read directory entries\n");
        fat16_release_directory_slot(slot);
        directory.entries    = nullptr;
        directory.pool_index = -1;
        goto out;
    }

    directory.sector_position        = cluster_sector;
    const uint32_t sectors_covered   = (uint32_t)((directory_size + disk->sector_size - 1U) / disk->sector_size);
    directory.ending_sector_position = cluster_sector + (int)(sectors_covered ? sectors_covered - 1U : 0U);

out:
    if (res != ALL_OK) {
        directory.entry_count = -1;
    }
    return directory;
}

/**
 * @brief Create a fat_item wrapper for a directory snapshot.
 *
 * @param dir Directory to wrap.
 * @return Newly allocated fat_item or nullptr on failure.
 */
struct fat_item *fat16_new_fat_item_for_directory(const struct fat_directory *dir)
{
    struct fat_item *f_item = fat16_item_alloc();
    if (!f_item) {
        return nullptr;
    }

    f_item->directory = fat16_clone_fat_directory(dir);
    if (dir->entry_count > 0 && f_item->directory.entries == nullptr) {
        fat16_item_release(f_item);
        return nullptr;
    }

    f_item->type = FAT_ITEM_TYPE_DIRECTORY;
    return f_item;
}

/**
 * @brief Create a fat_item wrapper for a directory entry.
 *
 * @param disk Disk hosting the entry.
 * @param entry FAT directory entry to wrap.
 * @return fat_item containing either a file or directory representation.
 */
struct fat_item *fat16_new_fat_item_for_directory_entry(const struct disk *disk,
                                                        const struct fat_directory_entry *entry)
{
    struct fat_item *f_item = fat16_item_alloc();
    if (!f_item) {
        return nullptr;
    }

    f_item->item = fat16_clone_fat_directory_entry(entry, sizeof(struct fat_directory_entry));
    if (!f_item->item) {
        fat16_item_release(f_item);
        return nullptr;
    }
    f_item->owns_item_entry = true;

    if (entry->attributes & FAT_FILE_SUBDIRECTORY) {
        f_item->type      = FAT_ITEM_TYPE_DIRECTORY;
        f_item->directory = fat16_load_fat_directory(disk, entry);
        if (f_item->directory.entry_count < 0) {
            fat16_entry_release(f_item->item);
            fat16_item_release(f_item);
            return nullptr;
        }
        return f_item;
    }

    f_item->type            = FAT_ITEM_TYPE_FILE;
    f_item->directory       = (struct fat_directory){.pool_index = -1};
    f_item->owns_item_entry = true;
    return f_item;
}

/**
 * @brief Locate an entry within a directory by name.
 *
 * @param disk Disk descriptor.
 * @param directory Directory to inspect.
 * @param name Null-terminated 8.3 name to match.
 * @return fat_item representing the entry or nullptr when not found.
 */
struct fat_item *fat16_find_item_in_directory(const struct disk *disk, const struct fat_directory *directory,
                                              const char *name)
{
    struct fat_item *f_item = {};
    for (int i = 0; i < directory->entry_count; i++) {
        char tmp_filename[MAX_PATH_LENGTH] = {0};
        fat16_get_relative_filename(&directory->entries[i], tmp_filename, sizeof(tmp_filename));
        if (istrncmp(tmp_filename, name, sizeof(tmp_filename)) == 0) {
            return fat16_new_fat_item_for_directory_entry(disk, &directory->entries[i]);
        }
    }

    return f_item;
}

/**
 * @brief Resolve a path into a FAT item by traversing directory entries.
 *
 * @param disk Disk hosting the file system.
 * @param path Linked list describing the path components.
 * @return fat_item for the final component or nullptr on failure.
 */
struct fat_item *fat16_get_directory_entry(const struct disk *disk, const struct path_part *path)
{
    dbgprintf("Getting directory entry for: %s\n", path->name);
    const struct fat_private *fat_private = disk->fs_private;
    struct fat_item *current_item         = {};
    struct fat_item *root_item = fat16_find_item_in_directory(disk, &fat_private->root_directory, path->name);

    if (!root_item) {
        warningf("Failed to find item: %s\n", path->name);
        goto out;
    }

    const struct path_part *next_part = path->next;
    current_item                      = root_item;
    while (next_part != nullptr) {
        if (current_item->type != FAT_ITEM_TYPE_DIRECTORY) {
            fat16_fat_item_free(current_item);
            current_item = nullptr;
            break;
        }

        struct fat_item *tmp_item = fat16_find_item_in_directory(disk, &current_item->directory, next_part->name);
        fat16_fat_item_free(current_item);
        current_item = tmp_item;
        if (!current_item) {
            break;
        }
        next_part = next_part->next;
    }

out:
    return current_item;
}

/**
 * @brief Open a file or directory described by a path.
 *
 * Creates descriptors, handles optional creation, and returns metadata to the caller.
 *
 * @param path Parsed path structure.
 * @param mode Open mode flags (supports `O_CREAT`).
 * @param type_out Output parameter for resolved inode type.
 * @param size_out Output parameter for file size or directory entry count.
 * @return Opaque descriptor pointer or encoded error via `ERROR()` macro.
 */
void *fat16_open(const struct path_root *path, const FILE_MODE mode, enum INODE_TYPE *type_out, uint32_t *size_out)
{
    int error_code = 0;

    struct fat_file_descriptor *descriptor = fat16_fd_alloc();
    if (!descriptor) {
        panic("Failed to allocate memory for file descriptor\n");
        error_code = -ENOMEM;
        goto error_out;
    }

    struct disk *disk = disk_get(path->drive_number);

    if (path->first != nullptr) {
        descriptor->item = fat16_get_directory_entry(disk, path->first);
        if (!descriptor->item) {
            warningf("Failed to get directory entry\n");
            if (mode & O_CREAT) {
                char path_str[MAX_PATH_LENGTH] = {0};

                int res = path_parser_unparse(path, path_str, sizeof(path_str));
                if (res < 0) {
                    error_code = res;
                    goto error_out;
                }
                res = fat16_create_file(path_str, nullptr, 0);
                if (res < 0) {
                    error_code = res;
                    goto error_out;
                }
                descriptor->item = fat16_get_directory_entry(disk, path->first);
                if (!descriptor->item) {
                    error_code = -EIO;
                    goto error_out;
                }
                goto success_out;
            }
            error_code = -EIO;
            goto error_out;
        }
    } else {
        const struct fat_private *fat_private      = disk->fs_private;
        const struct fat_directory *root_directory = &fat_private->root_directory;
        descriptor->item                           = fat16_new_fat_item_for_directory(root_directory);
        if (!descriptor->item) {
            error_code = -ENOMEM;
            goto error_out;
        }
    }

success_out:
    if (!descriptor->item) {
        error_code = -EIO;
        goto error_out;
    }
    if (descriptor->item->type == FAT_ITEM_TYPE_FILE) {
        *type_out = INODE_FILE;
    } else if (descriptor->item->type == FAT_ITEM_TYPE_DIRECTORY) {
        *type_out = INODE_DIRECTORY;
    }

    descriptor->position = 0;
    descriptor->disk     = disk;
    *size_out            = descriptor->item->type == FAT_ITEM_TYPE_FILE ? descriptor->item->item->size
                                                                        : (uint32_t)descriptor->item->directory.entry_count;

    return descriptor;

error_out:
    if (descriptor) {
        fat16_free_file_descriptor(descriptor);
    }

    return ERROR(error_code);
}

/**
 * @brief Modify an existing directory entry in place.
 *
 * @param directory Parent directory metadata.
 * @param entry Entry snapshot to locate on disk.
 * @param new_name New 8-character name (space padded as needed).
 * @param new_ext Optional 3-character extension.
 * @param attributes Attribute bitmask to store.
 * @param file_size Updated file length in bytes.
 * @return `ALL_OK` on success or negative errno value.
 */
int fat16_change_entry(const struct fat_directory *directory, const struct fat_directory_entry *entry,
                       const char *new_name, const char *new_ext, const uint8_t attributes, const uint32_t file_size)
{
    uint8_t buffer[512]             = {0};
    const uint32_t first_dir_sector = directory->sector_position;
    const uint32_t last_dir_sector  = directory->ending_sector_position;

    char cur_fullname[12] = {0};
    fat16_get_relative_filename(entry, cur_fullname, sizeof(cur_fullname));

    for (uint32_t dir_sector = first_dir_sector; dir_sector <= last_dir_sector; dir_sector++) {
        if (disk_read_sector(dir_sector, buffer) < 0) {
            panic("Error reading block\n");
            return -1;
        }

        for (int i = 0; i < (int)FAT_ENTRIES_PER_SECTOR; i++) {
            auto const dir_entry = (struct fat_directory_entry *)(buffer + i * sizeof(struct fat_directory_entry));
            if (dir_entry->name[0] == 0x00 || dir_entry->name[0] == 0xE5) {
                continue;
            }

            // Find the existing entry to change
            char tmp_filename[MAX_PATH_LENGTH] = {0};
            fat16_get_relative_filename(dir_entry, tmp_filename, sizeof(tmp_filename));
            if (istrncmp(tmp_filename, cur_fullname, sizeof(tmp_filename)) == 0) {
                memset(dir_entry->name, ' ', 8);
                memcpy(dir_entry->name, new_name, 8);

                memset(dir_entry->ext, ' ', 3);
                if (new_ext != nullptr) {
                    memcpy(dir_entry->ext, new_ext, 3);
                }

                dir_entry->attributes = attributes;
                dir_entry->size       = file_size;

                disk_write_sector(dir_sector, buffer);
                const struct disk *disk               = disk_get(0);
                const struct fat_private *fat_private = disk->fs_private;
                if (fat16_is_root_directory(directory, fat_private)) {
                    fat16_load_root_directory(disk);
                }
                return ALL_OK;
            }
        }
    }
    return -EIO;
}

/**
 * @brief Insert a new directory entry representing a file or subdirectory.
 *
 * @param directory Parent directory metadata.
 * @param name 8-character name (space padded if shorter).
 * @param ext Optional 3-character extension.
 * @param attributes Attribute flags to assign.
 * @param file_cluster First cluster of the file content.
 * @param file_size File size in bytes.
 * @return `ALL_OK` on success or negative errno value when no slot is available.
 */
int fat16_add_entry(const struct fat_directory *directory, const char *name, const char *ext, const uint8_t attributes,
                    const uint16_t file_cluster, const uint32_t file_size)
{
    uint8_t buffer[512]             = {0};
    const uint32_t first_dir_sector = directory->sector_position;
    const uint32_t last_dir_sector  = directory->ending_sector_position;

    for (uint32_t dir_sector = first_dir_sector; dir_sector <= last_dir_sector; dir_sector++) {
        if (disk_read_sector(dir_sector, buffer) < 0) {
            panic("Error reading block\n");
            return -EIO;
        }

        for (int entry = 0; entry < (int)FAT_ENTRIES_PER_SECTOR; entry++) {
            auto const dir_entry = (struct fat_directory_entry *)(buffer + entry * sizeof(struct fat_directory_entry));
            // Find an empty entry to write the new file
            if (dir_entry->name[0] == 0x00 || dir_entry->name[0] == 0xE5) {
                memset(dir_entry, 0, sizeof(struct fat_directory_entry));
                memset(dir_entry->name, ' ', 8);
                memcpy(dir_entry->name, name, 8);
                if (ext != nullptr) {
                    memset(dir_entry->ext, ' ', 3);
                    memcpy(dir_entry->ext, ext, 3);
                }
                dir_entry->attributes    = attributes;
                dir_entry->first_cluster = file_cluster;
                dir_entry->size          = file_size;

                disk_write_sector(dir_sector, buffer);
                return ALL_OK;
            }
        }
    }
    return -EIO;
}

/**
 * @brief Write contiguous data across a FAT cluster chain.
 *
 * @param data Buffer containing the bytes to write.
 * @param starting_cluster First cluster of the file.
 * @param size Number of bytes to persist.
 */
void fat16_write_data_to_clusters(uint8_t *data, const uint16_t starting_cluster, const uint32_t size)
{
    const struct disk *disk               = disk_get(0);
    const struct fat_private *fat_private = disk->fs_private;
    const uint16_t bytes_per_sector       = fat_private->header.primary_header.bytes_per_sector;
    const uint8_t sectors_per_cluster     = fat_private->header.primary_header.sectors_per_cluster;
    const uint32_t bytes_per_cluster      = bytes_per_sector * sectors_per_cluster;

    uint16_t current_cluster = starting_cluster;
    uint32_t data_offset     = 0;

    while (current_cluster < FAT16_EOC && data_offset < size) {
        const uint32_t first_cluster_sector = fat16_cluster_to_sector(fat_private, current_cluster);
        uint32_t bytes_to_write             = bytes_per_cluster;
        if ((size - data_offset) < bytes_per_cluster) {
            bytes_to_write = size - data_offset;
        }

        // Write an entire cluster
        disk_write_block(first_cluster_sector, sectors_per_cluster, (void *)(data + data_offset));
        data_offset += bytes_to_write;

        // Get the next cluster in the chain
        const uint16_t next_cluster = fat16_get_fat_entry(disk, current_cluster);

        // If we reached the end of the chain, but we need more space, allocate a new cluster
        // TODO: Allocate all the clusters needed at once
        if (next_cluster >= FAT16_EOC && data_offset < size) {
            const uint16_t new_cluster = fat16_allocate_new_entry(disk, 1);
            fat16_set_fat_entry(current_cluster, new_cluster);
            current_cluster = new_cluster;
        } else {
            current_cluster = next_cluster;
        }
    }
}

/**
 * @brief Allocate a chain of clusters and link them together.
 *
 * @param disk Disk against which to allocate clusters.
 * @param clusters_needed Number of clusters to reserve.
 * @return First cluster in the allocated chain or negative errno on failure.
 */
uint16_t fat16_allocate_new_entry(const struct disk *disk, const uint16_t clusters_needed)
{
    uint16_t previous_cluster = 0;
    uint16_t first_cluster    = 0;

    for (uint32_t i = 0; i < clusters_needed; i++) {
        const uint16_t next_cluster = (int)fat16_get_free_cluster(disk);
        if (next_cluster > FAT16_EOC) {
            panic("No free cluster found\n");
            return -EIO;
        }

        if (previous_cluster != 0) {
            // Link the next cluster in the chain
            fat16_set_fat_entry(previous_cluster, next_cluster);
        } else {
            // This is the first cluster in the chain
            first_cluster = next_cluster;
        }

        previous_cluster = next_cluster;
    }

    return first_cluster;
}

/**
 * @brief Debug helper that prints the cluster chain of a file.
 *
 * @param disk Disk descriptor.
 * @param first_cluster Starting cluster.
 * @param name File name component.
 * @param ext File extension component.
 */
void debug_print_fat_chain(const struct disk *disk, const uint16_t first_cluster, const char *name, const char *ext)
{
    printf("Chain for file %s.%s\n", name, ext);
    printf("Cluster: %d\n", first_cluster);
    int next_cluster = fat16_get_fat_entry(disk, first_cluster);
    while (next_cluster < FAT16_EOC) {
        printf("Cluster: %d\n", next_cluster);
        next_cluster = fat16_get_fat_entry(disk, next_cluster);
    }
}

/**
 * @brief Initialize `.` and `..` entries within a newly allocated directory cluster.
 *
 * @param disk Disk descriptor.
 * @param cluster Cluster containing the new directory data region.
 * @param parent_cluster Cluster index of the parent directory.
 * @param current_cluster Cluster index of the new directory.
 */
void fat16_initialize_directory(const struct disk *disk, const uint16_t cluster, const uint16_t parent_cluster,
                                const uint16_t current_cluster)
{
    uint8_t buffer[512] = {0};

    auto const dot_entry = (struct fat_directory_entry *)buffer;
    memset(dot_entry, 0, sizeof(struct fat_directory_entry));
    memset(dot_entry->name, ' ', 11);
    dot_entry->name[0]       = '.';
    dot_entry->attributes    = FAT_FILE_SUBDIRECTORY;
    dot_entry->first_cluster = current_cluster;
    dot_entry->size          = 0;

    auto const dotdot_entry = (struct fat_directory_entry *)(buffer + sizeof(struct fat_directory_entry));
    memset(dotdot_entry, 0, sizeof(struct fat_directory_entry));
    memset(dotdot_entry->name, ' ', 11);
    dotdot_entry->name[0]       = '.';
    dotdot_entry->name[1]       = '.';
    dotdot_entry->attributes    = FAT_FILE_SUBDIRECTORY;
    dotdot_entry->first_cluster = parent_cluster;
    dotdot_entry->size          = 0;

    const struct fat_private *fat_private = disk->fs_private;
    const uint32_t sector                 = fat16_cluster_to_sector(fat_private, cluster);

    disk_write_sector(sector, buffer);
}

/**
 * @brief Create a new directory at the provided path.
 *
 * Allocates the necessary cluster, seeds `.` and `..`, and links the entry into its parent directory.
 *
 * @param path Absolute path string pointing to the directory to create.
 * @retval ALL_OK Directory created successfully.
 * @retval -errno Negative errno code on parsing, allocation, or disk failure.
 */
int fat16_create_directory(const char *path)
{
    const struct path_root *root = path_parser_parse(path);
    const struct disk *disk      = disk_get(root->drive_number);
    const uint16_t first_cluster = fat16_allocate_new_entry(disk, 1);

    struct fat_directory parent_dir = {.pool_index = -1};
    fat16_get_directory(root, &parent_dir);
    const struct path_part *dir_part = path_parser_get_last_part(root);
    fat16_add_entry(&parent_dir, dir_part->name, nullptr, FAT_FILE_SUBDIRECTORY, first_cluster, 0);

    const struct fat_private *fat_private = disk->fs_private;
    const uint16_t parent_cluster         = fat16_sector_to_cluster(fat_private, parent_dir.sector_position);
    // Initialize the new directory with '.' and '..' entries
    fat16_initialize_directory(disk, first_cluster, parent_cluster, first_cluster);

    // // Reload the root directory if the directory was created in the root directory
    if (fat16_is_root_directory(&parent_dir, fat_private)) {
        fat16_load_root_directory(disk);
    }

    fat16_free_directory(parent_dir);

    return 0;
}

/**
 * @brief Create a new file and optionally populate its contents.
 *
 * Allocates sufficient clusters, inserts the directory entry, and writes initial data if provided.
 *
 * @param path Absolute path string for the file.
 * @param data Optional data buffer to write (may be NULL when `size` is zero).
 * @param size Number of bytes to write from `data`.
 * @retval ALL_OK File created successfully.
 * @retval -errno Negative errno from path resolution, allocation, or disk I/O.
 */
int fat16_create_file(const char *path, void *data, const int size)
{
    const struct path_root *path_root = path_parser_parse(path);

    const struct disk *disk               = disk_get(path_root->drive_number);
    const struct fat_private *fat_private = disk->fs_private;

    const uint16_t bytes_per_sector   = fat_private->header.primary_header.bytes_per_sector;
    const uint8_t sectors_per_cluster = fat_private->header.primary_header.sectors_per_cluster;
    const uint32_t bytes_per_cluster  = bytes_per_sector * sectors_per_cluster;

    uint16_t clusters_needed     = (size + bytes_per_cluster - 1) / bytes_per_cluster;
    clusters_needed              = clusters_needed == 0 ? 1 : clusters_needed; // At least one cluster
    const uint16_t first_cluster = fat16_allocate_new_entry(disk, clusters_needed);

    struct fat_directory parent_dir = {.pool_index = -1};
    int res                         = fat16_get_directory(path_root, &parent_dir);
    if (res < 0) {
        return res;
    }

    char file_name[12]                = {0};
    const struct path_part *file_part = path_parser_get_last_part(path_root);
    memcpy(file_name, file_part->name, strlen(file_part->name));

    const char *name = strtok(file_name, ".");
    const char *ext  = strtok(nullptr, ".");

    res = fat16_add_entry(&parent_dir, name, ext, FAT_FILE_ARCHIVE, first_cluster, size);
    if (res < 0) {
        return res;
    }

    if (size > 0 && data != nullptr) {
        fat16_write_data_to_clusters(data, first_cluster, size);
    }

    fat16_flush_table(fat_private);

    // Reload the root directory if the file was created in the root directory
    if (fat16_is_root_directory(&parent_dir, fat_private)) {
        fat16_load_root_directory(disk);
    }

    fat16_free_directory(parent_dir);

    return ALL_OK;
}

/**
 * @brief Write data to an open FAT16 file descriptor.
 *
 * Extends the file if necessary, merges the new payload at the current position, and flushes the data to disk.
 *
 * @param descriptor File descriptor returned by `fat16_open`.
 * @param data Buffer containing bytes to write.
 * @param size Number of bytes to write.
 * @retval ALL_OK Data written successfully.
 * @retval -errno Negative errno when allocation, parsing, or disk access fails.
 */
int fat16_write(const void *descriptor, const char *data, const size_t size)
{
    // TODO: lock
    // TODO: use stream to write data
    const struct file *desc              = descriptor;
    struct fat_file_descriptor *fat_desc = desc->fs_data;
    struct fat_directory_entry *entry    = fat_desc->item->item;

    if (entry->size - fat_desc->position < size) {
        entry->size = fat_desc->position + size;
    }

    // Save the current write position
    const uint32_t write_position = fat_desc->position;
    // Set the position to the beginning of the file so we can read the existing data
    fat_desc->position = 0;

    char existing_data[entry->size + 1] = {};
    fat16_read(descriptor, entry->size, 1, (char *)existing_data);
    // Restore the write position
    fat_desc->position = write_position;
    memcpy(existing_data + fat_desc->position, data, size);

    const struct path_root *path_root = path_parser_parse(desc->path);
    struct fat_directory directory    = {.pool_index = -1};
    fat16_get_directory(path_root, &directory);

    // Update the entry with the new size
    fat16_change_entry(&directory, entry, (char *)entry->name, (char *)entry->ext, entry->attributes, entry->size);

    // Write the file's content
    fat16_write_data_to_clusters((uint8_t *)existing_data, entry->first_cluster, entry->size);
    fat_desc->position = entry->size - 1;

    return ALL_OK;
}

/**
 * @brief Read data from an open FAT16 file descriptor.
 *
 * Traverses the cluster chain to copy `size * nmemb` bytes starting at the current file offset.
 *
 * @param descriptor File descriptor returned by `fat16_open`.
 * @param size Size of each element to read.
 * @param nmemb Number of elements to read.
 * @param out Destination buffer that receives the data.
 * @retval >=0 Total number of bytes read.
 * @retval -errno Negative errno value when the read operation fails.
 */
int fat16_read(const void *descriptor, const size_t size, const off_t nmemb, char *out)
{
    int res = 0;

    auto const desc                         = (struct file *)descriptor;
    struct fat_file_descriptor *fat_desc    = desc->fs_data;
    const struct fat_directory_entry *entry = fat_desc->item->item;
    const struct disk *disk                 = fat_desc->disk;
    uint32_t offset                         = fat_desc->position;

    hash_table_t *cache = ht_create();

    for (off_t i = 0; i < nmemb; i++) {
        res = fat16_read_internal(disk, entry->first_cluster, offset, size, out, cache);
        if (res == -FAT_EOC) {
            // Reached the end of the file
            return 0;
        }
        if (ISERR(res)) {
            warningf("Failed to read from file\n");
            ht_destroy(cache);
            return res;
        }


        out += size;
        offset += size;
    }

    ht_destroy(cache);

    res = (int)nmemb * (int)size;

    fat_desc->position += res;

    return res;
}

/**
 * @brief Adjust the file offset for an open descriptor.
 *
 * Validates bounds and updates the descriptor cursor relative to the requested origin.
 *
 * @param private File structure pointer passed by the VFS.
 * @param offset Offset relative to the selected origin.
 * @param seek_mode One of `SEEK_SET`, `SEEK_CURRENT`, `SEEK_END`.
 * @retval ALL_OK Seek succeeded.
 * @retval -errno Negative errno when the requested move is invalid or unsupported.
 */
int fat16_seek(void *private, const uint32_t offset, const enum FILE_SEEK_MODE seek_mode)
{
    int res = 0;

    auto const desc                      = (struct file *)private;
    struct fat_file_descriptor *fat_desc = desc->fs_data;
    const struct fat_item *desc_item     = fat_desc->item;
    if (desc_item->type != FAT_ITEM_TYPE_FILE) {
        warningf("Invalid file descriptor\n");
        res = -EINVARG;
        goto out;
    }

    const struct fat_directory_entry *entry = desc_item->item;
    if (offset > entry->size) {
        warningf("Offset exceeds file size\n");
        res = -EIO;
        goto out;
    }

    switch (seek_mode) {
    case SEEK_SET:
        fat_desc->position = offset;
        break;
    case SEEK_CURRENT:
        fat_desc->position += offset;
        break;
    case SEEK_END:
        panic("SEEK_END not implemented\n");
        res = -EUNIMP;
        break;
    default:
        panic("Invalid seek mode\n");
        res = -EINVARG;
        break;
    }

out:
    return res;
}

/**
 * @brief Populate POSIX-like metadata for a FAT item.
 *
 * Fills size, mode bits, timestamps, and long filename indicators into the provided `stat` buffer.
 *
 * @param descriptor File descriptor returned by `fat16_open`.
 * @param stat Output structure to fill.
 * @retval ALL_OK Metadata populated successfully.
 */
int fat16_stat(void *descriptor, struct stat *stat)
{
    auto const desc                  = (struct file *)descriptor;
    auto const fat_desc              = (struct fat_file_descriptor *)desc->fs_data;
    const struct fat_item *desc_item = fat_desc->item;

    stat->st_lfn  = false;
    stat->st_mode = 0;

    const struct fat_directory_entry *entry = nullptr;
    if (desc_item->item) {
        entry = desc_item->item;
    }
    if (desc_item->type == FAT_ITEM_TYPE_FILE) {

        stat->st_size = entry->size;
        stat->st_mode |= S_IRUSR;
        stat->st_mode |= S_IRGRP;
        stat->st_mode |= S_IROTH;

        stat->st_mode |= S_IXUSR;
        stat->st_mode |= S_IXGRP;
        stat->st_mode |= S_IXOTH;

        stat->st_mode |= S_IFREG;

        if (entry->attributes == FAT_FILE_LONG_NAME) {
            stat->st_lfn = true;
        }
    } else if (desc_item->type == FAT_ITEM_TYPE_DIRECTORY) {
        const struct fat_directory directory = desc_item->directory;

        stat->st_size = directory.entry_count;
        stat->st_mode |= S_IFDIR;

        stat->st_mode |= S_IRUSR;
        stat->st_mode |= S_IRGRP;
        stat->st_mode |= S_IROTH;
    }

    if (entry) {
        stat->st_mtime = fat_date_time_to_unix_time(entry->modification_date, entry->modification_time);

        if (!(entry->attributes & FAT_FILE_READ_ONLY)) {
            stat->st_mode |= S_IWUSR;
            stat->st_mode |= S_IWGRP;
            stat->st_mode |= S_IWOTH;
        }
    }

    return ALL_OK;
}

/**
 * @brief Tear down a FAT16 file descriptor and release associated resources.
 *
 * Frees the associated fat_item (file or directory snapshot) and the descriptor allocation.
 *
 * @param descriptor Descriptor to free; ignored when NULL.
 */
static void fat16_free_file_descriptor(struct fat_file_descriptor *descriptor)
{
    if (!descriptor) {
        return;
    }

    fat16_fat_item_free(descriptor->item);
    descriptor->item = nullptr;
    fat16_fd_release(descriptor);
}

/**
 * @brief Close a FAT16-backed file descriptor.
 *
 * Invoked by the VFS to release driver-specific resources.
 *
 * @param descriptor File structure pointer provided by the VFS.
 * @retval ALL_OK Descriptor resources released.
 */
int fat16_close(void *descriptor)
{
    auto const desc = (struct file *)descriptor;
    fat16_free_file_descriptor(desc->fs_data);
    return ALL_OK;
}

/**
 * @brief Resolve a path into a directory snapshot.
 *
 * Walks the FAT directory hierarchy, cloning directory entries so the caller owns the returned snapshot.
 *
 * @param path_root Parsed path structure.
 * @param fat_directory Output directory snapshot to populate.
 * @retval ALL_OK Directory snapshot produced.
 * @retval -ENOENT Requested directory could not be located.
 */
int fat16_get_directory(const struct path_root *path_root, struct fat_directory *fat_directory)
{
    auto const disk                       = disk_get(path_root->drive_number);
    const struct fat_private *fat_private = disk->fs_private;

    // If the first part of the path is null, then we are looking for the root directory
    if (path_root->first == nullptr) {
        fat16_load_root_directory(disk);
        const struct fat_directory *root_directory = &fat_private->root_directory;
        *fat_directory                             = *root_directory;
        return ALL_OK;
    }

    auto path_part                = path_root->first;
    struct fat_item *current_item = fat16_find_item_in_directory(disk, &fat_private->root_directory, path_part->name);
    // If the first part of the path is not found in the root directory, then we also return the root directory
    if (current_item == nullptr || current_item->type == FAT_ITEM_TYPE_FILE) {
        fat16_load_root_directory(disk);
        const struct fat_directory *root_directory = &fat_private->root_directory;
        *fat_directory                             = *root_directory;
        return ALL_OK;
    }
    path_part = path_part->next;

    while (path_part != nullptr && current_item != nullptr) {
        struct fat_item *next_item = fat16_find_item_in_directory(disk, &current_item->directory, path_part->name);
        if (next_item == nullptr || next_item->type != FAT_ITEM_TYPE_DIRECTORY) {
            fat16_fat_item_free(current_item);
            break;
        }
        fat16_fat_item_free(current_item);
        current_item = next_item;
        path_part    = path_part->next;
    }

    if (current_item == nullptr) {
        return -ENOENT;
    }

    ASSERT(current_item);

    const struct fat_directory result = current_item->directory;
    *fat_directory                    = result;

    // Detach directory ownership before releasing the fat_item wrapper.
    current_item->directory.entries     = nullptr;
    current_item->directory.entry_count = 0;
    current_item->directory.pool_index  = -1;
    fat16_fat_item_free(current_item);

    return ALL_OK;
}

/**
 * @brief Convert FAT date/time fields to Unix epoch seconds.
 *
 * Expands the packed FAT date and time fields and converts them via `mktime` to a POSIX timestamp.
 *
 * @param fat_date FAT date bitfield.
 * @param fat_time FAT time bitfield.
 * @return Unix timestamp representing the given FAT time.
 */
time_t fat_date_time_to_unix_time(const uint16_t fat_date, const uint16_t fat_time)
{
    const int day   = fat_date & 0x1F;
    const int month = (fat_date >> 5) & 0x0F;
    const int year  = ((fat_date >> 9) & 0x7F) + 1980;

    const int second = (fat_time & 0x1F) * 2;
    const int minute = (fat_time >> 5) & 0x3F;
    const int hour   = (fat_time >> 11) & 0x1F;

    struct tm t = {0};
    t.tm_year   = year - 1900; // tm_year is years since 1900
    t.tm_mon    = month - 1;   // tm_mon is months since January (0-11)
    t.tm_mday   = day;
    t.tm_hour   = hour;
    t.tm_min    = minute;
    t.tm_sec    = second;
    t.tm_isdst  = -1; // Let mktime determine whether DST is in effect

    const time_t unix_time = mktime(&t);

    return unix_time;
}

/**
 * @brief Translate a FAT directory entry into a VFS `dir_entry` record.
 *
 * Generates an inode number, lowercases and trims the 8.3 filename, and stores the result in the caller buffer.
 *
 * @param fat_entry Entry to convert.
 * @param index Index within the directory, used for inode numbering.
 * @param entry Output directory entry structure.
 * @retval ALL_OK Entry converted successfully.
 * @retval -ENOMEM Temporary allocation failed while formatting the name.
 */
int fat16_read_file_dir_entry(const struct fat_directory_entry *fat_entry, const size_t index, struct dir_entry *entry)
{
    char full_name[13] = {0};
    char name_buf[9]   = {0};
    char ext_buf[4]    = {0};

    memset(entry, 0, sizeof(struct dir_entry));

    const uint32_t cluster = ((uint32_t)fat_entry->cluster_high << 16) | fat_entry->first_cluster;
    entry->inode_number    = (unsigned long)(((uint64_t)cluster << 16) | (index & 0xFFFFu));

    memcpy(name_buf, fat_entry->name, 8);
    memcpy(ext_buf, fat_entry->ext, 3);
    char *name = trim(name_buf, sizeof(name_buf));
    char *ext  = trim(ext_buf, sizeof(ext_buf));

    for (size_t i = 0; i < strlen(name); i++) {
        name[i] = tolower(name[i]);
    }

    for (size_t i = 0; i < strlen(ext); i++) {
        ext[i] = tolower(ext[i]);
    }

    if (strlen(ext) > 0) {
        strcat(full_name, name);
        strcat(full_name, ".");
        strcat(full_name, ext);
    } else {
        strcat(full_name, name);
    }

    const size_t full_name_length = strlen(full_name);
    memcpy(entry->name, full_name, full_name_length);
    entry->name_length = full_name_length;

    return ALL_OK;
}

// ! This is supposed to read the NEXT directory entry
/**
 * @brief Iterate a directory descriptor and return the next entry.
 *
 * Converts the cached FAT directory entry into a portable `dir_entry` and advances the descriptor offset.
 *
 * @param descriptor Directory descriptor managed by the VFS.
 * @param entry Output buffer for the next entry.
 * @retval ALL_OK Entry emitted successfully.
 * @retval -ENOENT Directory enumeration has completed.
 */
int fat16_read_entry(struct file *descriptor, struct dir_entry *entry)
{
    const struct fat_file_descriptor *fat_desc = descriptor->fs_data;

    ASSERT(descriptor->type == INODE_DIRECTORY);
    if (descriptor->offset >= fat_desc->item->directory.entry_count) {
        return -ENOENT;
    }

    // const struct fat_directory *fat_directory      = fat_desc->item->directory;
    // const struct fat_directory_entry *current_item = &fat_directory->entries[descriptor->offset++];
    const struct fat_directory fat_directory       = fat_desc->item->directory;
    const size_t index                             = descriptor->offset++;
    const struct fat_directory_entry *current_item = &fat_directory.entries[index];

    return fat16_read_file_dir_entry(current_item, index, entry);
}

#if 0

/**
 * @brief Diagnostic helper that prints FAT16 partition metadata to the debug console.
 *
 * @param disk Disk descriptor whose FAT16 header should be inspected.
 */
void fat16_print_partition_stats(const struct disk *disk)
{
    struct fat_private *fat_private = disk->fs_private;
    struct disk_stream *stream      = disk_stream_create(disk->id);
    if (!stream) {
        warningf("Failed to create disk stream");
        panic("Failed to create disk stream");
    }

    if (disk_stream_read(stream, &fat_private->header, sizeof(fat_private->header)) != ALL_OK) {
        warningf("Failed to read FAT16 header\n");
        disk_stream_close(stream);
        panic("Failed to read FAT16 header");
    }

    const uint16_t sector_size         = fat_private->header.primary_header.bytes_per_sector;
    const uint8_t fat_copies           = fat_private->header.primary_header.fat_copies;
    const uint16_t heads               = fat_private->header.primary_header.heads;
    const uint16_t hidden_sectors      = fat_private->header.primary_header.hidden_sectors;
    const uint16_t media_type          = fat_private->header.primary_header.media_type;
    const uint16_t reserved_sectors    = fat_private->header.primary_header.reserved_sectors;
    const uint16_t root_entries        = fat_private->header.primary_header.root_entries;
    const uint16_t sectors_per_cluster = fat_private->header.primary_header.sectors_per_cluster;
    const uint16_t sectors_per_fat     = fat_private->header.primary_header.sectors_per_fat;
    const uint16_t sectors_per_track   = fat_private->header.primary_header.sectors_per_track;
    const uint16_t total_sectors       = fat_private->header.primary_header.total_sectors != 0
              ? fat_private->header.primary_header.total_sectors
              : fat_private->header.primary_header.total_sectors_large;

    dbgprintf("Sector size: %d\n", sector_size);
    dbgprintf("FAT copies: %d\n", fat_copies);
    dbgprintf("Heads: %d\n", heads);
    dbgprintf("Hidden sectors: %d\n", hidden_sectors);
    dbgprintf("Media type: %d\n", media_type);
    dbgprintf("OEM name: %s\n", fat_private->header.primary_header.oem_name);
    dbgprintf("Reserved sectors: %d\n", reserved_sectors);
    dbgprintf("Root entries: %d\n", root_entries);
    dbgprintf("Sectors per cluster: %d\n", sectors_per_cluster);
    dbgprintf("Sectors per FAT: %d\n", sectors_per_fat);
    dbgprintf("Sectors per track: %d\n", sectors_per_track);
    dbgprintf("Total sectors: %d\n", total_sectors);
    // dbgprintf("Partition size: %d MiB\n", partition_size / 1024 / 1024);

    disk_stream_close(stream);
}

#endif
