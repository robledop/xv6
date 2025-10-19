#!/bin/bash

set -euo pipefail

MOUNT_POINT="${MOUNT_POINT:-/mnt/xv6}"

#sudo -S mkdir -p "$MOUNT_POINT"

rm -rf ./disk.img

dd if=/dev/zero of=./disk.img bs=512 count=65536

# Create MBR partition table and a single bootable partition
parted -s ./disk.img mklabel msdos
parted -s ./disk.img mkpart primary ext2 1MiB 100%
parted -s ./disk.img set 1 boot on

# Set up loop device with partition scanning
ld=$(sudo losetup -fP --show ./disk.img)

sudo mkfs.ext2 "${ld}p1" -L xv6 -b 1024
sudo mount -t ext2 "${ld}p1" "$MOUNT_POINT"

sudo grub-install --target=i386-pc --boot-directory="$MOUNT_POINT/boot" --modules="normal part_msdos ext2 multiboot" "$ld"

sudo cp -r ./rootfs/. "$MOUNT_POINT/"

sudo umount "$MOUNT_POINT"
sudo losetup -d "$ld"