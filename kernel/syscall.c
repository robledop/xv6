#include "types.h"
#include "defs.h"
#include "proc.h"
#include "x86.h"
#include "syscall.h"

// User code makes a system call with INT T_SYSCALL.
// System call number in %eax.
// Arguments on the stack, from the user call to the C
// library system call function. The saved user %esp points
// to a saved program counter, and then the first argument.

/**
 * @brief Copy an integer from user space into the kernel.
 *
 * @param addr User virtual address to read.
 * @param ip Destination pointer in kernel space.
 * @return 0 on success, -1 if the address is invalid.
 */
int fetchint(u32 addr, int *ip)
{
    struct proc *curproc = myproc();

    if (addr >= curproc->size || addr + 4 > curproc->size)
        return -1;
    *ip = *(int *)(addr);
    return 0;
}

/**
 * @brief Validate a user string pointer and return its length.
 *
 * @param addr User address of the string.
 * @param pp Receives the user pointer.
 * @return Length excluding terminator, or -1 if invalid.
 */
int fetchstr(u32 addr, char **pp)
{
    struct proc *curproc = myproc();

    if (addr >= curproc->size)
        return -1;
    *pp      = (char *)addr;
    char *ep = (char *)curproc->size;
    for (char *s = *pp; s < ep; s++) {
        if (*s == 0)
            return s - *pp;
    }
    return -1;
}

/**
 * @brief Fetch the n-th 32-bit system call argument as an integer.
 *
 * @param n Argument index.
 * @param ip Destination pointer.
 * @return 0 on success, -1 on failure.
 */
int argint(int n, int *ip)
{
    return fetchint((myproc()->trap_frame->esp) + 4 + 4 * n, ip);
}

/**
 * @brief Fetch a pointer-sized argument, validating a buffer of given size.
 *
 * @param n Argument index.
 * @param pp Receives the user pointer.
 * @param size Size in bytes that must fit within process memory.
 * @return 0 on success, -1 on failure.
 */
int argptr(int n, char **pp, int size)
{
    int i;
    struct proc *curproc = myproc();

    if (argint(n, &i) < 0)
        return -1;
    if (size < 0 || (u32)i >= curproc->size || (u32)i + size > curproc->size)
        return -1;
    *pp = (char *)i;
    return 0;
}

/**
 * @brief Fetch a string argument, ensuring it is valid and terminated.
 *
 * @param n Argument index.
 * @param pp Receives the user string pointer.
 * @return 0 on success, -1 on failure.
 */
int argstr(int n, char **pp)
{
    int addr;
    if (argint(n, &addr) < 0)
        return -1;
    return fetchstr(addr, pp);
}

extern int sys_chdir(void);
extern int sys_close(void);
extern int sys_dup(void);
extern int sys_exec(void);
extern int sys_exit(void);
extern int sys_fork(void);
extern int sys_fstat(void);
extern int sys_getpid(void);
extern int sys_kill(void);
extern int sys_link(void);
extern int sys_mkdir(void);
extern int sys_mknod(void);
extern int sys_open(void);
extern int sys_pipe(void);
extern int sys_read(void);
extern int sys_sbrk(void);
extern int sys_sleep(void);
extern int sys_unlink(void);
extern int sys_wait(void);
extern int sys_write(void);
extern int sys_uptime(void);

/** @brief Dispatch table mapping syscall numbers to handlers. */
static int (*syscalls[])(void) = {
    [SYS_fork] = sys_fork,
    [SYS_exit] = sys_exit,
    [SYS_wait] = sys_wait,
    [SYS_pipe] = sys_pipe,
    [SYS_read] = sys_read,
    [SYS_kill] = sys_kill,
    [SYS_exec] = sys_exec,
    [SYS_fstat] = sys_fstat,
    [SYS_chdir] = sys_chdir,
    [SYS_dup] = sys_dup,
    [SYS_getpid] = sys_getpid,
    [SYS_sbrk] = sys_sbrk,
    [SYS_sleep] = sys_sleep,
    [SYS_uptime] = sys_uptime,
    [SYS_open] = sys_open,
    [SYS_write] = sys_write,
    [SYS_mknod] = sys_mknod,
    [SYS_unlink] = sys_unlink,
    [SYS_link] = sys_link,
    [SYS_mkdir] = sys_mkdir,
    [SYS_close] = sys_close,
};

/**
 * @brief Entry point for servicing a system call from user mode.
 */
void syscall(void)
{
    struct proc *curproc = myproc();

    int num = curproc->trap_frame->eax;
    if (num > 0 && num < (int)NELEM(syscalls) && syscalls[num]) {
        curproc->trap_frame->eax = syscalls[num]();
    } else {
        cprintf("%d %s: unknown sys call %d\n",
                curproc->pid,
                curproc->name,
                num);
        curproc->trap_frame->eax = -1;
    }
}