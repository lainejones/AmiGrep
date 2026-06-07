# AmiGrep — Code Review

Reviewed: `src/cli.c`, `src/grep.c`, `src/grep.h`, `src/gui.c`, `build.sh`, `README.md` (all read in full).

## 0. STATUS (2026-06-07 — fixes applied)

Addressed in the working tree and rebuilt:

- **H1 fixed** — `appendNum()` now takes a `bufsize` and never writes past `buf[bufsize-1]`.
- **H2 fixed** — `openResultDrawer()` now requires `WorkbenchBase->lib_Version >= 44` and shows a status message on OS 3.0–3.2 instead of guruing.
- **M1 fixed** — `nameFilterInit()`'s wildcard branch re-parses straight into `nf->parsed` (no truncated `CopyMem`); over-long FILE patterns now fail cleanly.
- **L5 fixed** — dead `overflowed` variable removed from `grepOneFile()`.
- **M3 fixed** — `grepOneFile()` now also polls once per `Read()` block, so files with very long / newline-free lines still honour Stop/Ctrl-C. Verified with `tools/test_engine.c` on a 300 KB single-line file: poll count went 0→1 and the requested abort now fires (`aborted` 0→1).
- **M4 fixed** — binary/text is now decided from the **first block only** (grep's first-buffer heuristic); a binary file is skipped whole before any match is reported, so stats are consistent. Verified: a file with matches followed by a NUL went from `2 line(s) in 0 file(s)` (with 2 bogus hits) to a clean `0 line(s) … (binaries skipped)`; a file whose NUL is only past 8 KB correctly stays text.
- **Size** — `build.sh` switched `-O2` → `-Os -msmall-code`: CLI 5,196 → **4,816 B**, GUI 12,032 → **10,400 B**. CLI re-verified under vamos.

Engine tests: `tools/test_engine.c` links `src/grep.c` and drives `grepTree()` with an instrumented poll/match callback — used to verify M3/M4 under vamos without the GUI.

Still open (not yet addressed): **M2** (iconify-failure hang/UAF — GUI-only, needs WinUAE + manual click to smoke-test) and the remaining Low items below.

**Verified-good things first** (so they don't get "fixed"): the big `struct Ctx` (≈9.7 KB) is `AllocMem`'d off the stack and freed on both exit paths of `grepTree` (grep.c:300, 318, 326); every `recurse()` level allocates its own FIB via `AllocDosObject` and frees it (grep.c:250, 287) — the correct Examine/ExNext pattern, no shared-FIB misuse; every `Lock` has a matching `UnLock` (grep.c:268–271, 315–323); `closeWin()` order is correct (ClearMenuStrip → FreeMenus → CloseWindow → FreeGadgets → FreeVisualInfo → UnlockPubScreen); exit cleanup order in gui.c:710–721 is correct (RemoveAppIcon before draining/deleting the port, libraries last); `__stack = 60000` is honored by the libnix startup and is ample — recursion depth is bounded to ~260 levels by the 520-byte path buffer (AddPart fails first), at ~50 bytes/frame that's ~13 KB worst case.

## 1. ISSUES

### High

**H1. Stack buffer overflow in `guiFound()` — gui.c:172–183.**
`buf[300]` receives the path via `strncpy` (up to 299 chars; `c->path` allows 519). Then:

```c
l = (int)strlen(buf);
if (l < (int)sizeof buf - 2) { buf[l++] = ':'; buf[l] = '\0'; appendNum(buf, line); }
```

The guard only ensures room for the `':'`. `appendNum()` (gui.c:117–126) appends up to 10 digits with **no bound check**. With a path of 290–297 chars and a large line number, digits are written at `buf[298..307]` plus a NUL at `buf[308]` — up to 9 bytes past the stack buffer, smashing saved registers/return address on a machine with no MMU protection. The second append (`": " + text`) at gui.c:182–183 is correctly bounded via `strncat`; only the `appendNum` path is unsafe. Fix: pass a remaining-space limit to `appendNum`, or require `l < sizeof buf - 12` before calling it.

**H2. `OpenWorkbenchObject()` is a V44 function, but workbench.library is opened at v37 — gui.c:591 vs gui.c:702.**
`OpenWorkbenchObjectA` was added in workbench.library V44 (OS 3.5). On OS 3.0/3.1 (V39/V40), `OpenLibrary("workbench.library", 37)` succeeds, the only guard is `if (!WorkbenchBase)` (gui.c:587), and the call jumps through a nonexistent LVO → guru. README claims "AmigaOS 3.2 (and any 3.x)". Fix: check `WorkbenchBase->lib_Version >= 44` before calling (keep v37 open for `AddAppIconA`, which is V36 and fine).

### Medium

**M1. `nameFilterInit()` truncates the parsed token stream — grep.c:121–126.**
The wildcard branch parses into a 600-byte `probe`, then `CopyMem(probe, nf->parsed, sizeof nf->parsed)` copies only the first 300 bytes. A tokenized pattern needs up to `2*len+2` bytes, so a CLI `FILE` pattern longer than ~149 chars parses successfully into `probe` but is **truncated mid-stream** in `nf->parsed`; `MatchPatternNoCase` then walks an unterminated token buffer — undefined behavior (misclassification or crash). The GUI is safe only by accident (`GTST_MaxChars, 100` → ≤202 bytes). Fix: parse directly into `nf->parsed` with `sizeof nf->parsed` and treat failure as over-long, same as the non-wildcard branch does.

**M2. Iconify failure paths can hang the GUI forever, plus latent use-after-free — gui.c:529, 534–541, 599–609, 503–511.**
- `doIconify()`: if `AddAppIconA` fails, `buildWindow(g)` is called with no check (gui.c:529). If *that* fails (screen lock/window open failure), there is no window and no AppIcon, but `g->appPort` exists — so `eventLoop`'s escape hatch `if (!winSig && !appSig) break;` never triggers and the task `Wait()`s on a port nothing will ever signal. Unkillable except via task-killer.
- `doUniconify()` (gui.c:540): same unchecked `buildWindow()`.
- Related latent bug: `closeWin()` NULLs `win/glist/vi/scr` but **not** `gPattern/gPath/gStatus/gList/gHit`. `setStatus()` guards only on `g->gStatus` (gui.c:161), so any future call while iconified would `GT_SetGadgetAttrs` a freed gadget with a NULL window. No current call path does this, but it's one refactor away. NULL the gadget pointers in `closeWin()` and guard `setStatus` on `g->win`.

**M3. Abort/poll only fires on newline boundaries — grep.c:207–209.**
Inside `grepOneFile`, the poll runs only when a `'\n'` is seen (every 64 lines). A large file with very long lines or no newlines at all (and no NUL byte — e.g. a long base64/log line) is processed to completion with **zero** polls: Ctrl-C is ignored and the GUI window freezes (no message pumping, no refresh) for the whole file. Cheap fix: poll once per `Read()` block as well (the loop at grep.c:183).

**M4. Binary detection happens too late and corrupts stats / output — grep.c:188, 219, 235.**
The NUL check fires wherever the first NUL appears. If it's deep in the file, matches before it have already been printed (CLI) or added to the listview (GUI) and `linesMatched`/`fileMatched` already bumped; then the file is retroactively reclassified (`filesScanned--; binarySkipped++`) and `filesMatched` is *not* incremented — so the summary says "N line(s) in 0 file(s) (binaries skipped)" while N hits are on screen. Standard approach: sniff the first buffer only (like grep's first-4KB heuristic) and decide up front. Related: binary detection is NUL-only, so matched lines containing ESC/CSI bytes are printed raw to the console (cli.c:39–41) — console-state injection on a real Amiga console.

### Low

**L1. `Read()` error treated as EOF — grep.c:183.** `n = Read(...) > 0` silently swallows `n == -1` (I/O error, e.g. bad sector); the file appears clean. Worth at least counting.

**L2. `ExNext()` failure not distinguished — grep.c:256.** A real disk error mid-directory (vs `ERROR_NO_MORE_ENTRIES`) silently truncates the listing. Check `IoErr() != ERROR_NO_MORE_ENTRIES` after the loop if you want correctness on flaky media.

**L3. `AddPart` failure path — grep.c:263–264, 278.** On failure you `continue`, skipping the `c->path[plen] = '\0'` restore — this relies on AddPart's documented buffer-unchanged-on-failure guarantee (true in V37+, but it's load-bearing and worth a comment). Also, too-deep subtrees are silently skipped with no count/warning.

