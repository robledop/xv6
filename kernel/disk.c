#include <ahci.h>
#include <assert.h>
#include <ata.h>
#include <attributes.h>
#include <bio.h>
#include <disk.h>
#include <kernel.h>
#include <memory.h>
#include <printf.h>
#include <status.h>
#include <thread.h>


int disk_read_block(uint32_t lba, int total, void *buffer);
int disk_read_sector(uint32_t sector, void *buffer);
NON_NULL int disk_write_block(uint32_t lba, int total, void *buffer);
NON_NULL int disk_write_sector(uint32_t sector, uint8_t *buffer);
NON_NULL int disk_write_sector_offset(const void *data, int size, int offset, int sector);
NON_NULL struct file_system *vfs_resolve(struct disk *disk);
struct disk disk;
static struct spinlock disk_lock;

void disk_init()
{
    ata_init();
    memset(&disk, 0, sizeof(disk));
    disk.type = DISK_TYPE_PHYSICAL;
    disk.id   = 0;

    printf("[DISK] using %s for disk operations\n", ahci_port_ready() ? "AHCI" : "legacy ATA");
    disk.sector_size = (uint16_t)(ahci_port_ready() ? AHCI_SECTOR_SIZE : (unsigned int)ata_get_sector_size());

    disk.fs = vfs_resolve(&disk);
}


/**
 * @brief Synchronize a buffer with disk, reading or writing as required.
 *
 * @param b Buffer to schedule; must be locked by the caller.
 */
void disk_sync_buffer(struct buf *b)
{
    struct buf **pp;

    if (!holdingsleep(&b->lock)) {
        panic("iderw: buf not locked");
    }
    if ((b->flags & (B_VALID | B_DIRTY)) == B_VALID) {
        panic("iderw: nothing to do");
    }
    // if (b->dev != 0 && !havedisk1) {
    //     panic("iderw: ide disk 1 not present");
    // }

    acquire(&disk_lock);

    // Append b to idequeue.
    // b->qnext = nullptr;
    // for (pp = &idequeue; *pp; pp = &(*pp)->qnext)
    //     ;
    // *pp = b;

    if (b->flags & B_DIRTY) {
        const int status = disk_write_block(b->blockno, 1, b->data);
        if (status < 0) {
            panic("disk_sync_buffer: write failed");
        }
    } else {
        const int status = disk_read_block(b->blockno, 1, b->data);
        if (status < 0) {
            panic("disk_sync_buffer: read failed");
        }
    }

    b->flags |= B_VALID;
    b->flags &= ~B_DIRTY;
    wakeup(b);

    // Start disk if necessary.
    // if (idequeue == b) {
    //     idestart(b);
    // }

    // Wait for request to finish.
    // while ((b->flags & (B_VALID | B_DIRTY)) != B_VALID) {
    //     sleep(b, &disk_lock);
    // }

    release(&disk_lock);
}

struct disk *disk_get(const int index)
{
    if (index != 0) {
        return nullptr;
    }

    return &disk;
}

int disk_read_block(const uint32_t lba, const int total, void *buffer)
{
    if (total <= 0) {
        return -EINVARG;
    }

    if (ahci_port_ready()) {
        const int status = ahci_read(lba, (uint32_t)total, buffer);
        if (status == ALL_OK) {
            return ALL_OK;
        }

        printf("[DISK] AHCI read failed with status %d; falling back to legacy ATA\n", status);
    }

    return ata_read_sectors(lba, total, buffer);
}

int disk_read_sector(const uint32_t sector, void *buffer)
{
    return disk_read_block(sector, 1, buffer);
}

int disk_write_block(const uint32_t lba, const int total, void *buffer)
{
    if (total <= 0) {
        return -EINVARG;
    }

    if (ahci_port_ready()) {
        const int status = ahci_write(lba, (uint32_t)total, buffer);
        if (status == ALL_OK) {
            return ALL_OK;
        }

        printf("[DISK] AHCI write failed with status %d; falling back to legacy ATA\n", status);
    }

    return ata_write_sectors(lba, total, buffer);
}

int disk_write_sector(const uint32_t sector, uint8_t *buffer)
{
    return disk_write_block(sector, 1, buffer);
}

int disk_write_sector_offset(const void *data, const int size, const int offset, const int sector)
{
    ASSERT(size <= 512 - offset);

    uint8_t buffer[512];
    disk_read_sector(sector, buffer);

    memcpy(&buffer[offset], data, size);
    return disk_write_sector(sector, buffer);
}
