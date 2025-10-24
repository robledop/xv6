#include "debug.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "ext2.h"
#include "printf.h"

/** @brief Process table guarded by a spinlock. */
struct
{
    struct spinlock lock;
    int active_count;
    struct proc proc[NPROC];
} ptable;

/** @brief Pointer to the very first user process. */
static struct proc *initproc;


extern pde_t *kpgdir;

/** @brief Next PID to assign during process creation. */
int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

/**
 * @brief Wake up processes sleeping on a specific channel.
 *
 * @param chan Sleep channel to target.
 */
static void wakeup1(void *chan);


static inline void stack_push_pointer(char **stack_pointer, const u32 value)
{
    *(u32 *)stack_pointer -= sizeof(u32); // make room for a pointer
    **(u32 **)stack_pointer = value;      // push the pointer onto the stack
}

/** @brief Initialize the process table lock. */
void pinit(void)
{
    initlock(&ptable.lock, "ptable");
}

/**
 * @brief Return the index of the current CPU.
 *
 * Must be called with interrupts disabled.
 */
int cpuid()
{
    return mycpu() - cpus;
}

/**
 * @brief Return a pointer to the cpu structure for the running CPU.
 *
 * Interrupts must be disabled to prevent migration during lookup.
 */
struct cpu *mycpu(void)
{
    if (read_eflags() & FL_IF)
        panic("mycpu called with interrupts enabled\n");

    int apicid = lapicid();
    // APIC IDs are not guaranteed to be contiguous. Maybe we should have
    // a reverse map, or reserve a register to store &cpus[i].
    for (int i = 0; i < ncpu; ++i) {
        if (cpus[i].apicid == apicid)
            return &cpus[i];
    }
    panic("unknown apicid\n");
}

/**
 * @brief Obtain the currently running process structure.
 *
 * Interrupts are temporarily disabled to avoid rescheduling during the read.
 */
struct proc *myproc(void)
{
    pushcli();
    struct cpu *c  = mycpu();
    struct proc *p = c->proc;
    popcli();
    return p;
}

static struct proc *init_proc(struct proc *p)
{
    // Allocate kernel stack.
    if ((p->kstack = kalloc()) == nullptr) {
        p->state = UNUSED;
        return nullptr;
    }
    char *stack_pointer = p->kstack + KSTACKSIZE;

    // Leave room for the trap frame.
    stack_pointer -= sizeof *p->trap_frame;
    p->trap_frame = (struct trapframe *)stack_pointer;

    // Set up new context to start executing at forkret,
    // which returns to trapret.
    stack_pointer -= 4;
    *(u32 *)stack_pointer = (u32)trapret;

    stack_pointer -= sizeof *p->context;
    p->context = (struct context *)stack_pointer;
    memset(p->context, 0, sizeof *p->context);
    p->context->eip = (u32)forkret;

    return p;
}

/**
 * @brief Allocate and partially initialize a process structure.
 *
 * @return Pointer to the new process or 0 if none are available.
 */
static struct proc *alloc_proc(void)
{
    struct proc *p;

    acquire(&ptable.lock);

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == UNUSED)
            goto found;

    release(&ptable.lock);
    return nullptr;

found:
    p->state = EMBRYO;
    p->pid = nextpid++;

    release(&ptable.lock);

    return init_proc(p);
}

static struct proc *alloc_kernel_proc(struct proc *p, void (*entry_point)(void))
{
    if ((p->kstack = kalloc()) == nullptr) {
        p->state = UNUSED;
        return nullptr;
    }
    char *stack_pointer = p->kstack + KSTACKSIZE;

    stack_push_pointer(&stack_pointer, (u32)entry_point);
    p->page_directory = kpgdir;

    stack_pointer -= sizeof *p->context;
    p->context = (struct context *)stack_pointer;
    memset(p->context, 0, sizeof *p->context);
    p->context->eip = (u32)forkret;
    p->state        = EMBRYO;

