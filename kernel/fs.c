// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
/** @brief Truncate an inode, releasing all associated blocks. */
static void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
/** @brief Cached superblock for the single disk device. */
struct superblock super_block;

/**
 * @brief Read the on-disk superblock into memory.
 *
 * @param dev Device number to read from.
 * @param sb Destination superblock structure.
 */
void readsb(int dev, struct superblock* sb)
{
    struct buf* bp = bread(dev, 1);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

/**
 * @brief Zero out a disk block and write it through the log.
 *
 * @param dev Device containing the block.
 * @param bno Block number to clear.
 */
static void bzero(int dev, int bno)
{
    struct buf* bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    log_write(bp);
    brelse(bp);
}

// Blocks.

/**
 * @brief Allocate a zero-initialized disk block.
 *
 * @param dev Device to allocate from.
 * @return Block number of the allocated block.
 */
static uint balloc(uint dev)
{
    struct buf* bp = 0;
    for (int b = 0; b < super_block.size; b += BPB)
    {
        bp = bread(dev, BBLOCK(b, super_block));
        for (int bi = 0; bi < BPB && b + bi < super_block.size; bi++)
        {
            int m = 1 << (bi % 8);
            if ((bp->data[bi / 8] & m) == 0)
            {
                // Is block free?
                bp->data[bi / 8] |= m; // Mark block in use.
                log_write(bp);
                brelse(bp);
                bzero(dev, b + bi);
                return b + bi;
            }
        }
        brelse(bp);
    }
    panic("balloc: out of blocks");
}

/**
 * @brief Release a disk block back to the free map.
 *
 * @param dev Device the block belongs to.
 * @param b Block number to free.
 */
static void bfree(int dev, uint b)
{
    struct buf* bp = bread(dev, BBLOCK(b, super_block));
    int bi = b % BPB;
    int m = 1 << (bi % 8);
    if ((bp->data[bi / 8] & m) == 0)
        panic("freeing free block");
    bp->data[bi / 8] &= ~m;
    log_write(bp);
    brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

/** @brief In-memory inode cache protected by a spinlock. */
struct
{
    struct spinlock lock;
    struct inode inode[NINODE];
} icache;

/**
 * @brief Initialize the inode cache and read the superblock.
 *
 * @param dev Device number containing the filesystem.
 */
void iinit(int dev)
{
    int i = 0;

    initlock(&icache.lock, "icache");
    for (i = 0; i < NINODE; i++)
    {
        initsleeplock(&icache.inode[i].lock, "inode");
    }

    readsb(dev, &super_block);
    cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n",
            super_block.size, super_block.nblocks,
            super_block.ninodes, super_block.nlog, super_block.logstart, super_block.inodestart,
            super_block.bmapstart);
}

static struct inode* iget(uint dev, uint inum);

/**
 * @brief Allocate a fresh inode of the given type.
 *
 * @param dev Device identifier.
 * @param type Inode type to assign.
 * @return Referenced but unlocked inode structure.
 */
struct inode*
ialloc(uint dev, short type)
{
    for (int inum = 1; inum < super_block.ninodes; inum++)
    {
        struct buf* bp = bread(dev, IBLOCK(inum, super_block));
        struct dinode* dip = (struct dinode*)bp->data + inum % IPB;
        if (dip->type == 0)
        {
            // a free inode
            memset(dip, 0, sizeof(*dip));
            dip->type = type;
            log_write(bp); // mark it allocated on the disk
            brelse(bp);
            return iget(dev, inum);
        }
        brelse(bp);
    }
    panic("ialloc: no inodes");
}

/**
 * @brief Write an in-memory inode back to disk.
 *
 * Callers must hold the inode's sleeplock.
 *
 * @param ip Inode to flush.
 */
void iupdate(struct inode* ip)
{
    struct buf* bp = bread(ip->dev, IBLOCK(ip->inum, super_block));
    struct dinode* dip = (struct dinode*)bp->data + ip->inum % IPB;
    dip->type = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink;
    dip->size = ip->size;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
    log_write(bp);
    brelse(bp);
}

/**
 * @brief Fetch an inode from the cache, creating an entry if needed.
 *
 * The returned inode is referenced but unlocked.
 *
 * @param dev Device identifier.
 * @param inum Inode number on disk.
 * @return In-core inode pointer.
 */
static struct inode* iget(uint dev, uint inum)
{
    struct inode* ip;

    acquire(&icache.lock);

    // Is the inode already cached?
    struct inode* empty = 0;
    for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++)
    {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
        {
            ip->ref++;
            release(&icache.lock);
            return ip;
        }
        if (empty == 0 && ip->ref == 0) // Remember empty slot.
            empty = ip;
    }

    // Recycle an inode cache entry.
    if (empty == 0)
        panic("iget: no inodes");

    ip = empty;
    ip->dev = dev;
    ip->inum = inum;
    ip->ref = 1;
    ip->valid = 0;
    release(&icache.lock);

    return ip;
}

