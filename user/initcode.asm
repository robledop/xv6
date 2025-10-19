; Initial process execs /init.
; This code runs in user space.

%include "syscall.asm"
%include "traps.asm"

;# exec(init, argv)
global start
start:
  push argv
  push init
  push 0  ; where caller pc would be
  mov eax, SYS_exec
  int T_SYSCALL

;# for(;;) exit();
exit:
  mov eax, SYS_exit
  int T_SYSCALL
  jmp exit

;# char init[] = "/bin/init\0";
init:
  db "/init", 0

;# char *argv[] = { init, 0 };
;.p2align 2
align 4
argv:
  dd init
  dd 0

