PATH := $(HOME)/opt/cross/bin:$(PATH)
export PATH

K=kernel
U=user

$(shell mkdir -p build)
$(shell mkdir -p $(U)/build)
$(shell mkdir -p rootfs/bin)
$(shell mkdir -p rootfs/boot/grub)
$(shell mkdir -p rootfs/dev)
$(shell mkdir -p rootfs/etc)
$(shell touch rootfs/etc/devtab)

# Create the grub.cfg file if it doesn't exist.
ifeq ("$(wildcard rootfs/boot/grub/grub.cfg)","")
    $(shell echo 'set timeout=0' > rootfs/boot/grub/grub.cfg && \
            echo '' >> rootfs/boot/grub/grub.cfg && \
            echo 'menuentry "xv6" {' >> rootfs/boot/grub/grub.cfg && \
            echo '	multiboot /boot/kernel' >> rootfs/boot/grub/grub.cfg && \
            echo '}' >> rootfs/boot/grub/grub.cfg)
endif

$(shell chmod 777 rootfs/etc/devtab)
$(shell echo "# /etc/devtab" > rootfs/etc/devtab && \
		echo "# inum   type    major   minor  # optional notes" >> rootfs/etc/devtab && \
	    echo "9	char	1	1	#/dev/console" >> rootfs/etc/devtab)

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
	build/trap.o\
	build/uart.o\
	build/vectors.o\
	build/vm.o\
	build/ubsan.o\
	build/ssp.o \
	build/gdt.o \
	build/mbr.o \
	build/ext2.o \
	build/math.o \
	build/printf.o \
	build/debug.o

TOOLPREFIX = i686-elf-

QEMU = qemu-system-i386

CC = $(TOOLPREFIX)gcc
AS = nasm
LD = $(TOOLPREFIX)ld
INCLUDE = -I./include
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
CFLAGS = -nostdlib -ffreestanding -fno-pic -static -fno-builtin -fno-strict-aliasing -O3 -Wall -MD -ggdb -m32 -fno-omit-frame-pointer -std=gnu23
CFLAGS += $(INCLUDE)
ASFLAGS += $(INCLUDE)
LDFLAGS += -m elf_i386
CFLAGS += -fno-pie -no-pie

$K/build/%.o: CFLAGS += -fsanitize=undefined -fstack-protector

asm_headers:
	./scripts/c_to_nasm.sh ./include syscall.asm traps.asm memlayout.asm mmu.asm asm.asm param.asm

build/entryother: asm_headers
	$(AS) $(ASFLAGS) -f elf $K/entryother.asm -o build/entryother.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o build/bootblockother.o build/entryother.o
	$(OBJCOPY) -S -O binary -j .text build/bootblockother.o build/entryother

$U/build/initcode: asm_headers
	$(AS) $(ASFLAGS) -f elf $U/initcode.asm -o $U/build/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/build/initcode.out $U/build/initcode.o
	$(OBJCOPY) -S -O binary $U/build/initcode.out $U/build/initcode

build/kernel: $(OBJS) build/entry.o build/entryother $U/build/initcode asm_headers
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o build/kernel build/entry.o $(OBJS) -b binary $U/build/initcode build/entryother

build/%.o: $K/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: $K/%.asm asm_headers
	$(AS) $(ASFLAGS) -f elf $< -o $@

ULIB = $U/build/ulib.o $U/build/usys.o $U/build/printf.o $U/build/umalloc.o $U/build/dirwalk.o

$(ULIB): asm_headers
	$(CC) $(CFLAGS) -c -o $U/build/ulib.o $U/ulib.c
	$(AS) $(ASFLAGS) -f elf $U/usys.asm -o $U/build/usys.o
	$(CC) $(CFLAGS) -c -o $U/build/printf.o $U/printf.c
	$(CC) $(CFLAGS) -c -o $U/build/umalloc.o $U/umalloc.c
	$(CC) $(CFLAGS) -c -o $U/build/dirwalk.o $U/dirwalk.c

$U/build/%.o: $U/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$U/build/%: $U/build/%.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^

$U/build/forktest: $U/build/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/build/forktest $U/build/forktest.o $U/build/ulib.o $U/build/usys.o

UPROGS=\
	$U/build/cat\
	$U/build/echo\
	$U/build/forktest\
	$U/build/grep\
	$U/build/init\
	$U/build/kill\
	$U/build/ln\
	$U/build/ls\
	$U/build/mkdir\
	$U/build/rm\
	$U/build/sh\
	$U/build/stressfs\
	$U/build/usertests\
	$U/build/wc\
	$U/build/zombie\

grub: build/kernel $(UPROGS)
	cp build/kernel ./rootfs/boot/kernel
	cp $(UPROGS) ./rootfs/bin/
	./scripts/create-grub-image.sh

-include build/*.d $U/build/*.d

CPUS := 8
MEMORY := 512
QEMUEXTRA := -display gtk,zoom-to-fit=on,gl=off,window-close=on,grab-on-hover=off
QEMUGDB = -S -gdb tcp::1234 -d int -D qemu.log
QEMUOPTS = -drive file=disk.img,index=0,media=disk,format=raw -smp $(CPUS) -m $(MEMORY)

qemu: grub
	$(QEMU) -serial mon:stdio $(QEMUOPTS) $(QEMUEXTRA)

qemu-nox: grub
	$(QEMU) -nographic $(QEMUOPTS)

qemu-gdb: grub
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -daemonize $(QEMUOPTS) $(QEMUGDB) $(QEMUEXTRA)

qemu-nox-gdb: grub
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -nographic $(QEMUOPTS) $(QEMUGDB)

clean:
	echo "Cleaning up..."
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*.o *.d *.asm *.sym entryother \
	initcode initcode.out kernel/*.o kernel/*.d user/*.o user/*.d  \
	$(UPROGS)

	rm -rf rootfs
	rm -rf build
	rm -rf $(U)/build