**L4. Links and cycles — grep.c:254, 266.** `fib_DirEntryType > 0` includes `ST_SOFTLINK` (3) and `ST_LINKDIR` (4). A link cycle (soft link to an ancestor) recurses until the 520-byte path cap stops it — bounded, but the subtree gets scanned repeatedly with duplicated output along the way.

**L5. `overflowed` is dead — grep.c:176, 212, 233.** Computed, then `(void)overflowed`. Consequence of the design it was meant to flag: a needle straddling the 1023-char truncation point silently never matches, and a truncated line is matched as if complete. Either use it (mark the hit, count truncations) or delete it.

**L6. `makeGadgets` partial failure leaves an inconsistent glist — gui.c:428–440.** If a mid-chain `CreateGadget` fails, `makeGadgets` returns FALSE, `relayout` doesn't `AddGList`, but `g->glist` still holds the context + partial gadgets. The *next* `relayout` then calls `RemoveGList(g->win, g->glist, -1)` on gadgets that were never added — undefined. Free the partial list on failure instead.

**L7. `cliPoll` never clears CTRL_C — cli.c:33** (`SetSignal(0L, 0L)` reads without clearing; conventional is `SetSignal(0L, SIGBREAKF_CTRL_C)`), and the CLI returns 0 after an abort (cli.c:49, 97) — `RETURN_WARN` (5) would be conventional. Also no `***Break` style message distinction beyond "(aborted)".

