/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023, 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include "ioss_i.h"

/* IPC Logging */
#define CONFIG_IOSS_DEBUG

#ifdef CONFIG_IOSS_DEBUG
#define IOSS_LOG_DEBUG_DEFAULT true
#else
#define IOSS_LOG_DEBUG_DEFAULT false
#endif

#define IOSS_IPC_LOG_PAGES_DEFAULT 128

static bool log_debug = IOSS_LOG_DEBUG_DEFAULT;
module_param(log_debug, bool, 0644);
MODULE_PARM_DESC(log_debug, "Log debug messages in IPC log");

static int log_pages = IOSS_IPC_LOG_PAGES_DEFAULT;
module_param(log_pages, int, 0444);
MODULE_PARM_DESC(log_pages, "Number of IPC log pages");

static void *ioss_ipclog_buf_norm;
static void *ioss_ipclog_buf_prio;
static void *ioss_qos_ipclog_buf;

void *ioss_get_ipclog_buf_norm(void)
{
	return ioss_ipclog_buf_norm;
}
EXPORT_SYMBOL(ioss_get_ipclog_buf_norm);

void *ioss_get_ipclog_buf_debug(void)
{
	return log_debug ? ioss_ipclog_buf_norm : NULL;
}
EXPORT_SYMBOL(ioss_get_ipclog_buf_debug);

void *ioss_get_ipclog_buf_prio(void)
{
	return ioss_ipclog_buf_prio;
}
EXPORT_SYMBOL(ioss_get_ipclog_buf_prio);

void *ioss_qos_get_ipclog_buf(void)
{
	return ioss_qos_ipclog_buf;
}
EXPORT_SYMBOL(ioss_qos_get_ipclog_buf);

#define IOSS_IPCLOG_NAME IOSS_SUBSYS
#define IOSS_IPCLOG_PRIO_NAME (IOSS_SUBSYS "_prio")
#define IOSS_QOS_IPCLOG_NAME IOSS_QOS_SUBSYS

#if IS_ENABLED(CONFIG_IPC_LOGGING)
int ioss_log_init(void)
{
	if (ioss_ipclog_buf_norm)
		return 0;

	ioss_ipclog_buf_prio =
		ipc_log_context_create(log_pages, IOSS_IPCLOG_PRIO_NAME, 0);
	if (!ioss_ipclog_buf_prio) {
		pr_err("IOSS: Failed to create IPC log context (prio)\n");
		return -EFAULT;
	}

	ioss_ipclog_buf_norm =
		ipc_log_context_create(log_pages, IOSS_IPCLOG_NAME, 0);
	if (!ioss_ipclog_buf_norm) {
		pr_err("IOSS: Failed to create IPC log context\n");

		ipc_log_context_destroy(ioss_ipclog_buf_prio);
		ioss_ipclog_buf_prio = NULL;

		return -EFAULT;
	}

	ioss_qos_ipclog_buf =
		ipc_log_context_create(log_pages, IOSS_QOS_IPCLOG_NAME, 0);
	if (!ioss_qos_ipclog_buf) {
		pr_err("IOSS QOS: Failed to create IPC log context\n");
		return -EFAULT;
	}

	ioss_log_cfg(NULL,
		"IOSS version 0x%lx, API version %lu", ioss_ver, ioss_api_ver);

	return 0;
}

void ioss_log_deinit(void)
{
	if (ioss_ipclog_buf_norm) {
		ipc_log_context_destroy(ioss_ipclog_buf_norm);
		ioss_ipclog_buf_norm = NULL;
	}

	if (ioss_ipclog_buf_prio) {
		ipc_log_context_destroy(ioss_ipclog_buf_prio);
		ioss_ipclog_buf_prio = NULL;
	}

	if (ioss_qos_ipclog_buf) {
		ipc_log_context_destroy(ioss_qos_ipclog_buf);
		ioss_qos_ipclog_buf = NULL;
	}
}
#endif

/* List operation helpers */

static int __ioss_list_iter_action_recurse(
	struct list_head *node, struct list_head *head,
	int (*action)(struct list_head *node, void *arg),
	void (*revert)(struct list_head *node, void *arg),
	void *arg)
{
	int rc;

	if (node == head)
		return 0;

	rc = action(node, arg);
	if (rc)
		return rc;

	rc = __ioss_list_iter_action_recurse(node->next, head, action, revert, arg);
	if (rc && revert)
		revert(node, arg);

	return rc;
}

int ioss_list_iter_action(struct list_head *head,
	int (*action)(struct list_head *node, void *arg),
	void (*revert)(struct list_head *node, void *arg),
	void *arg)
{
	return __ioss_list_iter_action_recurse(
			head->next, head, action, revert, arg);
}

/* Enum string conversions */

static const char * const ioss_if_states[IOSS_IF_ST_MAX] = {
	[IOSS_IF_ST_OFFLINE] = "`offline`",
	[IOSS_IF_ST_ONLINE] = "`online`",
	[IOSS_IF_ST_ERROR] = "`error`",
	[IOSS_IF_ST_RECOVERY] = "`recovery`",
};

const char *ioss_if_state_name(enum ioss_interface_state state)
{
	if (state < IOSS_IF_ST_MAX && ioss_if_states[state])
		return ioss_if_states[state];

	return "<unknown>";
}

static const char * const ioss_ch_dirs[IOSS_CH_DIR_MAX] = {
	[IOSS_CH_DIR_RX] = "RX",
	[IOSS_CH_DIR_TX] = "TX",
};

const char *ioss_ch_dir_name(enum ioss_channel_dir dir)
{
	if (dir < IOSS_CH_DIR_MAX && ioss_ch_dirs[dir])
		return ioss_ch_dirs[dir];

	return "<unknown>";
}

static const char * const traffic_type_map[IOSS_TRAFFIC_TYPE_MAX] = {
	[IOSS_TRAFFIC_BE] = "best-effort",
	[IOSS_TRAFFIC_BE_TAGGED] = "best-effort-tagged",
	[IOSS_TRAFFIC_LL] = "low-latency",
	[IOSS_TRAFFIC_QOS] = "qos",
};

const char *ioss_traffic_name(enum ioss_traffic_type t)
{
	if (t < ARRAY_SIZE(traffic_type_map))
		return traffic_type_map[t];

	return "<unknown>";
}
