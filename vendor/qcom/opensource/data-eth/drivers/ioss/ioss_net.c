/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/workqueue.h>
#include <linux/rtnetlink.h>
#include <linux/bitops.h>

#include "ioss_i.h"

#define IOSS_NET_DEVICE_MAX_EVENTS (NETDEV_CHANGE_TX_QUEUE_LEN + 1)

static const char * const
		ioss_netdev_event_names[IOSS_NET_DEVICE_MAX_EVENTS] = {
	[NETDEV_REGISTER] = "REGISTER",
	[NETDEV_UNREGISTER] = "UNREGISTER",
	[NETDEV_CHANGENAME] = "CHANGE_NAME",
	[NETDEV_PRE_UP] = "PRE_UP",
	[NETDEV_UP] = "UP",
	[NETDEV_GOING_DOWN] = "GOING_DOWN",
	[NETDEV_DOWN] = "DOWN",
	[NETDEV_CHANGE] = "CHANGE",
	[NETDEV_CHANGELOWERSTATE] = "CHANGE_LOWER_STATE",
	[NETDEV_PRECHANGEUPPER] = "PRE_CHANGE_UPPER",
	[NETDEV_CHANGEUPPER] = "CHANGE_UPPER",
	[NETDEV_JOIN] = "JOIN",
};

const char *ioss_netdev_event_name(unsigned long event)
{
	const char *name = "<unknown>";

	if (event < IOSS_NET_DEVICE_MAX_EVENTS &&
				ioss_netdev_event_names[event])
		name = ioss_netdev_event_names[event];

	return name;
}

static bool net_device_belongs_to(struct net_device *net_dev,
	struct ioss_device *idev)
{
	return net_dev->dev.parent == idev->dev.parent;
}

void ioss_iface_queue_refresh(struct ioss_interface *iface, bool flush)
{
	struct ioss_device *idev = ioss_iface_dev(iface);

	queue_work(idev->root->wq, &iface->refresh);

	if (flush)
		flush_work(&iface->refresh);
}

static void __netdev_get_stats64(struct net_device *net_dev,
		struct rtnl_link_stats64 *stats)
{
	struct ioss_interface *iface = NULL;
	const struct net_device_ops *real_ops = NULL;

	if (!net_dev)
		return;

	iface = ioss_netdev_to_iface(net_dev);
	if (!iface)
		return;

	real_ops = iface->netdev_ops_real;
	/* Retrieve stats for direct software path. Modeled after kernel API
	 * dev_get_stats().
	 */
	if (real_ops->ndo_get_stats64)
		real_ops->ndo_get_stats64(net_dev, stats);
	else if (real_ops->ndo_get_stats)
		netdev_stats_to_stats64(stats,
					real_ops->ndo_get_stats(net_dev));
	else
		netdev_stats_to_stats64(stats, &net_dev->stats);

	/* Add exception path stats */
	stats->rx_packets += iface->exception_stats.rx_packets;
	stats->rx_bytes += iface->exception_stats.rx_bytes;
}

static void __hijack_netdev_ops(struct ioss_interface *iface)
{
	struct net_device *net_dev = NULL;

	if(!iface)
		return;

	net_dev = ioss_iface_to_netdev(iface);
	if (!net_dev)
		return;

	iface->netdev_ops_real = net_dev->netdev_ops;
	iface->netdev_ops = *iface->netdev_ops_real;

	iface->netdev_ops.ndo_get_stats64 = __netdev_get_stats64;

	net_dev->netdev_ops = &iface->netdev_ops;
}

static void __restore_netdev_ops(struct ioss_interface *iface)
{
	struct net_device *net_dev = NULL;

	if (!iface)
		return ;

	net_dev = ioss_iface_to_netdev(iface);
	if (!net_dev)
		return ;

	net_dev->netdev_ops = iface->netdev_ops_real;
}