**L8. `doSearch` maps every error to "Cannot open that path." — gui.c:294–295**, including `ERROR_NO_FREE_STORE` from `grepTree`'s `AllocMem` failure. `Fault()` into the status buffer is ~free.

**L9. No `$VER:` version string in either binary** — `Version AmiGrep` fails; an Amiga convention worth ~30 bytes.

**L10. `GetDiskObject("ENV:Sys/def_Tool")` — gui.c:523.** The proper API is `GetDefDiskObject(WBTOOL)` (icon.library V36); `ENV:Sys/def_tool.info` isn't guaranteed to exist on minimal boots.

**L11. `pumpMessages` consumes-and-drops `IDCMP_NEWSIZE` and `IDCMP_MENUPICK` during a search — gui.c:198–216.** Resize during a search never triggers `relayout` afterwards (the event is eaten), leaving misplaced gadgets; menu Quit/Iconify during a search is silently ignored (Stop-then-Quit works, but users won't know).

**L12. GUI exits silently (rc=20, no requester) when intuition/graphics/gadtools v37 open fails — gui.c:705.** From Workbench this looks like "nothing happened."

**L13. Stale artifact:** `out/AmiGrep.dbg` (Jun 2) is no longer produced by build.sh — leftover.

## 2. SIZE REDUCTION

Context: these binaries are already lean. The CLI uses dos.library `Printf`/`PrintFault` (no stdio), the GUI uses hand-rolled `appendNum` instead of sprintf, and `-noixemul` + `-fomit-frame-pointer` are already in `CFLAGS` (build.sh:8). The big libnix/stdio bloat trap was already avoided — don't expect AmiFind-scale wins. Realistic target: CLI ~4.3–4.6 KB, GUI ~10–10.5 KB.

**build.sh strip check: correct.** Lines 25–26 strip `out/AmiGrep` and `out/AmiGrepGUI` in separate invocations, with the explanatory NB comment at lines 22–24 — the AmiFind multi-file `m68k-amigaos-strip` hunk-reloc corruption bug is *not* present. (Alternative: link with `-s` and skip the strip step entirely; ld-time stripping sidesteps the strip bug by construction.)

Ranked by expected payoff:

1. **`-msmall-code`** — likely the biggest single win on *file* size. PC-relative code eliminates most 32-bit absolute relocations; each reloc costs 4 bytes in the HUNK_RELOC32 table, plus `jsr.l`→`jsr.w` shrinkage in the code hunk. Both binaries are far below the 32 KB displacement limit, so it's safe. Estimate: 5–15% file size (~300–600 B CLI, ~700–1500 B GUI), plus faster loading.

2. **`-Os` instead of `-O2`** — disables inlining-for-speed and loop alignment. On gcc6-m68k typically 3–10% code size. Estimate: ~150–400 B CLI, ~400–900 B GUI. Verify `MatchPattern`-heavy search speed doesn't regress noticeably (it won't — the hot path is DOS calls).

