#include "ext2.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "icache.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
struct icache icache;


/**
 * @brief Fetch an inode from the cache, creating an entry if needed.
 *
 * The returned inode is referenced but unlocked.
 *
 * @param dev Device identifier.
 * @param inum Inode number on disk.
 * @return In-core inode pointer.
 */
struct inode *iget(u32 dev, u32 inum)
{
    struct inode *ip;

    acquire(&icache.lock);

    // Is the inode already cached?
    struct inode *empty = nullptr;
    for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;
            release(&icache.lock);
            return ip;
        }
        if (empty == nullptr && ip->ref == 0) // Remember the empty slot.
            empty = ip;
    }

    int i;
    for (i = 0; i < NINODE; i++) {
        if (ext2fs_addrs[i].busy == 0)
            break;
    }

    // Recycle an inode cache entry.
    if (empty == nullptr)
        panic("iget: no inodes");

    ip        = empty;
    ip->dev   = dev;
    ip->inum  = inum;
    ip->ref   = 1;
    ip->valid = 0;

    ip->iops             = &ext2fs_inode_ops;
    ip->addrs            = (void *)&ext2fs_addrs[i];
    ext2fs_addrs[i].busy = 1;

    release(&icache.lock);

    return ip;
}

/**
 * @brief Increment the reference count on an inode.
 *
 * @param ip Inode to retain.
 * @return The same inode pointer.
 */
struct inode *idup(struct inode *ip)
{
    acquire(&icache.lock);
    ip->ref++;
    release(&icache.lock);
    return ip;
}

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
 * @return Pointer to the remaining path or 0 if no elements remain.
 */
static char *skipelem(char *path, char *name)
{
    constexpr u32 dirlen = EXT2_NAME_LEN;
    while (*path == '/')
        path++;
    if (*path == 0) {
        return nullptr;
    }
    const char *s = path;
    while (*path != '/' && *path != 0) {
        path++;
    }
    const u32 len = path - s;
    if (len > dirlen) {
        return (char *)-1;
    }
    memmove(name, s, len);
    name[len] = 0;
    while (*path == '/') {
        path++;
    }
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
 * @return Referenced inode on success, 0 if the path cannot be resolved.
 */
static struct inode *namex(char *path, int nameiparent, char *name)
{
    struct inode *ip, *next;

    if (*path == '/') {
        ip = iget(ROOTDEV, EXT2INO);
    } else {
        ip = idup(myproc()->cwd);
    }

    while (true) {
        char *nextp = skipelem(path, name);
        if (nextp == nullptr)
            break;
        if (nextp == (char *)-1) {
            ip->iops->iput(ip);
            return nullptr;
        }

        ip->iops->ilock(ip);
        if (ip->type != T_DIR) {
            ip->iops->iunlockput(ip);
            return nullptr;
        }
        if (nameiparent && *nextp == '\0') {
            // Stop one level early.
            ip->iops->iunlock(ip);
            return ip;
        }
        if ((next = ip->iops->dirlookup(ip, name, nullptr)) == nullptr) {
            ip->iops->iunlockput(ip);
            return nullptr;
        }
        ip->iops->iunlockput(ip);
        ip   = next;
        path = nextp;
    }
    if (nameiparent) {
        ip->iops->iput(ip);
        return nullptr;
    }
    return ip;
}

/**
 * @brief Resolve a path to its final inode.
 *
 * @param path Path string.
 * @return Referenced inode or 0 if resolution fails.
 */
struct inode *namei(char *path)
{
    char name[EXT2_NAME_LEN + 1];
    return namex(path, 0, name);
}

/**
 * @brief Resolve a path to its parent directory inode.
 *
 * @param path Path string.
 * @param name Buffer to receive the final element.
 * @return Referenced parent inode or 0 if resolution fails.
 */
struct inode *nameiparent(char *path, char *name)
{
    return namex(path, 1, name);
}