static void ioss_net_event_register(struct ioss_interface *iface,
		unsigned long event, void *ptr)
{
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_log(idev, "Register event for %s", net_dev->name);

	idev->wol_activated = false;

	memset(&iface->exception_stats, 0, sizeof(iface->exception_stats));

	if (ioss_bus_register_iface(iface, net_dev)) {
		ioss_dev_err(idev,
			"Failed to register interface %s", net_dev->name);
		iface->state = IOSS_IF_ST_ERROR;
		return;
	}

	__hijack_netdev_ops(iface);


	ioss_iface_queue_refresh(iface, false);
}

static void ioss_net_event_unregister(struct ioss_interface *iface,
		unsigned long event, void *ptr)
{
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_log(idev, "Unregister event for %s", net_dev->name);

	__restore_netdev_ops(iface);
	ioss_bus_unregister_iface(iface);

	ioss_iface_queue_refresh(iface, false);
}

static void ioss_net_event_generic(struct ioss_interface *iface,
		unsigned long event, void *ptr)
{
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_log(idev, "Generic event for %s", net_dev->name);

	ioss_iface_queue_refresh(iface, false);
}

static void ioss_net_event_up(struct ioss_interface *iface,
		unsigned long event, void *ptr)
{
	struct ioss_device *idev = ioss_iface_dev(iface);
	struct ethtool_wolinfo *wol = &idev->wol;
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);
	const struct ethtool_ops *ops = net_dev->ethtool_ops;

	ioss_dev_log(idev, "UP event for %s", net_dev->name);

	if (wol->wolopts && !idev->wol_activated) {
		if (!ops->set_wol || ops->set_wol(net_dev, wol))
			ioss_dev_err(idev, "Failed to set Wake-on-LAN");
		else
			idev->wol_activated = true;
	}

	ioss_iface_queue_refresh(iface, false);
}

static void ioss_net_event_going_down(struct ioss_interface *iface,
		unsigned long event, void *ptr)
{
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_log(idev, "GOING_DOWN event for %s", net_dev->name);

	ioss_qos_clear_cache(idev);

	ioss_iface_queue_refresh(iface, false);
}

typedef void (*ioss_net_event_handler)(struct ioss_interface *iface,
		unsigned long event, void *ptr);

/* Event handlers for netdevice events from real interface */
static ioss_net_event_handler
		ioss_net_event_handlers[IOSS_NET_DEVICE_MAX_EVENTS] = {
	[NETDEV_REGISTER] = ioss_net_event_register,
	[NETDEV_UNREGISTER] = ioss_net_event_unregister,
	[NETDEV_UP] = ioss_net_event_up,
	[NETDEV_GOING_DOWN] = ioss_net_event_going_down,
	[NETDEV_CHANGE] = ioss_net_event_generic,
};

