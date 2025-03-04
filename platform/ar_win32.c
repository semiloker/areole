/*
 * areole - Win32 backend: window, back buffer, input, clock.
 * SPDX-License-Identifier: MIT
 *
 * Compiled as gnu89 rather than strict C89, because <windows.h> uses nameless
 * unions and __int64 and does not compile with language extensions disabled.
 * That is the entire reason the core and the platform layer are separate
 * targets, and CI greps to keep it that way.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0500 /* Windows 2000 */
#define WINVER       0x0500

#include <windows.h>
#include <mmsystem.h>

#include "areole_win32.h"

struct ar_win
{
    HWND    hwnd;
    HDC     memdc;
    HBITMAP dib;
    HBITMAP prev_bitmap;

    ar_surface surface;
    ar_input   input;

    int closed;
    int resized;
    int awake;          /* skip the block in the next pump */
    int tracking_leave; /* a TrackMouseEvent subscription is live */
};

/* ponytail: one window per process, so the message procedure can find its
   window without a lookup. Multiple windows need this moved into a context;
   nothing needs a second window yet. */
static ar_win g_win;
static int    g_win_open = 0;

static const WCHAR AR_WINDOW_CLASS[] = L"areole.window";

/* ------------------------------------------------------------------------
 * Back buffer
 *
 * 32 bits per pixel, BI_RGB, and biHeight negative so the rows run top down.
 * Matching the format GDI stores natively is what keeps presentation to a
 * straight copy; a mismatch makes the driver convert every pixel, and that
 * costs far more than the choice of blit call ever will.
 * ------------------------------------------------------------------------ */
static void ar__surface_destroy(ar_win *win)
{
    if (win->memdc)
    {
        if (win->prev_bitmap)
        {
            SelectObject(win->memdc, win->prev_bitmap);
            win->prev_bitmap = NULL;
        }
        DeleteDC(win->memdc);
        win->memdc = NULL;
    }
    if (win->dib)
    {
        DeleteObject(win->dib);
        win->dib = NULL;
    }
    win->surface.pixels = NULL;
    win->surface.w = 0;
    win->surface.h = 0;
    win->surface.stride = 0;
}

static int ar__surface_create(ar_win *win, ar_i32 w, ar_i32 h)
{
    BITMAPINFO bi;
    HDC        screen;
    void      *bits = NULL;

    if (w < 1)
    {
        w = 1;
    }
    if (h < 1)
    {
        h = 1;
    }

    ar__surface_destroy(win);

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h; /* top down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    win->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);

    if (!win->dib || !bits)
    {
        return 0;
    }

    win->memdc = CreateCompatibleDC(NULL);
    if (!win->memdc)
    {
        ar__surface_destroy(win);
        return 0;
    }
    win->prev_bitmap = (HBITMAP)SelectObject(win->memdc, win->dib);

    win->surface.pixels = (ar_u32 *)bits;
    win->surface.w = w;
    win->surface.h = h;
    win->surface.stride = w; /* a 32 bpp DIB row is already a multiple of 4 */
    return 1;
}

/* ------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------ */
static void ar__track_leave(ar_win *win)
{
    TRACKMOUSEEVENT tme;

    /* TrackMouseEvent is a one shot subscription, not a mode. Without
       re-arming it on every entry, the hover highlight stays lit forever once
       the cursor leaves the window. */
    if (win->tracking_leave)
    {
        return;
    }
    ZeroMemory(&tme, sizeof tme);
    tme.cbSize = sizeof tme;
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = win->hwnd;
    if (TrackMouseEvent(&tme))
    {
        win->tracking_leave = 1;
    }
}

static void ar__mouse_down(ar_win *win, ar_u32 button, LPARAM lp)
{
    win->input.mouse_x = (ar_i32)(short)LOWORD(lp);
    win->input.mouse_y = (ar_i32)(short)HIWORD(lp);
    win->input.mouse_down |= button;
    win->input.mouse_pressed |= button;

    /* Without capture, dragging a slider past the window edge silently stops
       delivering moves and the control sticks. */
    SetCapture(win->hwnd);
}

static void ar__mouse_up(ar_win *win, ar_u32 button, LPARAM lp)
{
    win->input.mouse_x = (ar_i32)(short)LOWORD(lp);
    win->input.mouse_y = (ar_i32)(short)HIWORD(lp);
    win->input.mouse_down &= ~button;
    win->input.mouse_released |= button;

    if (win->input.mouse_down == 0)
    {
        ReleaseCapture();
    }
}

