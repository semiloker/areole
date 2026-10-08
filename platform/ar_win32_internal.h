/*
 * areole - what the Win32 backend's files share and nobody else sees.
 * SPDX-License-Identifier: MIT
 */
#ifndef AR_WIN32_INTERNAL_H
#define AR_WIN32_INTERNAL_H

#include <windows.h>

#include "areole_win32.h"

/* WM_GETOBJECT, answered by ar_win32_a11y.c. Returns 0 when there is nothing
   to answer with, which sends the message on to DefWindowProc. */
LRESULT ar__win_a11y_getobject(HWND hwnd, WPARAM wp, LPARAM lp);

/* The window is going away: no object may answer for it again. */
void ar__win_a11y_detach(void);

/* The window itself, for the files that have to name it to Windows. */
HWND ar_win_hwnd(const ar_win *win);

#endif /* AR_WIN32_INTERNAL_H */
