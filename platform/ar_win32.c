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
#include <imm.h>
#include <commdlg.h>

#include "ar_win32_internal.h"

struct ar_win
{
    HWND    hwnd;
    HDC     memdc;
    HBITMAP dib;
    HBITMAP prev_bitmap;

    ar_surface surface;
    ar_input   input;

    /*
     * Rendering at a scale of the window: `render` is the buffer ar_frame_end
     * draws into, `render_scale` times the window each way, and ar_win_present
     * scales what changed down (an average of each block) or up (bilinear)
     * into `surface`. A scale of 0 or 1000 is the window's own pixels and none
     * of this exists.
     */
    ar_i32     render_scale;
    ar_surface render;
    SIZE_T     render_cap;

    /* The pointer the page asked for at the last frame, AR_CURSOR_*. */
    ar_i32 cursor;

    /* One frame's typed characters, as UTF-8, and the high half of a surrogate
       pair waiting for its partner. Windows sends an astral character as two
       messages and dropping the second is how an emoji becomes a question
       mark. */
    char   text_buf[64];
    ar_u32 text_n;
    ar_u32 pending_high;

    /*
     * What an input method is composing, as UTF-8, and how long it is. A state
     * that lasts across pumps -- the composition is shown until it is committed
     * or abandoned -- which is why it is not cleared with the per-frame edges.
     */
    char   compose_buf[256];
    ar_u32 compose_n;

    /* The clipboard's text on its way in, for Ctrl+V. Larger than a field can
       hold, because the field cuts it to fit and this should not cut first. */
    char paste_buf[8192];

    /* A run of presses close in time and place is a double or triple click.
       Counted here because the threshold is the user's setting, and Windows
       only reports the double -- never the triple a line selection needs. */
    DWORD  click_time;
    ar_i32 click_x, click_y;
    ar_u32 click_run;

    /* A timer is armed for the caret's next blink. */
    int timer_armed;

    /* What woke the last pump: the blink timer fired, and whether anything
       else did -- a message for the window that was not the timer or a
       repaint, a wake asked for, a resize. ar_win_idle is the pair. */
    int timer_fired;
    int only_time;

    int closed;
    int resized;
    int awake;          /* skip the block in the next pump */
    int tracking_leave; /* a TrackMouseEvent subscription is live */

    /* Wheel travel too small to fill a notch, carried between pumps. A
       high-resolution device reports far less than WHEEL_DELTA per message, so
       the leftover has to survive to the next one or it is thrown away. */
    ar_i32 wheel_accum;

    /* The same travel again, kept in pixel-units scaled by WHEEL_DELTA so the
       remainder stays exact. Separate from the one above because the two feed
       different fields and sharing an accumulator would spend the travel
       twice. */
    ar_i32 wheel_px_accum;
};

/*
 * How far one wheel notch travels, in pixels.
 *
 * A platform decision rather than a layout one, which is why the core does not
 * own this number: how much a click of the wheel should move is a property of
 * the machine and the person using it. It matches what the core falls back to
 * for a backend that reports only notches, so a mouse feels the same either
 * way and only a touchpad notices the difference.
 *
 * ponytail: fixed, where Windows would tell us. SPI_GETWHEELSCROLLLINES is the
 * user's setting and is in lines, so honouring it needs a line height this
 * layer does not have. Worth wiring up when the core can be asked for one.
 */
#define AR_WHEEL_PX 30

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

/* The render buffer for the window's current size at `scale`, grown when it
   has to be and never shrunk -- a window dragged smaller and back would
   otherwise allocate on every step. */
static int ar__render_fit(ar_win *win, ar_i32 scale)
{
    ar_i32 rw, rh;
    SIZE_T need;

    if (scale == 1000)
    {
        return 1;
    }
    /* Rounded up, so the page laid out in it covers the whole window. */
    rw = (win->surface.w * scale + 999) / 1000;
    rh = (win->surface.h * scale + 999) / 1000;
    rw = rw < 1 ? 1 : rw;
    rh = rh < 1 ? 1 : rh;
    need = (SIZE_T)rw * (SIZE_T)rh * sizeof(ar_u32);
    if (need > win->render_cap)
    {
        void *mem = VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        if (!mem)
        {
            return 0;
        }
        if (win->render.pixels)
        {
            VirtualFree(win->render.pixels, 0, MEM_RELEASE);
        }
        win->render.pixels = (ar_u32 *)mem;
        win->render_cap = need;
    }
    win->render.w = rw;
    win->render.h = rh;
    win->render.stride = rw;
    return 1;
}