static int ioss_net_device_event(struct notifier_block *nb,
	unsigned long event, void *ptr)
{
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);
	struct ioss_interface *iface = ioss_nb_to_iface(nb);
	struct ioss_device *idev = ioss_iface_dev(iface);

	if (strcmp(net_dev->name, idev->net_dev->name) != 0)
		return NOTIFY_OK;

	if (!net_device_belongs_to(net_dev, idev))
		return NOTIFY_OK;

	ioss_dev_dbg(idev, "Received netdev event %s (%lu) for %s",
			ioss_netdev_event_name(event), event, net_dev->name);

	if (event < IOSS_NET_DEVICE_MAX_EVENTS) {
		ioss_net_event_handler handler = ioss_net_event_handlers[event];

		if (handler)
			handler(iface, event, ptr);

	} else {
		ioss_dev_err(idev,
			"Netdev event number %lu out of bounds", event);
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

#ifdef LLCC_ENABLE
static int ioss_net_select_llcc_config(struct ioss_channel *ch)
{
	u32 ring_size;
	size_t mem_size;
	struct ioss_device *idev = ioss_ch_dev(ch);

	mem_size = ioss_llcc_alctr.get();

	ioss_dev_cfg(idev, "Received %lu bytes for LLCC", mem_size);

	if (!mem_size)
		return -ENOMEM;

	ring_size = mem_size / ch->config.buff_size;

	ioss_dev_cfg(idev,
		"Calculated ring size of %u with LLCC buffer size of %u bytes",
		ring_size, ch->config.buff_size);

	ch->config.ring_size = ring_size;
	ch->config.buff_alctr = &ioss_llcc_alctr;

	return 0;
}
#endif

static void ioss_net_select_channel_config(struct ioss_channel *ch)
{
#ifdef LLCC_ENABLE
	struct ioss_device *idev = ioss_ch_dev(ch);
	u32 link_speed = ch->iface->link_speed;
	u32 line_rate_for_llcc = idev->root->line_rate_for_llcc;
#endif

	ch->config = ch->default_config;

#ifdef LLCC_ENABLE
	if (idev->llcc_enabled && ch->direction == IOSS_CH_DIR_TX && link_speed >= line_rate_for_llcc)
		ioss_net_select_llcc_config(ch);
#endif
}

static void ioss_net_deselect_channel_config(struct ioss_channel *ch)
{
#ifdef LLCC_ENABLE
	if (ch->config.buff_alctr == &ioss_llcc_alctr)
		ioss_llcc_alctr.put();
#endif
	ch->config = ch->default_config;
}

static int __ioss_net_alloc_channel(struct ioss_channel *ch)
{
	int rc;
	struct ioss_device *idev = ioss_ch_dev(ch);

	ioss_dev_dbg(idev,
			"Allocating channel for %s", idev->net_dev->name);

	ioss_net_select_channel_config(ch);

	rc = ioss_dev_op(idev, request_channel, ch);
	if (rc) {
		ioss_dev_err(idev,
			"Failed to alloc channel for %s", idev->net_dev->name);
		ioss_net_deselect_channel_config(ch);
		return rc;
	}

	ch->allocated = true;

	ioss_dev_log(idev, "Allocated channel %d for interface %s",
			ch->id, idev->net_dev->name);

	rc = ioss_sysfs_add_channel(ch);
	if (rc) {
		ioss_dev_err(idev, "Failed to create sysfs nodes");
		goto err_sysfs;
	}

	return 0;

err_sysfs:
	ioss_dev_op(idev, release_channel, ch);
	ch->allocated = false;
	ioss_net_deselect_channel_config(ch);
	return rc;
}

static int __ioss_net_free_channel(struct ioss_channel *ch)
{
	int rc;
	int id = ch->id;
	struct ioss_device *idev = ioss_ch_dev(ch);

	ioss_sysfs_remove_channel(ch);
	ioss_dev_dbg(idev,
		"Releasing channel %d for %s", id, idev->net_dev->name);

	rc = ioss_dev_op(idev, release_channel, ch);
	if (rc) {
		ioss_dev_err(idev, "Failed to release channel %d", id);
		return rc;
	}

	ch->allocated = false;

	ioss_net_deselect_channel_config(ch);

	ioss_dev_log(idev,
		"Released channel %d on interface %s", id, idev->net_dev->name);

	return 0;
}

static int __alloc_channel_action(struct list_head *node, void *arg)
{
	return __ioss_net_alloc_channel(
			container_of(node, struct ioss_channel, node));
}

static void __alloc_channel_revert(struct list_head *node, void *arg)
{
	(void) __ioss_net_free_channel(
			container_of(node, struct ioss_channel, node));
}

static int ioss_net_alloc_channels(struct ioss_interface *iface)
{
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev, "Allocating channels for %s", idev->net_dev->name);

	return ioss_list_iter_action(&iface->valid_channels,
			__alloc_channel_action, __alloc_channel_revert, NULL);
}

static int ioss_net_free_channels(struct ioss_interface *iface)
{
	int rc = 0;
	struct ioss_channel *ch;
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev, "Releasing all channels for %s", idev->net_dev->name);

	ioss_for_each_channel(ch, iface)
		rc |= __ioss_net_free_channel(ch);

	return rc;
}