/**
 * @brief Increment the reference count on an inode.
 *
 * @param ip Inode to retain.
 * @return The same inode pointer.
 */
struct inode* idup(struct inode* ip)
{
    acquire(&icache.lock);
    ip->ref++;
    release(&icache.lock);
    return ip;
}

/**
 * @brief Acquire an inode's sleeplock and populate it from disk if needed.
 *
 * @param ip Inode to lock.
 */
void ilock(struct inode* ip)
{
    if (ip == 0 || ip->ref < 1)
        panic("ilock");

    acquiresleep(&ip->lock);

    if (ip->valid == 0)
    {
        struct buf* bp = bread(ip->dev, IBLOCK(ip->inum, super_block));
        struct dinode* dip = (struct dinode*)bp->data + ip->inum % IPB;
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
        brelse(bp);
        ip->valid = 1;
        if (ip->type == 0)
            panic("ilock: no type");
    }
}

/** @brief Release an inode's sleeplock. */
void iunlock(struct inode* ip)
{
    if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
        panic("iunlock");

    releasesleep(&ip->lock);
}

/**
 * @brief Release a reference to an inode, freeing it if unlinked.
 *
 * Must be called within a log transaction when it might free disk blocks.
 *
 * @param ip Inode to release.
 */
void iput(struct inode* ip)
{
    acquiresleep(&ip->lock);
    if (ip->valid && ip->nlink == 0)
    {
        acquire(&icache.lock);
        int r = ip->ref;
        release(&icache.lock);
        if (r == 1)
        {
            // inode has no links and no other references: truncate and free.
            itrunc(ip);
            ip->type = 0;
            iupdate(ip);
            ip->valid = 0;
        }
    }
    releasesleep(&ip->lock);

    acquire(&icache.lock);
    ip->ref--;
    release(&icache.lock);
}

/**
 * @brief Convenience helper to unlock and then release an inode reference.
 *
 * @param ip Inode to unlock and drop.
 */
