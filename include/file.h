#pragma once
#include "types.h"
#include "fs.h"
#include "sleeplock.h"
#include "stat.h"

struct file
{
    enum { FD_NONE, FD_PIPE, FD_INODE } type;

    int ref; // reference count
    char readable;
    char writable;
    struct pipe *pipe;
    struct inode *ip;
    u32 off;
};


struct inode_operations {
    int             (*dirlink)(struct inode*, char*, u32);
    struct inode*   (*dirlookup)(struct inode*, char*, u32*);
    struct inode*   (*ialloc)(u32, short);
    void            (*iinit)(int dev);
    void            (*ilock)(struct inode*);
    void            (*iput)(struct inode*);
    void            (*iunlock)(struct inode*);
    void            (*iunlockput)(struct inode*);
    void            (*iupdate)(struct inode*);
    int             (*readi)(struct inode*, char*, u32, u32);
    void            (*stati)(struct inode*, struct stat*);
    int             (*writei)(struct inode*, char*, u32, u32);
};


// in-memory copy of an inode
struct inode
{
    u32 dev;               // Device number
    u32 inum;              // Inode number
    int ref;               // Reference count
    struct sleeplock lock; // protects everything below here
    int valid;             // inode has been read from disk?
    struct inode_operations *iops;

    short type; // copy of disk inode
    short major;
    short minor;
    short nlink;
    u32 size;
    void *addrs;
};

// table mapping major device number to
// device functions
struct devsw
{
    int (*read)(struct inode *, char *, int);
    int (*write)(struct inode *, char *, int);
};

extern struct devsw devsw[];

#define CONSOLE 1