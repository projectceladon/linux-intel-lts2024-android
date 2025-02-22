/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM bl_hib

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_S2D_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_S2D_H

#include <trace/hooks/vendor_hooks.h>

struct file;

DECLARE_HOOK(android_vh_check_hibernation_swap,
	TP_PROTO(struct file *resume_block_file, bool *hib_swap),
	TP_ARGS(resume_block_file, hib_swap));

DECLARE_HOOK(android_vh_save_cpu_resume,
	TP_PROTO(u64 *addr, u64 phys_addr),
	TP_ARGS(addr, phys_addr));

DECLARE_HOOK(android_vh_save_hib_resume_bdev,
	TP_PROTO(struct file *hib_resume_bdev_file),
	TP_ARGS(hib_resume_bdev_file));

#endif /* _TRACE_HOOK_S2D_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
