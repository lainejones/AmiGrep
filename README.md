# AmiGrep

A content-search ("grep") tool for AmigaOS 2.04 and newer (tested on 3.1 and 3.2). Recurses a
volume, assign, directory or single file, opens every **text** file, and
reports each line that matches a pattern as `path:line: text`. Ships as a CLI
(`AmiGrep`) and a GadTools GUI (`AmiGrepGUI`).

Companion to [AmiFind](../AmiFind): AmiFind finds files *by name*, AmiGrep
finds text *inside* them — and AmiGrep's optional `FILE` filter reuses the same
AmigaDOS-wildcard name matching, so `AmiGrep TODO WORK: FILE #?.c` greps only
the C sources under `WORK:`.

Built with amiga-gcc (`m68k-amigaos-gcc`) under WSL — pure NDK, no MUI/ReAction.

## Layout

    src/grep.c / grep.h   shared recursive content-search engine + matcher
    src/cli.c             command-line front end
    src/gui.c             GadTools GUI front end
    build.sh              build both with amiga-gcc
    tools/uae_test.py     deploy / headless-test helper for the WinUAE 030 box
    icons/                the shipped icons: AmiGrepGUI.info, and drawer.info
                          (goes beside the package drawer as AmiGrep.info)
    out/                  compiled AmigaOS executables (not in git)

## Installing

Unpack the **.lha** wherever you keep tools (`LhA x AmiGrep-1.1.lha Work:`), open the
AmiGrep drawer and double-click **AmiGrepGUI**. For the CLI version in any Shell, copy it
to `C:`:

    Copy AmiGrep/AmiGrep C:

From the **.zip** instead: a zip can't store AmigaDOS protection bits, so the programs
arrive without their `e` (executable) flag and won't run until you set it:

    Protect AmiGrep/AmiGrep +e
    Protect AmiGrep/AmiGrepGUI +e

## Build

From Windows, inside WSL:

    wsl -e bash -lc 'cd /mnt/c/projects/AmiGrep && sh build.sh'

Outputs `out/AmiGrep` and `out/AmiGrepGUI` (AmigaOS m68k hunk executables) and
copies `icons/AmiGrepGUI.info` beside the GUI.

The release package is an `AmiGrep` drawer holding `AmiGrep`, `AmiGrepGUI`,
`AmiGrepGUI.info` and `README.md`, with `icons/drawer.info` beside the drawer as
`AmiGrep.info` (without it the unpacked drawer is invisible on Workbench). The
CLI `AmiGrep` ships without an icon. `icons/AmiGrepGUI.info` is made by
`makeicon_amigrep.py` in the shared Amiga tools folder next to this project:

    wsl -e bash -lc 'cd /mnt/c/projects/AmiGrep && python3 ../tools/makeicon_amigrep.py icons/AmiGrepGUI.info'

## CLI usage

    AmiGrep PATTERN [PATH] [FILE <namepat>] [CASE] [NAMES]

* `PATTERN` — text to find inside files. Plain text is matched as a
  **case-insensitive substring**. If it contains AmigaDOS wildcards
  (`#?  *  ?  [ ]  ~  |`) it is matched across the **whole line** as
  `#?<pattern>#?` (so it still behaves like "contains").
* `PATH` — volume, assign, directory or single file. Defaults to the
  **current directory**.
* `FILE` — only search files whose name matches this filter (case-insensitive).
  Plain text is a **substring** of the name (`txt` matches `notes.txt`); add
  AmigaDOS wildcards for stricter matching (`#?.c`, `ReadMe#?`).
* `CASE` — make the content match case-sensitive.
* `NAMES` — print only the names of files that contain a match (like `grep -l`).
* Press **Ctrl-C** to abort.

Binary files (those containing a NUL byte) are skipped automatically.

Examples

    AmiGrep TODO                       ; "TODO" anywhere under the current dir
    AmiGrep todo WORK: CASE            ; case-sensitive, under WORK:
    AmiGrep #?error LIBS: FILE #?.guide; wildcard line match in .guide files
    AmiGrep printf WORK:src NAMES      ; just the files that mention printf

## GUI usage

Run `AmiGrepGUI` (from Workbench or Shell). Enter the text to **Find**, an
**In** path (default `SYS:`), optionally a **Files** wildcard and tick **Case**,
then press **Search**. Matching lines appear in the listview as
`path:line: text`; the status line shows the totals. **Single-click** a result
to copy its full `path:line: text` into the scrollable **Hit** gadget at the
bottom (cursor keys scroll it — GadTools lists have no horizontal scroll).
**Double-click a result** to open its containing drawer in Workbench — this
needs `workbench.library` v44 (OS 3.5+); on OS 3.0–3.2 the status line says so
instead of opening anything. **Stop** (or closing the window) aborts a running
search. Matching is identical to the CLI. A **Project** menu (right mouse
button) offers **Iconify** and **Quit**.

## Testing

* **CLI** — runs on the host under vamos (no emulator):

      wsl -e bash -lc 'cd /mnt/c/projects/AmiGrep && ~/amitools-venv/bin/vamos out/AmiGrep TODO out/td'

  `out/td` is a small fixture tree created by the test runs.
* **GUI** — deploy to the WinUAE "clean 030" OS 3.2 box and launch it there
  (see `tools/uae_test.py`). Layout can be checked from the host by screenshot;
  clicking/typing needs a real session (RDP into the desktop, drive WinUAE).

## Notes

* Matching uses `dos.library/ParsePattern[NoCase]` + `MatchPattern[NoCase]`, so
  wildcard mode follows standard AmigaDOS semantics.
* The GUI search is synchronous; the Stop button and close gadget stay
  responsive because the scan pumps Intuition messages between lines.
* The engine keeps its 8 KB read buffer + path/line buffers in an
  `AllocMem`'d context, not on the stack, so deep recursion stays cheap.
* Requires AmigaOS 2.04 or newer: the libraries are opened at v37
  (asl/workbench/icon are optional), and the V39+ features are used only when
  present (`ExAllEnd`, the GUI's memory pool; opening a result's drawer needs
  workbench.library v44, as above). Tested on OS 3.1 and 3.2.