/* ------------------------------------------------------------------------
 * Window procedure
 * ------------------------------------------------------------------------ */
static LRESULT CALLBACK ar__wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ar_win *win = &g_win;

    if (!g_win_open || win->hwnd != hwnd)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        /* The single largest source of flicker. Left to DefWindowProc, GDI
           paints the whole client area with the class brush before WM_PAINT
           ever runs, and the frame is visibly built on top of a flash. */
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(hwnd, &ps);
        if (win->memdc)
        {
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, win->memdc, ps.rcPaint.left, ps.rcPaint.top,
                   SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
        {
            ar__surface_create(win, (ar_i32)LOWORD(lp), (ar_i32)HIWORD(lp));
            win->resized = 1;
            win->awake = 1;
        }
        return 0;

    case WM_MOUSEMOVE:
        win->input.mouse_x = (ar_i32)(short)LOWORD(lp);
        win->input.mouse_y = (ar_i32)(short)HIWORD(lp);
        win->input.mouse_inside = 1;
        ar__track_leave(win);
        return 0;

    case WM_MOUSELEAVE:
        win->tracking_leave = 0;
        win->input.mouse_inside = 0;
        return 0;

    case WM_LBUTTONDOWN:
        ar__mouse_down(win, AR_MOUSE_LEFT, lp);
        return 0;
    case WM_LBUTTONUP:
        ar__mouse_up(win, AR_MOUSE_LEFT, lp);
        return 0;
    case WM_RBUTTONDOWN:
        ar__mouse_down(win, AR_MOUSE_RIGHT, lp);
        return 0;
    case WM_RBUTTONUP:
        ar__mouse_up(win, AR_MOUSE_RIGHT, lp);
        return 0;
    case WM_MBUTTONDOWN:
        ar__mouse_down(win, AR_MOUSE_MIDDLE, lp);
        return 0;
    case WM_MBUTTONUP:
        ar__mouse_up(win, AR_MOUSE_MIDDLE, lp);
        return 0;

    case WM_CAPTURECHANGED:
        /* The system can take capture away: an alt-tab, another window on
           another thread. Anything still believing a button is held would
           leave a control latched, so drop the whole held state. */
        win->input.mouse_down = 0;
        return 0;

    case WM_MOUSEWHEEL:
        win->input.wheel += (ar_i32)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        win->closed = 1;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
ar_win *ar_win_open(const char *title, ar_i32 w, ar_i32 h)
{
    WNDCLASSEXW wc;
    RECT        r;
    WCHAR       wtitle[256];
    HINSTANCE   inst;
    int         n;

    if (g_win_open)
    {
        return NULL;
    }

    inst = GetModuleHandleW(NULL);

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = ar__wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* No background brush. Together with returning 1 from WM_ERASEBKGND this
       is what makes the window flicker free. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = AR_WINDOW_CLASS;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return NULL;
    }

    ZeroMemory(&g_win, sizeof g_win);
    g_win_open = 1;

    /* The caller asked for a client area, not a window, so grow the rect by
       whatever the current borders and caption happen to be. */
    r.left = 0;
    r.top = 0;
    r.right = (LONG)w;
    r.bottom = (LONG)h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    n = MultiByteToWideChar(CP_UTF8, 0, title ? title : "areole", -1, wtitle,
                            (int)(sizeof wtitle / sizeof wtitle[0]));
    if (n <= 0)
    {
        wtitle[0] = 0;
    }

    g_win.hwnd =
        CreateWindowExW(0, AR_WINDOW_CLASS, wtitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                        CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!g_win.hwnd)
    {
        g_win_open = 0;
        return NULL;
    }

    if (!ar__surface_create(&g_win, w, h))
    {
        DestroyWindow(g_win.hwnd);
        g_win_open = 0;
        return NULL;
    }

    ShowWindow(g_win.hwnd, SW_SHOW);
    UpdateWindow(g_win.hwnd);
    g_win.awake = 1;
    return &g_win;
}

void ar_win_close(ar_win *win)
{
    if (!win || !g_win_open)
    {
        return;
    }
    ar__surface_destroy(win);
    if (win->hwnd)
    {
        DestroyWindow(win->hwnd);
        win->hwnd = NULL;
    }
    g_win_open = 0;
}

void ar_win_set_title(ar_win *win, const char *title)
{
    WCHAR wtitle[256];

    if (!win || !win->hwnd || !title)
    {
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle,
                            (int)(sizeof wtitle / sizeof wtitle[0])) > 0)
    {
        SetWindowTextW(win->hwnd, wtitle);
    }
}

