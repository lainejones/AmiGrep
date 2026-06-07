/*
 * test_engine - host/vamos test harness for the AmiGrep search engine.
 *
 * Links grep.c directly and drives grepTree() with an instrumented PollFunc
 * (counts calls, optionally aborts after N) and a MatchFunc that prints hits.
 * Used to exercise engine-level behaviour that the CLI can't easily show under
 * vamos - specifically the abort/poll cadence (M3) and binary classification
 * (M4).
 *
 *   test_engine PATTERN PATH [abortAfterPolls]
 *
 * Build (WSL):
 *   m68k-amigaos-gcc -Os -msmall-code -noixemul -o out/test_engine \
 *       tools/test_engine.c src/grep.c -Isrc
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include "grep.h"

static LONG  pollCount;
static LONG  abortAfter;     /* 0 = never abort */

static BOOL myPoll(APTR user)
{
    (void)user;
    pollCount++;
    if (abortAfter && pollCount >= abortAfter) return TRUE;   /* request abort */
    return FALSE;
}

static BOOL myFound(CONST_STRPTR path, ULONG line, CONST_STRPTR text, APTR user)
{
    (void)user;
    Printf("  HIT %s:%ld: %s\n", (LONG)path, (LONG)line, (LONG)text);
    return TRUE;
}

static LONG myAtoL(const char *s)
{
    LONG v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char **argv)
{
    struct Matcher    m;
    struct NameFilter nf;
    struct GrepStats  st;
    LONG rc;

    if (argc < 3) {
        Printf("usage: test_engine PATTERN PATH [abortAfterPolls]\n");
        return 20;
    }
    abortAfter = (argc >= 4) ? myAtoL(argv[3]) : 0;
    pollCount  = 0;

    if (!matcherInit(&m, (CONST_STRPTR)argv[1], FALSE)) {
        Printf("bad pattern\n");
        return 20;
    }
    nf.active        = FALSE;
    st.namesOnly     = FALSE;

    rc = grepTree((CONST_STRPTR)argv[2], &m, &nf,
                  myFound, NULL, myPoll, NULL, &st);

    Printf("rc=%ld polls=%ld lines=%ld files=%ld scanned=%ld binSkip=%ld aborted=%ld\n",
           rc, pollCount,
           (LONG)st.linesMatched, (LONG)st.filesMatched,
           (LONG)st.filesScanned, (LONG)st.binarySkipped, (LONG)st.aborted);
    return 0;
}
