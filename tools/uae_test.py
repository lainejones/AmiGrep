#!/usr/bin/env python3
"""
Deploy / test helper for AmiGrep on the WinUAE "clean 030" OS 3.2 box.

DH0: is the host directory /mnt/c/Amiga/Clean 030 323, which we can read and
write from the host. This script:

  deploy    - copy out/AmiGrep + out/AmiGrepGUI (and the GUI icon) into
              DH0:AmiGrep/ so you can launch them from Workbench/Shell.
  setupcli  - back up S/Startup-Sequence, and auto-run the CLI on next boot,
              writing results to DH0:AmiGrep/out.txt (headless verification).
  setupgui  - auto-run AmiGrepGUI on next boot (so it opens for a screenshot).
  result    - print DH0:AmiGrep/out.txt (the CLI test output).
  restore   - put the original Startup-Sequence back.

Usage (run under WSL):
  python3 tools/uae_test.py deploy
  python3 tools/uae_test.py setupcli "TODO" "SYS:S"
  python3 tools/uae_test.py setupgui
  python3 tools/uae_test.py result
  python3 tools/uae_test.py restore

Then launch WinUAE headless, e.g. (from PowerShell):
  & 'C:\\Program Files\\WinUAE\\winuae64.exe' -f `
     'C:\\Users\\Public\\Documents\\Amiga Files\\WinUAE\\Configurations\\clean 030 32.uae' `
     -s use_gui=no
"""
import os, sys, shutil

DH0  = "/mnt/c/Amiga/Clean 030 323"
OUT  = os.path.join(os.path.dirname(__file__), "..", "out")
SS   = os.path.join(DH0, "S", "Startup-Sequence")
DEST = os.path.join(DH0, "AmiGrep")
MARK = ";AGTEST"

def read(p):  return open(p, "r", newline="").read()
def write(p, s):                       # always write LF only (Amiga style)
    with open(p, "w", newline="") as f: f.write(s.replace("\r\n", "\n"))

def deploy():
    os.makedirs(DEST, exist_ok=True)
    for name in ("AmiGrep", "AmiGrepGUI", "AmiGrepGUI.info"):
        src = os.path.join(OUT, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(DEST, name))
            print("deployed", name)
        elif name.endswith(".info"):
            print("(no icon yet:", name, "- run makeicon.py)")
        else:
            sys.exit("missing binary: " + src + " (run build.sh)")
    print("-> DH0:AmiGrep/")

def _strip(ss):
    """remove any previously-injected block, markers AND body lines."""
    out, skip = [], False
    for ln in ss:
        if MARK + " begin" in ln: skip = True;  continue
        if MARK + " end"   in ln: skip = False; continue
        if not skip: out.append(ln)
    return out

def _inject(lines):
    if not os.path.exists(SS + ".agbak"):
        shutil.copyfile(SS, SS + ".agbak")
    ss = _strip(read(SS).split("\n"))                         # idempotent
    block = [MARK + " begin"] + lines + [MARK + " end"]
    out, done = [], False
    for ln in ss:
        if (not done) and ln.strip().lower().startswith("loadwb"):
            out.append(ln); out.extend(block); done = True
        else:
            out.append(ln)
    if not done:                                              # fallback: append
        out.extend(block)
    write(SS, "\n".join(out))

def setupcli(pattern, path):
    deploy()
    _inject(["Wait 2",
             'DH0:AmiGrep/AmiGrep "%s" "%s" >DH0:AmiGrep/out.txt' % (pattern, path)])
    print('setupcli: will run AmiGrep "%s" "%s" -> DH0:AmiGrep/out.txt' % (pattern, path))

def setupgui():
    deploy()
    _inject(["Wait 2", "Run DH0:AmiGrep/AmiGrepGUI"])
    print("setupgui: AmiGrepGUI will open on next boot")

def result():
    p = os.path.join(DEST, "out.txt")
    if os.path.exists(p): sys.stdout.write(read(p))
    else: print("(no out.txt yet - boot the setupcli config first)")

def restore():
    if os.path.exists(SS + ".agbak"):
        shutil.copyfile(SS + ".agbak", SS)
        os.remove(SS + ".agbak")
        print("restored original Startup-Sequence")
    else:
        print("(no backup to restore)")

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if   cmd == "deploy":   deploy()
    elif cmd == "setupcli": setupcli(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "SYS:")
    elif cmd == "setupgui": setupgui()
    elif cmd == "result":   result()
    elif cmd == "restore":  restore()
    else: sys.exit(__doc__)
