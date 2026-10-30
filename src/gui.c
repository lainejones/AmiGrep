/*
 * AmiGrep - GadTools GUI front end (AmigaOS 2.04+, no external deps)
 *
 *  +------------------------------------------------+
 *  |   Find [____________________________]          |
 *  |     In [SYS:________________________] [...]    |
 *  |  Files [____________________]  Case [ ]        |
 *  |  [ Search ] [ Stop ]   Ready                   |
 *  |  +------------------------------------------+  |
 *  |  | DH0:s/Startup-Sequence:3: ...            |  |
 *  |  | ...                                      |  |
 *  |  +------------------------------------------+  |
 *  |    Hit [DH0:s/Startup-Sequence:3: ...______]   |
 *  +------------------------------------------------+
 *
 * The search runs synchronously when Search is pressed; while it runs the
 * poll callback pumps Intuition messages so the Stop button, the Stop menu
 * item and the close gadget stay live. Each matching line is collected into
 * an Exec list and attached to the listview when the scan finishes.
 *
 * The right mouse button menu: Project (About, Iconify, Quit), Search
 * (Search, Stop, Choose drawer, Clear results), Result (View, Open drawer)
 * and Settings (Search defaults, Programs, Snapshot / Unsnapshot window).
 *
 * Fonts: the gadgets use the screen font; if the smallest layout would not
 * fit the screen in it, everything falls back to Topaz 8. With Topaz 8 the
 * main window and every settings page fit a 640x200 NTSC screen.
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/wb.h>
#include <proto/icon.h>

#include <string.h>

#include "grep.h"
#include "amigui.h"

unsigned long __stack = 60000;   /* force a generous stack (deep recursion) */

#define AMIGREP_VERSION "1.1"

/* AmigaDOS version cookie (the C: Version command / $VER reads this) */
static const char verstag[] __attribute__((used)) =
    "$VER: AmiGrepGUI " AMIGREP_VERSION " (01.10.2026)";


/* library bases (exec & dos are auto-opened by the C startup) */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *AslBase       = NULL;   /* optional: file requester */
struct Library       *WorkbenchBase = NULL;   /* optional: AppIcon/iconify */
struct Library       *IconBase      = NULL;   /* optional: AppIcon icon    */

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

enum {
    GAD_PATTERN = 1,
    GAD_PATH,
    GAD_PICK,
    GAD_FILES,
    GAD_CASE,
    GAD_SEARCH,
    GAD_STOP,
    GAD_STATUS,
    GAD_LIST,
    GAD_HIT
};

#define CB_W 26                    /* a CHECKBOX_KIND's fixed image width */

struct Gui {
    struct Screen *scr;
    APTR           vi;
    struct Window *win;
    struct Gadget *glist;          /* gadtools gadget list (for FreeGadgets) */
    struct Gadget *gPattern;
    struct Gadget *gPath;
    struct Gadget *gPick;
    struct Gadget *gFiles;
    struct Gadget *gCase;
    struct Gadget *gSearch;
    struct Gadget *gStop;
    struct Gadget *gStatus;
    struct Gadget *gList;
    struct Gadget *gHit;           /* read-only, scrollable full line of selection */

    struct TextAttr  ta;           /* the gadgets' font: screen font or Topaz 8 */
    struct TextFont *font;
    struct RastPort  rp;           /* for measuring text in that font          */
    WORD             fh, lab, bw, pickw, casew;  /* font height + layout widths */

    struct List    results;        /* nodes shown in the listview            */
    APTR           pool;           /* exec pool holding the results (V39+),
                                    * NULL -> nodes are separate AllocVecs   */
    char           status[80];
    char           statusShown[80];/* status cut to what fits the gadget      */
    char           savePat[210];   /* Find text, preserved across resize/icon */
    char           savePath[210];  /* In text,   preserved across resize/icon */
    char           saveFiles[110]; /* Files filter, preserved                 */
    BOOL           caseSens;       /* Case checkbox state                     */
    BOOL           stopReq;        /* Stop pressed during a search           */
    BOOL           closeReq;       /* window closed during a search          */
    BOOL           resizePending;  /* NEWSIZE arrived -> relayout when safe   */
    BOOL           quit;           /* unrecoverable failure -> leave eventLoop */
    ULONG          uiCounter;

    struct MsgPort   *appPort;     /* AppIcon message port (iconified)        */
    struct AppIcon   *appIcon;     /* non-NULL while iconified                */
    struct DiskObject *dobj;       /* icon used for the AppIcon               */
    BOOL              iconified;

    struct Menu      *menu;        /* the right-button menu strip             */

    LONG  sel;                     /* selected result ordinal (-1 = none)     */
    LONG  lastSel;                 /* last listview ordinal clicked (-1 = none) */
    ULONG lastSecs, lastMicros;    /* timestamp of that click (dbl-click detect) */
};

/* one result: a List node whose ln_Name is the display line, plus the raw
 * file path (both stored in the same allocation, after the node). The display
 * line normally starts with the path, so then 'path' just points into it and
 * pathLen says where it ends (it is not NUL-terminated there). */
struct ResNode {
    struct Node node;
    char       *path;
    ULONG       pathLen;
};

/* every menu item carries one of these as its user data */
enum {
    MN_NONE = 0,
    MN_ABOUT, MN_ICONIFY, MN_QUIT,                       /* Project  */
    MN_SEARCH, MN_STOP, MN_PICK, MN_CLEAR,               /* Search   */
    MN_VIEW, MN_OPENDRAWER,                              /* Result   */
    MN_SET_SEARCH, MN_SET_PROGRAMS, MN_SNAPSHOT, MN_UNSNAPSHOT  /* Settings */
};

/* small unsigned-to-decimal helper (no sprintf under -noixemul);
 * bounded: never writes past buf[bufsize-1] */
static void appendNum(char *buf, ULONG v, int bufsize)
{
    char tmp[16];
    int  i = 0, j, l;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    l = (int)strlen(buf);
    for (j = i - 1; j >= 0 && l < bufsize - 1; j--) buf[l++] = tmp[j];
    buf[l] = '\0';
}

