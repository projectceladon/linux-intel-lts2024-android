// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2024 Intel Corporation
 */

#include "xe_gpufreqtracer.h"

#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/container_of.h>
#include <linux/gfp.h>
#include <linux/atomic.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_types.h"
#include "xe_guc_pc.h"
#include "xe_module.h"

#define CREATE_TRACE_POINTS
#include "xe_gpufreqtracer_trace.h"

/**
 * struct xe_gpufreqtracer_gt_data - Per-GT frequency monitoring data
 * @gt: Reference to the GT
 * @timer: Timer for periodic monitoring
 * @work: Work item for frequency sampling
 * @last_frequency: Last reported frequency to avoid duplicate reports (atomic)
 * @monitoring_active: Whether monitoring is currently active (atomic)
 */
struct xe_gpufreqtracer_gt_data {
	struct xe_gt *gt;
	struct timer_list timer;
	struct work_struct work;
	atomic_t last_frequency;
	atomic_t monitoring_active;
};

/**
 * struct xe_gpufreqtracer_data - Per-device frequency tracer data
 * @xe: Reference to the XE device
 * @gt_data: Array of per-GT monitoring data
 */
struct xe_gpufreqtracer_data {
	struct xe_device *xe;
	struct xe_gpufreqtracer_gt_data *gt_data;
};


/**
 * xe_gpufreqtracer_sample_work - Worker function to sample GPU frequency.
 * @work: Pointer to the work_struct representing the scheduled work.
 *
 * This function is executed in a workqueue context to periodically sample
 * the GPU frequency and perform any necessary tracing or logging operations.
 * It is part of the GPU frequency tracer subsystem.
 */
static void xe_gpufreqtracer_sample_work(struct work_struct *work)
{
	struct xe_gpufreqtracer_gt_data *gt_data =
		container_of(work, struct xe_gpufreqtracer_gt_data, work);
	struct xe_gt *gt = gt_data->gt;
	struct xe_guc_pc *pc = &gt->uc.guc.pc;
	u32 current_freq;
	u32 last_freq;

	if (!atomic_read(&gt_data->monitoring_active)) {
		drm_warn(&gt_to_xe(gt)->drm, "monitoring not active for GT%u, exiting",
			 gt->info.id);
		return;
	}

	current_freq = xe_guc_pc_get_act_freq(pc) * 1000; /* Convert MHz to KHz */
	last_freq = atomic_read(&gt_data->last_frequency);

	/* Only report if frequency has changed or this is the first sample */
	if (current_freq != last_freq) {
		drm_dbg(&gt_to_xe(gt)->drm, "GT%u frequency changed, tracing %u KHz",
			gt->info.id, current_freq);
		trace_gpu_frequency(current_freq, gt->info.id);
		atomic_set(&gt_data->last_frequency, current_freq);
	}
}

/**
 * xe_gpufreqtracer_timer_callback - Timer callback for GPU frequency tracer
 * @timer: Pointer to the timer_list structure associated with this callback
 *
 * This function is invoked when the timer associated with the GPU frequency tracer expires.
 * It is responsible for handling periodic tasks related to GPU frequency tracing, such as
 * sampling or logging frequency data.
 */
static void xe_gpufreqtracer_timer_callback(struct timer_list *timer)
{
	struct xe_gpufreqtracer_gt_data *gt_data =
		container_of(timer, struct xe_gpufreqtracer_gt_data, timer);

	if (atomic_read(&gt_data->monitoring_active)) {
		queue_work(system_highpri_wq, &gt_data->work);
		mod_timer(&gt_data->timer, jiffies +
			  msecs_to_jiffies(xe_modparam.gpufreq_monitoring_interval_ms));
	} else {
		drm_warn(&gt_to_xe(gt_data->gt)->drm, "timer callback for GT%u but monitoring inactive",
			 gt_data->gt->info.id);
	}
}

/**
 * xe_gpufreqtracer_init - Initialize GPU frequency tracer for a device
 * @xe: The XE device
 *
 * Sets up the frequency tracer infrastructure for all GTs in the device.
 *
 * Return: 0 on success, negative error code on failure
 */
int xe_gpufreqtracer_init(struct xe_device *xe)
{
	struct xe_gpufreqtracer_data *tracer_data;
	struct xe_gt *gt;
	u8 tile_id;
	int ret = 0;

	tracer_data = kzalloc(sizeof(*tracer_data), GFP_KERNEL);
	if (!tracer_data)
		return -ENOMEM;

	tracer_data->xe = xe;

	/* Allocate GT data array based on actual GT count */
	tracer_data->gt_data = kcalloc(xe->info.gt_count,
				       sizeof(*tracer_data->gt_data),
				       GFP_KERNEL);
	if (!tracer_data->gt_data) {
		ret = -ENOMEM;
		goto err_free_tracer;
	}

	/* Initialize per-GT data */
	for_each_gt(gt, xe, tile_id) {
		struct xe_gpufreqtracer_gt_data *gt_data =
			&tracer_data->gt_data[gt->info.id];

		drm_dbg(&xe->drm, "initializing GT%u (tile %u)", gt->info.id, tile_id);

		gt_data->gt = gt;
		atomic_set(&gt_data->monitoring_active, 0);
		atomic_set(&gt_data->last_frequency, 0);

		INIT_WORK(&gt_data->work, xe_gpufreqtracer_sample_work);
		timer_setup(&gt_data->timer, xe_gpufreqtracer_timer_callback, 0);

		drm_dbg(&xe->drm, "GT%u initialized with global interval=%u ms",
			 gt->info.id, xe_modparam.gpufreq_monitoring_interval_ms);
	}

	xe->gpufreqtracer_data = tracer_data;
	return 0;

err_free_tracer:
	drm_err(&xe->drm, "initialization failed, freeing tracer data");
	kfree(tracer_data);
	return ret;
}

