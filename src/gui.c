/*
 * AmiGrep - GadTools GUI front end (AmigaOS 3.x, no external deps)
 *
 *  +------------------------------------------------+
 *  |  Find  [____________________]                  |
 *  |  In    [SYS:________________] [...]            |
 *  |  Files [____________] Case [ ]                 |
 *  |  [ Search ] [ Stop ]   Ready                   |
 *  |  +------------------------------------------+  |
 *  |  | DH0:s/Startup-Sequence:3: ...            |  |
 *  |  | ...                                      |  |
 *  |  +------------------------------------------+  |
 *  +------------------------------------------------+
 *
 * The search runs synchronously when Search is pressed; while it runs the
 * poll callback pumps Intuition messages so the Stop button and close
 * gadget stay live. Each matching line is collected into an Exec list and
 * attached to the listview when the scan finishes.
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <graphics/rastport.h>
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

unsigned long __stack = 60000;   /* force a generous stack (deep recursion) */

/* AmigaDOS version cookie (the C: Version command / $VER reads this) */
static const char verstag[] __attribute__((used)) =
    "$VER: AmiGrepGUI 1.1 (01.10.2026)";


/* library bases (exec & dos are auto-opened by the C startup) */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *AslBase       = NULL;   /* optional: file requester */
struct Library       *WorkbenchBase = NULL;   /* optional: AppIcon/iconify */
struct Library       *IconBase      = NULL;   /* optional: AppIcon icon    */

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

    struct List    results;        /* nodes shown in the listview            */
    APTR           pool;           /* exec pool holding the results (V39+),
                                    * NULL -> nodes are separate AllocVecs   */
    char           status[80];
    char           savePat[210];   /* Find text, preserved across resize/icon */
    char           savePath[210];  /* In text,   preserved across resize/icon */
    char           saveFiles[110]; /* Files filter, preserved                 */
    BOOL           caseSens;       /* Case checkbox state                     */
    BOOL           stopReq;        /* Stop pressed during a search           */
    BOOL           closeReq;       /* window closed during a search          */
    BOOL           quit;           /* unrecoverable failure -> leave eventLoop */
    ULONG          uiCounter;

    struct MsgPort   *appPort;     /* AppIcon message port (iconified)        */
    struct AppIcon   *appIcon;     /* non-NULL while iconified                */
    struct DiskObject *dobj;       /* icon used for the AppIcon               */
    BOOL              iconified;

    struct Menu      *menu;        /* Project menu (Iconify / Quit)           */

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

#define MENU_ICONIFY 1
#define MENU_QUIT    2

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