static void setStr(char *dst, int size, const char *v)
{
    strncpy(dst, v, size - 1);
    dst[size - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */
/*
 * Where each setting comes from, lowest first:
 *   1. the built-in defaults (setsDefault): Find empty, In SYS:, Files empty
 *      (every file), Case off, the viewer SYS:Utilities/MultiView, the
 *      default window place and size;
 *   2. AmiGrepGUI reads no tooltypes, so there are none to apply here;
 *   3. the settings file AmiGrep.prefs - ENV:AmiGrep.prefs, else
 *      ENVARC:AmiGrep.prefs - applied on top. Settings > ... > Save writes
 *      both, Use writes ENV: only (so it lasts until the next reboot).
 *      Plain text, one KEY=value a line:
 *        PATTERN=<Find text at start>   PATH=<In drawer at start>
 *        FILES=<Files filter at start>  CASE=0|1 (the Case box at start)
 *        VIEWER=<program for View>      WINDOW=x,y,w,h (Snapshot window)
 *      Unknown keys are ignored; an empty PATH or VIEWER means the default.
 */

#define PREFS_NAME "AmiGrep.prefs"

struct Settings {
    char pattern[201];
    char path[201];
    char files[101];
    BOOL caseSens;
    char viewer[120];                  /* View: <viewer> "<file>" */
    LONG winx, winy, winw, winh;       /* snapshot; winw 0 = none */
};
static struct Settings g_set;

static void setsDefault(struct Settings *s)
{
    memset(s, 0, sizeof *s);
    strcpy(s->path, "SYS:");
    strcpy(s->viewer, "SYS:Utilities/MultiView");
}

static LONG parseNum(char **pp)
{
    char *p = *pp;
    LONG v = 0, sign = 1;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *pp = p;
    return v * sign;
}

static void setsLoad(struct Settings *s)
{
    char buf[1200], *line, *next;
    setsDefault(s);
    if (amg_env_read(PREFS_NAME, buf, sizeof buf) < 0) return;
    for (line = buf; line && *line; line = next) {
        char *v;
        if ((next = strchr(line, '\n'))) *next++ = '\0';
        if ((v = strchr(line, '\r'))) *v = '\0';
        if (!(v = strchr(line, '='))) continue;
        *v++ = '\0';
        if      (!strcmp(line, "PATTERN")) setStr(s->pattern, sizeof s->pattern, v);
        else if (!strcmp(line, "PATH"))    { if (*v) setStr(s->path, sizeof s->path, v); }
        else if (!strcmp(line, "FILES"))   setStr(s->files, sizeof s->files, v);
        else if (!strcmp(line, "CASE"))    s->caseSens = (*v == '1' || *v == 'Y' || *v == 'y');
        else if (!strcmp(line, "VIEWER"))  { if (*v) setStr(s->viewer, sizeof s->viewer, v); }
        else if (!strcmp(line, "WINDOW")) {
            LONG n[4]; int k;
            for (k = 0; k < 4 && *v; k++) {
                n[k] = parseNum(&v);
                if (*v == ',') v++;
            }
            if (k == 4 && n[2] > 0 && n[3] > 0) {
                s->winx = n[0]; s->winy = n[1]; s->winw = n[2]; s->winh = n[3];
            }
        }
    }
}

static void addLine(char *buf, int size, const char *key, const char *val)
{
    strncat(buf, key, size - 1 - strlen(buf));
    strncat(buf, "=", size - 1 - strlen(buf));
    strncat(buf, val, size - 1 - strlen(buf));
    strncat(buf, "\n", size - 1 - strlen(buf));
}

/* keep = Save (ENV: + ENVARC:), else Use (ENV: only) */
static BOOL setsWrite(const struct Settings *s, BOOL keep)
{
    char buf[1200];
    buf[0] = '\0';
    addLine(buf, sizeof buf, "PATTERN", s->pattern);
    addLine(buf, sizeof buf, "PATH", s->path);
    addLine(buf, sizeof buf, "FILES", s->files);
    addLine(buf, sizeof buf, "CASE", s->caseSens ? "1" : "0");
    addLine(buf, sizeof buf, "VIEWER", s->viewer);
    if (s->winw > 0) {
        char w[48];
        w[0] = '\0';
        appendNum(w, (ULONG)(s->winx < 0 ? 0 : s->winx), sizeof w); strcat(w, ",");
        appendNum(w, (ULONG)(s->winy < 0 ? 0 : s->winy), sizeof w); strcat(w, ",");
        appendNum(w, (ULONG)s->winw, sizeof w); strcat(w, ",");
        appendNum(w, (ULONG)s->winh, sizeof w);
        addLine(buf, sizeof buf, "WINDOW", w);
    }
    return amg_env_write(PREFS_NAME, buf, strlen(buf), keep) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* result list management                                             */
/* ------------------------------------------------------------------ */

static void initResults(struct Gui *g)
{
    /* init the result list without pulling in amiga.lib's NewList() */
    g->results.lh_Head     = (struct Node *)&g->results.lh_Tail;
    g->results.lh_Tail     = NULL;
    g->results.lh_TailPred = (struct Node *)&g->results.lh_Head;
    g->results.lh_Type     = NT_UNKNOWN;
}

static void freeResults(struct Gui *g)
{
    struct Node *n;
    if (g->pool) {                 /* every node lives in the pool: one call */
        DeletePool(g->pool);
        g->pool = NULL;
        initResults(g);
        return;
    }
    while ((n = RemHead(&g->results)))
        FreeVec(n);                /* node + name were one allocation        */
}

/* one allocation holds the ResNode, the display string and - only when the
 * display string doesn't already start with it - a copy of the path */
static void addResult(struct Gui *g, CONST_STRPTR path, CONST_STRPTR text)
{
    int dl = (int)strlen((const char *)text) + 1;
    int pl = (int)strlen((const char *)path) + 1;
    BOOL shared = (pl <= dl && memcmp(text, path, pl - 1) == 0);
    ULONG size = sizeof(struct ResNode) + dl + (shared ? 0 : pl);
    struct ResNode *rn;
    char *d;

    /* Pools are exec V39+. Only start one while the list is empty, so a list
     * is always entirely pooled or entirely AllocVec'd (see freeResults). */
    if (!g->pool && g->results.lh_Head->ln_Succ == NULL &&
        SysBase->LibNode.lib_Version >= 39)
        g->pool = CreatePool(MEMF_ANY, 8192, 2048);

    rn = (struct ResNode *)(g->pool ? AllocPooled(g->pool, size)
                                    : AllocVec(size, MEMF_ANY));
    if (!rn) return;
    d = (char *)(rn + 1);
    CopyMem((APTR)text, d, dl);
    rn->node.ln_Type = NT_UNKNOWN;
    rn->node.ln_Pri  = 0;
    rn->node.ln_Name = d;
    rn->pathLen      = (ULONG)(pl - 1);
    if (shared) {
        rn->path = d;
    } else {
        rn->path = d + dl;
        CopyMem((APTR)path, rn->path, pl);
    }
    AddTail(&g->results, &rn->node);
}

/* copy as much of src as fits in width pixels of the gadget font */
static void fitText(struct Gui *g, const char *src, char *dst, int size, int width)
{
    struct TextExtent te;
    ULONG n = strlen(src);
    if (g->font && width > 0)
        n = TextFit(&g->rp, (STRPTR)src, n, &te, NULL, 1, width, 32767);
    if ((int)n > size - 1) n = size - 1;
    CopyMem((APTR)src, dst, n);
    dst[n] = '\0';
}

static void setStatus(struct Gui *g, CONST_STRPTR text)
{
    strncpy(g->status, (const char *)text, sizeof g->status - 1);
    g->status[sizeof g->status - 1] = '\0';
    if (g->win && g->gStatus) {      /* both must be live (iconified -> neither) */
        fitText(g, g->status, g->statusShown, sizeof g->statusShown, g->gStatus->Width - 6);
        GT_SetGadgetAttrs(g->gStatus, g->win, NULL,
                          GTTX_Text, (ULONG)g->statusShown, TAG_END);
    }
}

/* ------------------------------------------------------------------ */
/* search callbacks                                                   */
/* ------------------------------------------------------------------ */

static BOOL guiFound(CONST_STRPTR path, ULONG line, CONST_STRPTR text, APTR user)
{
    struct Gui *g = (struct Gui *)user;
    char buf[300];
    int  l;

    strncpy(buf, (const char *)path, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    if (line) {                                   /* full match (path:line: text) */
        l = (int)strlen(buf);
        if (l < (int)sizeof buf - 2) { buf[l++] = ':'; buf[l] = '\0'; appendNum(buf, line, (int)sizeof buf); }
        l = (int)strlen(buf);
        if (l < (int)sizeof buf - 3) { buf[l++] = ':'; buf[l++] = ' '; buf[l] = '\0';
            strncat(buf, (const char *)text, sizeof buf - 1 - strlen(buf)); }
    }
    addResult(g, path, buf);
    return TRUE;
}

/* the user data of every item picked in one MENUPICK, in order */
static int menuPicks(struct Gui *g, UWORD code, ULONG *ids, int max)
{
    int n = 0;
    while (code != MENUNULL && g->menu && n < max) {
        struct MenuItem *it = ItemAddress(g->menu, code);
        if (!it) break;
        ids[n++] = (ULONG)GTMENUITEM_USERDATA(it);
        code = it->NextSelect;
    }
    return n;
}

/* process pending Intuition messages without blocking; returns TRUE to abort */
static BOOL pumpMessages(struct Gui *g)
{
    struct IntuiMessage *imsg;
    while ((imsg = GT_GetIMsg(g->win->UserPort))) {
        ULONG    cls = imsg->Class;
        UWORD    code = imsg->Code;
        struct Gadget *gad = (struct Gadget *)imsg->IAddress;
        GT_ReplyIMsg(imsg);

        switch (cls) {
        case IDCMP_CLOSEWINDOW:
            g->closeReq = TRUE;
            g->stopReq  = TRUE;
            break;
        case IDCMP_GADGETUP:
            if (gad->GadgetID == GAD_STOP)
                g->stopReq = TRUE;
            else if (gad->GadgetID == GAD_CASE)  /* takes effect next search */
                g->caseSens = (code != 0) ? TRUE : FALSE;
            break;
        case IDCMP_MENUPICK: {         /* only Stop and Quit mean anything now */
            ULONG ids[8]; int i, n = menuPicks(g, code, ids, 8);
            for (i = 0; i < n; i++) {
                if (ids[i] == MN_STOP) g->stopReq = TRUE;
                if (ids[i] == MN_QUIT) { g->closeReq = TRUE; g->stopReq = TRUE; }
            }
            break;
        }
        case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(g->win);
            GT_EndRefresh(g->win, TRUE);
            break;
        case IDCMP_NEWSIZE:
            /* can't relayout mid-search (it frees the gadgets we're driving);
             * rebuild once the scan returns */
            g->resizePending = TRUE;
            break;
        default:
            break;
        }
    }
    return g->stopReq;
}

static BOOL guiPoll(APTR user)
{
    struct Gui *g = (struct Gui *)user;
    BOOL abort = pumpMessages(g);
    if (((++g->uiCounter) & 1023) == 0)
        setStatus(g, "Searching...");
    return abort;
}

/* ------------------------------------------------------------------ */
/* run a search from the current gadget contents                      */
/* ------------------------------------------------------------------ */

static CONST_STRPTR gadgetString(struct Gadget *g)
{
    struct StringInfo *si = (struct StringInfo *)g->SpecialInfo;
    return (CONST_STRPTR)si->Buffer;
}

/* pop up an ASL drawer requester and put the chosen path in the In field */
static void doPickPath(struct Gui *g)
{
    struct FileRequester *fr;
    if (!AslBase || !g->gPath) return;    /* asl.library not available */

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Select a drawer to search",
            ASLFR_DrawersOnly,   TRUE,
            ASLFR_InitialDrawer, (ULONG)gadgetString(g->gPath),
            ASLFR_InitialWidth,  300,
            ASLFR_InitialHeight, 170,
            TAG_END);
    if (!fr) return;

    if (AslRequestTags(fr, ASLFR_Window, (ULONG)g->win, TAG_END)) {
        char buf[256];
        strncpy(buf, (const char *)fr->fr_Drawer, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        GT_SetGadgetAttrs(g->gPath, g->win, NULL, GTST_String, (ULONG)buf, TAG_END);
    }
    FreeAslRequest(fr);
}

static void relayout(struct Gui *g);

static void doSearch(struct Gui *g)
{
    struct Matcher     m;
    struct NameFilter  nf;
    struct GrepStats   st;
    CONST_STRPTR pat, path, files;
    LONG err;

    if (!g->gPattern) return;              /* no gadgets (low memory) */
    pat   = gadgetString(g->gPattern);
    path  = gadgetString(g->gPath);
    files = gadgetString(g->gFiles);
    if (!path || path[0] == '\0') { setStatus(g, "Enter a drawer to search."); return; }
    if (!pat  || pat[0]  == '\0') { setStatus(g, "Enter text to find.");     return; }
    if (!matcherInit(&m, pat, g->caseSens))  { setStatus(g, "Bad search pattern."); return; }
    if (!nameFilterInit(&nf, files))         { setStatus(g, "Bad Files pattern.");  return; }

    /* detach old list, free it, clear the Hit line */
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)~0, TAG_END);
    freeResults(g);
    if (g->gHit) GT_SetGadgetAttrs(g->gHit, g->win, NULL, GTST_String, (ULONG)"", TAG_END);

    g->stopReq = FALSE;
    g->uiCounter = 0;
    g->sel = g->lastSel = -1;          /* reset selection + double-click tracking */
    GT_SetGadgetAttrs(g->gSearch, g->win, NULL, GA_Disabled, TRUE, TAG_END);
    setStatus(g, "Searching...");

    st.namesOnly = FALSE;
    err = grepTree(path, &m, &nf, guiFound, g, guiPoll, g, &st);

    /* attach collected results */
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)&g->results,
                      GTLV_Selected, (ULONG)~0, TAG_END);
    GT_SetGadgetAttrs(g->gSearch, g->win, NULL, GA_Disabled, FALSE, TAG_END);

    if (err) {
        setStatus(g, "Cannot open that drawer.");
    } else {
        char buf[80];
        buf[0] = '\0';
        appendNum(buf, st.linesMatched, (int)sizeof buf);
        strcat(buf, " line(s) in ");
        appendNum(buf, st.filesMatched, (int)sizeof buf);
        strcat(buf, st.aborted ? " file(s) - stopped" : " file(s)");
        setStatus(g, buf);
    }

    /* a resize that arrived mid-search was deferred; apply it now */
    if (g->resizePending && !g->closeReq) { g->resizePending = FALSE; relayout(g); }
}