/**
 * xe_gpufreqtracer_fini - Cleanup GPU frequency tracer for a device
 * @xe: The XE device
 *
 * Stops all monitoring and cleans up tracer resources.
 */
void xe_gpufreqtracer_fini(struct xe_device *xe)
{
	struct xe_gpufreqtracer_data *tracer_data = xe->gpufreqtracer_data;
	struct xe_gt *gt;
	u8 tile_id;

	if (!tracer_data) {
		drm_warn(&xe->drm, "no tracer data found, nothing to cleanup");
		return;
	}

	/* Stop all monitoring */
	for_each_gt(gt, xe, tile_id) {
		drm_dbg(&xe->drm, "stopping monitoring for GT%u", gt->info.id);
		xe_gpufreqtracer_stop_monitoring(gt);
	}

	kfree(tracer_data->gt_data);
	kfree(tracer_data);
	xe->gpufreqtracer_data = NULL;
}

/**
 * xe_gpufreqtracer_report_frequency_change - Report frequency change directly
 * @gt: The GT instance
 * @frequency_khz: The new frequency in KHz
 *
 * Reports a frequency change immediately through the tracepoint.
 */
void xe_gpufreqtracer_report_frequency_change(struct xe_gt *gt, u32 frequency_khz)
{
	drm_dbg(&gt_to_xe(gt)->drm, "direct frequency report for GT%u: %u KHz",
		 gt->info.id, frequency_khz);

	if (frequency_khz > 0) {
		trace_gpu_frequency(frequency_khz, gt->info.id);
		drm_dbg(&gt_to_xe(gt)->drm, "traced frequency change for GT%u", gt->info.id);
	}
}

/**
 * xe_gpufreqtracer_start_monitoring - Start periodic frequency monitoring
 * @gt: The GT instance
 *
 * Starts periodic sampling of GPU frequency for the specified GT using the global
 * monitoring interval from module parameters.
 *
 * Return: 0 on success, negative error code on failure
 */
int xe_gpufreqtracer_start_monitoring(struct xe_gt *gt)
{
	struct xe_gpufreqtracer_data *tracer_data = gt_to_xe(gt)->gpufreqtracer_data;
	struct xe_gpufreqtracer_gt_data *gt_data;

	if (!tracer_data) {
		drm_warn(&gt_to_xe(gt)->drm, "no tracer data for GT%u, not supported", gt->info.id);
		return -EOPNOTSUPP;
	}

	if (gt->info.id >= gt_to_xe(gt)->info.gt_count) {
		drm_err(&gt_to_xe(gt)->drm, "invalid GT ID %u, max supported is %u",
			gt->info.id, gt_to_xe(gt)->info.gt_count - 1);
		return -EINVAL;
	}

	gt_data = &tracer_data->gt_data[gt->info.id];

	if (atomic_read(&gt_data->monitoring_active)) {
		drm_warn(&gt_to_xe(gt)->drm, "monitoring already active for GT%u", gt->info.id);
		return -EALREADY;
	}

	atomic_set(&gt_data->monitoring_active, 1);
	atomic_set(&gt_data->last_frequency, 0);

	/* Start the timer using global interval */
	mod_timer(&gt_data->timer, jiffies +
		  msecs_to_jiffies(xe_modparam.gpufreq_monitoring_interval_ms));

	drm_dbg(&gt_to_xe(gt)->drm, "monitoring started for GT%u with interval %u ms",
		 gt->info.id, xe_modparam.gpufreq_monitoring_interval_ms);

	return 0;
}

/**
 * xe_gpufreqtracer_stop_monitoring - Stop periodic frequency monitoring
 * @gt: The GT instance
 *
 * Stops periodic sampling of GPU frequency for the specified GT.
 */
void xe_gpufreqtracer_stop_monitoring(struct xe_gt *gt)
{
	struct xe_gpufreqtracer_data *tracer_data = gt_to_xe(gt)->gpufreqtracer_data;
	struct xe_gpufreqtracer_gt_data *gt_data;

	if (!tracer_data || gt->info.id >= gt_to_xe(gt)->info.gt_count) {
		drm_err(&gt_to_xe(gt)->drm, "invalid tracer data or GT ID %u for stop request",
			gt->info.id);
		return;
	}

	gt_data = &tracer_data->gt_data[gt->info.id];

	if (!atomic_read(&gt_data->monitoring_active)) {
		drm_warn(&gt_to_xe(gt)->drm, "monitoring not active for GT%u, nothing to stop",
			 gt->info.id);
		return;
	}

	atomic_set(&gt_data->monitoring_active, 0);

	del_timer_sync(&gt_data->timer);
	cancel_work_sync(&gt_data->work);
}
