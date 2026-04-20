/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#ifndef _IOSS_I_H_
#define _IOSS_I_H_

#include <linux/stat.h>
#include <linux/module.h>
#include <linux/if_vlan.h>
#include <linux/platform_device.h>
#include <ipa_eth.h>
#include <linux/panic_notifier.h>

#if IPA_ETH_API_VER < 4
#error Unsupported IPA interface IPA_ETH_API_VER
#endif

#include "ioss.h"
#include "ioss_qos.h"

#if IS_ENABLED(CONFIG_QCOM_LLCC)
#define LLCC_ENABLE
#endif

#define DEFAULT_IPA_CONFIG "default"
#define DEFAULT_IOSS_TRAFFIC_TYPE IOSS_TRAFFIC_BE

enum ioss_statuses {
	IOSS_ST_ERROR,
	IOSS_ST_PROBED,
	IOSS_ST_IPA_RDY,
};

struct ioss_priv_data {
	struct ioss *ioss;
	struct ipa_eth_ready ipa_ready;
};

struct ioss_ch_priv {
	struct ipa_eth_client_pipe_info ipa_pi;
	const struct ipa_eth_dma_ch_config *ipa_ch_config;
};

struct ioss_iface_priv {
	struct ipa_eth_client ipa_ec;
	struct ipa_eth_intf_info ipa_ii;
	struct ipa_eth_config ipa_config;
};

extern struct ioss_mem_allocator ioss_default_alctr;

extern unsigned long ioss_ver;
extern unsigned long ioss_api_ver;

#ifdef LLCC_ENABLE
extern struct ioss_mem_allocator ioss_llcc_alctr;
#endif

int ioss_pci_start(struct ioss *ioss);
void ioss_pci_stop(struct ioss *ioss);
void ioss_pci_hijack_pm_ops(struct ioss_device *idev);
void ioss_pci_restore_pm_ops(struct ioss_device *idev);
int ioss_pci_enable_pc(struct ioss_device *idev);
int ioss_pci_disable_pc(struct ioss_device *idev);

int ioss_plat_start(struct ioss *ioss);
void ioss_plat_stop(struct ioss *ioss);

int ioss_of_parse(struct ioss_device *idev);

struct platform_device *ioss_find_dev_from_of_node(
		struct device_node *np);

int ioss_ipa_register(struct ioss_interface *iface);
int ioss_ipa_unregister(struct ioss_interface *iface);

int ioss_ipa_validate_channels(struct ioss_interface *iface);
void ioss_ipa_invalidate_channels(struct ioss_interface *iface);

#if IPA_ETH_API_VER > 4
int ioss_ipa_enable_pipes(struct ioss_interface *iface);
int ioss_ipa_disable_pipes(struct ioss_interface *iface);
#else
static inline int ioss_ipa_enable_pipes(struct ioss_interface *iface)
{
	return 0;
}
static inline int ioss_ipa_disable_pipes(struct ioss_interface *iface)
{
	return 0;
}
#endif

enum ipa_eth_client_type ioss_ipa_hal_get_ctype(struct ioss_device *idev);
int ioss_ipa_hal_fill_si(struct ioss_channel *ch);

int ioss_bus_register_driver(struct ioss_driver *idrv);
void ioss_bus_unregister_driver(struct ioss_driver *idrv);

struct ioss_device *ioss_bus_alloc_idev(struct ioss *ioss,
			struct device *dev);
void ioss_bus_free_idev(struct ioss_device *idev);
int ioss_bus_register_idev(struct ioss_device *idev);
void ioss_bus_unregister_idev(struct ioss_device *idev);

int ioss_bus_register_iface(struct ioss_interface *iface,
		struct net_device *net_dev);
void ioss_bus_unregister_iface(struct ioss_interface *iface);

int ioss_net_watch_device(struct ioss_device *idev);
int ioss_net_unwatch_device(struct ioss_device *idev);
int ioss_net_link_device(struct ioss_device *idev);

#if IS_ENABLED(CONFIG_IPC_LOGGING)
int ioss_log_init(void);
void ioss_log_deinit(void);
#else
static inline int ioss_log_init(void)
{
	return 0;
}
static inline void ioss_log_deinit(void)
{
}
#endif

int ioss_list_iter_action(struct list_head *head,
	int (*action)(struct list_head *node, void *arg),
	void (*revert)(struct list_head *node, void *arg),
	void *arg);

const char *ioss_if_state_name(enum ioss_interface_state state);
const char *ioss_ch_dir_name(enum ioss_channel_dir dir);
const char *ioss_traffic_name(enum ioss_traffic_type t);

#define if_st_s(iface) ioss_if_state_name(iface->state)

void ioss_iface_queue_refresh(struct ioss_interface *iface, bool flush);

int ioss_sysfs_add_idev(struct ioss_device* idev);
void ioss_sysfs_remove_idev(struct ioss_device* idev);
int ioss_sysfs_add_channel(struct ioss_channel* ch);
void ioss_sysfs_remove_channel(struct ioss_channel* ch);

int ioss_qos_reconfigure(struct ioss_device *idev);
int ioss_qos_enable(struct ioss_device *idev);
void ioss_qos_clear_cache(struct ioss_device *idev);
void ioss_qos_refresh(struct ioss_device *idev);
void ioss_qos_remove_channels(struct ioss_interface *iface);
void ioss_qos_init(struct ioss_device *idev);
int ioss_qos_add_idev(struct ioss_device *idev);
void ioss_qos_remove_idev(struct ioss_device *idev);

#endif /* _IOSS_I_H_ */