static void doClear(struct Gui *g)
{
    if (!g->gList) return;
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)~0, TAG_END);
    freeResults(g);
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)&g->results,
                      GTLV_Selected, (ULONG)~0, TAG_END);
    if (g->gHit) GT_SetGadgetAttrs(g->gHit, g->win, NULL, GTST_String, (ULONG)"", TAG_END);
    g->sel = g->lastSel = -1;
    setStatus(g, "Ready");
}

/* ------------------------------------------------------------------ */
/* gadget / window construction                                       */
/* ------------------------------------------------------------------ */

static WORD tlen(struct Gui *g, const char *s)
{
    return (WORD)TextLength(&g->rp, (STRPTR)s, strlen(s));
}

static WORD wmax(WORD a, WORD b) { return a > b ? a : b; }

/* the layout's measures in the chosen font */
static void measure(struct Gui *g)
{
    g->fh    = g->font->tf_YSize;
    g->lab   = wmax(wmax(tlen(g, "Find"), tlen(g, "In")),
                    wmax(tlen(g, "Files"), tlen(g, "Hit"))) + 12;
    g->bw    = wmax(tlen(g, "Search"), tlen(g, "Stop")) + 24;
    g->pickw = tlen(g, "...") + 16;
    g->casew = tlen(g, "Case") + 8 + CB_W + 4;   /* label + checkbox + gap */
}

