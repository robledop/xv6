#pragma once
#include "types.h"

#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

struct stat
{
    short type; // Type of file
    int dev; // File system's disk device
    u32 ino; // Inode number
    short nlink; // Number of links to file
    u32 size; // Size of file in bytes
};