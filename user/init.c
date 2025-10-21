// init: The initial user-level program

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

char *argv[] = {"/bin/sh", nullptr};

int main(void)
{
    int wpid;

    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", 1, 1);
        open("/dev/console", O_RDWR);
    }
    dup(0); // stdout
    dup(0); // stderr

    for (;;) {
        printf(1, "init: starting sh\n");
        int pid = fork();
        if (pid < 0) {
            printf(1, "init: fork failed\n");
            exit();
        }

        if (pid == 0) {
            exec("/bin/sh", argv);
            printf(1, "init: exec sh failed\n");
            exit();
        }

        while ((wpid = wait()) >= 0 && wpid != pid) {
            printf(1, "zombie!\n");
        }
    }
}