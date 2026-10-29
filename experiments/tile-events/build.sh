#!/bin/sh
# Builds the BRISC token sender and the host tools.
#   SFPI     SFPI install (default ~/sfpi), for riscv-tt-elf-g++
#   KMD_DIR  tt-kmd checkout on joelsmith/events-ring (default ../../../tt-kmd)
set -e
cd "$(dirname "$0")"
SFPI=${SFPI:-$HOME/sfpi}
KMD_DIR=${KMD_DIR:-../../../tt-kmd}
SF=$SFPI/compiler/bin
$SF/riscv-tt-elf-g++ -O2 -std=gnu++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffreestanding \
    -fno-builtin -mcpu=tt-bh -march=rv32imf_zba_zbb_zicsr_zifencei -mabi=ilp32f -nostdlib \
    -Wl,-T,brisc.ld -Wl,--no-warn-rwx-segments -o brisc.elf brisc_entry.S brisc.cpp
$SF/riscv-tt-elf-objcopy -O binary brisc.elf brisc.bin
cc -Wall -O2 -I"$KMD_DIR" -o tiledrive tiledrive.c
cc -Wall -O2 -I"$KMD_DIR" -o stats stats.c
