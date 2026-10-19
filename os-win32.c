/*
 * os-win32.c
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2010 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <windows.h>
#include <mmsystem.h>
#include "system/runstate.h"

#ifndef CONFIG_UWP
static BOOL WINAPI qemu_ctrl_handler(DWORD type)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    /* Windows 7 kills application when the function returns.
       Sleep here to give QEMU a try for closing.
       Sleep period is 10000ms because Windows kills the program
       after 10 seconds anyway. */
    Sleep(10000);

    return TRUE;
}

static TIMECAPS mm_tc;

static void os_undo_timer_resolution(void)
{
    timeEndPeriod(mm_tc.wPeriodMin);
}
#endif

void os_setup_early_signal_handling(void)
{
#ifndef CONFIG_UWP
    SetConsoleCtrlHandler(qemu_ctrl_handler, TRUE);
    timeGetDevCaps(&mm_tc, sizeof(mm_tc));
    timeBeginPeriod(mm_tc.wPeriodMin);
    atexit(os_undo_timer_resolution);
#else
    /*
     * Raise the system timer resolution as on desktop Windows; otherwise
     * QEMU timers and sleeps quantize to the default 15.625 ms tick and
     * the VM runs well below real time.  The multimedia timer API is not
     * part of the UWP SDK surface but is present at runtime, so resolve
     * it dynamically.
     */
    typedef UINT(WINAPI *TimeBeginPeriodFn)(UINT);
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    TimeBeginPeriodFn begin_period = NULL;
    UINT begin_result = (UINT)-1;
    LARGE_INTEGER freq, t0, t1;
    int64_t total_us = 0;

    if (winmm == NULL) {
        winmm = LoadLibraryExW(L"winmm.dll", NULL,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (winmm != NULL) {
        begin_period = (TimeBeginPeriodFn)GetProcAddress(winmm,
                                                         "timeBeginPeriod");
    }
    if (begin_period != NULL) {
        begin_result = begin_period(1);
    }

    QueryPerformanceFrequency(&freq);
    for (int i = 0; i < 8; i++) {
        QueryPerformanceCounter(&t0);
        Sleep(1);
        QueryPerformanceCounter(&t1);
        total_us += (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;
    }
    fprintf(stderr,
            "xemu UWP: timeBeginPeriod(1) returned %u, Sleep(1) averages %d us\n",
            begin_result, (int)(total_us / 8));
#endif
}

void os_set_line_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}