    return p;
}

/**
 * @brief Create the initial user process containing initcode.
 */
void user_init(void)
{
    // This name depends on the path of the initcode file. I moved it to the user/build folder
    extern char _binary_user_build_initcode_start[], _binary_user_build_initcode_size[];

    struct proc *p = alloc_proc();

    initproc = p;
    if ((p->page_directory = setupkvm()) == nullptr) {
        panic("user_init: out of memory?");
    }
    inituvm(p->page_directory, _binary_user_build_initcode_start, (int)_binary_user_build_initcode_size);
    p->size = PGSIZE;
    memset(p->trap_frame, 0, sizeof(*p->trap_frame));
    p->trap_frame->cs     = (SEG_UCODE << 3) | DPL_USER;
    p->trap_frame->ds     = (SEG_UDATA << 3) | DPL_USER;
    p->trap_frame->es     = p->trap_frame->ds;
    p->trap_frame->ss     = p->trap_frame->ds;
    p->trap_frame->eflags = FL_IF;
    p->trap_frame->esp    = PGSIZE;
    p->trap_frame->eip    = 0; // beginning of initcode.S

    cprintf("cs: %x\n", p->trap_frame->cs);
    cprintf("ds: %x\n", p->trap_frame->ds);
    cprintf("es: %x\n", p->trap_frame->es);
    cprintf("ss: %x\n", p->trap_frame->ss);
    cprintf("eflags: %x\n", p->trap_frame->eflags);
    cprintf("esp: %x\n", p->trap_frame->esp);
    cprintf("eip: %x\n", p->trap_frame->eip);

    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    // this assignment to p->state lets other cores
    // run this process. the acquire forces the above
    // writes to be visible, and the lock is also needed
    // because the assignment might not be atomic.
    acquire(&ptable.lock);

    p->state = RUNNABLE;

    release(&ptable.lock);
}

/**
 * @brief Grow or shrink the current process's address space.
 *
 * @param n Positive delta to grow, negative to shrink.
 * @return 0 on success, -1 on failure.
 */
int growproc(int n)
{
    struct proc *curproc = myproc();

    u32 sz = curproc->size;
    if (n > 0) {
        if ((sz = allocuvm(curproc->page_directory, sz, sz + n)) == 0)
            return -1;
    } else if (n < 0) {
        if ((sz = deallocuvm(curproc->page_directory, sz, sz + n)) == 0)
            return -1;
    }
    curproc->size = sz;
    switch_uvm(curproc);
    return 0;
}

/**
 * @brief Create a child process that duplicates the current process.
 *
 * The caller must mark the returned process RUNNABLE.
 *
 * @return Child PID in the parent, ::0 in the child, or ::-1 on failure.
 */
int fork(void)
{
    struct proc *np;
    struct proc *curproc = myproc();

    // Allocate process.
    if ((np = alloc_proc()) == nullptr) {
        return -1;
    }

    // Copy process state from proc.
    if ((np->page_directory = copyuvm(curproc->page_directory, curproc->size)) == nullptr) {
        kfree(np->kstack);
        np->kstack = nullptr;
        np->state  = UNUSED;
        return -1;
    }
    np->size        = curproc->size;
    np->parent      = curproc;
    *np->trap_frame = *curproc->trap_frame;

    // Clear %eax so that fork returns 0 in the child.
    np->trap_frame->eax = 0;

    for (int i = 0; i < NOFILE; i++)
        if (curproc->ofile[i])
            np->ofile[i] = filedup(curproc->ofile[i]);
    np->cwd = idup(curproc->cwd);

    safestrcpy(np->name, curproc->name, sizeof(curproc->name));

    int pid = np->pid;

    acquire(&ptable.lock);

    np->state = RUNNABLE;

    release(&ptable.lock);

    return pid;
}

