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
 * @last_frequency: Last reported frequency to avoid duplicate reports
 * @monitoring_active: Whether monitoring is currently active
 */
struct xe_gpufreqtracer_gt_data {
	struct xe_gt *gt;
	struct timer_list timer;
	struct work_struct work;
	u32 last_frequency;
	bool monitoring_active;
};

/**
 * struct xe_gpufreqtracer_data - Per-device frequency tracer data
 * @xe: Reference to the XE device
 * @gt_data: Array of per-GT monitoring data
 * @workqueue: Dedicated workqueue for frequency sampling
 */
struct xe_gpufreqtracer_data {
	struct xe_device *xe;
	struct xe_gpufreqtracer_gt_data *gt_data;
	struct workqueue_struct *workqueue;
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

	if (!gt_data->monitoring_active) {
		pr_emerg("xe_gpufreqtracer: monitoring not active for GT%u, exiting", gt->info.id);
		return;
	}

	current_freq = xe_guc_pc_get_act_freq(pc) * 1000; /* Convert MHz to KHz */

	/* Only report if frequency has changed or this is the first sample */
	if (current_freq != gt_data->last_frequency) {
		pr_emerg("xe_gpufreqtracer: GT%u frequency changed, tracing %u KHz",
			 gt->info.id, current_freq);
		trace_gpu_frequency(current_freq, gt->info.id);
		gt_data->last_frequency = current_freq;
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
	struct xe_gpufreqtracer_data *tracer_data =
		gt_to_xe(gt_data->gt)->gpufreqtracer_data;

	if (gt_data->monitoring_active) {
		queue_work(tracer_data->workqueue, &gt_data->work);
		mod_timer(&gt_data->timer, jiffies +
			  msecs_to_jiffies(xe_modparam.gpufreq_monitoring_interval_ms));
	} else {
		pr_emerg("xe_gpufreqtracer: timer callback for GT%u but monitoring inactive",
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

	pr_emerg("xe_gpufreqtracer: initializing GPU frequency tracer for device");

	tracer_data = kzalloc(sizeof(*tracer_data), GFP_KERNEL);
	if (!tracer_data)
		return -ENOMEM;

	tracer_data->xe = xe;

	/* Create dedicated workqueue for frequency sampling */
	tracer_data->workqueue = alloc_workqueue("xe_gpufreq_tracer",
						 WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!tracer_data->workqueue) {
		pr_emerg("xe_gpufreqtracer: failed to create workqueue");
		ret = -ENOMEM;
		goto err_free_tracer;
	}

	/* Allocate GT data array - assume max 2 GTs for now */
	tracer_data->gt_data = kcalloc(2, sizeof(*tracer_data->gt_data), GFP_KERNEL);
	if (!tracer_data->gt_data) {
		ret = -ENOMEM;
		goto err_destroy_wq;
	}
	pr_emerg("xe_gpufreqtracer: allocated GT data array");

	/* Initialize per-GT data */
	for_each_gt(gt, xe, tile_id) {
		struct xe_gpufreqtracer_gt_data *gt_data =
			&tracer_data->gt_data[gt->info.id];

		pr_emerg("xe_gpufreqtracer: initializing GT%u (tile %u)", gt->info.id, tile_id);

		gt_data->gt = gt;
		gt_data->monitoring_active = false;
		gt_data->last_frequency = 0;

		INIT_WORK(&gt_data->work, xe_gpufreqtracer_sample_work);
		timer_setup(&gt_data->timer, xe_gpufreqtracer_timer_callback, 0);

		pr_emerg("xe_gpufreqtracer: GT%u initialized with global interval=%u ms",
			 gt->info.id, xe_modparam.gpufreq_monitoring_interval_ms);
	}

	xe->gpufreqtracer_data = tracer_data;
	pr_emerg("xe_gpufreqtracer: initialization completed successfully");
	return 0;

err_destroy_wq:
	pr_emerg("xe_gpufreqtracer: initialization failed, destroying workqueue");
	destroy_workqueue(tracer_data->workqueue);
err_free_tracer:
	pr_emerg("xe_gpufreqtracer: initialization failed, freeing tracer data");
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

	pr_emerg("xe_gpufreqtracer: finalizing GPU frequency tracer");

	if (!tracer_data) {
		pr_emerg("xe_gpufreqtracer: no tracer data found, nothing to cleanup");
		return;
	}

	pr_emerg("xe_gpufreqtracer: stopping all monitoring");
	/* Stop all monitoring */
	for_each_gt(gt, xe, tile_id) {
		pr_emerg("xe_gpufreqtracer: stopping monitoring for GT%u", gt->info.id);
		xe_gpufreqtracer_stop_monitoring(gt);
	}

	/* Flush and destroy workqueue */
	flush_workqueue(tracer_data->workqueue);
	destroy_workqueue(tracer_data->workqueue);

	kfree(tracer_data->gt_data);
	kfree(tracer_data);
	xe->gpufreqtracer_data = NULL;
	pr_emerg("xe_gpufreqtracer: finalization completed");
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
	pr_emerg("xe_gpufreqtracer: direct frequency report for GT%u: %u KHz",
		 gt->info.id, frequency_khz);

	if (frequency_khz > 0) {
		trace_gpu_frequency(frequency_khz, gt->info.id);
		pr_emerg("xe_gpufreqtracer: traced frequency change for GT%u", gt->info.id);
	} else {
		pr_emerg("xe_gpufreqtracer: skipping trace for GT%u, invalid frequency %u",
			 gt->info.id, frequency_khz);
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
		pr_emerg("xe_gpufreqtracer: no tracer data for GT%u, not supported", gt->info.id);
		return -EOPNOTSUPP;
	}

	if (gt->info.id >= 2) { /* Assume max 2 GTs */
		pr_emerg("xe_gpufreqtracer: invalid GT ID %u, max supported is 1", gt->info.id);
		return -EINVAL;
	}

	gt_data = &tracer_data->gt_data[gt->info.id];

	if (gt_data->monitoring_active) {
		pr_emerg("xe_gpufreqtracer: monitoring already active for GT%u", gt->info.id);
		return -EALREADY;
	}

	gt_data->monitoring_active = true;
	gt_data->last_frequency = 0;

	/* Start the timer using global interval */
	mod_timer(&gt_data->timer, jiffies +
		  msecs_to_jiffies(xe_modparam.gpufreq_monitoring_interval_ms));

	pr_emerg("xe_gpufreqtracer: monitoring started for GT%u with interval %u ms",
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

	if (!tracer_data || gt->info.id >= 2) {
		pr_emerg("xe_gpufreqtracer: invalid tracer data or GT ID %u for stop request",
			 gt->info.id);
		return;
	}

	gt_data = &tracer_data->gt_data[gt->info.id];

	if (!gt_data->monitoring_active) {
		pr_emerg("xe_gpufreqtracer: monitoring not active for GT%u, nothing to stop",
			 gt->info.id);
		return;
	}

	gt_data->monitoring_active = false;

	timer_delete(&gt_data->timer);
	cancel_work_sync(&gt_data->work);

	pr_emerg("xe_gpufreqtracer: monitoring stopped for GT%u", gt->info.id);
}
