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

int disk_stream_seek(struct disk_stream *stream, const u32 position)
{
    stream->position = position;
    return 0;
}

int disk_stream_read(struct disk_stream *stream, void *out, const u32 size)
{
    ASSERT(stream->disk->sector_size > 0, "Invalid sector size");
    if (size == 0) {
        return 0;
    }

    u32 remaining = size;
    u8 *out_bytes = (u8 *)out;

    while (remaining > 0) {
        const u32 sector = stream->position / stream->disk->sector_size;
        const u32 offset = stream->position % stream->disk->sector_size;
        u32 chunk        = stream->disk->sector_size - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        // u8 buffer[stream->disk->sector_size];
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

int disk_stream_write(struct disk_stream *stream, const void *in, const u32 size)
{
    ASSERT(stream->disk->sector_size > 0, "Invalid sector size");
    if (size == 0) {
        return 0;
    }

    u32 remaining   = size;
    const u8 *input = (const u8 *)in;

    while (remaining > 0) {
        const u32 sector = stream->position / stream->disk->sector_size;
        const u32 offset = stream->position % stream->disk->sector_size;
        u32 chunk        = stream->disk->sector_size - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        // u8 buffer[stream->disk->sector_size];
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