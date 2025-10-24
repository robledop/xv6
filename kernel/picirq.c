#include "x86.h"

// I/O Addresses of the two programmable interrupt controllers
#define IO_PIC1 0x20 // Master (IRQs 0-7)
#define IO_PIC2 0xA0 // Slave (IRQs 8-15)

/** @brief Disable legacy 8259A PIC interrupts in favor of the I/O APIC. */
void picinit(void)
{
    // mask all interrupts
    outb(IO_PIC1 + 1, 0xFF);
    outb(IO_PIC2 + 1, 0xFF);
}