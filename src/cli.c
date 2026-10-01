/*
 * AmiGrep - command line front end
 *
 *   AmiGrep PATTERN [PATH] [FILE <namepat>] [CASE] [NAMES]
 *
 *   PATTERN   text to look for inside files. Plain text = case-insensitive
 *             substring. Contains AmigaDOS wildcards (#? * ? [] ~ |) = it is
 *             matched across the whole line as #?<pattern>#?.
 *   PATH      volume / assign / directory / file to search (default: current).
 *   FILE      only search files whose name matches this AmigaDOS wildcard
 *             (e.g. FILE #?.c). Case-insensitive.
 *   CASE      make the content match case-sensitive.
 *   NAMES     print only the names of files that contain a match (like grep -l).
 *
 * Output:  <path>:<line>: <text>     (or just <path> with NAMES)
 * Press Ctrl-C to abort.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include "grep.h"

unsigned long __stack = 60000;   /* force a generous stack (deep recursion) */

/* AmigaDOS version cookie (the C: Version command / $VER reads this) */
static const char verstag[] __attribute__((used)) =
    "$VER: AmiGrep 1.1 (01.10.2026)";

#define TEMPLATE "PATTERN/A,PATH,FILE/K,CASE/S,NAMES/S"
enum { ARG_PATTERN, ARG_PATH, ARG_FILE, ARG_CASE, ARG_NAMES, ARG_COUNT };

static BOOL cliPoll(APTR user)
{
    (void)user;
    /* read AND clear CTRL_C so the break is consumed (a later run / the shell
     * won't see a stale break flag). */
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? TRUE : FALSE;
}

static BOOL cliFound(CONST_STRPTR path, ULONG line, CONST_STRPTR text, APTR user)
{
    if (user)  /* namesOnly */
        Printf("%s\n", (LONG)path);
    else
        Printf("%s:%ld: %s\n", (LONG)path, (LONG)line, (LONG)text);
    return TRUE;
}

int main(void)
{
    struct RDArgs *rd;
    LONG args[ARG_COUNT] = { 0, 0, 0, 0, 0 };
    int rc = 0;

    rd = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rd) {
        PrintFault(IoErr(), "AmiGrep");
        return 20;
    }

    {
        CONST_STRPTR pat   = (CONST_STRPTR)args[ARG_PATTERN];
        CONST_STRPTR path  = args[ARG_PATH] ? (CONST_STRPTR)args[ARG_PATH]
                                            : (CONST_STRPTR)"";   /* current dir */
        CONST_STRPTR filep = (CONST_STRPTR)args[ARG_FILE];        /* may be NULL  */
        BOOL caseSens = args[ARG_CASE]  ? TRUE : FALSE;
        BOOL names    = args[ARG_NAMES] ? TRUE : FALSE;
        struct Matcher    m;
        struct NameFilter nf;
        struct GrepStats  st;
        LONG err;

        if (!matcherInit(&m, pat, caseSens)) {
            Printf("AmiGrep: bad search pattern\n");
            FreeArgs(rd);
            return 20;
        }
        if (!nameFilterInit(&nf, filep)) {
            Printf("AmiGrep: bad FILE pattern\n");
            FreeArgs(rd);
            return 20;
        }

        st.namesOnly = names;
        err = grepTree(path, &m, &nf, cliFound, names ? (APTR)1 : NULL,
                       cliPoll, NULL, &st);
        if (err) {
            PrintFault(err, (STRPTR)(path[0] ? path : (CONST_STRPTR)"current dir"));
            rc = 20;
        } else {
            Printf("\n%ld line(s) in %ld file(s) - scanned %ld files, %ld dirs"
                   "%s%s\n",
                   (LONG)st.linesMatched, (LONG)st.filesMatched,
                   (LONG)st.filesScanned, (LONG)st.dirsScanned,
                   (LONG)(st.binarySkipped ? " (binaries skipped)" : ""),
                   (LONG)(st.aborted ? " (aborted)" : ""));
            if (st.aborted) rc = RETURN_WARN;    /* 5: stopped via Ctrl-C */
        }
    }

    FreeArgs(rd);
    return rc;
}