static void ar__render_free(ar_win *win)
{
    if (win->render.pixels)
    {
        VirtualFree(win->render.pixels, 0, MEM_RELEASE);
    }
    win->render.pixels = NULL;
    win->render_cap = 0;
}

static int ar__scaled(const ar_win *win)
{
    return win->render_scale != 0 && win->render_scale != 1000 && win->render.pixels;
}

int ar_win_set_render_scale(ar_win *win, ar_ctx *c, ar_i32 thousandths)
{
    if (!win)
    {
        return 0;
    }
    thousandths = thousandths < 250 ? 250 : thousandths > 8000 ? 8000 : thousandths;
    if (!ar__render_fit(win, thousandths))
    {
        return 0;
    }
    win->render_scale = thousandths;
    if (c)
    {
        ar_set_render_scale(c, thousandths);
    }
    win->resized = 1; /* the surface ar_win_surface hands out is a new one */
    win->awake = 1;
    return 1;
}

static void ar__apply_cursor(const ar_win *win)
{
    LPCTSTR shape = IDC_ARROW;

    switch (win->cursor)
    {
    case AR_CURSOR_POINTER:
    case AR_CURSOR_GRAB:
    case AR_CURSOR_GRABBING:
        shape = IDC_HAND;
        break;
    case AR_CURSOR_TEXT:
        shape = IDC_IBEAM;
        break;
    case AR_CURSOR_MOVE:
        shape = IDC_SIZEALL;
        break;
    case AR_CURSOR_NOT_ALLOWED:
        shape = IDC_NO;
        break;
    case AR_CURSOR_CROSSHAIR:
        shape = IDC_CROSS;
        break;
    case AR_CURSOR_WAIT:
        shape = IDC_WAIT;
        break;
    case AR_CURSOR_PROGRESS:
        shape = IDC_APPSTARTING;
        break;
    case AR_CURSOR_HELP:
        shape = IDC_HELP;
        break;
    case AR_CURSOR_EW_RESIZE:
        shape = IDC_SIZEWE;
        break;
    case AR_CURSOR_NS_RESIZE:
        shape = IDC_SIZENS;
        break;
    case AR_CURSOR_NESW_RESIZE:
        shape = IDC_SIZENESW;
        break;
    case AR_CURSOR_NWSE_RESIZE:
        shape = IDC_SIZENWSE;
        break;
    case AR_CURSOR_NONE:
        SetCursor(NULL);
        return;
    default:
        break;
    }
    SetCursor(LoadCursor(NULL, shape));
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

    /* The render buffer follows the window. If it cannot, the window's own
       pixels are drawn into instead -- a picture at the wrong scale is
       better than none. */
    if (win->render_scale != 0 && win->render_scale != 1000 &&
        !ar__render_fit(win, win->render_scale))
    {
        ar__render_free(win);
    }
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

    if (button == AR_MOUSE_LEFT)
    {
        DWORD  now = GetMessageTime();
        ar_i32 dx = win->input.mouse_x - win->click_x;
        ar_i32 dy = win->input.mouse_y - win->click_y;

        if (win->click_run > 0 && now - win->click_time <= GetDoubleClickTime() &&
            (dx < 0 ? -dx : dx) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
            (dy < 0 ? -dy : dy) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
        {
            win->click_run = win->click_run >= 3 ? 1 : win->click_run + 1;
        }
        else
        {
            win->click_run = 1;
        }
        win->click_time = now;
        win->click_x = win->input.mouse_x;
        win->click_y = win->input.mouse_y;
        win->input.clicks = win->click_run;

        /* Shift and a click extend a selection, so the qualifier rides with
           the press as it does with an arrow. */
        if (GetKeyState(VK_SHIFT) & 0x8000)
        {
            win->input.keys_pressed |= AR_KEY_SHIFT;
        }
    }

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

    /*
     * The keys that scroll, and only those.
     *
     * WM_KEYDOWN repeats while a key is held, which is exactly what is wanted:
     * the repeat rate is a thing the user configured and the platform already
     * honours, so holding Page Down pages at their speed rather than at one
     * this code invented.
     *
     * Shift-space pages up in every browser. There is no modifier tracking in
     * the core, so the mapping is done here where GetKeyState exists -- which
     * is the honest split rather than a shortcut.
     */
    /*
     * Typed text, which is a different input from the key that produced it.
     *
     * WM_CHAR is where Windows has already applied the layout, the dead keys
     * and the modifiers -- so `@` arrives as `@` whether the keyboard spelled
     * it Shift+2 or AltGr+Q, and the core never learns what a layout is. A
     * table of virtual key codes could not have told those apart.
     *
     * UTF-16 in, UTF-8 out, and surrogates are joined across two messages
     * because Windows sends an astral character as two: dropping the second
     * half is how an emoji becomes a question mark.
     */
    case WM_CHAR:
    {
        ar_u32 cp = (ar_u32)wp;

        if (cp >= 0xD800u && cp <= 0xDBFFu)
        {
            win->pending_high = cp;
            return 0;
        }
        if (cp >= 0xDC00u && cp <= 0xDFFFu && win->pending_high)
        {
            cp = 0x10000u + ((win->pending_high - 0xD800u) << 10) + (cp - 0xDC00u);
            win->pending_high = 0;
        }
        /* Control characters arrive here too -- Enter is 13, Tab is 9,
           Backspace is 8 -- and every one of them is handled as a key above.
           Letting them through would type a control character into a field. */
        if (cp >= 0x20u && cp != 0x7Fu)
        {
            ar_u32 n = win->text_n;

            if (cp < 0x80u && n + 1 < sizeof win->text_buf)
            {
                win->text_buf[n++] = (char)cp;
            }
            else if (cp < 0x800u && n + 2 < sizeof win->text_buf)
            {
                win->text_buf[n++] = (char)(0xC0u | (cp >> 6));
                win->text_buf[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
            else if (cp < 0x10000u && n + 3 < sizeof win->text_buf)
            {
                win->text_buf[n++] = (char)(0xE0u | (cp >> 12));
                win->text_buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                win->text_buf[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
            else if (n + 4 < sizeof win->text_buf)
            {
                win->text_buf[n++] = (char)(0xF0u | (cp >> 18));
                win->text_buf[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                win->text_buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                win->text_buf[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
            win->text_n = n;
            win->text_buf[n] = 0;
            win->input.text = win->text_buf;
            win->input.text_len = n;
        }
        ar_win_wake(win);
        return 0;
    }

    /*
     * An input method, drawn by the core rather than by Windows.
     *
     * The default composition window floats wherever the system puts it and
     * draws the composition in its own font, which is the "faked" IME that
     * anybody typing Japanese notices at once. So the system's window is told
     * not to show, the composition string is handed to the core as state, and
     * the core draws it inline at the caret, underlined. Only the candidate
     * list stays the system's, placed beside the caret by ar_win_set_caret.
     */
    case WM_IME_SETCONTEXT:
        lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_IME_STARTCOMPOSITION:
        win->compose_n = 0;
        return 0;

    case WM_IME_COMPOSITION:
    {
        HIMC imc = ImmGetContext(hwnd);

        if (imc)
        {
            if (lp & GCS_RESULTSTR)
            {
                LONG  bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0);
                WCHAR wbuf[128];

                if (bytes > 0 && bytes <= (LONG)sizeof wbuf)
                {
                    int got =
                        (int)(ImmGetCompositionStringW(imc, GCS_RESULTSTR, wbuf, (DWORD)bytes) /
                              (LONG)sizeof(WCHAR));
                    int room = (int)(sizeof win->text_buf - 1 - win->text_n);
                    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, got, win->text_buf + win->text_n,
                                                room, NULL, NULL);

                    if (n > 0)
                    {
                        win->text_n += (ar_u32)n;
                        win->text_buf[win->text_n] = 0;
                        win->input.text = win->text_buf;
                        win->input.text_len = win->text_n;
                    }
                }
                win->compose_n = 0;
            }
            if (lp & GCS_COMPSTR)
            {
                LONG  bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, NULL, 0);
                WCHAR wbuf[128];

                win->compose_n = 0;
                if (bytes > 0 && bytes <= (LONG)sizeof wbuf)
                {
                    int got = (int)(ImmGetCompositionStringW(imc, GCS_COMPSTR, wbuf, (DWORD)bytes) /
                                    (LONG)sizeof(WCHAR));
                    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, got, win->compose_buf,
                                                (int)sizeof win->compose_buf - 1, NULL, NULL);

                    win->compose_n = n > 0 ? (ar_u32)n : 0u;
                }
            }
            ImmReleaseContext(hwnd, imc);
        }
        win->input.compose = win->compose_n ? win->compose_buf : NULL;
        win->input.compose_len = win->compose_n;
        ar_win_wake(win);
        return 0;
    }

    case WM_IME_ENDCOMPOSITION:
        win->compose_n = 0;
        win->input.compose = NULL;
        win->input.compose_len = 0;
        ar_win_wake(win);
        return 0;

    case WM_TIMER:
        /* The caret's blink, and nothing else: one timer, re-armed each frame
           for exactly as long as the core says the caret will hold still.
           No ar_win_wake: the message arriving is what ended the wait, and a
           wake on top of it bought a second, empty frame after every blink. */
        KillTimer(hwnd, 1);
        win->timer_armed = 0;
        win->timer_fired = 1;
        return 0;

    case WM_GETOBJECT:
    {
        LRESULT r = ar__win_a11y_getobject(hwnd, wp, lp);

        if (r)
        {
            return r;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        switch (wp)
        {
        case VK_ESCAPE:
            win->input.keys_pressed |= AR_KEY_ESCAPE;
            break;
        case VK_UP:
            win->input.keys_pressed |= AR_KEY_UP;
            break;
        case VK_DOWN:
            win->input.keys_pressed |= AR_KEY_DOWN;
            break;
        case VK_LEFT:
            win->input.keys_pressed |= AR_KEY_LEFT;
            break;
        case VK_RIGHT:
            win->input.keys_pressed |= AR_KEY_RIGHT;
            break;
        case VK_PRIOR:
            win->input.keys_pressed |= AR_KEY_PAGE_UP;
            break;
        case VK_NEXT:
            win->input.keys_pressed |= AR_KEY_PAGE_DOWN;
            break;
        case VK_HOME:
            win->input.keys_pressed |= AR_KEY_HOME;
            break;
        case VK_END:
            win->input.keys_pressed |= AR_KEY_END;
            break;
        case VK_SPACE:
            win->input.keys_pressed |=
                (GetKeyState(VK_SHIFT) & 0x8000) ? AR_KEY_PAGE_UP : AR_KEY_SPACE;
            break;
        case VK_TAB:
            /* Shift is resolved here, because the platform knows about
               modifiers and the core deliberately does not. */
            win->input.keys_pressed |=
                (GetKeyState(VK_SHIFT) & 0x8000) ? AR_KEY_TAB_BACK : AR_KEY_TAB;
            break;
        case VK_RETURN:
            win->input.keys_pressed |= AR_KEY_ENTER;
            break;
        case VK_BACK:
            win->input.keys_pressed |= AR_KEY_BACKSPACE;
            break;
        case VK_DELETE:
            win->input.keys_pressed |= AR_KEY_DELETE;
            break;
        case 'C':
        case 'X':
        case 'V':
            /*
             * The clipboard. Copy and cut are requests the core answers with
             * the text, which ar_win_set_clipboard puts on the clipboard after
             * the frame; paste is the clipboard's text arriving as typing, with
             * a bit that says it was pasted so it is one undo step.
             */
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                if (wp == 'C')
                {
                    win->input.keys_pressed |= AR_KEY_COPY;
                }
                else if (wp == 'X')
                {
                    win->input.keys_pressed |= AR_KEY_CUT;
                }
                else if (OpenClipboard(hwnd))
                {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    WCHAR *w = h ? (WCHAR *)GlobalLock(h) : NULL;

                    if (w)
                    {
                        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, win->paste_buf,
                                                    (int)sizeof win->paste_buf, NULL, NULL);

                        GlobalUnlock(h);
                        if (n > 1)
                        {
                            win->input.text = win->paste_buf;
                            win->input.text_len = (ar_u32)(n - 1);
                            win->input.keys_pressed |= AR_KEY_PASTE;
                        }
                    }
                    CloseClipboard();
                }
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case 'A':
        case 'Z':
        case 'Y':
            /*
             * The three editing commands, and the only place a modifier turns
             * a letter into something else.
             *
             * Ctrl+Z is undo and Ctrl+Y is redo on Windows; Ctrl+Shift+Z is
             * redo everywhere else and is accepted too, because people bring
             * their fingers with them. Without Ctrl these fall through to
             * WM_CHAR and arrive as the letters they are.
             */
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                if (wp == 'A')
                {
                    win->input.keys_pressed |= AR_KEY_SELECT_ALL;
                }
                else if (wp == 'Y')
                {
                    win->input.keys_pressed |= AR_KEY_REDO;
                }
                else
                {
                    win->input.keys_pressed |=
                        (GetKeyState(VK_SHIFT) & 0x8000) ? AR_KEY_REDO : AR_KEY_UNDO;
                }
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        default:
            /* Every other key is somebody else's, and swallowing it would stop
               Alt+F4 working. Fall through to DefWindowProc. */
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (GetKeyState(VK_SHIFT) & 0x8000)
        {
            /* Not a modifier table -- the one modifier that changes what an
               arrow means, resolved where GetKeyState lives. */
            win->input.keys_pressed |= AR_KEY_SHIFT;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) &&
            (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_BACK || wp == VK_DELETE || wp == VK_HOME ||
             wp == VK_END))
        {
            /* And Ctrl, for the keys it gives a second meaning: by word, or to
               the very start and end. */
            win->input.keys_pressed |= AR_KEY_CTRL;
        }
        ar_win_wake(win);
        return 0;

    /* The pointer over the client area is the page's to choose; anywhere
       else -- a border, the caption -- Windows keeps its own. */
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            ar__apply_cursor(win);
            return TRUE;
        }
        break;

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
        /*
         * Accumulate the raw delta and keep what does not fill a notch.
         *
         * This divided by WHEEL_DELTA and dropped the remainder. A mouse wheel
         * sends exactly one WHEEL_DELTA per click, so that worked -- and hid
         * this for as long as nobody scrolled with a laptop. A Windows
         * precision touchpad reports a high-resolution delta, 8 or 10 or 40,
         * and every one of those truncated to zero notches: gentle scrolling
         * did nothing whatsoever, and only a flick hard enough to pass 120
         * inside a single message moved anything.
         *
         * Stepped rather than divided because C89 leaves the sign of % with a
         * negative operand to the implementation, and scrolling up is negative.
         */
        win->wheel_accum += (ar_i32)GET_WHEEL_DELTA_WPARAM(wp);
        while (win->wheel_accum >= WHEEL_DELTA)
        {
            win->input.wheel += 1;
            win->wheel_accum -= WHEEL_DELTA;
        }
        while (win->wheel_accum <= -WHEEL_DELTA)
        {
            win->input.wheel -= 1;
            win->wheel_accum += WHEEL_DELTA;
        }

        /*
         * And the same travel in pixels, which is what a touchpad actually
         * meant. One notch is AR_WHEEL_PX, so the conversion is a ratio, and
         * the accumulator holds pixel-units scaled by WHEEL_DELTA so that the
         * remainder is exact rather than rounded away a message at a time.
         *
         * Whole pixels come out, the rest stays for the next message: a slow
         * two-finger drag therefore moves a few pixels per message instead of
         * accumulating silently towards a notch it may never reach.
         */
        win->wheel_px_accum += (ar_i32)GET_WHEEL_DELTA_WPARAM(wp) * AR_WHEEL_PX;
        {
            ar_i32 whole = win->wheel_px_accum / WHEEL_DELTA;

            win->input.wheel_px += whole;
            win->wheel_px_accum -= whole * WHEEL_DELTA;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        win->closed = 1;
        ar__win_a11y_detach();
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
    ar__render_free(win);
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
    win->input.wheel_px = 0;
    win->input.keys_pressed = 0;
    win->input.text = 0;
    win->input.text_len = 0;
    win->input.clicks = 0;
    win->text_n = 0;
    win->resized = 0;
    win->timer_fired = 0;
    win->only_time = !win->awake;

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
        /* Messages for other windows on this thread -- COM's, marshalling a
           screen reader's calls -- change nothing a frame would see. */
        if (msg.hwnd == win->hwnd && msg.message != WM_TIMER && msg.message != WM_PAINT)
        {
            win->only_time = 0;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return win->closed ? 0 : 1;
}

int ar_win_idle(const ar_win *win)
{
    return win && win->timer_fired && win->only_time && !win->resized;
}

ar_surface *ar_win_surface(ar_win *win)
{
    return ar__scaled(win) ? &win->render : &win->surface;
}

HWND ar_win_hwnd(const ar_win *win)
{
    return win ? win->hwnd : NULL;
}

void ar_win_wake_after(ar_win *win, ar_u32 us)
{
    UINT ms;

    if (!win || !win->hwnd)
    {
        return;
    }
    if (us == 0)
    {
        if (win->timer_armed)
        {
            KillTimer(win->hwnd, 1);
            win->timer_armed = 0;
        }
        return;
    }
    ms = (UINT)((us + 999u) / 1000u);
    SetTimer(win->hwnd, 1, ms ? ms : 1, NULL);
    win->timer_armed = 1;
}

void ar_win_set_clipboard(ar_win *win, const char *utf8, ar_u32 len)
{
    int     w;
    HGLOBAL h;
    WCHAR  *dst;

    if (!win || !win->hwnd || !utf8 || len == 0)
    {
        return;
    }
    w = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    if (w <= 0)
    {
        return;
    }
    h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(w + 1) * sizeof(WCHAR));
    if (!h)
    {
        return;
    }
    dst = (WCHAR *)GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, dst, w);
    dst[w] = 0;
    GlobalUnlock(h);
    if (OpenClipboard(win->hwnd))
    {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, h))
        {
            h = NULL; /* the clipboard owns it now */
        }
        CloseClipboard();
    }
    if (h)
    {
        GlobalFree(h);
    }
}

/*
 * Where the caret is, for the input method's own windows.
 *
 * The composition is drawn by the core, so all that is left to place is the
 * candidate list -- under the caret, and excluding the caret's line so the
 * list never covers what is being composed.
 */
void ar_win_set_caret(ar_win *win, ar_rect caret)
{
    HIMC imc;

    if (!win || !win->hwnd || ar_rect_is_empty(caret))
    {
        return;
    }
    imc = ImmGetContext(win->hwnd);
    if (imc)
    {
        COMPOSITIONFORM cf;
        CANDIDATEFORM   cand;

        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = caret.x;
        cf.ptCurrentPos.y = caret.y;
        ImmSetCompositionWindow(imc, &cf);

        cand.dwIndex = 0;
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos.x = caret.x;
        cand.ptCurrentPos.y = caret.y + caret.h;
        cand.rcArea.left = caret.x;
        cand.rcArea.top = caret.y;
        cand.rcArea.right = caret.x + caret.w;
        cand.rcArea.bottom = caret.y + caret.h;
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(win->hwnd, imc);
    }
}

ar_u32 ar_win_choose_file(ar_win *win, char *out, ar_u32 cap)
{
    OPENFILENAMEW ofn;
    WCHAR         path[MAX_PATH];
    WCHAR        *name;
    int           n;

    if (!win || !out || cap == 0)
    {
        return 0;
    }
    path[0] = 0;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = win->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
    {
        return 0;
    }
    /* The name and not the path, which is all a file input exposes -- a browser
       says C:\fakepath\ in front of it rather than tell a page where your
       files are. */
    name = path + ofn.nFileOffset;
    n = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, (int)cap, NULL, NULL);
    return n > 1 ? (ar_u32)(n - 1) : 0u;
}

void ar_win_after_frame(ar_win *win, ar_ctx *c)
{
    ar_u32      n = 0;
    const char *clip;
    ar_i32      file;

    if (!win || !c)
    {
        return;
    }
    clip = ar_clipboard_text(c, &n);
    if (clip && n > 0)
    {
        ar_win_set_clipboard(win, clip, n);
    }

    /* A new pointer is shown now, not at the next mouse move: a box that
       starts asking for the hand under a still pointer gets it. */
    if (ar_cursor(c) != win->cursor)
    {
        POINT p;
        RECT  r;

        win->cursor = ar_cursor(c);
        if (GetCursorPos(&p) && ScreenToClient(win->hwnd, &p) && GetClientRect(win->hwnd, &r) &&
            PtInRect(&r, p))
        {
            ar__apply_cursor(win);
        }
    }
    ar_win_set_caret(win, ar_caret_rect(c));
    ar_win_wake_after(win, ar_caret_wait_us(c));

    file = ar_file_wanted(c);
    if (file >= 0)
    {
        char   name[260];
        ar_u32 len = ar_win_choose_file(win, name, sizeof name);

        if (len > 0)
        {
            ar_file_chosen(c, file, name, len);
        }
        ar_win_wake(win);
    }
    ar_win_a11y_update(win);
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

    if (ar__scaled(win))
    {
        ar_surface_resample(&win->render, &win->surface, d, win->render_scale);
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
