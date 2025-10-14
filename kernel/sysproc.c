#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

/** @brief System call wrapper for ::fork. */
int sys_fork(void)
{
    return fork();
}

/** @brief System call wrapper for ::exit. */
int sys_exit(void)
{
    exit();
    return 0; // not reached
}

/** @brief System call wrapper for ::wait. */
int sys_wait(void)
{
    return wait();
}

/** @brief Terminate a process by PID (syscall handler). */
int sys_kill(void)
{
    int pid;

    if (argint(0, &pid) < 0)
        return -1;
    return kill(pid);
}

/** @brief Return the current process ID. */
int sys_getpid(void)
{
    return myproc()->pid;
}

/**
 * @brief Adjust process memory size by a delta (syscall handler).
 *
 * @return Previous end-of-heap address or ::-1 on error.
 */
int sys_sbrk(void)
{
    int n;

    if (argint(0, &n) < 0)
        return -1;
    int addr = myproc()->size;
    if (growproc(n) < 0)
        return -1;
    return addr;
}

/**
 * @brief Sleep for a number of clock ticks (syscall handler).
 *
 * @return 0 on success, -1 if interrupted.
 */
int sys_sleep(void)
{
    int n;

    if (argint(0, &n) < 0)
        return -1;
    acquire(&tickslock);
    uint ticks0 = ticks;
    while (ticks - ticks0 < n)
    {
        if (myproc()->killed)
        {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

/**
 * @brief Return the number of clock ticks since boot.
 */
int sys_uptime(void)
{
    acquire(&tickslock);
    uint xticks = ticks;
    release(&tickslock);
    return xticks;
}