/**
 * @brief Terminate the current process and release associated resources.
 *
 * Transitions the process to ZOMBIE until the parent collects the status with
 * wait.
 */
void exit(void)
{
    struct proc *curproc = myproc();

    if (curproc == initproc)
        panic("init exiting");

    // Close all open files.
    for (int fd = 0; fd < NOFILE; fd++) {
        if (curproc->ofile[fd]) {
            fileclose(curproc->ofile[fd]);
            curproc->ofile[fd] = nullptr;
        }
    }

    // begin_op();
    curproc->cwd->iops->iput(curproc->cwd);
    // end_op();
    curproc->cwd = nullptr;

    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(curproc->parent);

    // Pass abandoned children to init.
    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->parent == curproc) {
            p->parent = initproc;
            if (p->state == ZOMBIE)
                wakeup1(initproc);
        }
    }

    // Jump into the scheduler, never to return.
    curproc->state = ZOMBIE;
    sched();
    panic("zombie exit");
}

/**
 * @brief Wait for a child process to exit and return its PID.
 *
 * @return Child PID on success, ::-1 if no children are alive.
 */
int wait(void)
{
    struct proc *curproc = myproc();

    acquire(&ptable.lock);
    for (;;) {
        // Scan through table looking for exited children.
        int havekids = 0;
        for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->parent != curproc)
                continue;
            havekids = 1;
            if (p->state == ZOMBIE) {
                // Found one.
                int pid = p->pid;
                kfree(p->kstack);
                p->kstack = nullptr;
                freevm(p->page_directory);
                p->pid     = 0;
                p->parent  = nullptr;
                p->name[0] = 0;
                p->killed  = 0;
                p->state   = UNUSED;
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || curproc->killed) {
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(curproc, &ptable.lock); // DOC: wait-sleep
    }
}

/**
 * @brief Per-CPU scheduler loop that selects and runs processes.
 *
 * Never returns; invoked once per CPU during initialization.
 */
void scheduler(void)
{
    struct cpu *current_cpu = mycpu();
    current_cpu->proc       = nullptr;

    for (;;) {
        // Enable interrupts on this processor.
        sti();

        // Loop over the process table looking for a process to run.
        acquire(&ptable.lock);
        ptable.active_count = 0;
        for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->state != RUNNABLE)
                continue;

            ptable.active_count++;

            // Switch to the chosen process.  It is the process's job
            // to release ptable.lock and then reacquire it
            // before jumping back to us.
            current_cpu->proc = p;
            switch_uvm(p);
            p->state = RUNNING;

            switch_context(&(current_cpu->scheduler), p->context);
            switch_kvm();

            // Process is done running for now.
            // It should have changed its p->state before coming back.
            current_cpu->proc = nullptr;
        }

        release(&ptable.lock);

        // Idle "thread"
        if (ptable.active_count == 0) {
            sti();
            hlt();
        }
    }
}

/**
 * @brief Enter the scheduler after marking the current process non-running.
 *
 * Requires ptable.lock to be held and saves/restores interrupt state so the
 * process can resume correctly.
 */
void sched(void)
{
    struct proc *p = myproc();

    if (!holding(&ptable.lock))
        panic("sched ptable.lock");
    if (mycpu()->ncli != 1)
        panic("sched locks");
    if (p->state == RUNNING)
        panic("sched running");
    if (read_eflags() & FL_IF)
        panic("sched interruptible");

    const int interrupts_enabled = mycpu()->interrupts_enabled;
    switch_context(&p->context, mycpu()->scheduler);
    mycpu()->interrupts_enabled = interrupts_enabled;
}

/** @brief Give up the CPU for one scheduling round. */
void yield(void)
{
    acquire(&ptable.lock);
    myproc()->state = RUNNABLE;
    sched();
    release(&ptable.lock);
}

/**
 * @brief Entry point for forked children on their first scheduled run.
 *
 * Releases ptable.lock and performs late initialization before returning to
 * user space via trapret.
 */
