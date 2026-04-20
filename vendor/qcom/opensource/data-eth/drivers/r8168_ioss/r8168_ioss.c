/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/pci.h>

#include <linux/module.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>
#include <net/rtnetlink.h>

#include "ioss/ioss_i.h"
#include "ioss/ioss_version.h"
#include "ioss/include/linux/msm/ioss.h"

#include "r8168.h"
#include "r8168_lib.h"

struct r8168_ioss_device {
	struct ioss_device *idev;
	struct rtl8168_private *_tp;
	struct notifier_block nb;
	struct rtl8168_counters stats;
	struct rtl8168_regs_save regs_save;
};

static int r8168_notifier_cb(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct r8168_ioss_device *rdev =
			container_of(nb, typeof(*rdev), nb);

	switch (action) {
	case RTL8168_NOTIFY_RESET_PREPARE:
		ioss_dev_log(rdev->idev, "RTL8168 reset prepare");
		break;
	case RTL8168_NOTIFY_RESET_COMPLETE:
		ioss_dev_log(rdev->idev, "RTL8168 reset complete");
		break;
	}

	return NOTIFY_DONE;
}

static int r8168_ioss_open_device(struct ioss_device *idev)
{
	struct r8168_ioss_device *rdev;

	ioss_dev_dbg(idev, "%s", __func__);

	rdev = kzalloc(sizeof(*rdev), GFP_KERNEL);
	if (!rdev)
		return -ENOMEM;

	rdev->idev = idev;
	rdev->_tp = netdev_priv(idev->net_dev);

	rdev->nb.notifier_call = r8168_notifier_cb;
	if (rtl8168_register_notifier(idev->net_dev, &rdev->nb)) {
		ioss_dev_err(idev, "Failed to register with r8168 notifier");
		goto err_notif;
	}

	idev->private = rdev;

	return 0;

err_notif:
	kfree_sensitive(rdev);
	return -EFAULT;
}

static int r8168_ioss_close_device(struct ioss_device *idev)
{
	struct r8168_ioss_device *rdev = idev->private;

	ioss_dev_dbg(idev, "%s", __func__);

	rtl8168_unregister_notifier(idev->net_dev, &rdev->nb);
	kfree_sensitive(rdev);

	return 0;
}

static int r8168_ioss_request_channel(struct ioss_channel *ch)
{
	int i;
	int rc = 0;
	enum rtl8168_channel_dir direction =
			(ch->direction == IOSS_CH_DIR_RX) ?
				RTL8168_CH_DIR_RX : RTL8168_CH_DIR_TX;
	struct rtl8168_ring *ring;

	ioss_dev_log(ioss_ch_dev(ch), "%s, ring_size=%d, buf_size=%d, dir=%d",
			__func__, ch->config.ring_size, ch->config.buff_size,
			direction);

	ring = rtl8168_request_ring(ioss_ch_dev(ch)->net_dev,
			ch->config.ring_size, ch->config.buff_size,
			direction, RTL8168_CONTIG_BUFS, NULL);
	if (!ring) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to request ring");
		return -EFAULT;
	}

	rc = ioss_channel_add_desc_mem(ch,
			ring->desc_addr, ring->desc_daddr, ring->desc_size);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to add desc mem");
		goto err_add_desc;
	}

	for (i = 0; i < ring->ring_size; i++) {
		struct rtl8168_buf *buff = &ring->bufs[i];

		rc = ioss_channel_add_buff_mem(ch,
				buff->addr, buff->dma_addr, buff->size);
		if (rc) {
			ioss_dev_err(ioss_ch_dev(ch), "Failed to add buff mem");
			goto err_add_buff;
		}
	}

	if (ch->direction == IOSS_CH_DIR_RX)
		ch->id = 0;
	else
		ch->id = ring->queue_num;

	ch->private = ring;

	return 0;

err_add_buff:
	for (i = 0; i < ring->ring_size; i++)
		ioss_channel_del_buff_mem(ch, ring->bufs[i].addr);

	ioss_channel_del_desc_mem(ch, ring->desc_addr);

err_add_desc:
	rtl8168_release_ring(ring);

	ch->id = -1;
	ch->private = NULL;

	return rc;
}

static int r8168_ioss_release_channel(struct ioss_channel *ch)
{
	int i;
	struct rtl8168_ring *ring = ch->private;

	ioss_dev_log(ioss_ch_dev(ch), "Release ring %d", ring->queue_num);

	ioss_channel_del_desc_mem(ch, ring->desc_addr);

	for (i = 0; i < ring->ring_size; i++)
		ioss_channel_del_buff_mem(ch, ring->bufs[i].addr);

	rtl8168_release_ring(ring);

	ch->id = -1;
	ch->private = NULL;

	return 0;
}

static int r8168_ioss_enable_channel(struct ioss_channel *ch)
{
	struct rtl8168_ring *ring = ch->private;

	return rtl8168_enable_ring(ring);
}

static int r8168_ioss_disable_channel(struct ioss_channel *ch)
{
	struct rtl8168_ring *ring = ch->private;

	rtl8168_disable_ring(ring);

	return 0;
}

static int r8168_ioss_request_event(struct ioss_channel *ch)
{
	int rc;
	int mod;
	struct rtl8168_ring *ring = ch->private;

	ioss_dev_log(ioss_ch_dev(ch), "Request EVENT: paddr=%pap, DATA: %llu",
		&ch->event.paddr, ch->event.data);

	if (ioss_channel_map_event(ch))
		return -EFAULT;

	rc = rtl8168_request_event(ring, MSIX_event_type,
					ch->event.daddr, ch->event.data, ch->event.paddr);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to request event");
		goto err_req_event;
	}

	/* Each mod unit is 2048 ns (~2 uS). */
	mod = ch->event.mod_usecs_max / 2;

	ioss_dev_log(ioss_ch_dev(ch), "EVENT: mod=%d", mod);

	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch),
			"Failed to set interrupt moderation");
		goto err_intr_mod;
	}

	return 0;

