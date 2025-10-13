#pragma once

#include "elf.h"
#include "multiboot.h"

struct symbol {
    elf32_addr address;
    const char *name;
};


#define FUNCTION_SYMBOL 0x02

void debug_stats(void);
void stack_trace(void);
void init_symbols(const multiboot_info_t *mbd);
char *debug_reserved_end(void);
struct symbol debug_function_symbol_lookup(elf32_addr address);
