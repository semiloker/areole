/*
 * ar_a11y_probe - read a window's accessibility tree the way a screen reader
 * does, from another process.
 * SPDX-License-Identifier: MIT
 *
 *     ar_a11y_probe                          example 16, expecting 20 controls
 *     ar_a11y_probe <exe> <title> <controls>
 *
 * Starts the program, waits for its window, and walks it twice: once through
 * MSAA (AccessibleObjectFromWindow, then accChild down the tree, which is how
 * NVDA reads an MSAA provider) and once through native UI Automation
 * (ElementFromHandle and FindAll, which is how Narrator reaches it, through
 * UIA's proxy for MSAA). Prints both and closes the program again.
 *
 * Exits 0 when the MSAA walk finds at least <controls> controls and every one
 * of them has a name, which is 0.10.0's sixth acceptance criterion as far as a
 * program can check it. The other half -- that the name is *said*, and said
 * well -- needs a person listening to Narrator and NVDA, and this does not
 * pretend to be that.
 *
 * Why it exists: the provider was written, compiled and shipped in a commit
 * whose README claimed it had been read back, and nothing had read it back.
 * The first run of this found that no reader had ever seen anything in the
 * window -- see the interface ids at the top of platform/ar_win32_a11y.c --
 * and that three kinds of container were named with every word inside them.
 *
 * The interface and class ids are spelled out for the same reason the
 * provider spells them out: MinGW's import libraries resolve some of them to
 * an import thunk, which compiles, links, and is not a GUID.
 */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleacc.h>
#include <uiautomation.h>

#include <stdio.h>
#include <stdlib.h>

static const IID PROBE_IID_IACCESSIBLE = {
    0x618736E0, 0x3C3D, 0x11CF, {0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const CLSID PROBE_CLSID_CUIAUTOMATION = {
    0xFF48DBA4, 0x60EF, 0x4201, {0xAA, 0x87, 0x54, 0x10, 0x3E, 0xEF, 0x59, 0x4E}};
static const IID PROBE_IID_IUIAUTOMATION = {
    0x30CBE57D, 0xD9D0, 0x452A, {0xAB, 0x13, 0x7A, 0xC5, 0xAC, 0x48, 0x25, 0xEE}};

static int g_controls; /* controls the MSAA walk found */
static int g_unnamed;  /* and how many of them had no name */

/* The MSAA roles a person operates, as opposed to text and containers. */
static int is_control(LONG role)
{
    switch (role)
    {
    case ROLE_SYSTEM_PUSHBUTTON:
    case ROLE_SYSTEM_CHECKBUTTON:
    case ROLE_SYSTEM_RADIOBUTTON:
    case ROLE_SYSTEM_COMBOBOX:
    case ROLE_SYSTEM_TEXT:
    case ROLE_SYSTEM_SLIDER:
    case ROLE_SYSTEM_PROGRESSBAR:
    case ROLE_SYSTEM_SPINBUTTON:
    case ROLE_SYSTEM_LINK:
        return 1;
    default:
        return 0;
    }
}

/* A BSTR as UTF-8, because C90's printf has no wide strings: one of three
   buffers in turn, so a line can print several. */
static const char *utf8(BSTR w)
{
    static char buf[3][512];
    static int  next;
    char       *b = buf[next];

    next = (next + 1) % 3;
    b[0] = 0;
    if (w && !WideCharToMultiByte(CP_UTF8, 0, w, -1, b, (int)sizeof buf[0], NULL, NULL))
    {
        b[0] = 0;
    }
    return b;
}

/* A role's name as Windows itself spells it, for the printout. */
static void role_text(LONG role, char *buf, UINT cap)
{
    if (!GetRoleTextA((DWORD)role, buf, cap))
    {
        sprintf(buf, "role %ld", role);
    }
}

static void msaa_walk(IAccessible *acc, int depth)
{
    LONG    n = 0, k;
    HRESULT hr = IAccessible_get_accChildCount(acc, &n);

    if (FAILED(hr))
    {
        printf("%*s(child count failed, 0x%08lx)\n", depth * 2, "", (unsigned long)hr);
        return;
    }
    for (k = 1; k <= n; ++k)
    {
        VARIANT      v, self, role;
        IDispatch   *d = NULL;
        IAccessible *c = NULL;
        BSTR         name = NULL, value = NULL;
        char         rt[64];

        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = k;
        if (FAILED(IAccessible_get_accChild(acc, v, &d)) || !d)
        {
            continue;
        }
        hr = IDispatch_QueryInterface(d, &PROBE_IID_IACCESSIBLE, (void **)&c);
        IDispatch_Release(d);
        if (FAILED(hr))
        {
            continue;
        }
        VariantInit(&self);
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        VariantInit(&role);
        IAccessible_get_accRole(c, self, &role);
        IAccessible_get_accName(c, self, &name);
        IAccessible_get_accValue(c, self, &value);
        role_text(role.vt == VT_I4 ? role.lVal : 0, rt, sizeof rt);
        printf("%*s%-16s \"%s\"%s%s%s\n", depth * 2, "", rt, utf8(name), value ? " = \"" : "",
               utf8(value), value ? "\"" : "");
        if (role.vt == VT_I4 && is_control(role.lVal))
        {
            ++g_controls;
            if (!name || !name[0])
            {
                ++g_unnamed;
            }
        }
        SysFreeString(name);
        SysFreeString(value);
        msaa_walk(c, depth + 1);
        IAccessible_Release(c);
    }
}

static void uia_walk(HWND hwnd)
{
    IUIAutomation             *uia = NULL;
    IUIAutomationElement      *win = NULL;
    IUIAutomationCondition    *all = NULL;
    IUIAutomationElementArray *arr = NULL;
    int                        n = 0, i;

    if (FAILED(CoCreateInstance(&PROBE_CLSID_CUIAUTOMATION, NULL, CLSCTX_INPROC_SERVER,
                                &PROBE_IID_IUIAUTOMATION, (void **)&uia)))
    {
        printf("UI Automation is not available\n");
        return;
    }
    if (SUCCEEDED(IUIAutomation_ElementFromHandle(uia, hwnd, &win)) &&
        SUCCEEDED(IUIAutomation_CreateTrueCondition(uia, &all)) &&
        SUCCEEDED(IUIAutomationElement_FindAll(win, TreeScope_Descendants, all, &arr)) && arr)
    {
        IUIAutomationElementArray_get_Length(arr, &n);
        printf("\nUI Automation: %d elements under the window\n", n);
        for (i = 0; i < n; ++i)
        {
            IUIAutomationElement *e = NULL;
            BSTR                  name = NULL, type = NULL;

            if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &e)) || !e)
            {
                continue;
            }
            IUIAutomationElement_get_CurrentName(e, &name);
            IUIAutomationElement_get_CurrentLocalizedControlType(e, &type);
            printf("  %-16s \"%s\"\n", utf8(type), utf8(name));
            SysFreeString(name);
            SysFreeString(type);
            IUIAutomationElement_Release(e);
        }
        IUIAutomationElementArray_Release(arr);
    }
    if (all)
    {
        IUIAutomationCondition_Release(all);
    }
    if (win)
    {
        IUIAutomationElement_Release(win);
    }
    IUIAutomation_Release(uia);
}

