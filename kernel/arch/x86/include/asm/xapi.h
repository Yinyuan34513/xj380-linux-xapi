/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_XAPI_H
#define _ASM_X86_XAPI_H

#include <linux/ptrace.h>

/* XJ380 XAPI custom syscall handler (non-GUI subset). */
extern long xapi_handle_syscall(const struct pt_regs *regs, unsigned int nr);

/* XJ380 XAPI graphic subset (windows/drawing on fbdev, 7392-7463). */
extern long xapi_gui_handle_syscall(const struct pt_regs *regs, unsigned int nr);

#endif /* _ASM_X86_XAPI_H */