/* the smallest inner size the layout works at */
static void minInner(struct Gui *g, WORD *w, WORD *h)
{
    WORD row = g->fh + 6, gh = g->fh + 3;
    *w = wmax(wmax(g->lab + 120 + 4 + g->pickw, g->lab + 80 + g->casew),
              2 * g->bw + 14 + 100) + 4;
    *h = 2 + 4 * row + 2 + (2 * g->fh + 6) + 3 + gh + 1;
}

static void minOuter(struct Gui *g, WORD *w, WORD *h)
{
    struct Screen *sc = g->scr;
    minInner(g, w, h);
    *w += sc->WBorLeft + sc->WBorRight + 4;
    *h += sc->WBorTop + sc->Font->ta_YSize + 1 + 11;    /* title bar; size gadget below */
}

/* the screen font, or Topaz 8 when the smallest layout won't fit in it */
static BOOL chooseFont(struct Gui *g)
{
    struct Screen *sc = g->scr;
    WORD w, h;
    g->ta = *sc->Font;
    if ((g->font = OpenFont(&g->ta)) != NULL) {
        InitRastPort(&g->rp);
        SetFont(&g->rp, g->font);
        measure(g);
        minOuter(g, &w, &h);
        if (w <= sc->Width && h <= sc->Height - sc->BarHeight - 2)
            return TRUE;
        CloseFont(g->font);
    }
    g->ta = topaz8;
    if (!(g->font = OpenFont(&g->ta))) return FALSE;
    InitRastPort(&g->rp);
    SetFont(&g->rp, g->font);
    measure(g);
    return TRUE;
}

static BOOL makeGadgets(struct Gui *g)
{
    struct NewGadget ng;
    struct Gadget *gad;
    struct Window *w = g->win;
    WORD fh  = g->fh;
    WORD cl  = w->BorderLeft + 2;            /* content left  (window coords) */
    WORD cr  = w->Width  - w->BorderRight - 2;
    WORD cb  = w->Height - w->BorderBottom - 1;
    WORD row = fh + 6;
    WORD gh  = fh + 3;
    WORD lab = g->lab;                       /* label column */
    WORD sx  = cl + 2 * g->bw + 14;          /* status text left */
    WORD y;

    gad = CreateContext(&g->glist);
    if (!gad) return FALSE;
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = g->vi;
    ng.ng_TextAttr   = &g->ta;

    y = w->BorderTop + 2;

    /* Find string */
    ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + lab); ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Find"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_PATTERN;
    gad = g->gPattern = CreateGadget(STRING_KIND, gad, &ng,
                                     GTST_MaxChars, 200,
                                     GTST_String, (ULONG)g->savePat, TAG_END);
    /* In string (leave room for the picker button) */
    y += row;
    ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + lab) - g->pickw - 4;
    ng.ng_GadgetText = (UBYTE *)"In"; ng.ng_GadgetID = GAD_PATH;
    gad = g->gPath = CreateGadget(STRING_KIND, gad, &ng,
                                  GTST_MaxChars, 200,
                                  GTST_String, (ULONG)g->savePath, TAG_END);
    /* picker "..." */
    ng.ng_LeftEdge = cr - g->pickw; ng.ng_Width = g->pickw;
    ng.ng_GadgetText = (UBYTE *)"..."; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_GadgetID = GAD_PICK;
    gad = g->gPick = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* Files filter + Case checkbox */
    y += row;
    ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + lab) - g->casew; if (ng.ng_Width < 40) ng.ng_Width = 40;
    ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Files"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_FILES;
    gad = g->gFiles = CreateGadget(STRING_KIND, gad, &ng,
                                   GTST_MaxChars, 100,
                                   GTST_String, (ULONG)g->saveFiles, TAG_END);
    /* Case checkbox at the right of the Files row. CHECKBOX_KIND uses a fixed
     * ~26px image and ignores ng_Width, so seat its right edge inside cr. */
    ng.ng_LeftEdge = cr - CB_W - 2; ng.ng_Width = CB_W;
    ng.ng_GadgetText = (UBYTE *)"Case"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_CASE;
    gad = g->gCase = CreateGadget(CHECKBOX_KIND, gad, &ng,
                                  GTCB_Checked, (ULONG)g->caseSens, TAG_END);

    /* Search / Stop + status */
    y += row;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y; ng.ng_Width = g->bw; ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Search"; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_SEARCH;
    gad = g->gSearch = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + g->bw + 6;
    ng.ng_GadgetText = (UBYTE *)"Stop"; ng.ng_GadgetID = GAD_STOP;
    gad = g->gStop = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    /* status text, cut to fit (GTTX_Clipped is V39+, so also by hand) */
    ng.ng_LeftEdge = sx; ng.ng_Width = cr - sx;
    if (ng.ng_Width < 8) ng.ng_Width = 8;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GAD_STATUS;
    fitText(g, g->status, g->statusShown, sizeof g->statusShown, ng.ng_Width - 6);
    gad = g->gStatus = CreateGadget(TEXT_KIND, gad, &ng,
                                    GTTX_Text, (ULONG)g->statusShown,
                                    GTTX_Border, FALSE, GTTX_Clipped, TRUE, TAG_END);
    /* Listview fills the middle; a scrollable "Hit" line sits at the bottom
     * showing the full path:line: text of the clicked result (GadTools lists
     * have no horizontal scroll, so this is how you read long lines). */
    y += row + 2;
    {
        WORD hitTop = cb - gh;                 /* bottom row */
        WORD listH  = (hitTop - 3) - y;
        struct Node *n;
        const char *hit = "";
        LONG i = 0;
        if (listH < gh) listH = gh;
        for (n = g->results.lh_Head; n->ln_Succ; n = n->ln_Succ, i++)
            if (i == g->sel) { hit = n->ln_Name; break; }
        ng.ng_LeftEdge = cl; ng.ng_TopEdge = y;
        ng.ng_Width = cr - cl; ng.ng_Height = listH;
        ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_LIST;
        gad = g->gList = CreateGadget(LISTVIEW_KIND, gad, &ng,
                                      GTLV_Labels, (ULONG)&g->results,
                                      GTLV_ShowSelected, 0,   /* highlight the picked hit */
                                      GTLV_Selected, (ULONG)(g->sel >= 0 ? g->sel : ~0),
                                      GTLV_MakeVisible, (ULONG)(g->sel >= 0 ? g->sel : 0),
                                      TAG_END);
        ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = hitTop;
        ng.ng_Width = cr - (cl + lab); ng.ng_Height = gh;
        ng.ng_GadgetText = (UBYTE *)"Hit"; ng.ng_Flags = PLACETEXT_LEFT;
        ng.ng_GadgetID = GAD_HIT;
        gad = g->gHit = CreateGadget(STRING_KIND, gad, &ng,
                                     GTST_MaxChars, 300,
                                     GTST_String, (ULONG)hit, TAG_END);
    }
    return (gad != NULL);
}

