PATH := $(HOME)/opt/cross/bin:$(PATH)
export PATH

K=kernel
U=user

$(shell mkdir -p build)
$(shell mkdir -p $(U)/build)

OBJS = \
	build/bio.o\
	build/console.o\
	build/exec.o\
	build/file.o\
	build/fs.o\
	build/ide.o\
	build/ioapic.o\
	build/kalloc.o\
	build/kbd.o\
	build/lapic.o\
	build/log.o\
	build/main.o\
	build/mp.o\
	build/picirq.o\
	build/pipe.o\
	build/proc.o\
	build/sleeplock.o\
	build/spinlock.o\
	build/string.o\
	build/swtch.o\
	build/syscall.o\
	build/sysfile.o\
	build/sysproc.o\
	build/trapasm.o\
	build/trap.o\
	build/uart.o\
	build/vectors.o\
	build/vm.o\

TOOLPREFIX = i686-elf-

QEMU = qemu-system-i386

CC = $(TOOLPREFIX)gcc
AS = nasm
LD = $(TOOLPREFIX)ld
INCLUDE = -I./include
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -O3 -Wall -MD -ggdb -m32 -fno-omit-frame-pointer
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
CFLAGS += $(INCLUDE)
#ASFLAGS = -m32 -gdwarf-2 -Wa,-divide
ASFLAGS += $(INCLUDE)
LDFLAGS += -m elf_i386
CFLAGS += -fno-pie -no-pie


asm_headers:
	./scripts/c_to_nasm.sh ./include syscall.asm traps.asm memlayout.asm mmu.asm asm.asm param.asm

xv6.img: build/bootblock build/kernel
	dd if=/dev/zero of=xv6.img count=10000
	dd if=./build/bootblock of=xv6.img conv=notrunc
	dd if=./build/kernel of=xv6.img seek=1 conv=notrunc

xv6memfs.img: build/bootblock build/kernelmemfs
	dd if=/dev/zero of=xv6memfs.img count=10000
	dd if=./build/bootblock of=xv6memfs.img conv=notrunc
	dd if=./build/kernelmemfs of=xv6memfs.img seek=1 conv=notrunc

build/bootblock: $K/bootasm.asm $K/bootmain.c asm_headers
	$(CC) $(CFLAGS) -fno-pic -O -nostdinc -I. -c $K/bootmain.c -o build/bootmain.o
	$(AS) $(ASFLAGS) -f elf $K/bootasm.asm -o build/bootasm.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7C00 -o build/bootblock.o build/bootasm.o build/bootmain.o
	$(OBJCOPY) -S -O binary -j .text build/bootblock.o build/bootblock
	./scripts/sign.pl ./build/bootblock

build/entryother: $K/entryother.asm asm_headers
	$(AS) $(ASFLAGS) -f elf $K/entryother.asm -o build/entryother.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o build/bootblockother.o build/entryother.o
	$(OBJCOPY) -S -O binary -j .text build/bootblockother.o build/entryother

$U/build/initcode: $U/initcode.asm asm_headers
	$(AS) $(ASFLAGS) -f elf $U/initcode.asm -o $U/build/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/build/initcode.out $U/build/initcode.o
	$(OBJCOPY) -S -O binary $U/build/initcode.out $U/build/initcode

build/kernel: $(OBJS) build/entry.o build/entryother $U/build/initcode asm_headers
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o build/kernel build/entry.o $(OBJS) -b binary $U/build/initcode build/entryother

build/%.o: $K/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: $K/%.asm asm_headers
	$(AS) $(ASFLAGS) -f elf $< -o $@

# kernelmemfs is a copy of kernel that maintains the
# disk image in memory instead of writing to a disk.
# This is not so useful for testing persistent storage or
# exploring disk buffering implementations, but it is
# great for testing the kernel on real hardware without
# needing a scratch disk.
MEMFSOBJS = $(filter-out build/ide.o,$(OBJS)) build/memide.o
build/kernelmemfs: $(MEMFSOBJS) build/entry.o build/entryother $U/build/initcode fs.img
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o build/kernelmemfs build/entry.o  $(MEMFSOBJS) -b binary $U/build/initcode build/entryother fs.img


ULIB = $U/build/ulib.o $U/build/usys.o $U/build/printf.o $U/build/umalloc.o

$(ULIB): 
	$(CC) $(CFLAGS) -c -o $U/build/ulib.o $U/ulib.c
	$(AS) $(ASFLAGS) -f elf $U/usys.asm -o $U/build/usys.o
	$(CC) $(CFLAGS) -c -o $U/build/printf.o $U/printf.c
	$(CC) $(CFLAGS) -c -o $U/build/umalloc.o $U/umalloc.c

$U/build/%.o: $U/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$U/build/_%: $U/build/%.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^

$U/build/_forktest: $U/build/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/build/_forktest $U/build/forktest.o $U/build/ulib.o $U/build/usys.o

mkfs/mkfs: mkfs/mkfs.c 
	rm -f mkfs/*.h
	cp ./include/fs.h ./mkfs/fs.h
	cp ./include/types.h ./mkfs/types.h
	cp ./include/stat.h ./mkfs/stat.h
	cp ./include/param.h ./mkfs/param.h
	gcc -Werror -Wall -o mkfs/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# https://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
# .PRECIOUS: %.o

UPROGS=\
	$U/build/_cat\
	$U/build/_echo\
	$U/build/_forktest\
	$U/build/_grep\
	$U/build/_init\
	$U/build/_kill\
	$U/build/_ln\
	$U/build/_ls\
	$U/build/_mkdir\
	$U/build/_rm\
	$U/build/_sh\
	$U/build/_stressfs\
	$U/build/_usertests\
	$U/build/_wc\
	$U/build/_zombie\

fs.img: mkfs/mkfs $(UPROGS)
	./mkfs/mkfs fs.img $(UPROGS)

-include build/*.d $U/build/*.d


bochs : fs.img xv6.img
	if [ ! -e .bochsrc ]; then ln -s dot-bochsrc .bochsrc; fi
	bochs -q

CPUS := 1
QEMUEXTRA := -display gtk,zoom-to-fit=on,gl=off,window-close=on,grab-on-hover=off
QEMUGDB = -S -gdb tcp::1234
QEMUOPTS = -drive file=fs.img,index=1,media=disk,format=raw -drive file=xv6.img,index=0,media=disk,format=raw -smp $(CPUS) -m 512

qemu: fs.img xv6.img
	$(QEMU) -serial mon:stdio $(QEMUOPTS) $(QEMUEXTRA)

qemu-memfs: xv6memfs.img
	$(QEMU) -drive file=xv6memfs.img,index=0,media=disk,format=raw -smp $(CPUS) -m 512

qemu-nox: fs.img xv6.img
	$(QEMU) -nographic $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl
	sed "s/localhost:1234/localhost:$(GDBPORT)/" < $^ > $@

qemu-gdb: fs.img xv6.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -daemonize $(QEMUOPTS) $(QEMUGDB) $(QEMUEXTRA)

qemu-nox-gdb: fs.img xv6.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -nographic $(QEMUOPTS) $(QEMUGDB)

clean:
	echo "Cleaning up..."
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*.o *.d *.asm *.sym vectors.S bootblock entryother \
	initcode initcode.out kernel/*.o kernel/*.d user/*.o user/*.d xv6.img fs.img kernelmemfs \
	xv6memfs.img mkfs/mkfs mkfs/*.h .gdbinit \
	$(UPROGS)
	rm -rf build
	rm -rf $(U)/build
