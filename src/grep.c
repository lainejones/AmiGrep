/*
 * AmiGrep - shared content-search engine (see grep.h)
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <string.h>

#include "grep.h"

/* ------------------------------------------------------------------ */
/* small case-insensitive helpers                                     */
/* ------------------------------------------------------------------ */

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return c;
}

static void str_lower(char *dst, CONST_STRPTR src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = to_lower((char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

/* substring search; needle already lower-cased when !caseSensitive */
static BOOL contains(CONST_STRPTR hay, CONST_STRPTR needle, int nl, BOOL cs)
{
    int hl, i, k;
    if (nl == 0) return TRUE;                 /* empty needle matches all      */
    hl = (int)strlen((const char *)hay);
    for (i = 0; i + nl <= hl; i++) {
        for (k = 0; k < nl; k++) {
            char h = (char)hay[i + k];
            if (!cs) h = to_lower(h);
            if (h != needle[k]) break;
        }
        if (k == nl) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* matcher                                                            */
/* ------------------------------------------------------------------ */

BOOL matcherInit(struct Matcher *m, CONST_STRPTR pattern, BOOL caseSensitive)
{
    char wrapped[260];
    LONG r;
    int  pl = (int)strlen((const char *)pattern);

    m->caseSensitive = caseSensitive;

    /* does the raw pattern contain AmigaDOS wildcard tokens? Probe with a
     * parse of the pattern as-is; r==1 means wildcards are present. */
    {
        UBYTE probe[600];
        LONG p = caseSensitive
               ? ParsePattern   ((CONST_STRPTR)pattern, (STRPTR)probe, (LONG)sizeof probe)
               : ParsePatternNoCase((CONST_STRPTR)pattern, (STRPTR)probe, (LONG)sizeof probe);
        if (p < 0) return FALSE;              /* malformed / too long          */
        m->useWild = (p == 1);
    }

    if (m->useWild) {
        /* wrap as "#?<pat>#?" so a whole-line MatchPattern behaves as a
         * "contains" test (grep semantics: match anywhere on the line). */
        if (pl + 4 >= (int)sizeof wrapped) return FALSE;
        wrapped[0] = '#'; wrapped[1] = '?';
        memcpy(wrapped + 2, (const char *)pattern, pl);
        wrapped[pl + 2] = '#'; wrapped[pl + 3] = '?';
        wrapped[pl + 4] = '\0';
        r = caseSensitive
          ? ParsePattern   ((CONST_STRPTR)wrapped, (STRPTR)m->parsed, (LONG)sizeof m->parsed)
          : ParsePatternNoCase((CONST_STRPTR)wrapped, (STRPTR)m->parsed, (LONG)sizeof m->parsed);
        if (r < 0) return FALSE;
        m->needle[0] = '\0';
        m->needleLen = 0;
    } else {
        if (pl >= (int)sizeof m->needle) return FALSE;
        if (caseSensitive) strcpy(m->needle, (const char *)pattern);
        else               str_lower(m->needle, pattern, (int)sizeof m->needle);
        m->needleLen = (int)strlen(m->needle);
    }
    return TRUE;
}

BOOL lineMatches(struct Matcher *m, CONST_STRPTR line)
{
    if (m->useWild) {
        return (m->caseSensitive
                ? MatchPattern      ((STRPTR)m->parsed, (STRPTR)line)
                : MatchPatternNoCase((STRPTR)m->parsed, (STRPTR)line)) ? TRUE : FALSE;
    }
    return contains(line, (CONST_STRPTR)m->needle, m->needleLen, m->caseSensitive);
}

/* ------------------------------------------------------------------ */
/* filename filter                                                    */
/* ------------------------------------------------------------------ */

BOOL nameFilterInit(struct NameFilter *nf, CONST_STRPTR pat)
{
    UBYTE probe[600];
    char  wrapped[260];
    LONG  r;
    int   pl;

    if (!pat || pat[0] == '\0') { nf->active = FALSE; return TRUE; }
    pl = (int)strlen((const char *)pat);

    /* Probe: does the filter contain AmigaDOS wildcards? r==1 -> yes. */
    r = ParsePatternNoCase((CONST_STRPTR)pat, (STRPTR)probe, (LONG)sizeof probe);
    if (r < 0) { nf->active = FALSE; return FALSE; }

    if (r == 1) {
        /* wildcards present: match the whole name against the pattern as-is.
         * Re-parse directly into nf->parsed: copying a truncated token stream
         * (probe is bigger than nf->parsed) would hand MatchPatternNoCase an
         * unterminated buffer -> undefined behaviour. This way an over-long
         * pattern fails loudly instead. */
        r = ParsePatternNoCase((CONST_STRPTR)pat, (STRPTR)nf->parsed, (LONG)sizeof nf->parsed);
        if (r < 0) { nf->active = FALSE; return FALSE; }
    } else {
        /* plain text: wrap as #?<pat>#? so it matches any name *containing* it
         * (mirrors the Find field's substring behaviour). */
        if (pl + 4 >= (int)sizeof wrapped) { nf->active = FALSE; return FALSE; }
        wrapped[0] = '#'; wrapped[1] = '?';
        memcpy(wrapped + 2, (const char *)pat, pl);
        wrapped[pl + 2] = '#'; wrapped[pl + 3] = '?';
        wrapped[pl + 4] = '\0';
        r = ParsePatternNoCase((CONST_STRPTR)wrapped, (STRPTR)nf->parsed, (LONG)sizeof nf->parsed);
        if (r < 0) { nf->active = FALSE; return FALSE; }
    }
    nf->active = TRUE;
    return TRUE;
}

BOOL nameFilterMatch(struct NameFilter *nf, CONST_STRPTR name)
{
    if (!nf->active) return TRUE;
    return MatchPatternNoCase((STRPTR)nf->parsed, (STRPTR)name) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* one file                                                           */
/* ------------------------------------------------------------------ */

struct Ctx {
    struct Matcher    *m;
    struct NameFilter *nf;
    MatchFunc          found;
    APTR               foundUser;
    PollFunc           poll;
    APTR               pollUser;
    struct GrepStats  *st;
    char               path[520];           /* current path, grown/shrunk     */
    char               buf[8192];            /* file read buffer               */
    char               line[GREP_MAXLINE];   /* line accumulator               */
    BOOL               stop;
    ULONG              pollCounter;
};

/* Search a single already-named file (c->path). Counts/reports via c->st. */
static void grepOneFile(struct Ctx *c)
{
    BPTR fh;
    LONG n;
    int  linePos = 0;
    ULONG lineNo = 1;
    BOOL fileMatched = FALSE;
    BOOL sniffed = FALSE;        /* have we classified the file yet?            */

    fh = Open((STRPTR)c->path, MODE_OLDFILE);
    if (!fh) return;

    while (!c->stop && (n = Read(fh, c->buf, (LONG)sizeof c->buf)) > 0) {
        LONG i;

        /* M4: decide text-vs-binary up front, from the FIRST block only (grep's
         * first-buffer heuristic). If that block holds a NUL the file is binary,
         * so skip it whole - before any match is reported - and the stats stay
         * consistent (no "N line(s) in 0 file(s)"). A NUL in a *later* block is
         * dropped as noise below; the file has already been judged text. */
        if (!sniffed) {
            LONG k;
            for (k = 0; k < n; k++) {
                if (c->buf[k] == '\0') {         /* binary: skip the whole file */
                    Close(fh);
                    c->st->binarySkipped++;
                    return;
                }
            }
            c->st->filesScanned++;
            sniffed = TRUE;
        }

        for (i = 0; i < n; i++) {
            char ch = c->buf[i];

            if (ch == '\0') continue;            /* stray NUL in a text file     */

            if (ch == '\n') {
                c->line[linePos] = '\0';
                if (lineMatches(c->m, (CONST_STRPTR)c->line)) {
                    c->st->linesMatched++;
                    fileMatched = TRUE;
                    if (c->st->namesOnly) {
                        if (c->found && !c->found(c->path, 0, (CONST_STRPTR)"", c->foundUser))
                            c->stop = TRUE;
                        goto done;               /* one hit is enough           */
                    }
                    if (c->found && !c->found(c->path, lineNo, (CONST_STRPTR)c->line,
                                              c->foundUser))
                        c->stop = TRUE;
                }
                lineNo++; linePos = 0;
                if (c->stop) break;

                if (c->poll && ((++c->pollCounter & 63) == 0)) {
                    if (c->poll(c->pollUser)) { c->stop = TRUE; break; }
                }
            } else if (ch != '\r') {
                /* keep scanning past GREP_MAXLINE, silently dropping the excess */
                if (linePos < (int)sizeof c->line - 1) c->line[linePos++] = ch;
            }
        }

        /* M3: poll once per block too. A file made of very long lines (or no
         * newline at all) would otherwise never reach the per-newline poll, so
         * Stop/Ctrl-C was ignored and the GUI froze for the whole file. */
        if (!c->stop && c->poll && c->poll(c->pollUser)) c->stop = TRUE;
    }

    /* trailing line with no newline */
    if (!c->stop && sniffed && linePos > 0) {
        c->line[linePos] = '\0';
        if (lineMatches(c->m, (CONST_STRPTR)c->line)) {
            c->st->linesMatched++;
            fileMatched = TRUE;
            if (c->found) {
                if (c->st->namesOnly) c->found(c->path, 0, (CONST_STRPTR)"", c->foundUser);
                else                  c->found(c->path, lineNo, (CONST_STRPTR)c->line,
                                                c->foundUser);
            }
        }
    }

    /* an empty file (or one whose first Read failed) opened fine but was never
     * sniffed; count it as a scanned text file with no matches, as before. */
    if (!sniffed) c->st->filesScanned++;

done:
    Close(fh);
    if (fileMatched) c->st->filesMatched++;
}

/* ------------------------------------------------------------------ */
/* recursive walk                                                     */
/* ------------------------------------------------------------------ */

static void recurse(struct Ctx *c, BPTR lock)
{
    struct FileInfoBlock *fib;
    LONG plen;

    if (c->stop) return;

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) return;

    if (Examine(lock, fib)) {
        if (fib->fib_DirEntryType > 0) {
            /* directory -> enumerate its children */
            while (!c->stop && ExNext(lock, fib)) {

                if (c->poll && ((++c->pollCounter & 63) == 0)) {
                    if (c->poll(c->pollUser)) { c->stop = TRUE; break; }
                }

                plen = (LONG)strlen(c->path);
                if (!AddPart((STRPTR)c->path, fib->fib_FileName, (ULONG)sizeof c->path))
                    continue;                    /* full path would overflow    */

                if (fib->fib_DirEntryType > 0) {
                    c->st->dirsScanned++;
                    BPTR child = Lock((STRPTR)c->path, ACCESS_READ);
                    if (child) {
                        recurse(c, child);
                        UnLock(child);
                    }
                } else {
                    if (nameFilterMatch(c->nf, fib->fib_FileName))
                        grepOneFile(c);
                }

                c->path[plen] = '\0';            /* restore parent path         */
            }
        } else {
            /* the root itself is a plain file */
            if (nameFilterMatch(c->nf, fib->fib_FileName))
                grepOneFile(c);
        }
    }

    FreeDosObject(DOS_FIB, fib);
}

LONG grepTree(CONST_STRPTR rootPath, struct Matcher *m, struct NameFilter *nf,
              MatchFunc found, APTR foundUser,
              PollFunc  poll,  APTR pollUser,
              struct GrepStats *stats)
{
    struct Ctx *c;
    BPTR lock;

    /* Ctx is large (read buffer + path + line); keep it off the stack so deep
     * recursion stays cheap. */
    c = (struct Ctx *)AllocMem(sizeof(struct Ctx), MEMF_CLEAR);
    if (!c) return ERROR_NO_FREE_STORE;

    c->m = m;          c->nf = nf;
    c->found = found;  c->foundUser = foundUser;
    c->poll  = poll;   c->pollUser  = pollUser;
    c->st = stats;

    stats->dirsScanned = stats->filesScanned = stats->filesMatched = 0;
    stats->linesMatched = stats->binarySkipped = 0;
    stats->aborted = FALSE;

    strncpy(c->path, (const char *)rootPath, sizeof c->path - 1);
    c->path[sizeof c->path - 1] = '\0';

    lock = Lock((STRPTR)rootPath, ACCESS_READ);
    if (!lock) {
        LONG err = IoErr();
        FreeMem(c, sizeof(struct Ctx));
        return err;
    }

    recurse(c, lock);
    UnLock(lock);

    stats->aborted = c->stop;
    FreeMem(c, sizeof(struct Ctx));
    return 0;
}
