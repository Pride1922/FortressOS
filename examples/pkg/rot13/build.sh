#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p build/pkg-rot13
nasm -f elf64 -DTOOL_ENTRY=rot13_main user/tools/start.asm -o build/pkg-rot13/start.o
for release in 1 2; do
  gcc -std=c11 -Os -ffreestanding -fno-pie -fno-builtin -mno-red-zone -mgeneral-regs-only -ffunction-sections -fdata-sections -Werror -Wall -Wextra -Wframe-larger-than=1024 -Isrc/include -Isrc/fs -Iuser/tools -DRELEASE=$release -c examples/pkg/rot13/rot13.c -o build/pkg-rot13/rot13.o
  gcc -std=c11 -Os -ffreestanding -fno-pie -fno-builtin -mno-red-zone -mgeneral-regs-only -ffunction-sections -fdata-sections -Isrc/include -Isrc/fs -Iuser/tools -c user/tools/common.c -o build/pkg-rot13/common.o
  ld -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld build/pkg-rot13/start.o build/pkg-rot13/rot13.o build/pkg-rot13/common.o -o build/pkg-rot13/rot13-v$release.elf
done
