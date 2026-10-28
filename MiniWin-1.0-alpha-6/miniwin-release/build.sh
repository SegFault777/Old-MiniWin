#!/bin/bash
set -e
cd "$(dirname "$0")"

BUILD=build
mkdir -p $BUILD

echo "[1/5] Assembling bootloader..."
nasm -f bin boot/boot.asm -o $BUILD/boot.bin

echo "[2/5] Assembling kernel entry stub..."
nasm -f elf32 kernel/kentry.asm -o $BUILD/kentry.o

echo "[3/5] Compiling C kernel..."
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc \
    -Wall -Wextra -O1 -c kernel/kernel.c -o $BUILD/kernel.o

echo "[4/5] Linking kernel..."
ld -m elf_i386 -T kernel/link.ld -nostdlib \
    -o $BUILD/kernel.elf $BUILD/kentry.o $BUILD/kernel.o
objcopy -O binary $BUILD/kernel.elf $BUILD/kernel.bin

echo "[5/5] Building final disk image..."
# Bootloader is exactly 1 sector (512 bytes). Kernel follows on subsequent sectors.
# Pad kernel to a multiple of 512 bytes, then pad whole image to 64 sectors (32KB) + 1.
cp $BUILD/boot.bin $BUILD/os-image.img
cat $BUILD/kernel.bin >> $BUILD/os-image.img

# pad image to at least 65 sectors (bootloader + 64 kernel sectors)
python3 - <<'EOF'
import os
path = "build/os-image.img"
target = 512 * 65
size = os.path.getsize(path)
if size < target:
    with open(path, "ab") as f:
        f.write(b"\x00" * (target - size))
print(f"Final image size: {os.path.getsize(path)} bytes")
EOF

echo "Build complete: $BUILD/os-image.img"
ls -la $BUILD/os-image.img