void iunlockput(struct inode* ip)
{
    iunlock(ip);
    iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

/**
 * @brief Translate a file-relative block number to a disk block.
 *
 * Allocates new direct or indirect blocks as required.
 *
 * @param ip Target inode.
 * @param bn Block number within the file.
 * @return Physical block number on disk.
 */
static uint bmap(struct inode* ip, uint bn)
{
    uint addr;

    if (bn < NDIRECT)
    {
        if ((addr = ip->addrs[bn]) == 0)
            ip->addrs[bn] = addr = balloc(ip->dev);
        return addr;
    }
    bn -= NDIRECT;

    if (bn < NINDIRECT)
    {
        // Load indirect block, allocating if necessary.
        if ((addr = ip->addrs[NDIRECT]) == 0)
            ip->addrs[NDIRECT] = addr = balloc(ip->dev);
        struct buf* bp = bread(ip->dev, addr);
        uint* a = (uint*)bp->data;
        if ((addr = a[bn]) == 0)
        {
            a[bn] = addr = balloc(ip->dev);
            log_write(bp);
        }
        brelse(bp);
        return addr;
    }

    panic("bmap: out of range");
}

/**
 * @brief Truncate an inode, freeing all associated data blocks.
 *
 * Only called once the inode is orphaned (no links, single reference).
 *
 * @param ip Inode to truncate.
 */
static void
itrunc(struct inode* ip)
{
    for (int i = 0; i < NDIRECT; i++)
    {
        if (ip->addrs[i])
        {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    if (ip->addrs[NDIRECT])
    {
        struct buf* bp = bread(ip->dev, ip->addrs[NDIRECT]);
        uint* a = (uint*)bp->data;
        for (int j = 0; j < NINDIRECT; j++)
        {
            if (a[j])
                bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0;
    iupdate(ip);
}

/**
 * @brief Populate a ::stat structure from an inode.
 *
 * Caller must hold the inode's sleeplock.
 *
 * @param ip Source inode.
 * @param st Destination stat buffer.
 */
void stati(struct inode* ip, struct stat* st)
{
    st->dev = ip->dev;
    st->ino = ip->inum;
    st->type = ip->type;
    st->nlink = ip->nlink;
    st->size = ip->size;
}

/**
 * @brief Read bytes from an inode into memory.
 *
 * Caller must hold the inode's sleeplock (except for device files, which
 * delegate to driver read callbacks).
 *
 * @param ip Inode to read.
 * @param dst Destination buffer.
 * @param off Byte offset within the file.
 * @param n Number of bytes to read.
 * @return Bytes read or -1 on error.
 */
int readi(struct inode* ip, char* dst, uint off, uint n)
{
    uint m;

    if (ip->type == T_DEV)
    {
        if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
            return -1;
        return devsw[ip->major].read(ip, dst, n);
    }

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > ip->size)
        n = ip->size - off;

    for (uint tot = 0; tot < n; tot += m, off += m, dst += m)
    {
        struct buf* bp = bread(ip->dev, bmap(ip, off / BSIZE));
        m = min(n - tot, BSIZE - off % BSIZE);
        memmove(dst, bp->data + off % BSIZE, m);
        brelse(bp);
    }
    return n;
}

/**
 * @brief Write bytes from memory into an inode.
 *
 * The caller must hold the inode's sleeplock; device files are forwarded to
 * driver write callbacks.
 *
 * @param ip Destination inode.
 * @param src Source buffer.
 * @param off Byte offset within the file.
 * @param n Number of bytes to write.
 * @return Bytes written or ::-1 on error.
 */
int writei(struct inode* ip, char* src, uint off, uint n)
{
    uint m;

    if (ip->type == T_DEV)
    {
        if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
            return -1;
        return devsw[ip->major].write(ip, src, n);
    }

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (uint tot = 0; tot < n; tot += m, off += m, src += m)
    {
        struct buf* bp = bread(ip->dev, bmap(ip, off / BSIZE));
        m = min(n - tot, BSIZE - off % BSIZE);
        memmove(bp->data + off % BSIZE, src, m);
        log_write(bp);
        brelse(bp);
    }

    if (n > 0 && off > ip->size)
    {
        ip->size = off;
        iupdate(ip);
    }
    return n;
}

// Directories

/**
 * @brief Compare directory entry names with fixed-size semantics.
 *
 * @param s First name.
 * @param t Second name.
 * @return Result of ::strncmp limited to DIRSIZ bytes.
 */
int namecmp(const char* s, const char* t)
{
    return strncmp(s, t, DIRSIZ);
}

/**
 * @brief Look up a directory entry by name.
 *
 * @param dp Directory inode.
 * @param name Entry to search for.
 * @param poff Optional pointer to receive byte offset of the entry.
 * @return Referenced inode on success, or ::0 if not found.
 */
struct inode* dirlookup(struct inode* dp, char* name, uint* poff)
{
    struct dirent de;

    if (dp->type != T_DIR)
        panic("dirlookup not DIR");

    for (uint off = 0; off < dp->size; off += sizeof(de))
    {
        if (readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");
        if (de.inum == 0)
            continue;
        if (namecmp(name, de.name) == 0)
        {
            // entry matches path element
            if (poff)
                *poff = off;
            uint inum = de.inum;
            return iget(dp->dev, inum);
        }
    }

    return 0;
}

/**
 * @brief Insert a new directory entry.
 *
 * @param dp Directory inode to modify.
 * @param name Entry name to create.
 * @param inum Inode number to reference.
 * @return ::0 on success or ::-1 if the name already exists.
 */
int dirlink(struct inode* dp, char* name, uint inum)
{
    int off;
    struct dirent de;
    struct inode* ip;

    // Check that name is not present.
    if ((ip = dirlookup(dp, name, 0)) != 0)
    {
        iput(ip);
        return -1;
    }

    // Look for an empty dirent.
    for (off = 0; off < dp->size; off += sizeof(de))
    {
        if (readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        if (de.inum == 0)
            break;
    }

    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    if (writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
        panic("dirlink");

    return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
/**
 * @brief Extract the next path element from a slash-delimited string.
 *
 * @param path Input path.
 * @param name Buffer receiving the next element (DIRSIZ bytes).
 * @return Pointer to the remaining path or ::0 if no elements remain.
 */
static char* skipelem(char* path, char* name)
{
    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    char* s = path;
    while (*path != '/' && *path != 0)
        path++;
    int len = path - s;
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else
    {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
/**
 * @brief Resolve a path to an inode, optionally returning the parent.
 *
 * @param path Path string to traverse.
 * @param nameiparent Non-zero to return parent directory inode.
 * @param name Buffer receiving the final path element.
 * @return Referenced inode on success, ::0 if the path cannot be resolved.
 */
static struct inode* namex(char* path, int nameiparent, char* name)
{
    struct inode *ip, *next;

    if (*path == '/')
        ip = iget(ROOTDEV, ROOTINO);
    else
        ip = idup(myproc()->cwd);

    while ((path = skipelem(path, name)) != 0)
    {
        ilock(ip);
        if (ip->type != T_DIR)
        {
            iunlockput(ip);
            return 0;
        }
        if (nameiparent && *path == '\0')
        {
            // Stop one level early.
            iunlock(ip);
            return ip;
        }
        if ((next = dirlookup(ip, name, 0)) == 0)
        {
            iunlockput(ip);
            return 0;
        }
        iunlockput(ip);
        ip = next;
    }
    if (nameiparent)
    {
        iput(ip);
        return 0;
    }
    return ip;
}

/**
 * @brief Resolve a path to its final inode.
 *
 * @param path Path string.
 * @return Referenced inode or ::0 if resolution fails.
 */
struct inode* namei(char* path)
{
    char name[DIRSIZ];
    return namex(path, 0, name);
}

/**
 * @brief Resolve a path to its parent directory inode.
 *
 * @param path Path string.
 * @param name Buffer to receive the final element.
 * @return Referenced parent inode or ::0 if resolution fails.
 */
struct inode* nameiparent(char* path, char* name)
{
    return namex(path, 1, name);
}