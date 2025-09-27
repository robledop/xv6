// Mutual exclusion spin locks.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"

/**
 * @brief Initialize a spinlock with the provided debug name.
 *
 * @param lk Spinlock to initialize.
 * @param name Identifier for diagnostics and panic messages.
 */
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

/**
 * @brief Acquire a spinlock, spinning until it becomes available.
 *
 * Disables interrupts on the current CPU to prevent deadlocks and records
 * the owning CPU and caller PCs for debugging.
 */
void
acquire(struct spinlock *lk)
{
  pushcli(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

  // The xchg is atomic.
  while(xchg(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen after the lock is acquired.
  __sync_synchronize();

  // Record info about lock acquisition for debugging.
  lk->cpu = mycpu();
  getcallerpcs(&lk, lk->pcs);
}

/**
 * @brief Release a spinlock and restore interrupts if appropriate.
 */
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->pcs[0] = 0;
  lk->cpu = 0;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other cores before the lock is released.
  // Both the C compiler and the hardware may re-order loads and
  // stores; __sync_synchronize() tells them both not to.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code can't use a C assignment, since it might
  // not be atomic. A real OS would use C atomics here.
  asm volatile("movl $0, %0" : "+m" (lk->locked) : );

  popcli();
}

/**
 * @brief Capture the current call stack by walking the frame pointer chain.
 *
 * @param v Starting frame pointer (obtained from the caller).
 * @param pcs Output array of return addresses; unfilled entries set to zero.
 */
void
getcallerpcs(void *v, uint pcs[])
{
  int i;

  uint* ebp = (uint*)v - 2;
  for(i = 0; i < 10; i++){
    if(ebp == 0 || ebp < (uint*)KERNBASE || ebp == (uint*)0xffffffff)
      break;
    pcs[i] = ebp[1];     // saved %eip
    ebp = (uint*)ebp[0]; // saved %ebp
  }
  for(; i < 10; i++)
    pcs[i] = 0;
}

/**
 * @brief Check whether the current CPU holds a spinlock.
 *
 * @return Non-zero if held by this CPU, zero otherwise.
 */
int
holding(struct spinlock *lock)
{
  pushcli();
  int r = lock->locked && lock->cpu == mycpu();
  popcli();
  return r;
}


/**
 * @brief Disable interrupts with nesting semantics to pair with popcli().
 */
void
pushcli(void)
{
  int eflags = read_eflags();
  cli();
  if(mycpu()->ncli == 0)
    mycpu()->interrupts_enabled = eflags & FL_IF;
  mycpu()->ncli += 1;
}

/**
 * @brief Restore interrupts when the outermost pushcli() is unwound.
 */
void
popcli(void)
{
  if(read_eflags()&FL_IF)
    panic("popcli - interruptible");
  if(--mycpu()->ncli < 0)
    panic("popcli");
  if(mycpu()->ncli == 0 && mycpu()->interrupts_enabled)
    sti();
}

