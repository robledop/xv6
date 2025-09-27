//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"

/** @brief Device switch table mapping major numbers to drivers. */
struct devsw devsw[NDEV];
/** @brief Global file table protected by a spinlock. */
struct
{
    struct spinlock lock;
    struct file file[NFILE];
} ftable;

/** @brief Initialize the global file table lock. */
void fileinit(void)
{
    initlock(&ftable.lock, "ftable");
}

/**
 * @brief Allocate an unused file structure.
 *
 * @return Pointer to the allocated file or ::0 if none are free.
 */
struct file *
filealloc(void)
{
    acquire(&ftable.lock);
    for (struct file* f = ftable.file; f < ftable.file + NFILE; f++)
    {
        if (f->ref == 0)
        {
            f->ref = 1;
            release(&ftable.lock);
            return f;
        }
    }
    release(&ftable.lock);
    return 0;
}

/**
 * @brief Increment the reference count for a file.
 *
 * @param f File to duplicate.
 * @return The same file pointer with incremented reference count.
 */
struct file *
filedup(struct file *f)
{
    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("filedup");
    f->ref++;
    release(&ftable.lock);
    return f;
}

/**
 * @brief Close a file descriptor, releasing resources when the last reference drops.
 *
 * @param f File to close.
 */
void fileclose(struct file *f)
{
    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("fileclose");
    if (--f->ref > 0)
    {
        release(&ftable.lock);
        return;
    }
    struct file ff = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release(&ftable.lock);

    if (ff.type == FD_PIPE)
        pipeclose(ff.pipe, ff.writable);
    else if (ff.type == FD_INODE)
    {
        begin_op();
        iput(ff.ip);
        end_op();
    }
}

/**
 * @brief Retrieve metadata for a file.
 *
 * @param f File to query (must be an inode-backed file).
 * @param st Destination buffer for statistics.
 * @return ::0 on success, ::-1 if unsupported for the file type.
 */
int filestat(struct file *f, struct stat *st)
{
    if (f->type == FD_INODE)
    {
        ilock(f->ip);
        stati(f->ip, st);
        iunlock(f->ip);
        return 0;
    }
    return -1;
}

/**
 * @brief Read data from a file into a buffer.
 *
 * @param f File to read.
 * @param addr Destination buffer.
 * @param n Maximum number of bytes to read.
 * @return Bytes read or ::-1 on error.
 */
int fileread(struct file *f, char *addr, int n)
{
    int r;

    if (f->readable == 0)
        return -1;
    if (f->type == FD_PIPE)
        return piperead(f->pipe, addr, n);
    if (f->type == FD_INODE)
    {
        ilock(f->ip);
        if ((r = readi(f->ip, addr, f->off, n)) > 0)
            f->off += r;
        iunlock(f->ip);
        return r;
    }
    panic("fileread");
}

/**
 * @brief Write data from a buffer to a file.
 *
 * @param f File to write.
 * @param addr Source buffer.
 * @param n Number of bytes to write.
 * @return Bytes written or ::-1 on error.
 */
int filewrite(struct file *f, char *addr, int n)
{
    int r;

    if (f->writable == 0)
        return -1;
    if (f->type == FD_PIPE)
        return pipewrite(f->pipe, addr, n);
    if (f->type == FD_INODE)
    {
        // write a few blocks at a time to avoid exceeding
        // the maximum log transaction size, including
        // i-node, indirect block, allocation blocks,
        // and 2 blocks of slop for non-aligned writes.
        // this really belongs lower down, since writei()
        // might be writing a device like the console.
        int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * 512;
        int i = 0;
        while (i < n)
        {
            int n1 = n - i;
            if (n1 > max)
                n1 = max;

            begin_op();
            ilock(f->ip);
            if ((r = writei(f->ip, addr + i, f->off, n1)) > 0)
                f->off += r;
            iunlock(f->ip);
            end_op();

            if (r < 0)
                break;
            if (r != n1)
                panic("short filewrite");
            i += r;
        }
        return i == n ? n : -1;
    }
    panic("filewrite");
}
