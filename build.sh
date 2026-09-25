#!/bin/sh
# Build AmiGrep (CLI + GUI) with amiga-gcc. Run inside WSL.
#   cd /mnt/c/projects/AmiGrep && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-Os -msmall-code -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"

mkdir -p out

echo "== AmiGrep (CLI) =="
$CC $CFLAGS -s -o out/AmiGrep    src/cli.c src/grep.c

echo "== AmiGrepGUI =="
$CC $CFLAGS -s -o out/AmiGrepGUI src/gui.c src/grep.c

# keep an unstripped + disassembly of the GUI for crash mapping
$CC $CFLAGS -g -o out/AmiGrepGUI.dbg src/gui.c src/grep.c
m68k-amigaos-objdump -dS out/AmiGrepGUI.dbg > out/AmiGrepGUI.dis 2>/dev/null || true

# Strip at LINK time with -s (above), NEVER the standalone m68k-amigaos-strip:
# the rebuilt binutils (amiga-2.46) strip CORRUPTS the hunk reloc table even for
# a single file -> wild-jump guru 8000000B. -s uses ld's correct stripping.
# (Same fix as AmiFind; see the reference_amiga_toolchain_wsl memory.)
ls -l out
echo "Done."
