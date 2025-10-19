KERNEL_CODE_SELECTOR equ 0x08
KERNEL_DATA_SELECTOR equ 0x10

global gdt_flush
gdt_flush:
    ; Reload the segment registers with the new selectors
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to reload the code segment selector and flush the pipeline
    jmp KERNEL_CODE_SELECTOR:flush_label

flush_label:
    ret
