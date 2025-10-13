#pragma once
// Format of an ELF executable file

#include <types.h>

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian

// File header
struct elfhdr
{
    uint magic; // must equal ELF_MAGIC
    uchar elf[12];
    ushort type;
    ushort machine;
    uint version;
    uint entry;
    uint phoff;
    uint shoff;
    uint flags;
    ushort ehsize;
    ushort phentsize;
    ushort phnum;
    ushort shentsize;
    ushort shnum;
    ushort shstrndx;
};

// Program section header
struct proghdr
{
    uint type;
    uint off;
    uint vaddr;
    uint paddr;
    uint filesz;
    uint memsz;
    uint flags;
    uint align;
};

// Values for Proghdr type
#define ELF_PROG_LOAD           1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4
#define EI_NIDENT 16

typedef ushort elf32_half;
typedef uint elf32_word;
typedef int elf32_sword;
typedef uint elf32_addr;
typedef int elf32_off;

#define SHN_UNDEF 0

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2

#define ELF32_ST_TYPE(info) ((info)&0xF)

/// @brief Program header
struct elf32_phdr {
    elf32_word p_type;   // Type of segment
    elf32_off p_offset;  // Offset in file
    elf32_addr p_vaddr;  // Virtual address in memory
    elf32_addr p_paddr;  // Reserved
    elf32_word p_filesz; // Size of segment in file
    elf32_word p_memsz;  // Size of segment in memory
    elf32_word p_flags;  // Segment flags
    elf32_word p_align;  // Alignment of segment
} __attribute__((packed));

/// @brief Section header
struct elf32_shdr {
    elf32_word sh_name;
    elf32_word sh_type;
    elf32_word sh_flags;
    elf32_addr sh_addr;
    elf32_off sh_offset;
    elf32_word sh_size;
    elf32_word sh_link;
    elf32_word sh_info;
    elf32_word sh_addralign;
    elf32_word sh_entsize;
} __attribute__((packed));

struct elf_header {
    unsigned char e_ident[EI_NIDENT];
    elf32_half e_type;
    elf32_half e_machine;
    elf32_word e_version;
    elf32_addr e_entry;
    elf32_off e_phoff;
    elf32_off e_shoff;
    elf32_word e_flags;
    elf32_half e_ehsize;
    elf32_half e_phentsize;
    elf32_half e_phnum;
    elf32_half e_shentsize;
    elf32_half e_shnum;
    elf32_half e_shstrndx;
} __attribute__((packed));

/// @brief Dynamic section entry
struct elf32_dyn {
    elf32_sword d_tag;
    union {
        elf32_word d_val;
        elf32_addr d_ptr;
    } d_un;

} __attribute__((packed));

/// @brief Symbol table entry
struct elf32_sym {
    elf32_word st_name;
    elf32_addr st_value;
    elf32_word st_size;
    unsigned char st_info;
    unsigned char st_other;
    elf32_half st_shndx;
} __attribute__((packed));