/* save the current field text into the persistent buffers */
static void saveFields(struct Gui *g)
{
    if (!g->glist || !g->gPattern) return;
    strncpy(g->savePat, (const char *)gadgetString(g->gPattern), sizeof g->savePat - 1);
    g->savePat[sizeof g->savePat - 1] = '\0';
    strncpy(g->savePath, (const char *)gadgetString(g->gPath), sizeof g->savePath - 1);
    g->savePath[sizeof g->savePath - 1] = '\0';
    strncpy(g->saveFiles, (const char *)gadgetString(g->gFiles), sizeof g->saveFiles - 1);
    g->saveFiles[sizeof g->saveFiles - 1] = '\0';
}

static void relayout(struct Gui *g)
{
    if (g->glist) {                          /* preserve typed-in text */
        saveFields(g);
        RemoveGList(g->win, g->glist, -1);
        FreeGadgets(g->glist);
        g->glist = NULL;
        EraseRect(g->win->RPort, g->win->BorderLeft, g->win->BorderTop,
                  g->win->Width - g->win->BorderRight - 1,
                  g->win->Height - g->win->BorderBottom - 1);
    }

    if (makeGadgets(g)) {
        AddGList(g->win, g->glist, ~0, -1, NULL);
        RefreshGList(g->glist, g->win, NULL, -1);
        GT_RefreshWindow(g->win, NULL);
    } else {
        /* low memory: free the partial, never-added list and drop every
         * gadget pointer so nothing touches freed gadgets */
        if (g->glist) { FreeGadgets(g->glist); g->glist = NULL; }
        g->gPattern = g->gPath = g->gPick = g->gFiles = g->gCase = NULL;
        g->gSearch  = g->gStop = g->gStatus = g->gList = g->gHit  = NULL;
    }
}

