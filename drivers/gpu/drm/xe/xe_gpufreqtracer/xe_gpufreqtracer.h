/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_GPUFREQTRACER_H_
#define _XE_GPUFREQTRACER_H_

#include <linux/types.h>

struct xe_device;
struct xe_gt;

#ifdef CONFIG_DRM_XE_GPUFREQTRACER

/*
 * Initialize the GPU frequency tracer for a device
 */
int xe_gpufreqtracer_init(struct xe_device *xe);

/*
 * Cleanup the GPU frequency tracer for a device
 */
void xe_gpufreqtracer_fini(struct xe_device *xe);

/*
 * Report a GPU frequency change directly
 * @gt: The GT instance
 * @frequency_khz: The new frequency in KHz
 */
void xe_gpufreqtracer_report_frequency_change(struct xe_gt *gt, u32 frequency_khz);

/*
 * Start periodic frequency monitoring for a GT
 * @gt: The GT instance
 *
 * Uses the global module parameter for monitoring interval.
 */
int xe_gpufreqtracer_start_monitoring(struct xe_gt *gt);

/*
 * Stop periodic frequency monitoring for a GT
 * @gt: The GT instance
 */
void xe_gpufreqtracer_stop_monitoring(struct xe_gt *gt);

#else /* CONFIG_DRM_XE_GPUFREQTRACER */

static inline int xe_gpufreqtracer_init(struct xe_device *xe)
{
	return 0;
}

static inline void xe_gpufreqtracer_fini(struct xe_device *xe)
{
}

static inline void xe_gpufreqtracer_report_frequency_change(struct xe_gt *gt, u32 frequency_khz)
{
}

static inline int xe_gpufreqtracer_start_monitoring(struct xe_gt *gt)
{
	return 0;
}

static inline void xe_gpufreqtracer_stop_monitoring(struct xe_gt *gt)
{
}

#endif /* CONFIG_DRM_XE_GPUFREQTRACER */

#endif /* _XE_GPUFREQTRACER_H_ */