static void setStatus(struct Gui *g, CONST_STRPTR text)
{
    strncpy(g->status, (const char *)text, sizeof g->status - 1);
    g->status[sizeof g->status - 1] = '\0';
    if (g->win && g->gStatus)        /* both must be live (iconified -> neither) */
        GT_SetGadgetAttrs(g->gStatus, g->win, NULL,
                          GTTX_Text, (ULONG)g->status, TAG_END);
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

/* process pending Intuition messages without blocking; returns TRUE to abort */
static BOOL pumpMessages(struct Gui *g)
{
    struct IntuiMessage *imsg;
    while ((imsg = GT_GetIMsg(g->win->UserPort))) {
        ULONG    cls = imsg->Class;
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
            break;
        case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(g->win);
            GT_EndRefresh(g->win, TRUE);
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
    if (!AslBase) return;                 /* asl.library not available */

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Select a volume, assign or drawer to search",
            ASLFR_DrawersOnly,   TRUE,
            ASLFR_InitialDrawer, (ULONG)gadgetString(g->gPath),
            ASLFR_InitialWidth,  360,
            ASLFR_InitialHeight, 280,
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

static void doSearch(struct Gui *g)
{
    struct Matcher     m;
    struct NameFilter  nf;
    struct GrepStats   st;
    CONST_STRPTR pat   = gadgetString(g->gPattern);
    CONST_STRPTR path  = gadgetString(g->gPath);
    CONST_STRPTR files = gadgetString(g->gFiles);
    LONG err;

    if (!path || path[0] == '\0') { setStatus(g, "Enter a path to search."); return; }
    if (!pat  || pat[0]  == '\0') { setStatus(g, "Enter text to find.");     return; }
    if (!matcherInit(&m, pat, g->caseSens))  { setStatus(g, "Bad search pattern."); return; }
    if (!nameFilterInit(&nf, files))         { setStatus(g, "Bad Files pattern.");  return; }

    /* detach old list, free it, clear the Hit line */
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)~0, TAG_END);
    freeResults(g);
    if (g->gHit) GT_SetGadgetAttrs(g->gHit, g->win, NULL, GTST_String, (ULONG)"", TAG_END);

    g->stopReq = FALSE;
    g->uiCounter = 0;
    g->lastSel = -1;                   /* reset double-click tracking */
    GT_SetGadgetAttrs(g->gSearch, g->win, NULL, GA_Disabled, TRUE, TAG_END);
    setStatus(g, "Searching...");

    st.namesOnly = FALSE;
    err = grepTree(path, &m, &nf, guiFound, g, guiPoll, g, &st);

    /* attach collected results */
    GT_SetGadgetAttrs(g->gList, g->win, NULL, GTLV_Labels, (ULONG)&g->results, TAG_END);
    GT_SetGadgetAttrs(g->gSearch, g->win, NULL, GA_Disabled, FALSE, TAG_END);

    if (err) {
        setStatus(g, "Cannot open that path.");
    } else {
        char buf[80];
        buf[0] = '\0';
        appendNum(buf, st.linesMatched, (int)sizeof buf);
        strcat(buf, " line(s) in ");
        appendNum(buf, st.filesMatched, (int)sizeof buf);
        strcat(buf, st.aborted ? " file(s) - stopped" : " file(s)");
        setStatus(g, buf);
    }
}

/* ------------------------------------------------------------------ */
/* gadget / window construction                                       */
/* ------------------------------------------------------------------ */

static BOOL makeGadgets(struct Gui *g)
{
    struct NewGadget ng;
    struct Gadget *gad;
    struct Window *w = g->win;
    WORD fh  = g->scr->Font ? g->scr->Font->ta_YSize : 8;
    WORD cl  = w->BorderLeft + 2;            /* content left  (window coords) */
    WORD cr  = w->Width  - w->BorderRight - 2;
    WORD cb  = w->Height - w->BorderBottom - 1;
    WORD row = fh + 6;
    WORD gh  = fh + 3;
    WORD lab = 46;                           /* label column */
    WORD y;

    gad = CreateContext(&g->glist);
    if (!gad) return FALSE;
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = g->vi;
    ng.ng_TextAttr   = g->scr->Font;

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
    ng.ng_Width = cr - (cl + lab) - 44;
    ng.ng_GadgetText = (UBYTE *)"In"; ng.ng_GadgetID = GAD_PATH;
    gad = g->gPath = CreateGadget(STRING_KIND, gad, &ng,
                                  GTST_MaxChars, 200,
                                  GTST_String, (ULONG)g->savePath, TAG_END);
    /* picker "..." */
    ng.ng_LeftEdge = cr - 40; ng.ng_Width = 40;
    ng.ng_GadgetText = (UBYTE *)"..."; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_GadgetID = GAD_PICK;
    gad = g->gPick = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    /* Files filter + Case checkbox */
    y += row;
    ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = y;
    ng.ng_Width = cr - (cl + lab) - 84; if (ng.ng_Width < 40) ng.ng_Width = 40;
    ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Files"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_FILES;
    gad = g->gFiles = CreateGadget(STRING_KIND, gad, &ng,
                                   GTST_MaxChars, 100,
                                   GTST_String, (ULONG)g->saveFiles, TAG_END);
    /* Case checkbox at the right of the Files row. CHECKBOX_KIND uses a fixed
     * ~26px image and ignores ng_Width, so seat its right edge inside cr. */
    ng.ng_LeftEdge = cr - 28; ng.ng_Width = 26;
    ng.ng_GadgetText = (UBYTE *)"Case"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GAD_CASE;
    gad = g->gCase = CreateGadget(CHECKBOX_KIND, gad, &ng,
                                  GTCB_Checked, (ULONG)g->caseSens, TAG_END);

    /* Search / Stop + status */
    y += row;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y; ng.ng_Width = 72; ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"Search"; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_SEARCH;
    gad = g->gSearch = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 78;
    ng.ng_GadgetText = (UBYTE *)"Stop"; ng.ng_GadgetID = GAD_STOP;
    gad = g->gStop = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = cl + 158; ng.ng_Width = cr - (cl + 158);
    if (ng.ng_Width < 8) ng.ng_Width = 8;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GAD_STATUS;
    gad = g->gStatus = CreateGadget(TEXT_KIND, gad, &ng,
                                    GTTX_Text, (ULONG)g->status,
                                    GTTX_Border, FALSE, TAG_END);
    /* Listview fills the middle; a scrollable "Hit" line sits at the bottom
     * showing the full path:line: text of the clicked result (GadTools lists
     * have no horizontal scroll, so this is how you read long lines). */
    y += row + 2;
    {
        WORD hitTop = cb - gh;                 /* bottom row */
        WORD listH  = (hitTop - 3) - y;
        if (listH < gh) listH = gh;
        ng.ng_LeftEdge = cl; ng.ng_TopEdge = y;
        ng.ng_Width = cr - cl; ng.ng_Height = listH;
        ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GAD_LIST;
        gad = g->gList = CreateGadget(LISTVIEW_KIND, gad, &ng,
                                      GTLV_Labels, (ULONG)&g->results,
                                      GTLV_ReadOnly, FALSE,   /* selectable: enables click events */
                                      TAG_END);
        ng.ng_LeftEdge = cl + lab; ng.ng_TopEdge = hitTop;
        ng.ng_Width = cr - (cl + lab); ng.ng_Height = gh;
        ng.ng_GadgetText = (UBYTE *)"Hit"; ng.ng_Flags = PLACETEXT_LEFT;
        ng.ng_GadgetID = GAD_HIT;
        gad = g->gHit = CreateGadget(STRING_KIND, gad, &ng,
                                     GTST_MaxChars, 300, TAG_END);
    }
    return (gad != NULL);
}

/* save the current field text into the persistent buffers */
static void saveFields(struct Gui *g)
{
    if (!g->glist) return;
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
    }
}