3. **`-ffunction-sections -fdata-sections -Wl,--gc-sections`** — bebbo's ld supports section GC for hunk output. Gains here are modest because almost everything in grep.c is reachable from both front ends (the CLI links `nameFilterInit/Match` and uses them; nothing is obviously dead). Maybe 100–300 B from libnix stragglers. Worth adding since it's free; verify with `m68k-amigaos-objdump -h` that hunks stay sane.

4. **One dual-mode binary** instead of two — `grep.c` (~2 KB of code) and the libnix startup (~1.5–2 KB) are duplicated across both executables. A single binary that checks `WBenchMsg`/argc to choose CLI vs GUI saves ~4–5 KB of *total* disk and lets the GUI front end be the icon target. Trade-off: the CLI then drags in the GUI's code and library opens unless you're careful — only worth it if total install size matters more than per-binary size.

5. **`-mregparm=N`** (bebbo extension, register-args ABI) — a few percent code size from dropped stack-arg traffic. Safe here: `MatchFunc`/`PollFunc` callbacks are only ever invoked by your own code compiled with the same flag, and there are no Hooks or function pointers handed to the OS. Compile *all* files with it or none.

6. **Static/stack buffers: nothing to gain.** `struct Ctx` (8 KB read buffer + path + line) is heap-allocated, so it never appears in the binary; `Matcher.parsed[600]`, the 600-byte parse probes, and `struct Gui` are stack/struct members. There is no large initialized static data — `amiNewMenu` and the string literals are it. BSS costs nothing in a hunk file. Don't bother shrinking buffers for size (shrinking the 8 KB read buffer would only hurt floppy/slow-device throughput).

7. **Engine simplification — marginal.** `matcherInit` parses the pattern twice (probe at grep.c:65–68, then wrapped at grep.c:81–83); the probe in `nameFilterInit` is similarly redundant *and* is the source of bug M1 — parsing the wrapped/raw pattern directly into the destination buffer fixes the bug and deletes ~600 bytes of stack plus a few dozen bytes of code. The hand-rolled `contains()` is already smaller than pulling in `strstr` + a lowercase pass; leave it. Dead `overflowed` logic (L5) is ~20 bytes.

8. **build.sh hygiene (build time, not size):** compile `src/grep.c` once to `out/grep.o` and link it into all three targets instead of recompiling it three times; the debug GUI build (line 19) recompiles everything a third time. Also consider adding `-g0` explicitly to release links (harmless; strip handles it today) and deleting the stale `out/AmiGrep.dbg`.

**Priority order if you only do three things:** fix **H1** and **H2** (both are crashes a user can hit), and fix **M1** (silent memory misread from a documented CLI option). For size, add `-msmall-code -Os` and measure — that's likely ~1–2 KB combined off the GUI for two flags.
