/*
 * areole - Win32 backend: the accessibility tree, as MSAA.
 * SPDX-License-Identifier: MIT
 *
 * The core computes the tree -- role, name, value, state, where each thing is
 * -- and knows nothing about any platform. This file is the adapter: an
 * IAccessible per node, answering a screen reader in the vocabulary Windows
 * has spoken since Windows 98. Narrator and NVDA both read MSAA, and UI
 * Automation reads it through the proxy every Windows since 7 ships, so one
 * adapter reaches all three.
 *
 * ------------------------------------------------------------------------
 * Why plain C
 *
 * COM is a vtable and a calling convention, both of which C has. A C++ class
 * would be shorter to write and would put a C++ runtime into a library whose
 * reason for existing is that it needs nothing; the vtable below is the same
 * table the compiler would have generated, written out.
 *
 * ------------------------------------------------------------------------
 * No allocation
 *
 * The objects are a static pool, one per node of the tree, and they live as
 * long as the process. AddRef and Release count nothing, because there is
 * nothing to free. The only memory this file asks Windows for is the BSTRs
 * COM's own contract makes the caller free.
 *
 * ponytail: object identity is the node's index in the current tree. After the
 * tree changes shape a reader still holding an old object gets the node that
 * now has that index -- the reorder event this file sends is what tells it to
 * ask again, and every reader does. Stable identities need a key per node that
 * outlives the frame, which the box key is; the upgrade is a small table from
 * key to object, worth doing when UI Automation is adapted natively.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0500
#define WINVER       0x0500
#define COBJMACROS

#include <windows.h>
#include <ole2.h>
#include <oleacc.h>

#include "ar_win32_internal.h"

/*
 * The three interface ids, spelled out here rather than taken from the link.
 *
 * MinGW's liboleacc.a exports IID_IAccessible as an import, and a data symbol
 * reached through an import library without __declspec(dllimport) is the
 * address of the import thunk: `IID_IAccessible` read as
 * {6DB225FF-0000-9090-...}, which is `jmp [rip+...]` and not a GUID. So
 * LresultFromObject was asked to marshal an interface nobody had registered,
 * answered REGDB_E_IIDNOTREG, and every reader saw a window with nothing in it
 * -- found by reading the tree back through UI Automation, the check that had
 * not been run. Sixteen bytes each cannot be resolved wrongly.
 */
