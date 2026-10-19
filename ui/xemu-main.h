/*
 * xemu frontend entry point helpers
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef UI_XEMU_MAIN_H
#define UI_XEMU_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

int xemu_main(int argc, char **argv);

#ifdef CONFIG_UWP
/* Applies a protocol launch that reached the running instance. Call with the
 * main loop lock held. */
void xemu_uwp_process_activation(void);
/* App suspend and resume, on the UI thread: suspend pauses the VM and
 * flushes its drive images; resume restarts a VM that suspend paused. */
void xemu_uwp_suspend_vm(void);
void xemu_uwp_resume_vm(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