static int __ioss_net_enable_channel(struct ioss_channel *ch)
{
	int rc;
	struct ioss_device *idev = ioss_ch_dev(ch);

	ioss_dev_dbg(idev,
			"Enabling channel for %s", idev->net_dev->name);

	rc = ioss_dev_op(idev, enable_channel, ch);
	if (rc) {
		ioss_dev_err(idev,
			"Failed to enable channel %d", ch->id);
		ioss_dev_op(ioss_ch_dev(ch), release_channel, ch);
		return rc;
	}

	ch->enabled = true;

	ioss_dev_log(idev,
		"Enabled channel %d for interface %s", ch->id, idev->net_dev->name);

	return 0;
}

static int __ioss_net_disable_channel(struct ioss_channel *ch)
{
	int rc;
	int id = ch->id;
	struct ioss_device *idev = ioss_ch_dev(ch);

	ioss_dev_dbg(idev,
		"Disabling channel %d for %s", id, idev->net_dev->name);

	rc = ioss_dev_op(idev, disable_channel, ch);
	if (rc) {
		ioss_dev_err(idev, "Failed to disable channel %d", id);
		return rc;
	}

	ch->enabled = false;

	ioss_dev_log(idev,
		"Disabled channel %d on interface %s", id, idev->net_dev->name);

	return 0;
}

static int __enable_channel_action(struct list_head *node, void *arg)
{
	enum ioss_channel_dir dir = (enum ioss_channel_dir)arg;
	struct ioss_channel *ch = container_of(node, struct ioss_channel, node);

	return (ch->direction == dir) ? __ioss_net_enable_channel(ch) : 0;
}

static void __enable_channel_revert(struct list_head *node, void *arg)
{
	enum ioss_channel_dir dir = (enum ioss_channel_dir)arg;
	struct ioss_channel *ch = container_of(node, struct ioss_channel, node);

	if (ch->direction == dir)
		(void) __ioss_net_disable_channel(ch);
}

static int ioss_net_enable_channels(struct ioss_interface *iface, enum ioss_channel_dir dir)
{
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev, "Enabling channels for %s", idev->net_dev->name);

	return ioss_list_iter_action(&iface->valid_channels,
			__enable_channel_action, __enable_channel_revert, (void *)dir);
}

static int ioss_net_disable_channels(struct ioss_interface *iface, enum ioss_channel_dir dir)
{
	int rc = 0;
	struct ioss_channel *ch;
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev, "Disabling all channels for %s", idev->net_dev->name);

	ioss_for_each_channel(ch, iface) {
		if (ch->direction == dir)
			rc |= __ioss_net_disable_channel(ch);
	}
	return rc;
}

static int __ioss_net_setup_event(struct ioss_channel *ch)
{
	int rc;
	struct ioss_device *idev = ioss_ch_dev(ch);

	ioss_dev_dbg(idev,
			"Setting up event for channel %d", ch->id);

	rc = ioss_dev_op(idev, request_event, ch);
	if (rc) {
		ioss_dev_err(idev,
			"Failed to alloc event for channel %d", ch->id);
		goto err_alloc;
	}

	ch->event.allocated = true;

	rc = ioss_dev_op(idev, enable_event, ch);
	if (rc) {
		ioss_dev_err(idev,
			"Failed to enable event for channel %d", ch->id);
		goto err_enable;
	}

	ch->event.enabled = true;

	ioss_dev_log(idev,
		"Setup event for channel %d", ch->id);

	return 0;

err_enable:
	ioss_dev_op(idev, release_event, ch);

err_alloc:
	return rc;
}

