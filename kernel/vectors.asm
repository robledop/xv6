%include "mmu.asm"

; Define a macro for interrupt vectors
; Interrupts 8, 10, 11, 12, 13, 14, 17, 18, 19 push error codes automatically
%macro vector 1
global vector%1
vector%1:
  %if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 18 && %1 != 19
    ; No error code, so push a dummy value
    push dword 0
  %endif
  push dword %1 ; Push the interrupt number

  jmp alltraps

%endmacro

alltraps:
  push ds
  push es
  push fs
  push gs
  pushad

  ; Set up data segments.
  mov ax, (SEG_KDATA<<3)
  mov ds, ax
  mov es, ax

  ; Call trap(tf), where tf=%esp
  push esp
  extern trap
  call trap
  add esp, 4

; Return falls through to trapret...
global trapret:function
trapret:
  popad
  pop gs
  pop fs
  pop es
  pop ds
  add esp, 0x8  ; remove trapno and errcode from stack
  iretd

; Generate all 256 interrupt vectors
%assign i 0
%rep 256
  vector i
  %assign i i+1
%endrep

; vector table
section .data
global vectors
vectors:
%assign i 0
%rep 256
  dd vector%[i]
  %assign i i+1
%endrep
