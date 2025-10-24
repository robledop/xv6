#include "stat.h"
#include "user.h"
#include "dirwalk.h"

#define FMTNAME_WIDTH 14
#define PATHBUF_SZ 512

char *fmtname(char *path)
{
    static char buf[FMTNAME_WIDTH + 1];
    char *p;

    // Find first character after last slash.
    for (p = path + strlen(path); p >= path && *p != '/'; p--) {
    }
    p++;

    // Return blank-padded name.
    if (strlen(p) >= FMTNAME_WIDTH)
        return p;
    memmove(buf, p, strlen(p));
    memset(buf + strlen(p), ' ', FMTNAME_WIDTH - strlen(p));
    buf[FMTNAME_WIDTH] = 0;
    return buf;
}

struct ls_ctx
{
    char path[PATHBUF_SZ];
    int base_len;
};

static int ls_visit(const struct dirent_view *entry, void *arg)
{
    struct ls_ctx *ctx = (struct ls_ctx *)arg;

    if (ctx->base_len + entry->name_len + 1 >= PATHBUF_SZ) {
        printf(1, "ls: path too long\n");
        return 0;
    }

    memmove(ctx->path + ctx->base_len, entry->name, entry->name_len);
    ctx->path[ctx->base_len + entry->name_len] = 0;

    struct stat st;
    if (stat(ctx->path, &st) < 0) {
        printf(1, "ls: cannot stat %s\n", ctx->path);
        return -1;
    }

    printf(1, "%s %d %d %d\n", fmtname(ctx->path), st.type, st.ino, st.size);
    return 0;
}

void ls(char *path)
{
    int fd;
    struct stat st;

    if ((fd = open(path, 0)) < 0) {
        printf(2, "ls: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        printf(2, "ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (st.type) {
    case T_FILE:
        printf(1, "%s %d %d %d\n", fmtname(path), st.type, st.ino, st.size);
        break;

    case T_DIR: {
        struct ls_ctx ctx;
        if (strlen(path) + 1 + EXT2_DIRENT_NAME_MAX + 1 > sizeof(ctx.path)) {
            printf(1, "ls: path too long\n");
            break;
        }
        strcpy(ctx.path, path);
        ctx.base_len = strlen(ctx.path);
        if (ctx.base_len == 0 || ctx.path[ctx.base_len - 1] != '/') {
            ctx.path[ctx.base_len++] = '/';
            ctx.path[ctx.base_len]   = 0;
        }
        if (dirwalk(fd, ls_visit, &ctx) < 0)
            printf(1, "ls: cannot read directory %s\n", path);
        break;
    }
    default: ;
        printf(1, "ls: unknown type %d for %s\n", st.type, path);
    }
    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        ls(".");
        exit();
    }
    for (int i = 1; i < argc; i++)
        ls(argv[i]);
    exit();
}