static int __ioss_net_teardown_event(struct ioss_channel *ch)
{
	int rc1, rc2;
	struct ioss_device *idev = ioss_ch_dev(ch);

	ioss_dev_dbg(idev,
			"Tearing down event for channel %d", ch->id);

	rc1 = ioss_dev_op(idev, disable_event, ch);
	if (rc1)
		ioss_dev_err(idev,
			"Failed to disable event for channel %d", ch->id);
	else
		ch->event.enabled = false;

	rc2 = ioss_dev_op(idev, release_event, ch);
	if (rc2)
		ioss_dev_err(idev,
			"Failed to dealloc event for channel %d", ch->id);
	else
		ch->event.allocated = false;

	if (rc1 || rc2)
		return -EFAULT;

	ioss_dev_log(idev,
		"Teared down event for channel %d", ch->id);

	return 0;
}

static int __setup_event_action(struct list_head *node, void *arg)
{
	enum ioss_channel_dir dir = (enum ioss_channel_dir)arg;
	struct ioss_channel *ch = container_of(node, struct ioss_channel, node);

	return (ch->direction == dir) ? __ioss_net_setup_event(ch) : 0;
}

static void __setup_event_revert(struct list_head *node, void *arg)
{
	enum ioss_channel_dir dir = (enum ioss_channel_dir)arg;
	struct ioss_channel *ch = container_of(node, struct ioss_channel, node);

	if (ch->direction == dir)
		(void) __ioss_net_teardown_event(ch);
}

static int ioss_net_setup_events(struct ioss_interface *iface, enum ioss_channel_dir dir)
{
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev, "Setting up all device events");

	return ioss_list_iter_action(&iface->valid_channels,
		__setup_event_action, __setup_event_revert, (void *)dir);
}

static int ioss_net_teardown_events(struct ioss_interface *iface, enum ioss_channel_dir dir)
{
	int rc = 0;
	struct ioss_channel *ch;
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev, "Tearing down all device events");

	ioss_for_each_channel(ch, iface) {
		if (ch->direction == dir)
			rc |= __ioss_net_teardown_event(ch);
	}

	return rc;
}

static void ioss_iface_set_online(struct ioss_interface *iface)
{
	int rc;
	struct ioss_device *idev = ioss_iface_dev(iface);

	if (iface->state == IOSS_IF_ST_ERROR) {
		iface->state = IOSS_IF_ST_OFFLINE;
		ioss_dev_dbg(idev,
			"Interface %s state changed to %s",
			idev->net_dev->name, if_st_s(iface));
	}

	if (iface->state != IOSS_IF_ST_OFFLINE) {
		ioss_dev_dbg(idev,
			"Interface %s state is %s; required is %s",
			idev->net_dev->name, if_st_s(iface),
			ioss_if_state_name(IOSS_IF_ST_OFFLINE));
		return;
	}

	ioss_dev_log(idev, "Bringing up %s", idev->net_dev->name);

	rc = ioss_ipa_validate_channels(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to validate channels");
		goto err_validate_channels;
	}

	ioss_qos_refresh(idev);

	rc = ioss_net_alloc_channels(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to allocate channels");
		goto err_alloc_channels;
	}

	rc = ioss_ipa_register(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to register with IPA");
		goto err_ipa_register;
	}

	rc = ioss_net_setup_events(iface, IOSS_CH_DIR_TX);
	if (rc) {
		ioss_dev_err(idev, "Failed to setup events for Tx");
		goto err_setup_events_tx;
	}

	rc = ioss_net_enable_channels(iface, IOSS_CH_DIR_TX);
	if (rc) {
		ioss_dev_err(idev, "Failed to enable Tx channels");
		goto err_enable_channels_tx;
	}

	rc = ioss_ipa_enable_pipes(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to enable IPA pipes");
		goto err_ipa_enable_pipes;
	}

	rc = ioss_net_setup_events(iface, IOSS_CH_DIR_RX);
	if (rc) {
		ioss_dev_err(idev, "Failed to setup events for Rx");
		goto err_setup_events_rx;
	}

	rc = ioss_net_enable_channels(iface, IOSS_CH_DIR_RX);
	if (rc) {
		ioss_dev_err(idev, "Failed to enable Rx channels");
		goto err_enable_channels_rx;
	}

	if(iface->is_pci_device){
		rc = ioss_pci_disable_pc(idev);
		if (rc) {
			ioss_dev_err(idev, "Failed to disable PCI power collapse");
			goto err_disable_pc;
		}
	}
	iface->state = IOSS_IF_ST_ONLINE;

	ioss_qos_enable(idev);

	ioss_dev_log(idev, "Brought up %s successfully", idev->net_dev->name);

	return;
err_disable_pc:
	ioss_net_disable_channels(iface, IOSS_CH_DIR_RX);
err_enable_channels_rx:
	ioss_net_teardown_events(iface, IOSS_CH_DIR_RX);
err_setup_events_rx:
	ioss_ipa_disable_pipes(iface);
err_ipa_enable_pipes:
	ioss_net_disable_channels(iface, IOSS_CH_DIR_TX);
err_enable_channels_tx:
	ioss_net_teardown_events(iface, IOSS_CH_DIR_TX);
err_setup_events_tx:
	ioss_ipa_unregister(iface);
err_ipa_register:
	ioss_net_free_channels(iface);
err_alloc_channels:
	ioss_ipa_invalidate_channels(iface);
err_validate_channels:
	iface->state = IOSS_IF_ST_ERROR;

	return;
}