err_intr_mod:
	rtl8168_release_event(ring);
err_req_event:
	ioss_channel_unmap_event(ch);

	return -EFAULT;
}

static int r8168_ioss_release_event(struct ioss_channel *ch)
{
	struct rtl8168_ring *ring = ch->private;

	ioss_dev_log(ioss_ch_dev(ch), "Release EVENT: daddr=%pad, DATA: %llu",
			&ch->event.daddr, ch->event.data);

	rtl8168_release_event(ring);
	ioss_channel_unmap_event(ch);

	return 0;
}

static int __r8168_ioss_enable_filters(struct ioss_channel *ch)
{
	struct rtl8168_ring *ring = ch->private;
	struct net_device *net_dev = ioss_ch_dev(ch)->net_dev;
	enum ioss_filter_types filters = ch->filter_types;

	ioss_dev_log(ioss_ch_dev(ch), "Enabling filters %d", filters);

	if (!filters)
		return 0;

	if (ch->direction != IOSS_CH_DIR_RX)
		return -EFAULT;

	if (filters & IOSS_RXF_F_IP) {
		ioss_dev_log(ioss_ch_dev(ch), "Enabling RSS filter");

		if (rtl8168_rss_redirect(net_dev, 0, ring)) {
			ioss_dev_err(ioss_ch_dev(ch),
					"Failed to install rss filter");
			return -EFAULT;
		}

		filters &= ~IOSS_RXF_F_IP;
	}

	if (filters) {
		ioss_dev_err(ioss_ch_dev(ch), "Unsupported filters requested");
		rtl8168_rss_reset(net_dev);
		return -EFAULT;
	}

	return 0;
}

static int __r8168_ioss_disable_filters(struct ioss_channel *ch)
{
	struct net_device *net_dev = ioss_ch_dev(ch)->net_dev;
	enum ioss_filter_types filters = ch->filter_types;

	ioss_dev_log(ioss_ch_dev(ch), "Disabling filters %d", filters);

	if (!filters)
		return 0;

	if (ch->direction != IOSS_CH_DIR_RX)
		return -EFAULT;

	if (filters & IOSS_RXF_F_IP) {
		ioss_dev_log(ioss_ch_dev(ch), "Disabling RSS filter");

		if (rtl8168_rss_reset(net_dev)) {
			ioss_dev_err(ioss_ch_dev(ch), "Failed to reset rss filter");
			return -EFAULT;
		}

		filters &= ~IOSS_RXF_F_IP;
	}

	return 0;
}

static int r8168_ioss_enable_event(struct ioss_channel *ch)
{
	int rc;
	struct rtl8168_ring *ring = ch->private;

	rc = rtl8168_enable_event(ring);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to enable event");
		return rc;
	}

    rc = __r8168_ioss_enable_filters(ch);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to enable filters");
		rtl8168_disable_event(ring);
		return rc;
	}

	return 0;
}

static int r8168_ioss_disable_event(struct ioss_channel *ch)
{
	int rc;
	struct rtl8168_ring *ring = ch->private;

	rc = __r8168_ioss_disable_filters(ch);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to disable filters");
		return rc;
	}

	rc = rtl8168_disable_event(ring);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to disable event");
		return rc;
	}

	return 0;
}

static int r8168_save_regs(struct ioss_device *idev,
		void **regs, size_t *size)
{
	int rc = 0;
	struct r8168_ioss_device *rtldev = idev->private;

	rc = rtl8168_lib_get_stats(idev->net_dev, &rtldev->stats);
	if (rc)
		ioss_dev_err(idev, "Failed to get r8168 device statistics");

	rc = rtl8168_lib_save_regs(idev->net_dev, &rtldev->regs_save);
	if (rc)
		ioss_dev_err(idev, "Failed to save r8168 device registers");

	return rc;
}

static struct ioss_driver_ops r8168_ioss_ops = {
	.open_device = r8168_ioss_open_device,
	.close_device = r8168_ioss_close_device,

	.request_channel = r8168_ioss_request_channel,
	.release_channel = r8168_ioss_release_channel,

	.enable_channel = r8168_ioss_enable_channel,
	.disable_channel = r8168_ioss_disable_channel,

	.request_event = r8168_ioss_request_event,
	.release_event = r8168_ioss_release_event,

	.enable_event = r8168_ioss_enable_event,
	.disable_event = r8168_ioss_disable_event,

	.save_regs = r8168_save_regs,

};

static bool r8168_driver_match(struct device *dev)
{
	ioss_log_dbg(NULL, "MATCH %s", dev_name(dev));

	/* Just match the driver name */
	return (dev->bus == &pci_bus_type) &&
		!strcmp(to_pci_driver(dev->driver)->name, MODULENAME);
}

static struct ioss_driver r8168_ioss_drv = {
	.name = MODULENAME "_ioss",
	.match = r8168_driver_match,
	.ops = &r8168_ioss_ops,
	.filter_types = IOSS_RXF_F_IP,
};

static int __init r8168_ioss_init(void)
{
	return ioss_pci_register_driver(&r8168_ioss_drv);
}
module_init(r8168_ioss_init);

static void __exit r8168_ioss_exit(void)
{
	ioss_pci_unregister_driver(&r8168_ioss_drv);
}
module_exit(r8168_ioss_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Realtek R8168 IOSS Glue Driver");
