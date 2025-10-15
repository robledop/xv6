; The xv6 kernel starts executing in this file. This file is linked with
; the kernel C code, so it can refer to kernel symbols such as main().
; The boot block (bootasm.S and bootmain.c) jumps to entry below.

; Multiboot header, for multiboot boot loaders like GNU Grub.
; http://www.gnu.org/software/grub/manual/multiboot/multiboot.html
;
; Using GRUB 2, you can boot xv6 from a file stored in a
; Linux file system by copying kernel or kernelmemfs to /boot
; and then adding this menu entry:
;
; menuentry "xv6" {
; 	insmod ext2
; 	set root='(hd0,msdos1)'
; 	set kernel='/boot/kernel'
; 	echo "Loading ${kernel}..."
; 	multiboot ${kernel} ${kernel}
; 	boot
; }

%include "asm.asm"
%include "memlayout.asm"
%include "mmu.asm"
%include "param.asm"

; Declare constants for the multiboot header.
MBALIGN  equ  1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ  1 << 1            ; provide memory map
%ifdef PIXEL_RENDERING
VIDEOMODE equ  1 << 2            ; request video mode
MBFLAGS equ MBALIGN | MEMINFO | VIDEOMODE
%else
MBFLAGS equ MBALIGN | MEMINFO
%endif
MAGIC    equ  0x1BADB002        ; 'magic number' lets bootloader find the header
CHECKSUM equ -(MAGIC + MBFLAGS)   ; checksum of above, to prove we are multiboot

section .multiboot
align 4
	dd MAGIC
	dd MBFLAGS
	dd CHECKSUM
%ifdef PIXEL_RENDERING
    ; Video mode fields (required when VIDEOMODE flag is set)
	dd 0    ; header_addr (not used)
	dd 0    ; load_addr (not used)
	dd 0    ; load_end_addr (not used)
	dd 0    ; bss_end_addr (not used)
	dd 0    ; entry_addr (not used)
	dd 0    ; mode_type (0 = linear graphics mode)
	dd 1024 ; width
	dd 768  ; height
	dd 32   ; depth
%endif

; By convention, the _start symbol specifies the ELF entry point.
; Since we haven't set up virtual memory yet, our entry point is
; the physical address of 'entry'.
global _start
;_start equ V2P_WO(entry)

; Entering xv6 on boot processor, with paging off.
;global entry
_start:
    mov esi, eax ; Save the magic number from grub
    mov edi, ebx ; Save the address of the multiboot info structure from grub

    ; Turn on page size extension for 4Mbyte pages
    mov     eax, cr4
    or      eax, CR4_PSE
    mov     cr4, eax

    ; Set page directory
    extern entrypgdir
    mov     eax, V2P_WO(entrypgdir)
    mov     cr3, eax

    ; Turn on paging.
    mov     eax, cr0
    or      eax, (CR0_PG|CR0_WP)
    mov     cr0, eax

    ; Set up the stack pointer.
    mov esp, (stack + KSTACKSIZE)

    ; Manually set up the call frame for kernel_main
    push esi        ; Second parameter (magic number)
    push edi        ; First parameter (multiboot info)
    push 0          ; Fake return address (kernel_main shouldn't return anyway)

    ; Jump to main(), and switch to executing at
    ; high addresses. The indirect call is needed because
    ; the assembler produces a PC-relative instruction
    ; for a direct jump.
    extern main
    mov eax, main
    jmp eax

section .bss
global stack
stack: resb KSTACKSIZE
