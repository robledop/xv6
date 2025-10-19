set breakpoint pending on

add-symbol-file ./build/kernel 0x80100000
add-symbol-file ./rootfs/init 0x0

#break trapret
#break vector32
break panic