/* the right mouse button menu */
static struct NewMenu amiNewMenu[] = {
    { NM_TITLE, (STRPTR)"Project",           NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"About...",          (STRPTR)"?", 0, 0, (APTR)MN_ABOUT },
    { NM_ITEM,  (STRPTR)"Iconify",           (STRPTR)"I", 0, 0, (APTR)MN_ICONIFY },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,         NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",              (STRPTR)"Q", 0, 0, (APTR)MN_QUIT },
    { NM_TITLE, (STRPTR)"Search",            NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Search",            (STRPTR)"S", 0, 0, (APTR)MN_SEARCH },
    { NM_ITEM,  (STRPTR)"Stop",              (STRPTR)".", 0, 0, (APTR)MN_STOP },
    { NM_ITEM,  (STRPTR)"Choose drawer...",  (STRPTR)"D", 0, 0, (APTR)MN_PICK },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,         NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Clear results",     (STRPTR)"L", 0, 0, (APTR)MN_CLEAR },
    { NM_TITLE, (STRPTR)"Result",            NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"View",              (STRPTR)"V", 0, 0, (APTR)MN_VIEW },
    { NM_ITEM,  (STRPTR)"Open drawer",       (STRPTR)"O", 0, 0, (APTR)MN_OPENDRAWER },
    { NM_TITLE, (STRPTR)"Settings",          NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Search defaults...",NULL,        0, 0, (APTR)MN_SET_SEARCH },
    { NM_ITEM,  (STRPTR)"Programs...",       NULL,        0, 0, (APTR)MN_SET_PROGRAMS },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,         NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Snapshot window",   NULL,        0, 0, (APTR)MN_SNAPSHOT },
    { NM_ITEM,  (STRPTR)"Unsnapshot window", NULL,        0, 0, (APTR)MN_UNSNAPSHOT },
    { NM_END,   NULL,                        NULL,        0, 0, NULL }
};

static void setupMenu(struct Gui *g)
{
    if (!GadToolsBase) return;
    g->menu = CreateMenus(amiNewMenu, TAG_END);
    if (g->menu) {
        if (LayoutMenus(g->menu, g->vi, TAG_END))
            SetMenuStrip(g->win, g->menu);
        else { FreeMenus(g->menu); g->menu = NULL; }
    }
}

static BOOL buildWindow(struct Gui *g)
{
    struct Screen *sc;
    WORD wtop, winw, winh, minw, minh, left;

    g->scr = LockPubScreen(NULL);
    if (!g->scr) return FALSE;
    g->vi = GetVisualInfo(g->scr, TAG_END);
    if (!g->vi) return FALSE;
    if (!chooseFont(g)) return FALSE;
    sc = g->scr;
    strcpy(g->status, "Ready");

    /* a classic-sized default window: 520x240 at most (never spread over a
     * wide multi-monitor screen), clamped to the screen. On a 640x200 NTSC
     * screen with Topaz 8 that is 520x188 under the title bar. */
    minOuter(g, &minw, &minh);
    wtop = sc->BarHeight + 2;
    winw = 520; if (winw < minw) winw = minw; if (winw > sc->Width) winw = sc->Width;
    winh = 240; if (winh > sc->Height - wtop) winh = sc->Height - wtop;
    if (winh < minh) winh = minh;
    left = (sc->Width - winw) / 2;
    if (left > 40) left = 40;               /* near the left, like a classic window */
    if (left < 0) left = 0;
    if (g_set.winw > 0) {                   /* Settings > Snapshot window, kept on screen */
        left = (WORD)g_set.winx; wtop = (WORD)g_set.winy;
        winw = (WORD)g_set.winw; winh = (WORD)g_set.winh;
        if (winw > sc->Width)  winw = sc->Width;
        if (winh > sc->Height) winh = sc->Height;
        if (winw < minw) winw = minw;
        if (winh < minh) winh = minh;
        if (left + winw > sc->Width)  left = sc->Width - winw;
        if (wtop + winh > sc->Height) wtop = sc->Height - winh;
        if (left < 0) left = 0;
        if (wtop < 0) wtop = 0;
    }

    g->win = OpenWindowTags(NULL,
        WA_Title,     (ULONG)"AmiGrep",
        WA_Left,      left, WA_Top, wtop,
        WA_Width,     winw, WA_Height, winh,
        WA_MinWidth,  minw, WA_MinHeight, minh,
        WA_MaxWidth,  sc->Width, WA_MaxHeight, sc->Height,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                      WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE |
                      IDCMP_MENUPICK | BUTTONIDCMP | STRINGIDCMP | LISTVIEWIDCMP |
                      CHECKBOXIDCMP,
        WA_PubScreen, (ULONG)g->scr,
        TAG_END);
    if (!g->win) return FALSE;

    relayout(g);                             /* build + attach the gadgets   */
    setupMenu(g);                            /* the right-button menu        */
    return TRUE;
}

/* close just the window + its display resources (results are preserved) */
static void closeWin(struct Gui *g)
{
    if (g->win && g->menu)  { ClearMenuStrip(g->win); }
    if (g->menu)            { FreeMenus(g->menu); g->menu = NULL; }
    if (g->win)   { CloseWindow(g->win);          g->win = NULL; }
    if (g->glist) { FreeGadgets(g->glist);        g->glist = NULL; }
    if (g->font)  { CloseFont(g->font);            g->font = NULL; }
    if (g->vi)    { FreeVisualInfo(g->vi);         g->vi = NULL; }
    if (g->scr)   { UnlockPubScreen(NULL, g->scr); g->scr = NULL; }
    /* FreeGadgets just freed every gadget; drop the dangling pointers so a
     * stray setStatus()/refresh while iconified can't touch freed memory. */
    g->gPattern = g->gPath = g->gPick = g->gFiles = g->gCase = NULL;
    g->gSearch  = g->gStop = g->gStatus = g->gList = g->gHit  = NULL;
}

/* hide the window to a Workbench AppIcon */
static void doIconify(struct Gui *g)
{
    if (!WorkbenchBase || !IconBase) return;     /* not available */

    saveFields(g);
    if (!g->appPort) g->appPort = CreateMsgPort();
    if (!g->appPort) return;
    if (!g->dobj) {
        g->dobj = (struct DiskObject *)GetDiskObject((STRPTR)"PROGDIR:AmiGrepGUI");
        if (!g->dobj) g->dobj = (struct DiskObject *)GetDiskObject((STRPTR)"ENV:Sys/def_Tool");
        if (!g->dobj) { setStatus(g, "No icon available to iconify."); return; }
    }
    closeWin(g);
    g->appIcon = AddAppIconA(0L, 0L, (STRPTR)"AmiGrep", g->appPort,
                             (BPTR)0, g->dobj, NULL);
    if (!g->appIcon) {
        /* AppIcon failed: we already closed the window, so reopen it rather
         * than sit with neither a window nor an icon (eventLoop would Wait()
         * on the AppIcon port forever - an unkillable task). */
        if (!buildWindow(g)) { g->quit = TRUE; return; }  /* truly stuck: bail */
        setStatus(g, "Could not iconify.");
        return;
    }
    g->iconified = TRUE;
}

/* bring the window back from the AppIcon */
static void doUniconify(struct Gui *g)
{
    struct Message *m;
    if (g->appIcon) { RemoveAppIcon(g->appIcon); g->appIcon = NULL; }
    if (g->appPort) while ((m = GetMsg(g->appPort))) ReplyMsg(m);
    g->iconified = FALSE;
    /* If the window can't be reopened (screen lock / no RAM) there is nothing
     * left to drive the loop, so quit cleanly instead of hanging. */
    if (!buildWindow(g)) g->quit = TRUE;         /* restores saved fields */
}

/* ------------------------------------------------------------------ */
/* acting on a result: View, Open drawer (double-click)               */
/* ------------------------------------------------------------------ */

/* derive the drawer (containing directory) of an AmigaDOS path:
 *   "SYS:s/Startup-Sequence" -> "SYS:s"   ; "SYS:Disk.info" -> "SYS:"   */
static void resultDrawer(CONST_STRPTR path, int len, char *buf, int bufsz)
{
    int i, n = 0;
    for (i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { n = i;     break; }   /* drop the slash       */
        if (path[i] == ':') { n = i + 1; break; }   /* keep the volume ':'  */
    }
    if (n <= 0) { buf[0] = '\0'; return; }
    if (n >= bufsz) n = bufsz - 1;
    CopyMem((APTR)path, buf, n);
    buf[n] = '\0';
}

static struct ResNode *nthResult(struct Gui *g, LONG sel)
{
    struct Node *n;
    LONG i = 0;
    if (sel < 0) return NULL;
    for (n = g->results.lh_Head; n->ln_Succ; n = n->ln_Succ, i++)
        if (i == sel) return (struct ResNode *)n;
    return NULL;
}

static struct ResNode *selected(struct Gui *g)
{
    struct ResNode *rn = nthResult(g, g->sel);
    if (!rn) setStatus(g, "Click a result first.");
    return rn;
}

/* drop the full clicked line into the scrollable Hit field */
static void showHit(struct Gui *g, LONG sel)
{
    struct ResNode *rn = nthResult(g, sel);
    if (rn && g->gHit)
        GT_SetGadgetAttrs(g->gHit, g->win, NULL,
                          GTST_String, (ULONG)rn->node.ln_Name, TAG_END);
}

static void doOpenDrawer(struct Gui *g)
{
    struct ResNode *rn = selected(g);
    char drawer[300];

    if (!rn) return;
    /* OpenWorkbenchObject() is a V44 (OS 3.5+) LVO; we open the library at
     * v37 for AddAppIconA, so calling it on OS 3.0/3.1 would jump into a
     * nonexistent vector and guru. Check the actual library version. */
    if (!WorkbenchBase || WorkbenchBase->lib_Version < 44) {
        setStatus(g, "Open drawer needs OS 3.5+.");
        return;
    }
    resultDrawer((CONST_STRPTR)rn->path, (int)rn->pathLen, drawer, (int)sizeof drawer);
    if (drawer[0] == '\0') { setStatus(g, "Cannot tell its drawer."); return; }

    if (OpenWorkbenchObject((STRPTR)drawer, TAG_END))
        setStatus(g, "Opened the drawer.");
    else
        setStatus(g, "Could not open the drawer.");
}

static void doView(struct Gui *g)
{
    struct ResNode *rn = selected(g);
    char cmd[400];
    BPTR in, out;
    LONG rc;
    int l;

    if (!rn) return;
    setStr(cmd, sizeof cmd, g_set.viewer);
    strcat(cmd, " \"");
    l = (int)strlen(cmd);
    if (l + (int)rn->pathLen + 2 > (int)sizeof cmd) { setStatus(g, "The path is too long."); return; }
    CopyMem((APTR)rn->path, cmd + l, rn->pathLen);
    cmd[l + rn->pathLen] = '\0';
    strcat(cmd, "\"");

    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    rc = SystemTags((STRPTR)cmd, SYS_Input, (ULONG)in, SYS_Output, (ULONG)out,
                    SYS_Asynch, TRUE, TAG_END);
    if (rc != 0) {                     /* on success System() owns in/out */
        if (in) Close(in);
        if (out) Close(out);
        setStatus(g, "Could not start the viewer.");
    } else
        setStatus(g, "Started the viewer.");
}

/* ------------------------------------------------------------------ */
/* Settings pages (amigui frame: Save / Use / Defaults / Cancel)      */
/* ------------------------------------------------------------------ */

#define SP_MAX        3
#define ID_SP_CURRENT 50               /* "Use current" button */
#define ID_SP_CHECK   51               /* the page's checkbox  */
#define SP_CHECK      SP_MAX           /* its slot in pg->g[]  */

struct SetPage {
    const char *title, *hint;
    int n;
    const char *label[SP_MAX];
    char       *val[SP_MAX];           /* into g_set */
    int         size[SP_MAX];
    const char *def[SP_MAX];           /* the built-in defaults */
    const char *cur[SP_MAX];           /* "Use current": the main window's fields */
    BOOL        current;
    const char *checkLabel;            /* a checkbox under the fields, or NULL */
    BOOL       *check, checkDef, checkCur;
};

static void spGadget(AmgPage *pg, int id, UWORD code, void *ctx)
{
    struct SetPage *sp = (struct SetPage *)ctx;
    int i;
    (void)code;
    if (id != AMG_ID_DEFAULTS && id != ID_SP_CURRENT) return;
    for (i = 0; i < sp->n; i++) {
        const char *v = id == AMG_ID_DEFAULTS ? sp->def[i] : sp->cur[i];
        if (v)
            GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, GTST_String, (ULONG)v, TAG_END);
    }
    if (sp->checkLabel)
        GT_SetGadgetAttrs(pg->g[SP_CHECK], pg->win, NULL, GTCB_Checked,
                          (ULONG)(id == AMG_ID_DEFAULTS ? sp->checkDef : sp->checkCur), TAG_END);
}

/* the page's inner size in font ta; FALSE if the font can't be opened */
static BOOL spSize(struct SetPage *sp, struct TextAttr *ta, int *lw, int *w, int *h)
{
    struct TextFont *f = OpenFont(ta);
    struct RastPort rp;
    int i, bh, rowh, fw, hw;
    if (!f) return FALSE;
    InitRastPort(&rp);
    SetFont(&rp, f);
    *lw = 0;
    for (i = 0; i < sp->n; i++) {
        int l = TextLength(&rp, (STRPTR)sp->label[i], strlen(sp->label[i]));
        if (l > *lw) *lw = l;
    }
    if (sp->checkLabel) {
        int l = TextLength(&rp, (STRPTR)sp->checkLabel, strlen(sp->checkLabel));
        if (l > *lw) *lw = l;
    }
    hw = TextLength(&rp, (STRPTR)sp->hint, strlen(sp->hint));
    fw = 36 * f->tf_XSize;                       /* a field ~36 characters wide */
    bh = f->tf_YSize + 6;
    CloseFont(f);
    *lw += 12;
    rowh = bh + 4;
    *w = 2 * 8 + *lw + fw;
    if (*w < 2 * 8 + hw) *w = 2 * 8 + hw;
    if (*w < amg_min_width(ta)) *w = amg_min_width(ta);
    *h = 4 + sp->n * rowh + (sp->checkLabel ? rowh : 0) + rowh +
         (sp->current ? rowh : 0) + 6 + bh + 4;
    return TRUE;
}

/* Main window busy while a page is open: clicks made meanwhile are dropped */
static void flushMain(struct Gui *g)
{
    struct IntuiMessage *imsg;
    if (!g->win) return;
    while ((imsg = GT_GetIMsg(g->win->UserPort))) {
        ULONG cls = imsg->Class;
        GT_ReplyIMsg(imsg);
        if (cls == IDCMP_REFRESHWINDOW) {
            GT_BeginRefresh(g->win);
            GT_EndRefresh(g->win, TRUE);
        } else if (cls == IDCMP_NEWSIZE)
            g->resizePending = TRUE;
    }
}

static void runSetPage(struct Gui *g, struct SetPage *sp)
{
    AmgPage pg;
    struct NewGadget ng;
    struct Gadget *gad;
    struct TextAttr *ta = &g->ta;
    struct Screen *sc = g->scr;
    int i, lw, w, h, bh, rowh, y, r = AMG_CANCEL;

    /* the gadgets' font if the page fits the screen in it, else Topaz 8 */
    if (!spSize(sp, ta, &lw, &w, &h) || w > sc->Width - 20 ||
        h > sc->Height - sc->BarHeight - 1 - sc->WBorTop - sc->Font->ta_YSize - 4) {
        ta = &topaz8;
        if (!spSize(sp, ta, &lw, &w, &h)) return;
    }
    if (!amg_page_open(&pg, sc, g->vi, ta, g->win, sp->title, w, h)) return;
    if (IntuitionBase->LibNode.lib_Version >= 39)
        SetWindowPointer(g->win, WA_BusyPointer, TRUE, TAG_END);
    bh = amg_bh(&pg);
    rowh = bh + 4;
    gad = CreateContext(&pg.glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = g->vi; ng.ng_TextAttr = ta;
    ng.ng_LeftEdge = pg.win->BorderLeft + 8 + lw; ng.ng_Width = w - 2 * 8 - lw;
    ng.ng_Height = bh; ng.ng_Flags = PLACETEXT_LEFT;
    y = 4;
    for (i = 0; i < sp->n; i++) {
        ng.ng_TopEdge = pg.win->BorderTop + y; y += rowh;
        ng.ng_GadgetText = (STRPTR)sp->label[i]; ng.ng_GadgetID = 1 + i;
        pg.g[i] = gad = CreateGadget(STRING_KIND, gad, &ng, GTST_String, (ULONG)sp->val[i],
                                     GTST_MaxChars, sp->size[i] - 1, TAG_END);
    }
    if (sp->checkLabel) {
        ng.ng_TopEdge = pg.win->BorderTop + y + (bh - 11) / 2; y += rowh;
        ng.ng_Width = CB_W; ng.ng_Height = 11;
        ng.ng_GadgetText = (STRPTR)sp->checkLabel; ng.ng_GadgetID = ID_SP_CHECK;
        pg.g[SP_CHECK] = gad = CreateGadget(CHECKBOX_KIND, gad, &ng,
                                            GTCB_Checked, (ULONG)*sp->check, TAG_END);
        ng.ng_Height = bh;
    }
    ng.ng_LeftEdge = pg.win->BorderLeft + 8; ng.ng_Width = w - 2 * 8;   /* the hint line */
    ng.ng_TopEdge = pg.win->BorderTop + y; y += rowh;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = 0; ng.ng_Flags = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)sp->hint, GTTX_Clipped, TRUE, TAG_END);
    if (sp->current) {
        ng.ng_TopEdge = pg.win->BorderTop + y; y += rowh;
        ng.ng_Width = amg_tlen(&pg, "Use current") + 24; ng.ng_Flags = PLACETEXT_IN;
        ng.ng_GadgetText = (STRPTR)"Use current"; ng.ng_GadgetID = ID_SP_CURRENT;
        gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    }
    gad = amg_buttons(&pg, gad, y + 6, w, 1);
    if (gad) {
        amg_page_show(&pg);
        r = amg_page_run(&pg, spGadget, sp);
        if (r != AMG_CANCEL) {
            for (i = 0; i < sp->n; i++) {
                const char *s = amg_string(pg.g[i]);
                if (!*s && sp->def[i][0]) s = sp->def[i];    /* empty = the default */
                setStr(sp->val[i], sp->size[i], s);
            }
            if (sp->checkLabel)
                *sp->check = amg_checked(pg.g[SP_CHECK]) ? TRUE : FALSE;
        }
    }
    amg_page_close(&pg);
    if (IntuitionBase->LibNode.lib_Version >= 39)
        SetWindowPointer(g->win, TAG_END);
    flushMain(g);
    if (r == AMG_CANCEL) return;
    if (!setsWrite(&g_set, r == AMG_SAVE))
        setStatus(g, r == AMG_SAVE ? "Could not write ENVARC:AmiGrep.prefs."
                                   : "Could not write ENV:AmiGrep.prefs.");
    else
        setStatus(g, r == AMG_SAVE ? "Settings saved." : "Settings in use until reboot.");
}

