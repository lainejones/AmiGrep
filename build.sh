#!/bin/sh
# Build AmiGrep (CLI + GUI) with amiga-gcc. Run inside WSL.
#   cd /mnt/c/projects/AmiGrep && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-O2 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"

mkdir -p out

echo "== AmiGrep (CLI) =="
$CC $CFLAGS -o out/AmiGrep    src/cli.c src/grep.c

echo "== AmiGrepGUI =="
$CC $CFLAGS -o out/AmiGrepGUI src/gui.c src/grep.c

# keep an unstripped + disassembly of the GUI for crash mapping
$CC $CFLAGS -g -o out/AmiGrepGUI.dbg src/gui.c src/grep.c
m68k-amigaos-objdump -dS out/AmiGrepGUI.dbg > out/AmiGrepGUI.dis 2>/dev/null || true

# NB: strip each file in its OWN invocation. m68k-amigaos-strip (GNU strip 2.39)
# corrupts the hunk relocation table of the 2nd+ file when given several at once,
# producing a binary that jumps to garbage in the C startup (guru #8000000x).
m68k-amigaos-strip out/AmiGrep    || true
m68k-amigaos-strip out/AmiGrepGUI || true
ls -l out
echo "Done."
