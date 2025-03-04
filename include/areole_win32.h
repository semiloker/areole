/*
 * areole - Win32 backend.
 * SPDX-License-Identifier: MIT
 *
 * This header, and only this header, knows what an HWND is. The portable core
 * in areole.h must never include it.
 *
 * Targets Windows 2000 and up. Nothing here calls an API newer than that
 * without resolving it at run time first.
 */
#ifndef AREOLE_WIN32_H
#define AREOLE_WIN32_H

#include "areole.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ar_win ar_win;

/* Mouse button bits, as they appear in ar_input. */
#define AR_MOUSE_LEFT   0x01u
#define AR_MOUSE_RIGHT  0x02u
#define AR_MOUSE_MIDDLE 0x04u

/* A snapshot of the input state for one frame. Edges are cleared by the next
   ar_win_pump, so a caller that draws once per pump sees each click once. */
typedef struct ar_input
{
    ar_i32 mouse_x, mouse_y;
    ar_u32 mouse_down;     /* held right now              */
    ar_u32 mouse_pressed;  /* went down since last pump   */
    ar_u32 mouse_released; /* came up since last pump     */
    ar_i32 wheel;          /* notches, positive is away from the user */
    int    mouse_inside;   /* cursor is over the client area */
} ar_input;

/* Opens a window and its back buffer. Returns NULL on failure.
   ponytail: one window per process. Multiple windows need the context to own
   the list rather than a file static; add it when something needs a second. */
ar_win *ar_win_open(const char *title, ar_i32 w, ar_i32 h);
void    ar_win_close(ar_win *win);

/* Drains pending messages. Blocks until something happens if there is nothing
   pending and no redraw was requested, which is what keeps idle CPU at zero.
   Returns 0 once the window has been closed. */
int ar_win_pump(ar_win *win);

/* Replaces the title bar text. The examples use it as a readout, because a
   library that cannot draw a glyph yet still has numbers worth showing. */
void ar_win_set_title(ar_win *win, const char *title);

/* Asks the next pump not to block. Call this while animating. */
void ar_win_wake(ar_win *win);

/* The back buffer. This is the memory CreateDIBSection handed back, so
   rasterizing into it and presenting costs one copy rather than two. The
   pointer changes when the window is resized: fetch it once per frame. */
ar_surface *ar_win_surface(ar_win *win);

const ar_input *ar_win_input(const ar_win *win);

/* Copies the given region of the back buffer to the screen. Passing an empty
   rect presents nothing. */
void ar_win_present(ar_win *win, ar_rect dirty);

/* True for the frame after the window changed size, when the surface pointer
   and dimensions are new and everything must be redrawn. */
int ar_win_resized(const ar_win *win);

/* ------------------------------------------------------------------------
 * Clock
 *
 * Microseconds since the first call. Monotonic by construction.
 *
 * QueryPerformanceCounter is the obvious choice and the wrong one to trust
 * blindly on the hardware areole targets: on XP x64 it ran at half speed under
 * AMD power management, and on early dual cores it could step backwards when a
 * thread migrated. Both faults are detected here at run time and demoted to
 * timeGetTime, which is dull, 1 ms, and always right.
 * ------------------------------------------------------------------------ */
ar_u32 ar_time_us(void);

/* Which clock is actually in use, for the benchmark report to state. */
const char *ar_time_source(void);

#ifdef __cplusplus
}
#endif

#endif /* AREOLE_WIN32_H */
