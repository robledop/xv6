#pragma once
#include "types.h"
// Routines to let C code use special x86 instructions.

static inline uchar inb(ushort port)
{
    uchar data;

    __asm__ volatile("in %1,%0" : "=a" (data) : "d" (port));
    return data;
}

static inline void insl(int port, void* addr, int cnt)
{
    __asm__ volatile("cld; rep insl" :
        "=D" (addr), "=c" (cnt) :
        "d" (port), "0" (addr), "1" (cnt) :
        "memory", "cc");
}

static inline void
outb(ushort port, uchar data)
{
    __asm__ volatile("out %0,%1" : : "a" (data), "d" (port));
}

static inline void
outw(ushort port, ushort data)
{
    __asm__ volatile("out %0,%1" : : "a" (data), "d" (port));
}

static inline void outsl(int port, const void* addr, int cnt)
{
    __asm__ volatile("cld; rep outsl" :
        "=S" (addr), "=c" (cnt) :
        "d" (port), "0" (addr), "1" (cnt) :
        "cc");
}

static inline void stosb(void* addr, int data, int cnt)
{
    __asm__ volatile("cld; rep stosb" :
        "=D" (addr), "=c" (cnt) :
        "0" (addr), "1" (cnt), "a" (data) :
        "memory", "cc");
}

static inline void stosl(void* addr, int data, int cnt)
{
    __asm__ volatile("cld; rep stosl" :
        "=D" (addr), "=c" (cnt) :
        "0" (addr), "1" (cnt), "a" (data) :
        "memory", "cc");
}

struct segdesc;

static inline void lgdt(struct segdesc* p, int size)
{
    volatile ushort pd[3];

    pd[0] = size - 1;
    pd[1] = (uint)p;
    pd[2] = (uint)p >> 16;

    __asm__ volatile("lgdt (%0)" : : "r" (pd));
}

struct gate_desc;

static inline void lidt(struct gate_desc* p, int size)
{
    volatile ushort pd[3];

    pd[0] = size - 1;
    pd[1] = (uint)p;
    pd[2] = (uint)p >> 16;

    __asm__ volatile("lidt (%0)" : : "r" (pd));
}

static inline void ltr(ushort sel)
{
    __asm__ volatile("ltr %0" : : "r" (sel));
}

static inline uint read_eflags(void)
{
    uint eflags;
    __asm__ volatile("pushfl; popl %0" : "=r" (eflags));
    return eflags;
}

static inline void load_gs(ushort v)
{
    __asm__ volatile("movw %0, %%gs" : : "r" (v));
}

static inline void cli(void)
{
    __asm__ volatile("cli");
}

static inline void sti(void)
{
    __asm__ volatile("sti");
}

static inline uint xchg(volatile uint* addr, uint newval)
{
    uint result;

    // The + in "+m" denotes a read-modify-write operand.
    __asm__ volatile("lock; xchgl %0, %1" :
        "+m" (*addr), "=a" (result) :
        "1" (newval) :
        "cc");
    return result;
}

static inline uint
rcr2(void)
{
    uint val;
    __asm__ volatile("movl %%cr2,%0" : "=r" (val));
    return val;
}

static inline void
lcr3(uint val)
{
    __asm__ volatile("movl %0,%%cr3" : : "r" (val));
}

static inline void hlt(void)
{
    __asm__ volatile("hlt");
}

// Layout of the trap frame built on the stack by the
// hardware and by trapasm.S, and passed to trap().
struct trapframe
{
    // registers as pushed by pusha
    uint edi;
    uint esi;
    uint ebp;
    uint oesp; // useless & ignored
    uint ebx;
    uint edx;
    uint ecx;
    uint eax;

    // rest of trap frame
    ushort gs;
    ushort padding1;
    ushort fs;
    ushort padding2;
    ushort es;
    ushort padding3;
    ushort ds;
    ushort padding4;
    uint trapno;

    // below here defined by x86 hardware
    uint err;
    uint eip;
    ushort cs;
    ushort padding5;
    uint eflags;

    // below here only when crossing rings, such as from user to kernel
    uint esp;
    ushort ss;
    ushort padding6;
};