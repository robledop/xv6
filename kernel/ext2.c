#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "ext2.h"
#include "buf.h"
#include "file.h"
#include "icache.h"
#include "mbr.h"

struct inode_operations ext2fs_inode_ops = {
    ext2fs_dirlink,
    ext2fs_dirlookup,
    ext2fs_ialloc,
    ext2fs_iinit,
    ext2fs_ilock,
    ext2fs_iput,
    ext2fs_iunlock,
    ext2fs_iunlockput,
    ext2fs_iupdate,
    ext2fs_readi,
    ext2fs_stati,
    ext2fs_writei,
};

#define min(a,b) ((a) < (b) ? (a) : (b))

static void ext2fs_bzero(int dev, int bno);
static u32 ext2fs_balloc(u32 dev, u32 inum);
static void ext2fs_bfree(int dev, u32 b);
static u32 ext2fs_bmap(struct inode *ip, u32 bn);
static void ext2fs_itrunc(struct inode *ip);
struct ext2fs_addrs ext2fs_addrs[NINODE];
struct ext2_super_block ext2_sb;
u32 first_partition_block = 0;
extern struct mbr mbr;

void ext2fs_readsb(int dev, struct ext2_super_block *sb)
{
    first_partition_block = (mbr.part[0].lba_start / 2);
    const u32 sb_blockno  = first_partition_block + 1; // superblock is at offset 1024 bytes
    struct buf *bp        = bread(dev, sb_blockno);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

// Zero a block.
static void ext2fs_bzero(int dev, int bno)
{
    struct buf *bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    bwrite(bp);
    brelse(bp);
}

// check if a block is free and return it's bit number

static u32 ext2fs_free_block(char *bitmap)
{
    for (int i = 0; i < ext2_sb.s_blocks_per_group * 8; i++) {
        for (int j = 0; j < 8; j++) {
            int mask = 1 << (7 - j);
            if ((bitmap[i] & mask) == 0) {
                bitmap[i] |= mask;
                return i * 8 + j;
            }
        }
    }
    return -1;
}

// Allocate a zeroed disk block.
static u32 ext2fs_balloc(u32 dev, u32 inum)
{
    struct ext2_group_desc bgdesc;
    u32 desc_blockno = first_partition_block + 2; // block group descriptor table starts at block 2

    int gno         = GET_GROUP_NO(inum, ext2_sb);
    struct buf *bp1 = bread(dev, desc_blockno);
    memmove(&bgdesc, bp1->data + gno * sizeof(bgdesc), sizeof(bgdesc));
    brelse(bp1);
    struct buf *bp2 = bread(dev, bgdesc.bg_block_bitmap + first_partition_block);

    int fbit = ext2fs_free_block((char *)bp2->data);
    if (fbit > -1) {
        int zbno = bgdesc.bg_block_bitmap + fbit + first_partition_block;
        bwrite(bp2);
        ext2fs_bzero(dev, zbno);
        brelse(bp2);
        return zbno;
    }
    brelse(bp2);
    panic("ext2_balloc: out of blocks\n");
}

// Free a disk block.
static void ext2fs_bfree(int dev, u32 b)
{
    struct ext2_group_desc bgdesc;
    u32 desc_blockno = first_partition_block + 2; // block group descriptor table starts at block 2

    int gno         = GET_GROUP_NO(b, ext2_sb);
    int iindex      = GET_INODE_INDEX(b, ext2_sb);
    struct buf *bp1 = bread(dev, desc_blockno);
    memmove(&bgdesc, bp1->data + gno * sizeof(bgdesc), sizeof(bgdesc));
    struct buf *bp2 = bread(dev, bgdesc.bg_block_bitmap + first_partition_block);
    iindex -= bgdesc.bg_block_bitmap;
    int mask = 1 << (iindex % 8);

    if ((bp2->data[iindex / 8] & mask) == 0)
        panic("ext2fs_bfree: block already free\n");
    bp2->data[iindex / 8] = bp2->data[iindex / 8] & ~mask;
    bwrite(bp2);
    brelse(bp2);
    brelse(bp1);
}

void ext2fs_iinit(int dev)
{
    mbr_load();
    ext2fs_readsb(dev, &ext2_sb);
    cprintf("ext2_sb: magic_number %x size %d nblocks %d ninodes %d \
inodes_per_group %d inode_size %d\n",
            ext2_sb.s_magic,
            1024 << ext2_sb.s_log_block_size,
            ext2_sb.s_blocks_count,
            ext2_sb.s_inodes_count,
            ext2_sb.s_inodes_per_group,
            ext2_sb.s_inode_size);
}

struct inode *ext2fs_ialloc(u32 dev, short type)
{
    struct ext2_group_desc bgdesc;
    u32 desc_blockno = first_partition_block + 2; // block group descriptor table starts at block 2

    int bgcount = ext2_sb.s_blocks_count / ext2_sb.s_blocks_per_group;
    for (int i = 0; i <= bgcount; i++) {
        struct buf *bp1 = bread(dev, desc_blockno);
        memmove(&bgdesc, bp1->data + i * sizeof(bgdesc), sizeof(bgdesc));
        brelse(bp1);

        struct buf *bp2 = bread(dev, bgdesc.bg_inode_bitmap + first_partition_block);
        int fbit        = ext2fs_free_block((char *)bp2->data);
        if (fbit == -1) {
            brelse(bp2);
            continue;
        }

        int bno = bgdesc.bg_inode_table + fbit / (EXT2_BSIZE / sizeof(struct ext2_inode)) + first_partition_block;
        int iindex = fbit % (EXT2_BSIZE / sizeof(struct ext2_inode));
        struct buf *bp3 = bread(dev, bno);
        struct ext2_inode *din = (struct ext2_inode *)bp3->data + iindex;
        memset(din, 0, sizeof(*din));
        if (type == T_DIR) {
            din->i_mode = S_IFDIR;
        } else if (type == T_FILE) {
            din->i_mode = S_IFREG;
        } else if (type == T_DEV) {
            din->i_mode = S_IFCHR;
        }
        bwrite(bp3);
        bwrite(bp2);
        brelse(bp3);
        brelse(bp2);

        int inum = i * ext2_sb.s_inodes_per_group + fbit + 1;
        return iget(dev, inum);
    }
    panic("ext2_ialloc: no inodes");
}

void ext2fs_iupdate(struct inode *ip)
{
    struct ext2_group_desc bgdesc;
    struct ext2_inode din;
    u32 desc_blockno = first_partition_block + 2; // block group descriptor table starts at block 2

    int gno        = GET_GROUP_NO(ip->inum, ext2_sb);
    int ioff       = GET_INODE_INDEX(ip->inum, ext2_sb);
    struct buf *bp = bread(ip->dev, desc_blockno);
    memmove(&bgdesc, bp->data + gno * sizeof(bgdesc), sizeof(bgdesc));
    brelse(bp);
    int bno         = bgdesc.bg_inode_table + ioff / (EXT2_BSIZE / ext2_sb.s_inode_size) + first_partition_block;
    int iindex      = ioff % (EXT2_BSIZE / ext2_sb.s_inode_size);
    struct buf *bp1 = bread(ip->dev, bno);
    memmove(&din, bp1->data + iindex * ext2_sb.s_inode_size, sizeof(din));

    if (ip->type == T_DIR)
        din.i_mode = S_IFDIR;
    if (ip->type == T_FILE)
        din.i_mode = S_IFREG;
    din.i_links_count = ip->nlink;
    din.i_size        = ip->size;
    din.i_dtime       = 0;
    din.i_faddr       = 0;
    din.i_file_acl    = 0;
    din.i_flags       = 0;
    din.i_generation  = 0;
    din.i_gid         = 0;
    din.i_mtime       = 0;
    din.i_uid         = 0;
    din.i_atime       = 0;

    struct ext2fs_addrs *ad = (struct ext2fs_addrs *)ip->addrs;
    memmove(din.i_block, ad->addrs, sizeof(ad->addrs));
    memmove(bp1->data + (iindex * ext2_sb.s_inode_size), &din, sizeof(din));
    bwrite(bp1);
    brelse(bp1);
}

void ext2fs_ilock(struct inode *ip)
{
    struct ext2_group_desc bgdesc;
    struct ext2_inode din;
    if (ip == nullptr || ip->ref < 1)
        panic("ext2fs_ilock");

    acquiresleep(&ip->lock);
    const auto ad  = (struct ext2fs_addrs *)ip->addrs;
    u32 desc_block = first_partition_block + 2; // Group descriptor at ext2 block 2 = sector 4

    if (ip->valid == 0) {
        const int gno  = GET_GROUP_NO(ip->inum, ext2_sb);
        const int ioff = GET_INODE_INDEX(ip->inum, ext2_sb);
        struct buf *bp = bread(ip->dev, desc_block);
        memmove(&bgdesc, bp->data + gno * sizeof(bgdesc), sizeof(bgdesc));
        brelse(bp);
        const int bno    = bgdesc.bg_inode_table + ioff / (EXT2_BSIZE / ext2_sb.s_inode_size) + first_partition_block;
        const int iindex = ioff % (EXT2_BSIZE / ext2_sb.s_inode_size);
        struct buf *bp1  = bread(ip->dev, bno);
        memmove(&din, bp1->data + iindex * ext2_sb.s_inode_size, sizeof(din));
        brelse(bp1);

        if (S_ISDIR(din.i_mode) || din.i_mode == T_DIR)
            ip->type = T_DIR;
        else
            ip->type = T_FILE;
        ip->major = 0;
        ip->minor = 0;
        ip->nlink = din.i_links_count;
        ip->size  = din.i_size;
        ip->iops  = &ext2fs_inode_ops;
        memmove(ad->addrs, din.i_block, sizeof(ad->addrs));

        ip->valid = 1;
        if (ip->type == 0)
            panic("ext2fs_ilock: no type");
    }
    return;
}

void ext2fs_iunlock(struct inode *ip)
{
    if (ip == nullptr || !holdingsleep(&ip->lock) || ip->ref < 1)
        panic("ext2fs_iunlock");

    releasesleep(&ip->lock);
}

// Free a inode
static void ext2fs_ifree(struct inode *ip)
{
    struct ext2_group_desc bgdesc;
    u32 desc_blockno = first_partition_block + 2; // block group descriptor table starts at block 2

    int gno         = GET_GROUP_NO(ip->inum, ext2_sb);
    struct buf *bp1 = bread(ip->dev, desc_blockno);
    memmove(&bgdesc, bp1->data + gno * sizeof(bgdesc), sizeof(bgdesc));
    brelse(bp1);
    struct buf *bp2 = bread(ip->dev, bgdesc.bg_inode_bitmap + first_partition_block);
    int index       = (ip->inum - 1) % ext2_sb.s_inodes_per_group;
    int mask        = 1 << (index % 8);

    if ((bp2->data[index / 8] & mask) == 0)
        panic("ext2fs_ifree: inode already free\n");
    bp2->data[index / 8] = bp2->data[index / 8] & ~mask;
    bwrite(bp2);
    brelse(bp2);
}

void ext2fs_iput(struct inode *ip)
{
    acquiresleep(&ip->lock);
    struct ext2fs_addrs *ad = (struct ext2fs_addrs *)ip->addrs;
    if (ip->valid && ip->nlink == 0) {
        acquire(&icache.lock);
        int r = ip->ref;
        release(&icache.lock);
        if (r == 1) {
            // inode has no links and no other references: truncate and free.
            ext2fs_ifree(ip);
            ext2fs_itrunc(ip);
            ip->type = 0;
            ip->iops->iupdate(ip);
            ip->valid = 0;
            ip->iops  = nullptr;
            ip->addrs = nullptr;
        }
    }
    releasesleep(&ip->lock);

    acquire(&icache.lock);
    ip->ref--;
    if (ip->ref == 0) {
        ad->busy  = 0;
        ip->addrs = nullptr;
    }
    release(&icache.lock);

    return;
}

void ext2fs_iunlockput(struct inode *ip)
{
    ip->iops->iunlock(ip);
    ip->iops->iput(ip);
}

void ext2fs_stati(struct inode *ip, struct stat *st)
{
    st->dev   = ip->dev;
    st->ino   = ip->inum;
    st->type  = ip->type;
    st->nlink = ip->nlink;
    st->size  = ip->size;
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
/*
 * EXT2BSIZE -> 1024
 * If < EXT2_NDIR_BLOCKS then it is directly mapped, allocate and return
 * If < 128 (Indirect blocks) then need to allocate using indirect block
 * If < 128*128 (Double indirect) ...
 * If < 128*128*128 (Triple indirect) ...
 * Else panic()
*/
static u32 ext2fs_bmap(struct inode *ip, u32 bn)
{
    u32 addr, *a, *b;
    struct buf *bp, *bp1;
    struct ext2fs_addrs *ad = (struct ext2fs_addrs *)ip->addrs;

    if (bn < EXT2_NDIR_BLOCKS) {
        if ((addr = ad->addrs[bn]) == 0) {
            ad->addrs[bn] = addr = ext2fs_balloc(ip->dev, ip->inum);
            return addr;
        }

        return addr + first_partition_block;
    }
    bn -= EXT2_NDIR_BLOCKS;
    if (bn < EXT2_INDIRECT) {
        if ((addr                     = ad->addrs[EXT2_IND_BLOCK]) == 0)
            ad->addrs[EXT2_IND_BLOCK] = addr = ext2fs_balloc(ip->dev, ip->inum);
        bp = bread(ip->dev, first_partition_block + addr);
        a  = (u32 *)bp->data;
        if ((addr = a[bn]) == 0)
            a[bn] = addr = ext2fs_balloc(ip->dev, ip->inum);
        brelse(bp);
        return addr;
    }
    bn -= EXT2_INDIRECT;

    if (bn < EXT2_DINDIRECT) {
        if ((addr                      = ad->addrs[EXT2_DIND_BLOCK]) == 0)
            ad->addrs[EXT2_DIND_BLOCK] = addr = ext2fs_balloc(ip->dev, ip->inum);
        bp = bread(ip->dev, addr);
        a  = (u32 *)bp->data;
        if ((addr                 = a[bn / EXT2_INDIRECT]) == 0)
            a[bn / EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
        bp1 = bread(ip->dev, addr);
        b   = (u32 *)bp1->data;
        if ((addr                 = b[bn / EXT2_INDIRECT]) == 0)
            b[bn / EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
        brelse(bp);
        brelse(bp1);
        return addr;
    }
    bn -= EXT2_DINDIRECT;

    if (bn < EXT2_TINDIRECT) {
        if ((addr                      = ad->addrs[EXT2_TIND_BLOCK]) == 0)
            ad->addrs[EXT2_TIND_BLOCK] = addr = ext2fs_balloc(ip->dev, ip->inum);
        bp = bread(ip->dev, addr);
        a  = (u32 *)bp->data;
        if ((addr                 = a[bn / EXT2_INDIRECT]) == 0)
            a[bn / EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
        bp1 = bread(ip->dev, addr);
        b   = (u32 *)bp1->data;
        if ((addr                 = b[bn / EXT2_INDIRECT]) == 0)
            b[bn / EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
        struct buf *bp2 = bread(ip->dev, addr);
        u32 *c          = (u32 *)bp2->data;
        if ((addr                 = c[bn / EXT2_INDIRECT]) == 0)
            c[bn / EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
        brelse(bp);
        brelse(bp1);
        brelse(bp2);
        return addr;
    }
    panic("ext2_bmap: block number out of range\n");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void ext2fs_itrunc(struct inode *ip)
{
    int i, j;
    struct buf *bp1, *bp2;
    u32 *a, *b;
    struct ext2fs_addrs *ad = (struct ext2fs_addrs *)ip->addrs;

    // for direct blocks
    for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (ad->addrs[i]) {
            ext2fs_bfree(ip->dev, ad->addrs[i]);
            ad->addrs[i] = 0;
        }
    }
    // EXT2_INDIRECT -> (EXT2_BSIZE / sizeof(u32))
    // for indirect blocks
    if (ad->addrs[EXT2_IND_BLOCK]) {
        bp1 = bread(ip->dev, ad->addrs[EXT2_IND_BLOCK] + first_partition_block);
        a   = (u32 *)bp1->data;
        for (i = 0; i < EXT2_INDIRECT; i++) {
            if (a[i]) {
                ext2fs_bfree(ip->dev, a[i]);
                a[i] = 0;
            }
        }
        brelse(bp1);
        ext2fs_bfree(ip->dev, ad->addrs[EXT2_IND_BLOCK]);
        ad->addrs[EXT2_IND_BLOCK] = 0;
    }

    // for double indirect blocks
    if (ad->addrs[EXT2_DIND_BLOCK]) {
        bp1 = bread(ip->dev, ad->addrs[EXT2_DIND_BLOCK] + first_partition_block);
        a   = (u32 *)bp1->data;
        for (i = 0; i < EXT2_INDIRECT; i++) {
            if (a[i]) {
                bp2 = bread(ip->dev, a[i] + first_partition_block);
                b   = (u32 *)bp2->data;
                for (j = 0; j < EXT2_INDIRECT; j++) {
                    if (b[j]) {
                        ext2fs_bfree(ip->dev, b[j]);
                        b[j] = 0;
                    }
                }
                brelse(bp2);
                ext2fs_bfree(ip->dev, a[i]);
                a[i] = 0;
            }
        }
        brelse(bp1);
        ext2fs_bfree(ip->dev, ad->addrs[EXT2_DIND_BLOCK]);
        ad->addrs[EXT2_DIND_BLOCK] = 0;
    }

    // for triple indirect blocks
    if (ad->addrs[EXT2_TIND_BLOCK]) {
        bp1 = bread(ip->dev, ad->addrs[EXT2_TIND_BLOCK] + first_partition_block);
        a   = (u32 *)bp1->data;
        for (i = 0; i < EXT2_INDIRECT; i++) {
            if (a[i]) {
                bp2 = bread(ip->dev, a[i] + first_partition_block);
                b   = (u32 *)bp2->data;
                for (j = 0; j < EXT2_INDIRECT; j++) {
                    if (b[j]) {
                        struct buf *bp3 = bread(ip->dev, b[j] + first_partition_block);
                        u32 *c          = (u32 *)bp3->data;
                        for (int k = 0; k < EXT2_INDIRECT; k++) {
                            if (c[k]) {
                                ext2fs_bfree(ip->dev, c[k]);
                                c[k] = 0;
                            }
                        }
                        brelse(bp3);
                        ext2fs_bfree(ip->dev, b[j]);
                        b[j] = 0;
                    }
                }
                brelse(bp2);
                ext2fs_bfree(ip->dev, a[i]);
                a[i] = 0;
            }
        }
        brelse(bp1);
        ext2fs_bfree(ip->dev, ad->addrs[EXT2_TIND_BLOCK]);
        ad->addrs[EXT2_TIND_BLOCK] = 0;
    }

    ip->size = 0;
    ip->iops->iupdate(ip);
}

int ext2fs_readi(struct inode *ip, char *dst, u32 off, u32 n)
{
    u32 m;

    if (ip->type == T_DEV) {
        if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
            return -1;
        return devsw[ip->major].read(ip, dst, n);
    }

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > ip->size)
        n = ip->size - off;

    for (u32 tot = 0; tot < n; tot += m, off += m, dst += m) {
        u32 block      = ext2fs_bmap(ip, off / EXT2_BSIZE);
        struct buf *bp = bread(ip->dev, block);
        m              = min(n - tot, EXT2_BSIZE - off % EXT2_BSIZE);
        memmove(dst, bp->data + off % EXT2_BSIZE, m);
        brelse(bp);
    }
    return n;
}

int ext2fs_writei(struct inode *ip, char *src, u32 off, u32 n)
{
    u32 m;

    if (ip->type == T_DEV) {
        if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
            return -1;
        return devsw[ip->major].write(ip, src, n);
    }

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > EXT2_MAXFILE * EXT2_BSIZE)
        return -1;

    for (u32 tot = 0; tot < n; tot += m, off += m, src += m) {
        u32 block      = ext2fs_bmap(ip, off / EXT2_BSIZE);
        struct buf *bp = bread(ip->dev, block);
        m              = min(n - tot, EXT2_BSIZE - off%EXT2_BSIZE);
        memmove(bp->data + off % EXT2_BSIZE, src, m);
        bwrite(bp);
        brelse(bp);
    }

    if (n > 0 && off > ip->size) {
        ip->size = off;
        ip->iops->iupdate(ip);
    }
    return n;
}

int ext2fs_namecmp(const char *s, const char *t)
{
    return strncmp(s, t, EXT2_NAME_LEN);
}

struct inode *ext2fs_dirlookup(struct inode *dp, char *name, u32 *poff)
{
    struct ext2_dir_entry_2 de;
    char file_name[EXT2_NAME_LEN + 1];
    for (u32 off = 0; off < dp->size; off += de.rec_len) {
        if (dp->iops->readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
            panic("ext2fs_dirlookup: read error");
        if (de.inode == 0)
            continue;
        strncpy(file_name, de.name, de.name_len);
        file_name[de.name_len] = '\0';
        if (ext2fs_namecmp(name, file_name) == 0) {
            if (poff)
                *poff = off;
            return iget(dp->dev, de.inode);
        }
    }
    return nullptr;
}

int ext2fs_dirlink(struct inode *dp, char *name, u32 inum)
{
    int off;
    struct ext2_dir_entry_2 de;
    struct inode *ip;

    if ((ip = dp->iops->dirlookup(dp, name, nullptr)) != nullptr) {
        ip->iops->iput(ip);
        return -1;
    }

    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (dp->iops->readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
            panic("ext2fs_dirlink read");
        if (de.inode == 0)
            break;
    }

    de.name_len = strlen(name);
    strncpy(de.name, name, de.name_len);
    de.inode   = inum;
    de.rec_len = EXT2_BSIZE;
    dp->size   = off + de.rec_len;
    dp->iops->iupdate(dp);
    dp->iops->writei(dp, (char *)&de, off, sizeof(de));

    return 0;
}