static void settingsPage(struct Gui *g, ULONG which)
{
    static struct Settings def;
    struct SetPage sp;
    setsDefault(&def);
    memset(&sp, 0, sizeof sp);
    if (which == MN_SET_SEARCH) {
        saveFields(g);
        sp.title = "AmiGrep: Search defaults";
        sp.hint  = "Filled in when AmiGrep starts.";
        sp.n = 3; sp.current = TRUE;
        sp.label[0] = "Find";  sp.val[0] = g_set.pattern; sp.size[0] = sizeof g_set.pattern;
        sp.def[0] = def.pattern; sp.cur[0] = g->savePat;
        sp.label[1] = "In";    sp.val[1] = g_set.path;    sp.size[1] = sizeof g_set.path;
        sp.def[1] = def.path;    sp.cur[1] = g->savePath;
        sp.label[2] = "Files"; sp.val[2] = g_set.files;   sp.size[2] = sizeof g_set.files;
        sp.def[2] = def.files;   sp.cur[2] = g->saveFiles;
        sp.checkLabel = "Case"; sp.check = &g_set.caseSens;
        sp.checkDef = def.caseSens; sp.checkCur = g->caseSens;
    } else {
        sp.title = "AmiGrep: Programs";
        sp.hint  = "View runs: <viewer> \"<file>\"";
        sp.n = 1;
        sp.label[0] = "Viewer"; sp.val[0] = g_set.viewer; sp.size[0] = sizeof g_set.viewer;
        sp.def[0] = def.viewer;
    }
    runSetPage(g, &sp);
}

