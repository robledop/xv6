#include "types.h"
#include "stat.h"
#include "user.h"
#include "dirwalk.h"

struct ext2_dirent_raw
{
    u32 inode;
    u16 rec_len;
    u8 name_len;
    u8 file_type;
};

int dirwalk(int fd, dirwalk_cb cb, void *arg)
{
    if (cb == nullptr)
        return -1;

    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;
    if (st.type != T_DIR)
        return -1;

    int bufsz = st.size > 0 ? st.size : 1;
    char *data = malloc(bufsz);
    if (data == 0)
        return -1;

    u32 total = 0;
    while (total < st.size) {
        int r = read(fd, data + total, st.size - total);
        if (r < 0) {
            free(data);
            return -1;
        }
        if (r == 0)
            break;
        total += r;
    }
    if (st.size == 0)
        total = 0;

    int result = 0;
    for (u32 off = 0; off + sizeof(struct ext2_dirent_raw) <= (u32)total;) {
        struct ext2_dirent_raw *raw = (struct ext2_dirent_raw *)(data + off);
        if (raw->rec_len < sizeof(struct ext2_dirent_raw))
            break;
        if (off + raw->rec_len > (u32)total)
            break;
        if (raw->inode != 0) {
            struct dirent_view view;
            view.inode     = raw->inode;
            view.file_type = raw->file_type;
            view.name_len  = raw->name_len;
            int name_len   = raw->name_len;
            if (name_len > EXT2_DIRENT_NAME_MAX)
                name_len = EXT2_DIRENT_NAME_MAX;
            memmove(view.name, data + off + sizeof(struct ext2_dirent_raw), name_len);
            view.name[name_len] = 0;
            result = cb(&view, arg);
            if (result != 0)
                break;
        }
        off += raw->rec_len;
        if (raw->rec_len == 0)
            break;
    }

    free(data);
    return result;
}
