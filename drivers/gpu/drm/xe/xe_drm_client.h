/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_DRM_CLIENT_H_
#define _XE_DRM_CLIENT_H_

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct drm_file;
struct drm_printer;
struct xe_bo;

struct xe_drm_client {
	struct kref kref;
	unsigned int id;
#ifdef CONFIG_PROC_FS
	/**
	 * @bos_lock: lock protecting @bos_list
	 */
	spinlock_t bos_lock;
	/**
	 * @bos_list: list of bos created by this client
	 *
	 * Protected by @bos_lock.
	 */
	struct list_head bos_list;
#endif
};

/**
 * This is a per process/user id structure for a xe device
 * client. It is allocated when a new process/app opens the
 * xe device and destroyed when the last xe file for this
 * process is destroyed
 */
struct xe_user {
	struct kref kref;
	struct xe_device *xe;
	/**
	 * @filelist_lock: lock protecting the filelist list
	 */
	spinlock_t filelist_lock;
	/**
	 * @filelist: list of xe files belonging to this process
	 */
	struct list_head filelist;
	/**
	 * @entry: entry into the xe.work_period.user_list list
	 */
	struct list_head entry;
	/**
	 * @work: work to emit the gpu work period event for this xe user
	 */
	struct work_struct work;
	/**
	 * @uid: user id for this process/app
	 *
	 * In android each app has its own user id. So we use uid to identify
	 * an app that is using the gpu
	 */
	u32 uid;
	/**
	 * @active_duration_ns: sum total of xe_file.active_duration_ns for all
	 * xe files belonging to this xe user
	 */
	u64 active_duration_ns;
	/**
	 * @last_timestamp_ns: timestamp in ns when we last emitted event for
	 * this xe user
	 */
	u64 last_timestamp_ns;
};

	static inline struct xe_drm_client *
xe_drm_client_get(struct xe_drm_client *client)
{
	kref_get(&client->kref);
	return client;
}

void __xe_drm_client_free(struct kref *kref);

static inline void xe_drm_client_put(struct xe_drm_client *client)
{
	kref_put(&client->kref, __xe_drm_client_free);
}

struct xe_drm_client *xe_drm_client_alloc(void);
static inline struct xe_drm_client *
xe_drm_client_get(struct xe_drm_client *client);
static inline void xe_drm_client_put(struct xe_drm_client *client);
#ifdef CONFIG_PROC_FS
void xe_drm_client_fdinfo(struct drm_printer *p, struct drm_file *file);
void xe_drm_client_add_bo(struct xe_drm_client *client,
			  struct xe_bo *bo);
void xe_drm_client_remove_bo(struct xe_bo *bo);
#else
static inline void xe_drm_client_add_bo(struct xe_drm_client *client,
					struct xe_bo *bo)
{
}

static inline void xe_drm_client_remove_bo(struct xe_bo *bo)
{
}
#endif

struct xe_user *xe_user_alloc(const unsigned int uid);

static inline struct xe_user *
xe_user_get(struct xe_user *user)
{
	kref_get(&user->kref);
	return user;
}

void __xe_user_free(struct kref *kref);

static inline void xe_user_put(struct xe_user *user)
{
	kref_put(&user->kref, __xe_user_free);
}
#endif