/* one menu pick (by its MN_* user data); TRUE = quit */
static BOOL doMenu(struct Gui *g, ULONG id)
{
    switch (id) {
    case MN_ABOUT:
        amg_about(g->win, "AmiGrep", AMIGREP_VERSION,
                  "Finds text inside the files of\n"
                  "any volume, assign or drawer.\n\n"
                  "Licence: GPL-3.0");
        break;
    case MN_ICONIFY:      doIconify(g); break;
    case MN_QUIT:         return TRUE;
    case MN_SEARCH:       doSearch(g); break;
    case MN_PICK:         doPickPath(g); break;
    case MN_CLEAR:        doClear(g); break;
    case MN_VIEW:         doView(g); break;
    case MN_OPENDRAWER:   doOpenDrawer(g); break;
    case MN_SET_SEARCH:
    case MN_SET_PROGRAMS: settingsPage(g, id); break;
    case MN_SNAPSHOT:     /* this place and size from now on (saved, like Save) */
        g_set.winx = g->win->LeftEdge; g_set.winy = g->win->TopEdge;
        g_set.winw = g->win->Width;    g_set.winh = g->win->Height;
        setStatus(g, setsWrite(&g_set, TRUE) ? "Window place and size saved."
                                             : "Could not write ENVARC:AmiGrep.prefs.");
        break;
    case MN_UNSNAPSHOT:   /* back to the default place and size at the next start */
        g_set.winw = 0;
        setStatus(g, setsWrite(&g_set, TRUE) ? "Default window from the next start."
                                             : "Could not write ENVARC:AmiGrep.prefs.");
        break;
    default:
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */

static void eventLoop(struct Gui *g)
{
    BOOL done = FALSE;
    while (!done && !g->quit) {
        ULONG winSig = (g->win && g->win->UserPort)
                     ? (1UL << g->win->UserPort->mp_SigBit) : 0;
        ULONG appSig = g->appPort ? (1UL << g->appPort->mp_SigBit) : 0;
        ULONG got;

        if (!winSig && !appSig) break;        /* nothing to wait on */
        got = Wait(winSig | appSig);

        /* ---- window events ---- */
        if (winSig && (got & winSig)) {
            struct IntuiMessage *imsg;
            while (g->win && (imsg = GT_GetIMsg(g->win->UserPort))) {
                ULONG    cls  = imsg->Class;
                UWORD    code = imsg->Code;
                ULONG    isecs = imsg->Seconds, imicros = imsg->Micros;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (cls) {
                case IDCMP_CLOSEWINDOW:
                    done = TRUE;
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GAD_SEARCH) {
                        doSearch(g);
                        if (g->closeReq) done = TRUE;
                    } else if (gad->GadgetID == GAD_PICK) {
                        doPickPath(g);
                    } else if (gad->GadgetID == GAD_CASE) {
                        g->caseSens = (code != 0) ? TRUE : FALSE;
                    } else if (gad->GadgetID == GAD_LIST) {
                        /* code = selected ordinal; show full line, open drawer on double-click */
                        LONG sel = (LONG)code;
                        g->sel = sel;
                        showHit(g, sel);
                        if (sel == g->lastSel &&
                            DoubleClick(g->lastSecs, g->lastMicros, isecs, imicros)) {
                            doOpenDrawer(g);
                            g->lastSel = -1;
                        } else {
                            g->lastSel = sel;
                            g->lastSecs = isecs; g->lastMicros = imicros;
                        }
                    }
                    break;
                case IDCMP_MENUPICK: {
                    /* collect first: an action may iconify (menu strip gone) */
                    ULONG ids[8]; int i, n = menuPicks(g, code, ids, 8);
                    for (i = 0; i < n && !done && !g->quit && !g->iconified && g->win; i++) {
                        if (doMenu(g, ids[i])) done = TRUE;
                        if (g->closeReq)       done = TRUE;
                    }
                    break;
                }
                case IDCMP_NEWSIZE:
                    /* defer: relaying out here frees gadgets that later-queued
                     * messages in this same drain still reference */
                    g->resizePending = TRUE;
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(g->win);
                    GT_EndRefresh(g->win, TRUE);
                    break;
                default:
                    break;
                }
                if (g->iconified || done || g->quit) break;  /* window gone / quitting */
            }
            if (g->win && !done && !g->quit && !g->iconified && g->resizePending) {
                g->resizePending = FALSE;
                relayout(g);
            }
        }

        /* ---- AppIcon clicked: restore the window ---- */
        if (g->appPort && (got & appSig)) {
            struct Message *m;
            BOOL wake = FALSE;
            while ((m = GetMsg(g->appPort))) { ReplyMsg(m); wake = TRUE; }
            if (wake && g->iconified) doUniconify(g);
        }
    }
}

int main(void)
{
    struct Gui g;
    int rc = 20;
    memset(&g, 0, sizeof g);
    initResults(&g);
    g.sel = g.lastSel = -1;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    AslBase       = OpenLibrary("asl.library", 38);          /* optional */
    WorkbenchBase = OpenLibrary("workbench.library", 37);    /* optional (iconify) */
    IconBase      = OpenLibrary("icon.library", 37);         /* optional (iconify) */

    /* the settings (AmiGrep.prefs over the defaults) fill the fields */
    setsLoad(&g_set);
    setStr(g.savePat, sizeof g.savePat, g_set.pattern);
    setStr(g.savePath, sizeof g.savePath, g_set.path);
    setStr(g.saveFiles, sizeof g.saveFiles, g_set.files);
    g.caseSens = g_set.caseSens;

    if (IntuitionBase && GfxBase && GadToolsBase && buildWindow(&g)) {
        eventLoop(&g);
        rc = 0;
    }

    closeWin(&g);
    if (g.appIcon) RemoveAppIcon(g.appIcon);
    if (g.appPort) { struct Message *m; while ((m = GetMsg(g.appPort))) ReplyMsg(m);
                     DeleteMsgPort(g.appPort); }
    if (g.dobj)    FreeDiskObject(g.dobj);
    freeResults(&g);
    if (IconBase)      CloseLibrary(IconBase);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (AslBase)       CloseLibrary(AslBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
