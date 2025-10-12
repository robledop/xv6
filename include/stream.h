#pragma once

#include <stdint.h>
#include <disk.h>

struct disk_stream {
    uint32_t position; // Byte position in the disk
    struct disk *disk;
};

struct disk_stream disk_stream_create(int disk_index);
int disk_stream_seek(struct disk_stream *stream, uint32_t position);
int disk_stream_read(struct disk_stream *stream, void *out, uint32_t size);
int disk_stream_write(struct disk_stream *stream, const void *in, uint32_t size);