/* Project menu - iconify/quit via the right mouse button */
static struct NewMenu amiNewMenu[] = {
    { NM_TITLE, (STRPTR)"Project",      NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Iconify",      (STRPTR)"I", 0, 0, (APTR)MENU_ICONIFY },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,    NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",         (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
    { NM_END,   NULL,                   NULL,        0, 0, NULL }
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
    WORD wtop, winw, winh, maxw, maxh, left;

    g->scr = LockPubScreen(NULL);
    if (!g->scr) return FALSE;
    g->vi = GetVisualInfo(g->scr, TAG_END);
    if (!g->vi) return FALSE;
    sc = g->scr;
    strcpy(g->status, "Ready");

    maxw = sc->Width;
    maxh = sc->Height;
    winw = 480; if (winw > maxw) winw = maxw;
    winh = 240; if (winh > maxh - (sc->BarHeight + 2)) winh = maxh - (sc->BarHeight + 2);
    wtop = sc->BarHeight + 2;
    left = (maxw - winw) / 2; if (left < 0) left = 0;

    g->win = OpenWindowTags(NULL,
        WA_Title,     (ULONG)"AmiGrep",
        WA_Left,      left, WA_Top, wtop,
        WA_Width,     winw, WA_Height, winh,
        WA_MinWidth,  300,  WA_MinHeight, 130,
        WA_MaxWidth,  maxw, WA_MaxHeight, maxh,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                      WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE |
                      IDCMP_MENUPICK | BUTTONIDCMP | STRINGIDCMP | LISTVIEWIDCMP |
                      CHECKBOXIDCMP,
        WA_PubScreen, (ULONG)g->scr,
        TAG_END);
    if (!g->win) return FALSE;

    relayout(g);                             /* build + attach the gadgets   */
    setupMenu(g);                            /* Project menu (right-click iconify) */
    return TRUE;
}

/* close just the window + its display resources (results are preserved) */
static void closeWin(struct Gui *g)
{
    if (g->win && g->menu)  { ClearMenuStrip(g->win); }
    if (g->menu)            { FreeMenus(g->menu); g->menu = NULL; }
    if (g->win)   { CloseWindow(g->win);          g->win = NULL; }
    if (g->glist) { FreeGadgets(g->glist);        g->glist = NULL; }
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
        setStatus(g, "Could not iconify (AddAppIcon failed).");
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
/* double-click a result -> open its containing drawer in Workbench    */
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

/* drop the full clicked line into the scrollable Hit field */
static void showHit(struct Gui *g, LONG sel)
{
    struct ResNode *rn = nthResult(g, sel);
    if (rn && g->gHit)
        GT_SetGadgetAttrs(g->gHit, g->win, NULL,
                          GTST_String, (ULONG)rn->node.ln_Name, TAG_END);
}

static void openResultDrawer(struct Gui *g, LONG sel)
{
    struct ResNode *rn = nthResult(g, sel);
    char drawer[300];

    if (!rn) return;
    /* OpenWorkbenchObject() is a V44 (OS 3.5+) LVO; we open the library at
     * v37 for AddAppIconA, so calling it on OS 3.0/3.1 would jump into a
     * nonexistent vector and guru. Check the actual library version. */
    if (!WorkbenchBase || WorkbenchBase->lib_Version < 44) {
        setStatus(g, "Needs workbench.library v44+ (OS 3.5).");
        return;
    }
    resultDrawer((CONST_STRPTR)rn->path, (int)rn->pathLen, drawer, (int)sizeof drawer);
    if (drawer[0] == '\0') { setStatus(g, "Cannot determine drawer."); return; }

    if (OpenWorkbenchObject((STRPTR)drawer, TAG_END))
        setStatus(g, "Opened drawer.");
    else
        setStatus(g, "Could not open drawer.");
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
                        showHit(g, sel);
                        if (sel == g->lastSel &&
                            DoubleClick(g->lastSecs, g->lastMicros, isecs, imicros)) {
                            openResultDrawer(g, sel);
                            g->lastSel = -1;
                        } else {
                            g->lastSel = sel;
                            g->lastSecs = isecs; g->lastMicros = imicros;
                        }
                    }
                    break;
                case IDCMP_MENUPICK: {
                    UWORD mn = code;
                    while (mn != MENUNULL && g->win && g->menu) {
                        struct MenuItem *it = ItemAddress(g->menu, mn);
                        ULONG ud;
                        if (!it) break;
                        ud = (ULONG)GTMENUITEM_USERDATA(it);
                        if (ud == MENU_ICONIFY)   doIconify(g);
                        else if (ud == MENU_QUIT) done = TRUE;
                        if (g->iconified || done || g->quit) break;
                        mn = it->NextSelect;
                    }
                    break;
                }
                case IDCMP_NEWSIZE:
                    relayout(g);
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
    g.lastSel = -1;
    strcpy(g.savePath, "SYS:");        /* default In; other fields stay empty */

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    AslBase       = OpenLibrary("asl.library", 38);          /* optional */
    WorkbenchBase = OpenLibrary("workbench.library", 37);    /* optional (iconify) */
    IconBase      = OpenLibrary("icon.library", 37);         /* optional (iconify) */

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
