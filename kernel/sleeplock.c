// Sleeping locks

#include "defs.h"
#include "proc.h"
#include "sleeplock.h"

/**
 * @brief Initialize a sleeplock that can block and wake processes.
 *
 * @param lk Sleeplock instance to initialize.
 * @param name Human-readable lock name for debugging.
 */
void initsleeplock(struct sleeplock *lk, char *name)
{
    initlock(&lk->lk, "sleep lock");
    lk->name   = name;
    lk->locked = 0;
    lk->pid    = 0;
}

/**
 * @brief Acquire a sleeplock, sleeping while another process holds it.
 *
 * Records the owner PID to aid debugging.
 */
void acquiresleep(struct sleeplock *lk)
{
    acquire(&lk->lk);
    while (lk->locked) {
        sleep(lk, &lk->lk);
    }
    auto p     = myproc();
    lk->locked = 1;
    lk->pid    = p->pid;
    release(&lk->lk);
}

/** @brief Release a sleeplock and wake any waiters. */
void releasesleep(struct sleeplock *lk)
{
    acquire(&lk->lk);
    lk->locked = 0;
    lk->pid    = 0;
    wakeup(lk);
    release(&lk->lk);
}

/**
 * @brief Check whether the current process holds a sleeplock.
 *
 * @return Non-zero if held by the caller, zero otherwise.
 */
int holdingsleep(struct sleeplock *lk)
{
    acquire(&lk->lk);
    int r = lk->locked && (lk->pid == myproc()->pid);
    release(&lk->lk);
    return r;
}