void ar_win_wake(ar_win *win)
{
    win->awake = 1;
}

int ar_win_pump(ar_win *win)
{
    MSG msg;

    /* Edges last exactly one frame. */
    win->input.mouse_pressed = 0;
    win->input.mouse_released = 0;
    win->input.wheel = 0;
    win->resized = 0;

    /* With nothing pending and nothing animating, block instead of spinning.
       This is the whole of the idle CPU story: a window nobody is touching
       costs nothing at all. */
    if (!win->awake && !PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE))
    {
        WaitMessage();
    }
    win->awake = 0;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            win->closed = 1;
            return 0;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return win->closed ? 0 : 1;
}

ar_surface *ar_win_surface(ar_win *win)
{
    return &win->surface;
}

const ar_input *ar_win_input(const ar_win *win)
{
    return &win->input;
}

int ar_win_resized(const ar_win *win)
{
    return win->resized;
}

void ar_win_present(ar_win *win, ar_rect dirty)
{
    HDC     dc;
    ar_rect d;

    if (!win->memdc || !win->hwnd)
    {
        return;
    }

    d = ar_rect_intersect(dirty, ar_rect_make(0, 0, win->surface.w, win->surface.h));
    if (ar_rect_is_empty(d))
    {
        return;
    }

    dc = GetDC(win->hwnd);
    if (!dc)
    {
        return;
    }
    /* One straight copy, of only what changed. The source is the same memory
       the rasterizer just wrote, so nothing is copied twice. */
    BitBlt(dc, (int)d.x, (int)d.y, (int)d.w, (int)d.h, win->memdc, (int)d.x, (int)d.y, SRCCOPY);
    ReleaseDC(win->hwnd, dc);
}

/* ------------------------------------------------------------------------
 * Clock
 * ------------------------------------------------------------------------ */
static int           g_clock_ready = 0;
static int           g_use_qpc = 0;
static int           g_qpc_vetted = 0;
static LARGE_INTEGER g_qpc_freq;
static LARGE_INTEGER g_qpc_origin;
static DWORD         g_mm_origin;
static ar_u32        g_last_us = 0;

static void ar__clock_init(void)
{
    g_clock_ready = 1;

    /* 1 ms scheduling and 1 ms timeGetTime resolution for the session. */
    timeBeginPeriod(1);
    g_mm_origin = timeGetTime();

    if (QueryPerformanceFrequency(&g_qpc_freq) && g_qpc_freq.QuadPart > 0 &&
        QueryPerformanceCounter(&g_qpc_origin))
    {
        g_use_qpc = 1;
    }
}

ar_u32 ar_time_us(void)
{
    ar_u32        us;
    DWORD         mm_elapsed;
    LARGE_INTEGER now;

    if (!g_clock_ready)
    {
        ar__clock_init();
    }

    mm_elapsed = timeGetTime() - g_mm_origin;

    if (g_use_qpc && QueryPerformanceCounter(&now))
    {
        __int64 ticks = now.QuadPart - g_qpc_origin.QuadPart;
        us = (ar_u32)((ticks * 1000000) / g_qpc_freq.QuadPart);

        /* On early dual cores QPC could step backwards when a thread migrated
           between cores. Once seen, it is never trusted again. */
        if (us < g_last_us)
        {
            g_use_qpc = 0;
        }
        /* On XP x64 QPC ran at half speed under AMD power management, which is
           a wrong rate rather than a wrong direction and needs a second clock
           to notice. Cross-check once, after enough real time has passed that
           the 1 ms granularity of timeGetTime does not dominate. Doing it this
           way costs nothing at startup, which matters: cold start is a number
           this library intends to publish. */
        else if (!g_qpc_vetted && mm_elapsed >= 200)
        {
            ar_u32 mm_us = (ar_u32)mm_elapsed * 1000u;
            ar_u32 diff = us > mm_us ? us - mm_us : mm_us - us;
            g_qpc_vetted = 1;
            if (diff > mm_us / 8u)
            {
                g_use_qpc = 0;
            }
        }

        if (g_use_qpc)
        {
            g_last_us = us;
            return us;
        }
    }

    us = (ar_u32)mm_elapsed * 1000u;
    if (us < g_last_us)
    {
        us = g_last_us; /* timeGetTime wraps at 49.7 days; never go backwards */
    }
    g_last_us = us;
    return us;
}

const char *ar_time_source(void)
{
    if (!g_clock_ready)
    {
        ar__clock_init();
    }
    return g_use_qpc ? "QueryPerformanceCounter" : "timeGetTime";
}
