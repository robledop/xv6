%include "syscall.asm"
%include "traps.asm"

section .text

%macro SYSCALL 1
global %1
%1:
    mov eax, SYS_%1
    int T_SYSCALL
    ret
%endmacro

SYSCALL fork
SYSCALL exit

; NASM treats "wait" as the legacy FPU instruction mnemonic, so it cannot be
; used directly as a label. Provide the wrapper under wait_ and alias it from
; C headers.
global wait_
wait_:
    mov eax, SYS_wait
    int T_SYSCALL
    ret

SYSCALL pipe
SYSCALL read
SYSCALL write
SYSCALL close
SYSCALL kill
SYSCALL exec
SYSCALL open
SYSCALL mknod
SYSCALL unlink
SYSCALL fstat
SYSCALL link
SYSCALL mkdir
SYSCALL chdir
SYSCALL dup
SYSCALL getpid
SYSCALL sbrk
SYSCALL sleep
SYSCALL uptime
