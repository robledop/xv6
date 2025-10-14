; Define a macro for interrupt vectors
; Interrupts 8, 10, 11, 12, 13, 14, 17, 18, 19 push error codes automatically
%macro vector 1
global vector%1
vector%1:
  %if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 18 && %1 != 19
    push 0
  %endif
  push %1
  extern alltraps
  jmp alltraps
%endmacro

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