static void ioss_iface_set_offline(struct ioss_interface *iface)
{
	int rc;
	struct ioss_device *idev = ioss_iface_dev(iface);

	if (iface->state != IOSS_IF_ST_ONLINE) {
		ioss_dev_dbg(idev,
			"Interface %s state is %s; required is %s",
			idev->net_dev->name, if_st_s(iface),
			ioss_if_state_name(IOSS_IF_ST_ONLINE));
		return;
	}

	ioss_dev_log(idev, "Bringing down %s", idev->net_dev->name);

	iface->state = IOSS_IF_ST_OFFLINE;

	if(iface->is_pci_device){
		rc = ioss_pci_enable_pc(idev);
		if (rc) {
			ioss_dev_err(idev, "Failed to enable PCI power collapse");
			iface->state = IOSS_IF_ST_ERROR;
		}
	}

	rc = ioss_net_disable_channels(iface, IOSS_CH_DIR_RX);
	if (rc) {
		ioss_dev_err(idev, "Failed to disable channels for Rx");
		iface->state = IOSS_IF_ST_ERROR;
	}

	rc = ioss_net_teardown_events(iface, IOSS_CH_DIR_RX);
	if (rc) {
		ioss_dev_err(idev, "Failed to teardown events for Rx");
		iface->state = IOSS_IF_ST_ERROR;
	}

	rc = ioss_ipa_disable_pipes(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to disable IPA pipes");
		iface->state = IOSS_IF_ST_ERROR;
	}

	rc = ioss_net_disable_channels(iface, IOSS_CH_DIR_TX);
	if (rc) {
		ioss_dev_err(idev, "Failed to disable channels for Tx");
		iface->state = IOSS_IF_ST_ERROR;
	}

	rc = ioss_net_teardown_events(iface, IOSS_CH_DIR_TX);
	if (rc) {
		ioss_dev_err(idev, "Failed to teardown events for Tx");
		iface->state = IOSS_IF_ST_ERROR;
	}

	rc = ioss_ipa_unregister(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to unregister with IPA");
		iface->state = IOSS_IF_ST_ERROR;
	}

	rc = ioss_net_free_channels(iface);
	if (rc) {
		ioss_dev_err(idev, "Failed to release channels");
		iface->state = IOSS_IF_ST_ERROR;
	}

	ioss_ipa_invalidate_channels(iface);

	ioss_qos_remove_channels(iface);

	ioss_dev_log(idev, "Brought down %s successsfully", idev->net_dev->name);
}

static u32 __fetch_ethtool_link_speed(struct net_device *net_dev)
{
	int rc;
	struct ethtool_link_ksettings link_ksettings;

	rtnl_lock();
	rc = __ethtool_get_link_ksettings(net_dev, &link_ksettings);
	rtnl_unlock();

	if (!rc)
		return link_ksettings.base.speed;
	else
		return 0;
}

