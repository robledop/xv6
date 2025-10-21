#include "types.h"
#include "x86.h"
#include "defs.h"
#include "kbd.h"

int kbdgetc(void)
{
    static u32 shift;
    static u8 *charcode[4] = {
        normalmap, shiftmap, ctlmap, ctlmap
    };

    u32 st = inb(KBSTATP);
    if ((st & KBS_DIB) == 0)
        return -1;
    u32 data = inb(KBDATAP);

    if (data == 0xE0) {
        shift |= E0ESC;
        return 0;
    } else if (data & 0x80) {
        // Key released
        data = (shift & E0ESC ? data : data & 0x7F);
        shift &= ~(shiftcode[data] | E0ESC);
        return 0;
    } else if (shift & E0ESC) {
        // The last character was an E0 escape; or with 0x80
        data |= 0x80;
        shift &= ~E0ESC;
    }

    shift |= shiftcode[data];
    shift ^= togglecode[data];
    u32 c = charcode[shift & (CTL | SHIFT)][data];
    if (shift & CAPSLOCK) {
        if ('a' <= c && c <= 'z')
            c += 'A' - 'a';
        else if ('A' <= c && c <= 'Z')
            c += 'a' - 'A';
    }
    return c;
}

void kbdintr(void)
{
    consoleintr(kbdgetc);
}