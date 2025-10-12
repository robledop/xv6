#include <assert.h>
#include <disk.h>
#include <kernel.h>
#include <memory.h>
#include <serial.h>
#include <stream.h>

struct disk_stream disk_stream_create(const int disk_index)
{
    struct disk *disk = disk_get(disk_index);
    if (!disk) {
        panic("Failed to get disk %d\n");
    }

    return (struct disk_stream){.position = 0, .disk = disk};
}

int disk_stream_seek(struct disk_stream *stream, const uint32_t position)
{
    stream->position = position;
    return 0;
}

int disk_stream_read(struct disk_stream *stream, void *out, const uint32_t size)
{
    ASSERT(stream->disk->sector_size > 0, "Invalid sector size");
    if (size == 0) {
        return 0;
    }

    uint32_t remaining = size;
    uint8_t *out_bytes = (uint8_t *)out;

    while (remaining > 0) {
        const uint32_t sector = stream->position / stream->disk->sector_size;
        const uint32_t offset = stream->position % stream->disk->sector_size;
        uint32_t chunk        = stream->disk->sector_size - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        // uint8_t buffer[stream->disk->sector_size];
        auto buf = bread(0, sector);
        // const int res = disk_read_sector(sector, buffer);
        // if (res < 0) {
        //     panic("Failed to read block\n");
        //     return res;
        // }

        memcpy(out_bytes, buf->data + offset, chunk);
        brelse(buf);

        out_bytes += chunk;
        stream->position += chunk;
        remaining -= chunk;
    }

    return 0;
}

int disk_stream_write(struct disk_stream *stream, const void *in, const uint32_t size)
{
    ASSERT(stream->disk->sector_size > 0, "Invalid sector size");
    if (size == 0) {
        return 0;
    }

    uint32_t remaining   = size;
    const uint8_t *input = (const uint8_t *)in;

    while (remaining > 0) {
        const uint32_t sector = stream->position / stream->disk->sector_size;
        const uint32_t offset = stream->position % stream->disk->sector_size;
        uint32_t chunk        = stream->disk->sector_size - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        // uint8_t buffer[stream->disk->sector_size];
        auto buf = bread(0, sector);
        // int res = disk_read_sector(sector, buffer);
        // if (res < 0) {
        //     warningf("Failed to read block\n");
        //     return res;
        // }

        memcpy(buf->data + offset, input, chunk);


        // res = disk_write_sector(sector, buffer);
        // if (res < 0) {
        //     warningf("Failed to write block\n");
        //     return res;
        // }

        bwrite(buf);

        input += chunk;
        stream->position += chunk;
        remaining -= chunk;
    }

    return 0;
}