void forkret(void)
{
    static int first = 1;
    // Still holding ptable.lock from scheduler.
    release(&ptable.lock);

    if (first) {
        // Some initialization functions must be run in the context
        // of a regular process (e.g., they call sleep), and thus cannot
        // be run from main().
        first = 0;
        // iinit(ROOTDEV);
        // initlog(ROOTDEV);
        ext2fs_iinit(ROOTDEV);
    }

    // Return to "caller", actually trapret (see allocproc).
}

/**
 * @brief Atomically release a lock and put the current process to sleep.
 *
 * @param chan Sleep channel identifier.
 * @param lk Lock currently held by the caller.
 */
void sleep(void *chan, struct spinlock *lk)
{
    struct proc *p = myproc();

    if (p == nullptr) {
        sti();
        return;
        // panic("sleep");
    }

    if (lk == nullptr)
        panic("sleep without lk");

    // Must acquire ptable.lock in order to
    // change p->state and then call sched.
    // Once we hold ptable.lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup runs with ptable.lock locked),
    // so it's okay to release lk.
    if (lk != &ptable.lock) {
        // DOC: sleeplock0
        acquire(&ptable.lock); // DOC: sleeplock1
        release(lk);
    }
    // Go to sleep.
    p->chan  = chan;
    p->state = SLEEPING;

    sched();

    // Tidy up.
    p->chan = nullptr;

    // Reacquire original lock.
    if (lk != &ptable.lock) {
        // DOC: sleeplock2
        release(&ptable.lock);
        acquire(lk);
    }
}

/**
 * @brief Wake all processes sleeping on a channel.
 *
 * Requires ptable.lock to be held.
 */
static void wakeup1(void *chan)
{
    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == SLEEPING && p->chan == chan)
            p->state = RUNNABLE;
}

/**
 * @brief Wake any processes sleeping on @p chan.
 *
 * Acquires and releases ptable.lock internally.
 */
void wakeup(void *chan)
{
    acquire(&ptable.lock);
    wakeup1(chan);
    release(&ptable.lock);
}

/**
 * @brief Request termination of the process with the given PID.
 *
 * The process transitions to killed state and exits upon returning to user
 * space.
 *
 * @param pid Process identifier to terminate.
 * @return 0 on success, -1 if no such process exists.
 */
int kill(int pid)
{
    acquire(&ptable.lock);
    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->pid == pid) {
            p->killed = 1;
            // Wake process from sleep if necessary.
            if (p->state == SLEEPING)
                p->state = RUNNABLE;
            release(&ptable.lock);
            return 0;
        }
    }
    release(&ptable.lock);
    return -1;
}

/**
 * @brief Emit a process table listing for debugging purposes.
 *
 * Invoked via the console ^P handler without acquiring locks to avoid deadlock on
 * wedged systems.
 */
void procdump(void)
{
    static char *states[] = {
        [UNUSED] = "unused",
        [EMBRYO] = "embryo",
        [SLEEPING] = "sleep ",
        [RUNNABLE] = "runnable",
        [RUNNING] = "run   ",
        [ZOMBIE] = "zombie"
    };
    char *state;
    u32 pc[10];

    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state == UNUSED) {
            continue;
        }
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state]) {
            state = states[p->state];
        } else {
            state = "???";
        }
        cprintf("%s, pid: %d, state: %s\n", p->name, p->pid, state);
        cprintf("stack trace:\n");
        if (p->state == SLEEPING) {
            getcallerpcs((u32 *)p->context->ebp + 2, pc);
            for (int i = 0; i < 10 && pc[i] != 0; i++) {
                struct symbol symbol = debug_function_symbol_lookup(pc[i]);
                cprintf("\t[%p] %s\n", pc[i], (symbol.name == nullptr) ? "[unknown]" : symbol.name);
            }
        }
        cprintf("\n");
    }
}