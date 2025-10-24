// Intel 8250 serial port (UART).

#include "defs.h"
#include "traps.h"
#include "x86.h"

/** @brief Base I/O port for the primary serial interface. */
#define COM1 0x3f8

/** @brief Indicates whether a UART is present and initialized. */
static int uart;

/** @brief Initialize the 8250-compatible UART and announce availability. */
void uartinit(void)
{
    // Turn off the FIFO
    outb(COM1 + 2, 0);

    // 9600 baud, 8 data bits, 1 stop bit, parity off.
    outb(COM1 + 3, 0x80); // Unlock divisor
    outb(COM1 + 0, 115200 / 9600);
    outb(COM1 + 1, 0);
    outb(COM1 + 3, 0x03); // Lock divisor, 8 data bits.
    outb(COM1 + 4, 0);
    outb(COM1 + 1, 0x01); // Enable receive interrupts.

    // If the status is 0xFF, no serial port.
    if (inb(COM1 + 5) == 0xFF) {
        return;
    }
    uart = 1;

    // Acknowledge pre-existing interrupt conditions;
    // enable interrupts.
    inb(COM1 + 2);
    inb(COM1 + 0);
    ioapicenable(IRQ_COM1, 0);

    // Announce that we're here.
    for (char *p = "xv6...\n"; *p; p++) {
        uartputc(*p);
    }
}

/**
 * @brief Send a byte over the serial port, waiting for space as needed.
 *
 * @param c Byte to transmit.
 */
void uartputc(int c)
{
    if (!uart) {
        return;
    }
    for (int i = 0; i < 128 && !(inb(COM1 + 5) & 0x20); i++) {
        microdelay(10);
    }
    outb(COM1 + 0, c);
}

/**
 * @brief Non-blocking read of the next received byte.
 *
 * @return Byte value or -1 if no data is available.
 */
static int uartgetc(void)
{
    if (!uart) {
        return -1;
    }
    if (!(inb(COM1 + 5) & 0x01)) {
        return -1;
    }
    return inb(COM1 + 0);
}

/** @brief UART interrupt handler that feeds the console input path. */
void uartintr(void)
{
    consoleintr(uartgetc);
}