#pragma once

#include "types.h"
#include "stat.h"

#define EXT2_DIRENT_NAME_MAX 255

struct dirent_view
{
    u32 inode;
    u8 file_type;
    u8 name_len;
    char name[EXT2_DIRENT_NAME_MAX + 1];
};

typedef int (*dirwalk_cb)(const struct dirent_view *, void *);

int dirwalk(int fd, dirwalk_cb cb, void *arg);