static const IID AR__IID_IUNKNOWN = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID AR__IID_IDISPATCH = {
    0x00020400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID AR__IID_IACCESSIBLE = {
    0x618736E0, 0x3C3D, 0x11CF, {0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* A form with twenty controls is about sixty nodes and a long document a few
   thousand; past this the rest of the tree is not exposed, which is stated
   here rather than discovered by somebody who cannot see the page. */
#define AR_ACC_MAX 4096

typedef struct ar__acc
{
    IAccessible iface; /* first, so an IAccessible * is an ar__acc * */
    ar_i32      item;  /* index into the tree, or -1 for the document */
} ar__acc;

static struct
{
    ar_win       *win;
    HWND          hwnd;
    ar_ctx       *ctx;
    const ar_doc *doc;
    int           ole_ready;

    ar_a11y_item items[AR_ACC_MAX];
    ar_i32       n;
    ar_u32       core_gen;
    int          built;

    ar__acc root;
    ar__acc objs[AR_ACC_MAX];

    /* What the last update saw, so the next can say what changed. */
    ar_i32 focus_node;
    ar_u32 focus_value_hash;
} g_a;

/* Scratch for names and values on their way to UTF-16. One thread talks to
   these objects -- COM calls into an apartment on the thread that created
   it -- so one buffer each is enough. */
static char  g_utf8[8192];
static WCHAR g_wide[8192];

/* Not const: CONST_VTBL is empty in C, so lpVtbl is a plain pointer, and
   casting the qualifier away is what -Wcast-qual exists to refuse. */
static IAccessibleVtbl AR__VTBL;

/* ------------------------------------------------------------------------
 * The tree
 * ------------------------------------------------------------------------ */
static void ar__rebuild(void)
{
    ar_i32 n;

    if (!g_a.ctx || !g_a.doc)
    {
        g_a.n = 0;
        return;
    }
    n = ar_a11y_tree(g_a.ctx, g_a.doc, g_a.items, AR_ACC_MAX);
    g_a.n = n > AR_ACC_MAX ? AR_ACC_MAX : n;
    g_a.built = 1;
}

static ar__acc *ar__obj(ar_i32 item)
{
    ar__acc *a;

    if (item < 0)
    {
        return &g_a.root;
    }
    a = &g_a.objs[item];
    a->iface.lpVtbl = &AR__VTBL;
    a->item = item;
    return a;
}

static int ar__alive(const ar__acc *a)
{
    return g_a.ctx && g_a.doc && g_a.built && a->item < g_a.n;
}

/* Is item `i` inside `anc`? -1 is the document, which holds everything. */
static int ar__within(ar_i32 i, ar_i32 anc)
{
    if (anc < 0)
    {
        return 1;
    }
    while (i >= 0)
    {
        if (i == anc)
        {
            return 1;
        }
        i = g_a.items[i].parent;
    }
    return 0;
}

/* The k-th child of `parent`, one-based as MSAA counts, or -2 for none. */
static ar_i32 ar__nth_child(ar_i32 parent, LONG k)
{
    ar_i32 i;

    for (i = 0; i < g_a.n; ++i)
    {
        if (g_a.items[i].parent == parent && --k == 0)
        {
            return i;
        }
    }
    return -2;
}

/*
 * Which node a VARIANT names, relative to the object it was asked of.
 *
 * CHILDID_SELF is the object itself, which is what every reader asks once it
 * holds the object. A positive id is the n-th child, which is how a reader
 * enumerates; a negative one is a node anywhere in the tree, which is what an
 * event carries -- the convention Firefox and Chrome use, because an event
 * has to name a node without knowing where it is.
 */
static int ar__target(const ar__acc *a, VARIANT v, ar_i32 *item)
{
    if (v.vt != VT_I4)
    {
        return 0;
    }
    if (v.lVal == CHILDID_SELF)
    {
        *item = a->item;
        return 1;
    }
    if (v.lVal < 0)
    {
        ar_i32 i = -v.lVal - 1;

        if (i >= g_a.n || !ar__within(i, a->item))
        {
            return 0;
        }
        *item = i;
        return 1;
    }
    *item = ar__nth_child(a->item, v.lVal);
    return *item != -2;
}

static BSTR ar__bstr(const char *s, ar_u32 n)
{
    int w;

    if (n == 0)
    {
        return SysAllocString(L"");
    }
    w = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, g_wide, (int)(sizeof g_wide / sizeof g_wide[0]));
    return w > 0 ? SysAllocStringLen(g_wide, (UINT)w) : SysAllocString(L"");
}

static void ar__wake(void)
{
    if (g_a.win)
    {
        ar_win_wake(g_a.win);
        PostMessageW(g_a.hwnd, WM_NULL, 0, 0);
    }
}

/* ------------------------------------------------------------------------
 * The vocabulary
 * ------------------------------------------------------------------------ */
static LONG ar__msaa_role(ar_u8 role)
{
    switch (role)
    {
    case AR_ROLE_BUTTON:
        return ROLE_SYSTEM_PUSHBUTTON;
    case AR_ROLE_LINK:
        return ROLE_SYSTEM_LINK;
    case AR_ROLE_CHECKBOX:
        return ROLE_SYSTEM_CHECKBUTTON;
    case AR_ROLE_RADIO:
        return ROLE_SYSTEM_RADIOBUTTON;
    case AR_ROLE_TEXTBOX:
    case AR_ROLE_SEARCHBOX:
    case AR_ROLE_SPINBUTTON:
        return ROLE_SYSTEM_TEXT;
    case AR_ROLE_SLIDER:
        return ROLE_SYSTEM_SLIDER;
    case AR_ROLE_COMBOBOX:
        return ROLE_SYSTEM_COMBOBOX;
    case AR_ROLE_OPTION:
        return ROLE_SYSTEM_LISTITEM;
    case AR_ROLE_LISTBOX:
        return ROLE_SYSTEM_LIST;
    case AR_ROLE_HEADING:
    case AR_ROLE_PARAGRAPH:
        return ROLE_SYSTEM_STATICTEXT;
    case AR_ROLE_LIST:
        return ROLE_SYSTEM_LIST;
    case AR_ROLE_LISTITEM:
        return ROLE_SYSTEM_LISTITEM;
    case AR_ROLE_TABLE:
        return ROLE_SYSTEM_TABLE;
    case AR_ROLE_ROW:
        return ROLE_SYSTEM_ROW;
    case AR_ROLE_CELL:
        return ROLE_SYSTEM_CELL;
    case AR_ROLE_COLUMNHEADER:
        return ROLE_SYSTEM_COLUMNHEADER;
    case AR_ROLE_IMAGE:
        return ROLE_SYSTEM_GRAPHIC;
    case AR_ROLE_DIALOG:
        return ROLE_SYSTEM_DIALOG;
    case AR_ROLE_PROGRESSBAR:
    case AR_ROLE_METER:
        return ROLE_SYSTEM_PROGRESSBAR;
    default:
        return ROLE_SYSTEM_GROUPING;
    }
}

static LONG ar__msaa_state(const ar_a11y_item *it)
{
    LONG s = 0;

    if (it->state & AR_A11Y_CHECKED)
    {
        s |= STATE_SYSTEM_CHECKED;
    }
    if (it->state & AR_A11Y_DISABLED)
    {
        s |= STATE_SYSTEM_UNAVAILABLE;
    }
    if (it->state & AR_A11Y_EXPANDED)
    {
        s |= STATE_SYSTEM_EXPANDED;
    }
    if (it->state & AR_A11Y_COLLAPSED)
    {
        s |= STATE_SYSTEM_COLLAPSED;
    }
    if (it->state & AR_A11Y_FOCUSED)
    {
        s |= STATE_SYSTEM_FOCUSED;
    }
    if (it->state & AR_A11Y_FOCUSABLE)
    {
        s |= STATE_SYSTEM_FOCUSABLE;
    }
    if (it->state & AR_A11Y_READONLY)
    {
        s |= STATE_SYSTEM_READONLY;
    }
    if (it->state & AR_A11Y_PROTECTED)
    {
        s |= STATE_SYSTEM_PROTECTED;
    }
    if (it->state & AR_A11Y_SELECTED)
    {
        s |= STATE_SYSTEM_SELECTED;
    }
    if (it->role == AR_ROLE_OPTION)
    {
        s |= STATE_SYSTEM_SELECTABLE;
    }
    if (it->role == AR_ROLE_COMBOBOX)
    {
        s |= STATE_SYSTEM_HASPOPUP;
    }
    if (it->role == AR_ROLE_LINK)
    {
        s |= STATE_SYSTEM_LINKED;
    }
    if (it->state & AR_A11Y_OFFSCREEN)
    {
        s |= STATE_SYSTEM_OFFSCREEN;
    }
    return s;
}

static const char *ar__action(const ar_a11y_item *it)
{
    switch (it->role)
    {
    case AR_ROLE_CHECKBOX:
        return (it->state & AR_A11Y_CHECKED) ? "Uncheck" : "Check";
    case AR_ROLE_RADIO:
    case AR_ROLE_OPTION:
        return "Select";
    case AR_ROLE_COMBOBOX:
        return (it->state & AR_A11Y_EXPANDED) ? "Close" : "Open";
    case AR_ROLE_LINK:
        return "Jump";
    case AR_ROLE_BUTTON:
        return "Press";
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------
 * IUnknown and IDispatch
 * ------------------------------------------------------------------------ */
static HRESULT STDMETHODCALLTYPE ar__qi(IAccessible *This, REFIID riid, void **out)
{
    if (!out)
    {
        return E_POINTER;
    }
    if (IsEqualIID(riid, &AR__IID_IUNKNOWN) || IsEqualIID(riid, &AR__IID_IDISPATCH) ||
        IsEqualIID(riid, &AR__IID_IACCESSIBLE))
    {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* Static objects: nothing to count and nothing to free. */
static ULONG STDMETHODCALLTYPE ar__addref(IAccessible *This)
{
    (void)This;
    return 2;
}

static ULONG STDMETHODCALLTYPE ar__release(IAccessible *This)
{
    (void)This;
    return 1;
}

static HRESULT STDMETHODCALLTYPE ar__ti_count(IAccessible *This, UINT *n)
{
    (void)This;
    if (n)
    {
        *n = 0;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__ti(IAccessible *This, UINT i, LCID l, ITypeInfo **t)
{
    (void)This;
    (void)i;
    (void)l;
    if (t)
    {
        *t = NULL;
    }
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ar__ids(IAccessible *This, REFIID r, LPOLESTR *names, UINT n,
                                         LCID l, DISPID *ids)
{
    (void)This;
    (void)r;
    (void)names;
    (void)n;
    (void)l;
    (void)ids;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ar__invoke(IAccessible *This, DISPID id, REFIID r, LCID l, WORD f,
                                            DISPPARAMS *p, VARIANT *res, EXCEPINFO *e, UINT *arg)
{
    (void)This;
    (void)id;
    (void)r;
    (void)l;
    (void)f;
    (void)p;
    (void)res;
    (void)e;
    (void)arg;
    return E_NOTIMPL;
}

/* ------------------------------------------------------------------------
 * IAccessible
 * ------------------------------------------------------------------------ */
static HRESULT STDMETHODCALLTYPE ar__parent(IAccessible *This, IDispatch **out)
{
    ar__acc *a = (ar__acc *)This;

    if (!out)
    {
        return E_POINTER;
    }
    *out = NULL;
    if (a->item < 0)
    {
        /* The document's parent is the window, which Windows already knows
           how to describe. */
        return CreateStdAccessibleObject(g_a.hwnd, OBJID_WINDOW, &AR__IID_IDISPATCH, (void **)out);
    }
    if (!ar__alive(a))
    {
        return CO_E_OBJNOTCONNECTED;
    }
    *out = (IDispatch *)&ar__obj(g_a.items[a->item].parent)->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__child_count(IAccessible *This, LONG *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   i;
    LONG     k = 0;

    if (!out)
    {
        return E_POINTER;
    }
    *out = 0;
    if (!ar__alive(a))
    {
        return a->item < 0 ? S_OK : CO_E_OBJNOTCONNECTED;
    }
    for (i = 0; i < g_a.n; ++i)
    {
        if (g_a.items[i].parent == a->item)
        {
            ++k;
        }
    }
    *out = k;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__child(IAccessible *This, VARIANT v, IDispatch **out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;

    if (!out)
    {
        return E_POINTER;
    }
    *out = NULL;
    if (!ar__alive(a) || !ar__target(a, v, &item))
    {
        return E_INVALIDARG;
    }
    *out = (IDispatch *)&ar__obj(item)->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__name(IAccessible *This, VARIANT v, BSTR *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;
    ar_u32   n;

    if (!out)
    {
        return E_POINTER;
    }
    *out = NULL;
    if (!ar__alive(a) || !ar__target(a, v, &item))
    {
        return E_INVALIDARG;
    }
    if (item < 0)
    {
        /* The document is called what the window is called. */
        int w = GetWindowTextW(g_a.hwnd, g_wide, (int)(sizeof g_wide / sizeof g_wide[0]));

        *out = SysAllocStringLen(g_wide, (UINT)(w > 0 ? w : 0));
        return S_OK;
    }
    n = ar_a11y_name(g_a.doc, g_a.items[item].node, g_utf8, sizeof g_utf8);
    if (n == 0)
    {
        return S_FALSE;
    }
    *out = ar__bstr(g_utf8, n);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__value(IAccessible *This, VARIANT v, BSTR *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;
    ar_u32   n;

    if (!out)
    {
        return E_POINTER;
    }
    *out = NULL;
    if (!ar__alive(a) || !ar__target(a, v, &item) || item < 0)
    {
        return item < 0 ? S_FALSE : E_INVALIDARG;
    }
    n = ar_a11y_value(g_a.ctx, g_a.doc, g_a.items[item].node, g_utf8, sizeof g_utf8);
    if (n == 0 &&
        !(g_a.items[item].role == AR_ROLE_TEXTBOX || g_a.items[item].role == AR_ROLE_SEARCHBOX))
    {
        return S_FALSE;
    }
    *out = ar__bstr(g_utf8, n);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__description(IAccessible *This, VARIANT v, BSTR *out)
{
    (void)This;
    (void)v;
    if (out)
    {
        *out = NULL;
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ar__role(IAccessible *This, VARIANT v, VARIANT *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;

    if (!out)
    {
        return E_POINTER;
    }
    VariantInit(out);
    if (a->item < 0 && v.vt == VT_I4 && v.lVal == CHILDID_SELF)
    {
        out->vt = VT_I4;
        out->lVal = ROLE_SYSTEM_DOCUMENT;
        return S_OK;
    }
    if (!ar__alive(a) || !ar__target(a, v, &item))
    {
        return E_INVALIDARG;
    }
    out->vt = VT_I4;
    out->lVal = item < 0 ? ROLE_SYSTEM_DOCUMENT : ar__msaa_role(g_a.items[item].role);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__state(IAccessible *This, VARIANT v, VARIANT *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;

    if (!out)
    {
        return E_POINTER;
    }
    VariantInit(out);
    if (!ar__alive(a) || !ar__target(a, v, &item))
    {
        if (a->item < 0)
        {
            out->vt = VT_I4;
            out->lVal = STATE_SYSTEM_READONLY;
            return S_OK;
        }
        return E_INVALIDARG;
    }
    out->vt = VT_I4;
    out->lVal = item < 0 ? STATE_SYSTEM_READONLY : ar__msaa_state(&g_a.items[item]);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__help(IAccessible *This, VARIANT v, BSTR *out)
{
    (void)This;
    (void)v;
    if (out)
    {
        *out = NULL;
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ar__help_topic(IAccessible *This, BSTR *file, VARIANT v,
                                                LONG *topic)
{
    (void)This;
    (void)v;
    if (file)
    {
        *file = NULL;
    }
    if (topic)
    {
        *topic = 0;
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ar__shortcut(IAccessible *This, VARIANT v, BSTR *out)
{
    (void)This;
    (void)v;
    if (out)
    {
        *out = NULL;
    }
    return S_FALSE;
}

static ar_i32 ar__focused_item(void)
{
    ar_i32 i;

    for (i = 0; i < g_a.n; ++i)
    {
        if (g_a.items[i].state & AR_A11Y_FOCUSED)
        {
            return i;
        }
    }
    return -1;
}

static HRESULT STDMETHODCALLTYPE ar__focus(IAccessible *This, VARIANT *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   f;

    if (!out)
    {
        return E_POINTER;
    }
    VariantInit(out);
    if (!ar__alive(a))
    {
        return S_FALSE;
    }
    f = ar__focused_item();
    if (f < 0 || !ar__within(f, a->item))
    {
        return S_FALSE;
    }
    if (f == a->item)
    {
        out->vt = VT_I4;
        out->lVal = CHILDID_SELF;
        return S_OK;
    }
    out->vt = VT_DISPATCH;
    out->pdispVal = (IDispatch *)&ar__obj(f)->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__selection(IAccessible *This, VARIANT *out)
{
    (void)This;
    if (out)
    {
        VariantInit(out);
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ar__default_action(IAccessible *This, VARIANT v, BSTR *out)
{
    ar__acc    *a = (ar__acc *)This;
    ar_i32      item;
    const char *act;

    if (!out)
    {
        return E_POINTER;
    }
    *out = NULL;
    if (!ar__alive(a) || !ar__target(a, v, &item) || item < 0)
    {
        return S_FALSE;
    }
    act = ar__action(&g_a.items[item]);
    if (!act)
    {
        return S_FALSE;
    }
    *out = ar__bstr(act, (ar_u32)lstrlenA(act));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__select(IAccessible *This, LONG flags, VARIANT v)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;

    if (!ar__alive(a) || !ar__target(a, v, &item) || item < 0)
    {
        return E_INVALIDARG;
    }
    if ((flags & SELFLAG_TAKEFOCUS) && ar_a11y_focus(g_a.ctx, g_a.doc, g_a.items[item].node))
    {
        ar__wake();
        return S_OK;
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ar__location(IAccessible *This, LONG *x, LONG *y, LONG *w, LONG *h,
                                              VARIANT v)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;
    POINT    pt;
    ar_rect  r;

    if (!x || !y || !w || !h)
    {
        return E_POINTER;
    }
    if (!ar__alive(a) || !ar__target(a, v, &item))
    {
        return E_INVALIDARG;
    }
    if (item < 0)
    {
        RECT cr;

        GetClientRect(g_a.hwnd, &cr);
        r = ar_rect_make(0, 0, cr.right, cr.bottom);
    }
    else
    {
        r = g_a.items[item].rect;
    }
    pt.x = r.x;
    pt.y = r.y;
    ClientToScreen(g_a.hwnd, &pt);
    *x = pt.x;
    *y = pt.y;
    *w = r.w;
    *h = r.h;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__navigate(IAccessible *This, LONG dir, VARIANT start,
                                              VARIANT *out)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   from, i, hit = -2;

    if (!out)
    {
        return E_POINTER;
    }
    VariantInit(out);
    if (!ar__alive(a) || !ar__target(a, start, &from))
    {
        return E_INVALIDARG;
    }
    switch (dir)
    {
    case NAVDIR_FIRSTCHILD:
        hit = ar__nth_child(from, 1);
        break;
    case NAVDIR_LASTCHILD:
        for (i = 0; i < g_a.n; ++i)
        {
            if (g_a.items[i].parent == from)
            {
                hit = i;
            }
        }
        break;
    case NAVDIR_NEXT:
        if (from >= 0)
        {
            for (i = from + 1; i < g_a.n; ++i)
            {
                if (g_a.items[i].parent == g_a.items[from].parent)
                {
                    hit = i;
                    break;
                }
            }
        }
        break;
    case NAVDIR_PREVIOUS:
        if (from >= 0)
        {
            for (i = from - 1; i >= 0; --i)
            {
                if (g_a.items[i].parent == g_a.items[from].parent)
                {
                    hit = i;
                    break;
                }
            }
        }
        break;
    default:
        return E_NOTIMPL;
    }
    if (hit == -2)
    {
        return S_FALSE;
    }
    out->vt = VT_DISPATCH;
    out->pdispVal = (IDispatch *)&ar__obj(hit)->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__hit(IAccessible *This, LONG sx, LONG sy, VARIANT *out)
{
    ar__acc *a = (ar__acc *)This;
    POINT    pt;
    ar_i32   i, best = -2;

    if (!out)
    {
        return E_POINTER;
    }
    VariantInit(out);
    if (!ar__alive(a))
    {
        return S_FALSE;
    }
    pt.x = sx;
    pt.y = sy;
    ScreenToClient(g_a.hwnd, &pt);

    /* The deepest node under the point: pre-order puts children after their
       parent and later siblings after earlier ones, so the last hit wins. */
    for (i = 0; i < g_a.n; ++i)
    {
        ar_rect r = g_a.items[i].rect;

        if (pt.x >= r.x && pt.y >= r.y && pt.x < r.x + r.w && pt.y < r.y + r.h &&
            ar__within(i, a->item) && i != a->item)
        {
            best = i;
        }
    }
    if (best == -2)
    {
        out->vt = VT_I4;
        out->lVal = CHILDID_SELF;
        return S_OK;
    }
    out->vt = VT_DISPATCH;
    out->pdispVal = (IDispatch *)&ar__obj(best)->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__do_default(IAccessible *This, VARIANT v)
{
    ar__acc *a = (ar__acc *)This;
    ar_i32   item;

    if (!ar__alive(a) || !ar__target(a, v, &item) || item < 0)
    {
        return E_INVALIDARG;
    }
    if (!ar_a11y_activate(g_a.ctx, g_a.doc, g_a.items[item].node))
    {
        return DISP_E_MEMBERNOTFOUND;
    }
    ar__wake();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ar__put_name(IAccessible *This, VARIANT v, BSTR s)
{
    (void)This;
    (void)v;
    (void)s;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ar__put_value(IAccessible *This, VARIANT v, BSTR s)
{
    (void)This;
    (void)v;
    (void)s;
    return E_NOTIMPL;
}

/* In the order IAccessibleVtbl declares them, which is the order the compiler
   would have laid a C++ class out in. A slot out of place calls the wrong
   method with the wrong arguments, so this list is checked against
   oleacc.h rather than written from memory. */
static IAccessibleVtbl AR__VTBL = {
    ar__qi,          ar__addref,     ar__release,        ar__ti_count, ar__ti,         ar__ids,
    ar__invoke,      ar__parent,     ar__child_count,    ar__child,    ar__name,       ar__value,
    ar__description, ar__role,       ar__state,          ar__help,     ar__help_topic, ar__shortcut,
    ar__focus,       ar__selection,  ar__default_action, ar__select,   ar__location,   ar__navigate,
    ar__hit,         ar__do_default, ar__put_name,       ar__put_value};

/* ------------------------------------------------------------------------
 * The backend's side
 * ------------------------------------------------------------------------ */
LRESULT ar__win_a11y_getobject(HWND hwnd, WPARAM wp, LPARAM lp)
{
    if ((LONG)(DWORD)lp != OBJID_CLIENT || !g_a.ctx || !g_a.doc || hwnd != g_a.hwnd)
    {
        return 0;
    }
    if (!g_a.built)
    {
        ar__rebuild();
    }
    g_a.root.iface.lpVtbl = &AR__VTBL;
    g_a.root.item = -1;
    return LresultFromObject(&AR__IID_IACCESSIBLE, wp, (LPUNKNOWN)&g_a.root.iface);
}

void ar__win_a11y_detach(void)
{
    g_a.ctx = NULL;
    g_a.doc = NULL;
    g_a.n = 0;
    g_a.built = 0;
}

void ar_win_a11y(ar_win *win, ar_ctx *c, const ar_doc *d)
{
    if (!g_a.ole_ready)
    {
        /* COM in this thread, which LresultFromObject needs to marshal the
           objects out to a reader in another process. */
        OleInitialize(NULL);
        g_a.ole_ready = 1;
    }
    g_a.win = win;
    g_a.hwnd = ar_win_hwnd(win);
    g_a.ctx = c;
    g_a.doc = d;
    g_a.built = 0;
    g_a.focus_node = -1;
    g_a.core_gen = c ? ar_a11y_generation(c) : 0;
}

/* FNV over a string, for noticing that the focused field's value changed
   without keeping a copy of it. */
static ar_u32 ar__hash(const char *s, ar_u32 n)
{
    ar_u32 h = 2166136261u, i;

    for (i = 0; i < n; ++i)
    {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }
    return h;
}

/*
 * After a frame: rebuild the tree and tell any reader what moved.
 *
 * Three events, each the one a reader actually acts on: focus, so it speaks
 * the newly focused control; reorder, so it throws away what it cached when
 * the shape changed; value change, so typing is echoed. The child id is the
 * node's negative index, which ar__target resolves from the document object.
 */
void ar_win_a11y_update(ar_win *win)
{
    ar_u32 gen;
    ar_i32 f;
    HWND   hwnd;

    if (!win || !g_a.ctx || !g_a.doc)
    {
        return;
    }
    hwnd = ar_win_hwnd(win);
    gen = ar_a11y_generation(g_a.ctx);
    ar__rebuild();

    if (gen != g_a.core_gen)
    {
        g_a.core_gen = gen;
        NotifyWinEvent(EVENT_OBJECT_REORDER, hwnd, OBJID_CLIENT, CHILDID_SELF);
    }

    f = ar__focused_item();
    if (f >= 0 && g_a.items[f].node != g_a.focus_node)
    {
        g_a.focus_node = g_a.items[f].node;
        g_a.focus_value_hash = 0;
        NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, -(LONG)(f + 1));
    }
    else if (f < 0)
    {
        g_a.focus_node = -1;
    }
    if (f >= 0)
    {
        ar_u32 n = ar_a11y_value(g_a.ctx, g_a.doc, g_a.items[f].node, g_utf8, sizeof g_utf8);
        ar_u32 h = ar__hash(g_utf8, n) ^ g_a.items[f].state;

        if (g_a.focus_value_hash != 0 && h != g_a.focus_value_hash)
        {
            NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, -(LONG)(f + 1));
            NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, -(LONG)(f + 1));
        }
        g_a.focus_value_hash = h ? h : 1u;
    }
}