static void ioss_refresh_work(struct work_struct *work)
{
	struct ioss_interface *iface = ioss_refresh_work_to_iface(work);
	struct net_device *net_dev = ioss_iface_to_netdev(iface);
	struct ioss_device *idev = ioss_iface_dev(iface);
	struct device *dev = &idev->dev;

	if (!net_dev)
		return;

	ioss_dev_dbg(idev, "Refreshing interface %s", idev->net_dev->name);

	iface->link_speed = __fetch_ethtool_link_speed(net_dev);

	if (netif_running(net_dev) && netif_carrier_ok(net_dev)
	    && !(dev->offline) && !idev->unbinding)
		ioss_iface_set_online(iface);
	else
		ioss_iface_set_offline(iface);

	ioss_dev_log(idev,
		"Interface %s state is %s", idev->net_dev->name, if_st_s(iface));
}

int ioss_net_watch_device(struct ioss_device *idev)
{
	int rc = 0;
	struct ioss_interface *iface = &idev->interface;

	idev->unbinding = false;

	INIT_WORK(&iface->refresh, ioss_refresh_work);
	iface->net_dev_nb.notifier_call = ioss_net_device_event;

	ioss_dev_log(idev, "Watching interface %s", idev->net_dev->name);

	rc = register_netdevice_notifier(&iface->net_dev_nb);
	if (rc) {
		ioss_dev_err(idev,
			"Failed to register netdev notifier for %s",
			idev->net_dev->name);
		goto err_register;
	}

	return 0;

err_register:
	(void) unregister_netdevice_notifier(&iface->net_dev_nb);

	flush_work(&iface->refresh);
	return rc;
}

int ioss_net_unwatch_device(struct ioss_device *idev)
{
	int rc;
	struct ioss_interface *iface = &idev->interface;

	ioss_dev_log(idev, "Unwatching interface %s", idev->net_dev->name);

	idev->unbinding = true;
	ioss_iface_queue_refresh(iface, true);

	rc = unregister_netdevice_notifier(&iface->net_dev_nb);
	if (rc)
		ioss_dev_err(idev,
			"Failed to unregister netdev notifier for %s",
			idev->net_dev->name);

	flush_work(&iface->refresh);

	return rc;
}

int ioss_net_link_device(struct ioss_device *idev)
{
	struct net *net;
	struct net_device *net_dev;

	if (idev->net_dev)
		return 0;

	rtnl_lock();
	for_each_net(net) {
		for_each_netdev(net, net_dev) {
			ioss_dev_log(idev, "%s: netdev=%s, dev=%s",
						__func__,
						net_dev->name,
						dev_name(&net_dev->dev));
			if(net_dev->dev.parent) {
			ioss_dev_log(idev, "%s: net_dev->dev.parent = %s, idev->dev.parent =%s",
						__func__,
						dev_name(net_dev->dev.parent),
						dev_name(idev->dev.parent));
				}
			else
				ioss_dev_log(idev, "%s: netdev =%s, dev=%s has parent null",
							__func__,
							net_dev->name,
							dev_name(&net_dev->dev));
			if (net_dev->dev.parent == idev->dev.parent) {
				idev->net_dev = net_dev;
				break;
			}
		}
	}
	rtnl_unlock();

	if (!idev->net_dev)
		return -ENOENT;

	ioss_dev_log(idev, "Linked to net device %s", net_dev->name);

	memset(&idev->drv_info, 0, sizeof(idev->drv_info));

	if (net_dev->ethtool_ops && net_dev->ethtool_ops->get_drvinfo)
		net_dev->ethtool_ops->get_drvinfo(net_dev, &idev->drv_info);

	ioss_dev_cfg(idev, "addr: %s, driver: %s %s, firmware: %s, erom: %s",
			idev->drv_info.bus_info,
			idev->drv_info.driver, idev->drv_info.version,
			idev->drv_info.fw_version, idev->drv_info.erom_version);

	return 0;
}