int main(int argc, char **argv)
{
    const char         *exe = argc > 1 ? argv[1] : "example_forms.exe";
    const char         *title = argc > 2 ? argv[2] : "areole - a form";
    int                 want = argc > 3 ? atoi(argv[3]) : 20;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    HWND                hwnd = NULL;
    IAccessible        *acc = NULL;
    HRESULT             hr;
    int                 i, ok;

    SetConsoleOutputCP(CP_UTF8); /* the names are printed as UTF-8 */
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        fprintf(stderr, "cannot start %s\n", exe);
        return 2;
    }
    for (i = 0; i < 40 && !hwnd; ++i)
    {
        Sleep(250);
        hwnd = FindWindowA(NULL, title);
    }
    if (!hwnd)
    {
        fprintf(stderr, "no window titled \"%s\"\n", title);
        TerminateProcess(pi.hProcess, 0);
        return 2;
    }
    Sleep(1000); /* a frame or two, so the tree has been built */

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = AccessibleObjectFromWindow(hwnd, (DWORD)OBJID_CLIENT, &PROBE_IID_IACCESSIBLE,
                                    (void **)&acc);
    if (FAILED(hr) || !acc)
    {
        printf("MSAA: the window gave no object (0x%08lx)\n", (unsigned long)hr);
    }
    else
    {
        printf("MSAA:\n");
        msaa_walk(acc, 1);
        IAccessible_Release(acc);
    }
    uia_walk(hwnd);

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CoUninitialize();

    ok = g_controls >= want && g_unnamed == 0;
    printf("\n%d controls, %d without a name, %d expected: %s\n", g_controls, g_unnamed, want,
           ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
