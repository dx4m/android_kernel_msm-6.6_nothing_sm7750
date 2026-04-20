/* Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Copyright (c) 2021 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/minmax.h>
#include "stmmac.h"
#include "ioss.h"
#include "emac_ipa_intf.h"
#include "dwmac-qcom-ethqos.h"
#include "common.h"

void *ipc_stmmac_log_ctxt;
struct qos_struct qos_tables;

struct stmmac_ioss_device {
	struct ioss_device *idev;
	struct stmmac_priv *_priv;
};

static void qos_adjust_txq_cbs_bw(struct list_head *qos_tx, u16 available_bw)
{
	struct qos_routing_tx *temp_tx;
	int total_min_bw = 0, total_allocated_bw = 0, remaining_bw = 0;
	u16 min_bw, max_bw;

	list_for_each_entry(temp_tx, qos_tx, node) {
	if (temp_tx->disabled)
		continue;
        total_min_bw += temp_tx->cbs_bw.low_bw;
    }

    ioss_qos_dev_log(NULL, "[ioss qos] aggr min bw = %d",total_min_bw);
    list_for_each_entry(temp_tx, qos_tx, node) {
		if (temp_tx->disabled)
			continue;
		min_bw = temp_tx->cbs_bw.low_bw;
		max_bw = temp_tx->cbs_bw.high_bw;
		total_min_bw -= temp_tx->cbs_bw.low_bw;
		ioss_qos_dev_log(NULL, "[ioss qos] aggr min bw = %d",total_min_bw);
		ioss_qos_dev_log(NULL, "[ioss qos] TC%d ==> MinBW = %d MaxBW = %d TotalCapacity = %d total_allocated_bw = %d",
				 temp_tx->tc_prio, min_bw,max_bw, (available_bw - total_allocated_bw), total_allocated_bw);

		remaining_bw = min(max_bw, (available_bw - total_min_bw - total_allocated_bw));
		temp_tx->bw_allocated = max(min_bw, remaining_bw);
		total_allocated_bw += temp_tx->bw_allocated;
		qos_tables.bw_allocated[temp_tx->tc_prio] = temp_tx->bw_allocated;
		ioss_qos_dev_log(NULL, "[ioss qos] Alloted BW for TC%d = %d", temp_tx->tc_prio, temp_tx->bw_allocated);
    }
}

static void *stmmac_ioss_dma_alloc(struct ioss_device *idev,
				   size_t size, dma_addr_t *daddr, gfp_t gfp,
				   struct ioss_mem_allocator *alctr)
{
	return alctr->alloc(idev, size, daddr, gfp, alctr);
}

static void stmmac_ioss_dma_free(struct ioss_device *idev,
				 size_t size, void *buf, dma_addr_t daddr,
				 struct ioss_mem_allocator *alctr)
{
	return alctr->free(idev, size, buf, daddr, alctr);
}

static void *stmmac_ioss_alloc_descs(struct net_device *ndev, size_t size,
				     dma_addr_t *daddr, gfp_t gfp, struct mem_ops *mem_ops,
				     struct channel_info *ch_info)
{
	struct ioss_channel *ch = ch_info->client_ch_priv;
	return stmmac_ioss_dma_alloc(ioss_ch_dev(ch), size, daddr, gfp,
			ch->config.desc_alctr);
}

static void *stmmac_ioss_alloc_buf(struct net_device *ndev, size_t size,
				   dma_addr_t *daddr, gfp_t gfp, struct mem_ops *mem_ops,
				   struct channel_info *ch_info)
{
	struct ioss_channel *ch = ch_info->client_ch_priv;

	return stmmac_ioss_dma_alloc(ioss_ch_dev(ch), size, daddr, gfp,
			ch->config.buff_alctr);
}

static void stmmac_ioss_free_descs(struct net_device *ndev, void *buf, size_t size,
				   dma_addr_t *daddr, struct mem_ops *mem_ops,
				   struct channel_info *ch_info)
{
	struct ioss_channel *ch = ch_info->client_ch_priv;

	return stmmac_ioss_dma_free(ioss_ch_dev(ch), size, buf, *daddr,
			ch->config.desc_alctr);
}

static void stmmac_ioss_free_buf(struct net_device *ndev, void *buf, size_t size,
				 dma_addr_t *daddr, struct mem_ops *mem_ops,
				 struct channel_info *ch_info)
{
	struct ioss_channel *ch = ch_info->client_ch_priv;

	return stmmac_ioss_dma_free(ioss_ch_dev(ch), size, buf, *daddr,
			ch->config.buff_alctr);
}

static int stmmac_ioss_open_device(struct ioss_device *idev)
{
	struct stmmac_ioss_device *stmmac_dev;

	ioss_dev_dbg(idev, "Enter");

	stmmac_dev = kzalloc(sizeof(*stmmac_dev), GFP_KERNEL);
	if (!stmmac_dev)
		return -ENOMEM;

	stmmac_dev->idev = idev;
	stmmac_dev->_priv = netdev_priv(idev->net_dev);

	idev->private = stmmac_dev;

	return 0;
}

static int stmmac_ioss_close_device(struct ioss_device *idev)
{
	struct stmmac_ioss_device *stmmac_dev = idev->private;

	ioss_dev_dbg(idev, "Enter");

	kfree_sensitive(stmmac_dev);

	return 0;
}

static int stmmac_ioss_request_channel(struct ioss_channel *ch)
{
	int i;
	int rc = -EFAULT;
	struct request_channel_input ipa_channel_info;
	enum channel_dir direction =
			(ch->direction == IOSS_CH_DIR_RX) ?
				CH_DIR_RX : CH_DIR_TX;
	struct channel_info *ring;

	ioss_dev_log(ioss_ch_dev(ch), "ring_size=%d, buf_size=%d, dir=%d, id=%d, tc_map=%u",
				 ch->config.ring_size, ch->config.buff_size,
				 ch->direction, ch->channel_num, ch->tc_mapping);

	ipa_channel_info.mem_ops = kzalloc(sizeof(*ipa_channel_info.mem_ops), GFP_KERNEL);
	if (!ipa_channel_info.mem_ops)
		return -ENOMEM;

	ipa_channel_info.mem_ops->alloc_descs = stmmac_ioss_alloc_descs;
	ipa_channel_info.mem_ops->alloc_buf = stmmac_ioss_alloc_buf;
	ipa_channel_info.mem_ops->free_descs = stmmac_ioss_free_descs;
	ipa_channel_info.mem_ops->free_buf = stmmac_ioss_free_buf;
	ipa_channel_info.client_ch_priv = ch;

	ipa_channel_info.ndev = ioss_ch_dev(ch)->net_dev;
	ipa_channel_info.desc_cnt = ch->config.ring_size;
	ipa_channel_info.ch_dir = direction;
	ipa_channel_info.channel_num = ch->channel_num;
	ipa_channel_info.buf_size = ch->config.buff_size;
	ipa_channel_info.ch_flags = STMMAC_CONTIG_BUFS;
	ipa_channel_info.flags = GFP_KERNEL;

	strscpy(ipa_channel_info.ipa_config_info, ch->iface->ipa_config, CONFIG_LEN);
	ipa_channel_info.traffic_type_info = ch->traffic_type;

	ring = request_channel(&ipa_channel_info);

	if (!ring) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to request ring\n");
		kfree_sensitive(ipa_channel_info.mem_ops);
		return -ENOMEM;
	}

	ch->tail_ptr_addr = ipa_channel_info.tail_ptr_addr;

	rc = ioss_channel_add_desc_mem(ch,
				       ring->desc_addr.desc_virt_addrs_base,
				       ring->desc_addr.desc_dma_addrs_base,
				       ring->desc_size * ring->desc_cnt);

	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to add desc mem\n");
		goto release_channel;
	}

	for (i = 0; i < ring->desc_cnt; i++) {
		void *vaddr = (void *)ring->buff_pool_addr.buff_pool_va_addrs_base[0];
		void *addr = vaddr + (ring->buf_size * i);
		dma_addr_t daddr = ring->buff_pool_addr.buff_pool_dma_addrs_base[0] +
			(ring->buf_size * i);

		rc = ioss_channel_add_buff_mem(ch, addr, daddr,
					       ring->buf_size);

		if (rc) {
			ioss_dev_err(ioss_ch_dev(ch), "Failed to add buff mem\n");
			goto release_desc;
		}
	}

	ch->id = ring->channel_num;
	ch->private = ring;

	return 0;

release_desc:
	ioss_channel_del_desc_mem(ch, ring->desc_addr.desc_virt_addrs_base);
	for (i = 0; i < ring->desc_cnt; i++) {
		void *vaddr = (void *)ring->buff_pool_addr.buff_pool_va_addrs_base[0];
		void *addr = vaddr + (ring->buf_size * i);

		ioss_channel_del_buff_mem(ch, addr);
	}
release_channel:
	release_channel(ioss_ch_dev(ch)->net_dev, ring);
	kfree_sensitive(ipa_channel_info.mem_ops);
	return -ENOMEM;
}

static int stmmac_ioss_release_channel(struct ioss_channel *ch)
{
	int i;
	struct channel_info *ring = ch->private;
	struct mem_ops *mem_ops = ring->mem_ops;

	ioss_dev_log(ioss_ch_dev(ch), "Release ring %d\n", ring->channel_num);

	ioss_channel_del_desc_mem(ch, ring->desc_addr.desc_virt_addrs_base);

	for (i = 0; i < ring->desc_cnt; i++) {
		void *vaddr = (void *)ring->buff_pool_addr.buff_pool_va_addrs_base[0];
		void *addr = vaddr + (ring->buf_size * i);

		ioss_channel_del_buff_mem(ch, addr);
	}

	release_channel(ioss_ch_dev(ch)->net_dev, ring);
	kfree_sensitive(mem_ops);

	ch->id = -1;
	ch->private = NULL;

	return 0;
}

static int stmmac_ioss_enable_channel(struct ioss_channel *ch)
{
	struct channel_info *ring = ch->private;

	ioss_dev_dbg(ioss_ch_dev(ch), "Enter");

	return start_channel(ioss_ch_dev(ch)->net_dev, ring);
}

static int stmmac_ioss_disable_channel(struct ioss_channel *ch)
{
	struct channel_info *ring = ch->private;

	ioss_dev_dbg(ioss_ch_dev(ch), "Enter");

	return stop_channel(ioss_ch_dev(ch)->net_dev, ring);
}

static int stmmac_ioss_request_event(struct ioss_channel *ch)
{
	int rc;
	int wdt;
	struct channel_info *ring = ch->private;

	ioss_dev_log(ioss_ch_dev(ch), "Request EVENT: paddr=%pap, DATA: %llu",
		&ch->event.paddr, ch->event.data);

	if (ioss_channel_map_event(ch))
		return -EFAULT;

	rc = request_event(ioss_ch_dev(ch)->net_dev,ring,
						ch->event.daddr, ch->event.data);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to request event\n");
		return rc;
	}

	// call for rx and not tx direction
	if (ch->direction == IOSS_CH_DIR_RX)
	{
		// Each wdt unit is 2048 ns (~2 uS).
		wdt = ch->event.mod_usecs_max / 2;
		ioss_dev_log(ioss_ch_dev(ch),"EVENT: wdt=%d\n", wdt);

		rc = set_event_mod(ioss_ch_dev(ch)->net_dev,ring, wdt);
		if (rc) {
			ioss_dev_err(ioss_ch_dev(ch), "Failed to set interrupt moderation\n");
			release_event(ioss_ch_dev(ch)->net_dev,ring);
			return rc;
		}
	}

	return 0;
}

static int stmmac_ioss_release_event(struct ioss_channel *ch)
{
	struct channel_info *ring = ch->private;

	ioss_dev_dbg(ioss_ch_dev(ch), "Enter");

	ioss_dev_log(ioss_ch_dev(ch), "Release EVENT: daddr=%pad, DATA: %llu",
			&ch->event.daddr, ch->event.data);

	release_event(ioss_ch_dev(ch)->net_dev,ring);
	ioss_channel_unmap_event(ch);

	return 0;
}


enum {
	FLT_TYPE_IP4,
	FLT_TYPE_IP6,
	FLT_TYPE_VLAN,

	/* Must be the last entry */
	FLT_NUM_TYPES,
};

static int stmmac_ioss_enable_event(struct ioss_channel *ch)
{
	int rc;
	struct channel_info *ring = ch->private;

	ioss_dev_dbg(ioss_ch_dev(ch), "Enter");

	rc = enable_event(ioss_ch_dev(ch)->net_dev, ring);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to enable event\n");
		return rc;
	}

	return 0;
}

static int stmmac_ioss_disable_event(struct ioss_channel *ch)
{
	int rc;
	struct channel_info *ring = ch->private;

	ioss_log_msg(NULL, "Enter");

	rc = disable_event(ioss_ch_dev(ch)->net_dev, ring);
	if (rc) {
		ioss_dev_err(ioss_ch_dev(ch), "Failed to disable event\n");
		return rc;
	}

	return 0;
}

static u64 __get_stats_data(const char *name, u64 data[], const u8 *strings_data, int scount)
{
	int i = 0;
	const char (*strings)[ETH_GSTRING_LEN] = (typeof(strings))strings_data;

	ioss_log_msg(NULL, "Enter");

	/* iterate through strings[], find matchging index */
	for (i = 0; i < scount; i++) {
		if (strcmp(name, strings[i]) == 0)
			return data[i];
	}

	return 0;
}

static int stmmac_ioss_device_statistics(struct ioss_device *idev,
					 struct ioss_device_stats *statistics)
{
	u64 *data;
	u8 *strings;
	int strings_count = 0;
	struct ethtool_stats stats;
	const struct ethtool_ops *ops = idev->net_dev->ethtool_ops;

	ioss_dev_dbg(idev, "Enter");

	if (!ops || !ops->get_sset_count ||
	    !ops->get_ethtool_stats || !ops->get_strings)
		return -EOPNOTSUPP;

	strings_count = ops->get_sset_count(idev->net_dev, ETH_SS_STATS);

	data = kcalloc(strings_count, sizeof(u64), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	strings = kcalloc(strings_count, ETH_GSTRING_LEN, GFP_KERNEL);
	if (!strings) {
		kfree_sensitive(data);
		return -ENOMEM;
	}

	memset(&stats, 0, sizeof(stats));
	stats.n_stats = strings_count;

	rtnl_lock();
	ops->get_ethtool_stats(idev->net_dev, &stats, data);
	rtnl_unlock();

	ops->get_strings(idev->net_dev, ETH_SS_STATS, strings);

	statistics->emac_rx_packets =
		__get_stats_data("mmc_rx_framecount_gb", data, strings, strings_count);
	statistics->emac_tx_packets =
		__get_stats_data("mmc_tx_framecount_gb", data, strings, strings_count);
	statistics->emac_rx_bytes =
		__get_stats_data("mmc_rx_octetcount_gb", data, strings, strings_count);
	statistics->emac_tx_bytes =
		__get_stats_data("mmc_tx_octetcount_gb", data, strings, strings_count);
	statistics->emac_rx_drops =
		__get_stats_data("mmc_rx_fifo_overflow", data, strings, strings_count);
	statistics->emac_rx_pause_frames =
		__get_stats_data("mmc_rx_pause_frames", data, strings, strings_count);
	statistics->emac_tx_pause_frames =
		__get_stats_data("mmc_tx_pause_frame", data, strings, strings_count);

	kfree_sensitive(data);
	kfree_sensitive(strings);

	return 0;
}

static int stmmac_ioss_channel_statistics(struct ioss_channel *ch,
		struct ioss_channel_stats *statistics)
{
	struct ioss_device *idev = ioss_ch_dev(ch);
	struct stmmac_priv *priv = netdev_priv(idev->net_dev);

	if (ch->direction == IOSS_CH_DIR_RX) {
		statistics->desc_unavail = priv->xstats.rxq_stats[ch->id].rx_buf_unav_irq;
		statistics->overflow_error = priv->xstats.overflow_error;
	} else {
		statistics->underflow_error = priv->xstats.tx_underflow;
	}

	return 0;
}

static int stmmac_ioss_channel_status(struct ioss_channel *ch, struct ioss_channel_status *status)
{
	u64 *data;
	int strings_count = 0;
	struct ethtool_stats stats;
	struct ioss_device *idev = ioss_ch_dev(ch);
	const struct ethtool_ops *ops = idev->net_dev->ethtool_ops;
	struct stmmac_priv *priv = netdev_priv(idev->net_dev);

	if (ops == NULL || ops->get_sset_count == NULL ||
		ops->get_ethtool_stats == NULL || ops->get_strings == NULL)
		return -EOPNOTSUPP;

	strings_count = ops->get_sset_count(idev->net_dev, ETH_SS_STATS);

	data = kcalloc(strings_count, sizeof(u64), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memset(&stats, 0, sizeof(stats));
	stats.n_stats = strings_count;

	rtnl_lock();
	ops->get_ethtool_stats(idev->net_dev, &stats, data);
	rtnl_unlock();

	if (ch->direction == IOSS_CH_DIR_RX) {
		status->ring_size = priv->xstats.rxq_stats[ch->id].rxch_desc_ring_len;
		status->head_ptr = priv->xstats.rxq_stats[ch->id].rxch_desc_list_laddr;
		status->tail_ptr = priv->xstats.rxq_stats[ch->id].rxch_desc_tail;
	} else {
		status->ring_size = priv->xstats.txq_stats[ch->id].txch_desc_ring_len;
		status->head_ptr = priv->xstats.txq_stats[ch->id].txch_desc_list_laddr;
		status->tail_ptr = priv->xstats.txq_stats[ch->id].txch_desc_tail;
	}
	kfree_sensitive(data);

	return 0;
}

static int stmmac_ioss_update_skb(struct ioss_channel *ch, struct sk_buff *skb)
{
	struct ioss_device *idev = ioss_ch_dev(ch);
	struct stmmac_priv *priv = netdev_priv(idev->net_dev);

	if (priv->hw->rx_csum && skb->ip_summed == CHECKSUM_NONE)
		skb->ip_summed = CHECKSUM_UNNECESSARY;

	return 0;
}

static int stmmac_get_rx_tc_info(struct list_head *rx_qos, enum data_path path)
{
	int tc_cnt = 0;
	int i = 0;

	struct qos_routing_rx *temp;
	list_for_each_entry(temp, rx_qos, node) {
		if (path == SW_PATH) {
			if (temp->tc_prio >= 0 && (temp->action == IOSS_QOS_SW_PATH)) {
				tc_cnt++;
				for (i = 0; i < temp->pcp.len; i++) {
					if (temp->pcp.arr[i]) {
						qos_tables.asgn_sw_queue = true;
						break;
					}
				}
			}
		} else if (path == HW_PATH) {
			if (temp->tc_prio >= 0 && (temp->action == IOSS_QOS_HW_PATH)) {
				tc_cnt++;
				for (i = 0; i < temp->pcp.len; i++) {
					if (temp->pcp.arr[i]) {
						qos_tables.asgn_hw_queue = true;
						break;
					}
				}
			}
		} else if (path == SW_HW_PATH) {
			tc_cnt++;
		}
	}
	return tc_cnt;
}

static int stmmac_get_tx_tc_count(struct list_head *tx_qos, enum data_path path)
{
	int tc_cnt = 0;

	struct qos_routing_tx *temp;
	list_for_each_entry(temp, tx_qos, node) {
		if (path == SW_PATH) {
			if (temp->tc_prio >= 0 &&  (temp->action == IOSS_QOS_SW_PATH))
				tc_cnt++;
		} else if (path == HW_PATH) {
			if (temp->tc_prio >= 0 && (temp->action == IOSS_QOS_HW_PATH))
				tc_cnt++;
		} else if (path == SW_HW_PATH) {
			tc_cnt++;
		}
	}
	return tc_cnt;
}

static bool compare_ipv6(unsigned char *curr, unsigned char next[16], u8 prefix)
{
	u8 bytes = prefix/8;
	u8 bits = prefix % 8;
	int i = 0;
	u8 bitmask = 0xFF >> bits;

	for (i = 0; i < 16 - bytes; i++) {
		if (curr[i] != next[i])
			return false;
	}

	if (bits) {
		if ((curr[i] | bitmask) != (next[i] | bitmask))
			return false;
	}

	return true;
}

static bool compare_ipv4(u32 curr, u32 next, u8 mask_len)
{
	u32 ip_bitmask = 0xFFFFFFFF;

	ip_bitmask = ip_bitmask << (32 - mask_len);

	if ((curr ^ next) & ip_bitmask)
		return false;

	return true;
}

static void delete_route_table(struct list_head *filter_table)
{
	struct pcp_routing *filter_node, *filter_node_next;

	list_for_each_entry_safe(filter_node, filter_node_next, filter_table, node) {
		list_del(&filter_node->node);
		kfree(filter_node);
	}
}

static void delete_filter_table(struct list_head *filter_table)
{
	struct dma_filter_table *filter_node, *filter_node_next;

	list_for_each_entry_safe(filter_node, filter_node_next, filter_table, node) {
		list_del(&filter_node->node);
		kfree(filter_node);
	}
}

static bool is_param_unique(struct list_head *filter_table, enum qos_filter_type filter)
{
	struct dma_filter_table *filter_node_curr, *filter_node_next, *filter_node_tmp;

	u8 mask_len;
	bool is_last = false;

	list_for_each_entry(filter_node_curr, filter_table, node) {
		list_for_each_entry_safe(filter_node_next, filter_node_tmp, &filter_node_curr->node, node) {
			if (list_is_last(&filter_node_next->node, filter_table)) {
				is_last = true;
			} else {
				is_last = false;
			}
			if (!list_is_last(&filter_node_curr->node, filter_table)) {
				if (filter == DEST_PORT) {
					if (filter_node_curr->dst_port.proto ==
                                            filter_node_next->dst_port.proto &&
                                            filter_node_curr->dst_port.port_num ==
                                            filter_node_next->dst_port.port_num &&
                                            filter_node_curr->dma_ch == filter_node_next->dma_ch) {
						list_del(&filter_node_next->node);
						if (is_last)
							break;
					} else if (filter_node_curr->dst_port.proto ==
                                                   filter_node_next->dst_port.proto &&
                                                   filter_node_curr->dst_port.port_num ==
                                                   filter_node_next->dst_port.port_num &&
                                                   filter_node_curr->dma_ch != filter_node_next->dma_ch)
						return false;
				} else if (filter == SRC_PORT) {
					if (filter_node_curr->src_port.proto ==
                                            filter_node_next->src_port.proto &&
                                            filter_node_curr->src_port.port_num ==
                                            filter_node_next->src_port.port_num  &&
                                            filter_node_curr->dma_ch == filter_node_next->dma_ch) {
						list_del(&filter_node_next->node);
						if (is_last)
							break;
					} else if (filter_node_curr->src_port.proto == filter_node_next->src_port.proto &&
						filter_node_curr->src_port.port_num == filter_node_next->src_port.port_num  &&
						(filter_node_curr->dma_ch != filter_node_next->dma_ch)) {
						return false;
                                        }
				} else if (filter == VLAN_ID) {
					if (filter_node_curr->vlan_id == filter_node_next->vlan_id &&
						(filter_node_curr->dma_ch == filter_node_next->dma_ch)) {
						list_del(&filter_node_next->node);
						if (is_last)
							break;
					} else if (filter_node_curr->vlan_id == filter_node_next->vlan_id &&
						(filter_node_curr->dma_ch != filter_node_next->dma_ch))
						return false;
				} else if (filter == DEST_IP) {
					 mask_len = filter_node_curr->ip_dest.dst_mask_length > filter_node_next->ip_dest.dst_mask_length?
								filter_node_next->ip_dest.dst_mask_length: filter_node_curr->ip_dest.dst_mask_length;
					 if (filter_node_curr->dma_ch != filter_node_next->dma_ch) {
						 if (filter_node_curr->ip_dest.ipv6_dst == filter_node_next->ip_dest.ipv6_dst) {
							 if (filter_node_curr->ip_dest.ipv6_dst) {
								 if (compare_ipv6 (filter_node_curr->ip_dest.ipv6_dst_addr,
												filter_node_next->ip_dest.ipv6_dst_addr, 128 - mask_len))
									 return false;
							 } else {
								 if (compare_ipv4(filter_node_curr->ip_dest.ipv4_dst_addr,
									filter_node_next->ip_dest.ipv4_dst_addr, mask_len))
									 return false;
							 }
						 }
					 } else {
						 if (filter_node_curr->ip_dest.ipv6_dst == filter_node_next->ip_dest.ipv6_dst) {
							 if (filter_node_curr->ip_dest.ipv6_dst) {
								 if (compare_ipv6(filter_node_curr->ip_dest.ipv6_dst_addr,
								     filter_node_next->ip_dest.ipv6_dst_addr, 128 - mask_len)) {
									 list_del(&filter_node_next->node);
									if (is_last)
										break;
								 }
							 } else {
								 if (compare_ipv4(filter_node_curr->ip_dest.ipv4_dst_addr,
								     filter_node_next->ip_dest.ipv4_dst_addr, mask_len)) {
									 list_del(&filter_node_next->node);
									if (is_last)
										break;
								 }
							 }
						 }
					 }

				} else if (filter == SRC_IP) {
					mask_len = filter_node_curr->ip_src.src_mask_length > filter_node_next->ip_src.src_mask_length?
						filter_node_next->ip_src.src_mask_length: filter_node_curr->ip_src.src_mask_length;
					if (filter_node_curr->dma_ch != filter_node_next->dma_ch) {
						if (filter_node_curr->ip_src.ipv6_src == filter_node_next->ip_src.ipv6_src) {
							if (filter_node_curr->ip_src.ipv6_src) {
								if (compare_ipv6(filter_node_curr->ip_src.ipv6_src_addr,
								   filter_node_next->ip_src.ipv6_src_addr, 128 - mask_len))
				  					return false;
							} else {
								if (compare_ipv4(filter_node_curr->ip_src.ipv4_src_addr,
								    filter_node_next->ip_src.ipv4_src_addr, mask_len))
				  					return false;
							}
						}
					} else {
						if (filter_node_curr->ip_src.ipv6_src == filter_node_next->ip_src.ipv6_src) {
							if (filter_node_curr->ip_src.ipv6_src) {
								if (compare_ipv6(filter_node_curr->ip_src.ipv6_src_addr,
								    filter_node_next->ip_src.ipv6_src_addr,128 - mask_len)) {
				  					list_del(&filter_node_next->node);
									if (is_last)
										break;
								}
							} else {
								if (compare_ipv4(filter_node_curr->ip_src.ipv4_src_addr,
								   filter_node_next->ip_src.ipv4_src_addr, mask_len)) {
				  					list_del(&filter_node_next->node);
									if (is_last)
										break;
								}
							}
						}
					}
				}
				if (&filter_node_next->node) {
					if (list_is_last(&filter_node_next->node, filter_table)) {
						break;
					}
				} else {
					break;
				}

			}
		}
		if (&filter_node_curr->node) {
			if (list_is_last(&filter_node_curr->node, filter_table)) {
				break;
			}
		} else {
			break;
		}
	}

	return true;
}

void convert_ip_addr_to_str(struct sockaddr_storage *addr, u32 *ipv4_addr, unsigned char *ipv6_addr)
{
	if (addr->ss_family == AF_INET) {
		*ipv4_addr = ntohl(((struct sockaddr_in *)addr)->sin_addr.s_addr);
	} else if (addr->ss_family == AF_INET6) {
		memcpy(ipv6_addr, &((struct sockaddr_in6 *)addr)->sin6_addr, 16);
	}
}

enum protocol get_proto(char *s) {
	if (!s)
		return IOSS_IPPROTO_TCP_UDP;
	else if (!strncmp(s, "udp", 3))
		return IOSS_IPPROTO_UDP;
	else if (!strncmp(s, "tcp", 3))
		return IOSS_IPPROTO_TCP;
	else
		return IOSS_IPPROTO_INVALID_PROTO;
}

static enum qos_filter_type find_filter(struct list_head *qos_rx)
{
	enum qos_filter_type filter = INVALID_FILTER;
	struct qos_routing_rx *temp;
	struct dma_filter_table *filter_node, *filter_node_temp;
	struct filter_map_info *filter_map;
	int count = 0;
	int i=0;
	bool unique = 0;
	bool not_a_filter = false;
	u32 ipv4_addr;
	unsigned char ipv6_addr[16];
	int cnt = 0, cnt1=0;
	enum protocol tmp_proto;

	INIT_LIST_HEAD(&qos_tables.dma_filter_table);

	/*check if dest port is unique in filter table*/
	not_a_filter = false;
	count = 0;

	filter_map = kzalloc(sizeof(struct filter_map_info), GFP_KERNEL);
	list_for_each_entry(temp, qos_rx, node) {
		/* If TC is for HW path and no IPA ch are available don't add to the filter table*/
		if (temp->action == IOSS_QOS_HW_PATH && !qos_tables.ipa_qos_rx_ch)
			continue;

		/* if queue is assigned don't do pcp filtering */
		filter_map = (struct filter_map_info *)(temp->filter_info);
		if (filter_map->queue)
			continue;

		/* dest_port */
		if (temp->dst.len) {
			for(i=0; i< temp->dst.len; i++) {
				cnt++;
				if (temp->dst.arr[i].port_num) {
					tmp_proto = get_proto(temp->dst.arr[i].proto);
					if (tmp_proto == IOSS_IPPROTO_INVALID_PROTO) {
						ioss_qos_dev_err(NULL, "[ioss qos] Invalid dest proto for TC = %d\n", temp->tc_prio);
						continue;
					} else if (tmp_proto == IOSS_IPPROTO_TCP_UDP) {
						/* Add filter for TCP */
						filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
						if (!filter_node)
							goto alloc_err;

						filter_node->dst_port.port_num = temp->dst.arr[i].port_num;
						filter_node->dst_port.proto = IOSS_IPPROTO_TCP;
						filter_node->dma_ch = filter_map->channel;
						filter_node->tc_prio = temp->tc_prio;
						INIT_LIST_HEAD(&filter_node->node);
						list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
						/* Add filter for UDP */
						filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
						if (!filter_node)
							goto alloc_err;

						filter_node->dst_port.port_num = temp->dst.arr[i].port_num;
						filter_node->dst_port.proto = IOSS_IPPROTO_UDP;
						filter_node->dma_ch = filter_map->channel;
						filter_node->tc_prio = temp->tc_prio;
						INIT_LIST_HEAD(&filter_node->node);
						list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
					} else {
						filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
						if (!filter_node)
							goto alloc_err;

						filter_node->dst_port.port_num = temp->dst.arr[i].port_num;
						filter_node->dst_port.proto = tmp_proto;

						filter_node->dma_ch = filter_map->channel;
						filter_node->tc_prio = temp->tc_prio;
						INIT_LIST_HEAD(&filter_node->node);
						list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
					}
				}
				else {
					cnt1++;
				}

			}
			if (cnt == cnt1) {
				not_a_filter = true;
				break;
			} else {
				not_a_filter = false;
				cnt = 0;
				cnt1 = 0;
			}
		} else {
			not_a_filter = true;
			break;
		}
	}

	if (not_a_filter)
		unique = false;
	else
		unique = is_param_unique(&qos_tables.dma_filter_table, DEST_PORT);
	if (unique) {
		filter = DEST_PORT;
		ioss_qos_dev_log(NULL, "[ioss qos] Unique filter DEST PORT\n");
		goto filter_found;
	} else {
		/* do cleanup*/
		delete_filter_table(&qos_tables.dma_filter_table);
	}

	/*check if src port is unique in filter table*/
	cnt = 0;
	cnt1 = 0;
	count = 0;
	not_a_filter = false;
	list_for_each_entry(temp, qos_rx, node) {
		/* If TC is for HW path and no IPA ch are available don't add to the filter table*/
		if (temp->action == IOSS_QOS_HW_PATH && !qos_tables.ipa_qos_rx_ch)
			continue;

		/* if queue is assigned don't do dma filtering */
		filter_map = (struct filter_map_info *)(temp->filter_info);
		if (filter_map->queue)
			continue;
		/* src_port */
		if (temp->src.len) {
			for(i = 0; i < temp->src.len; i++) {
				cnt++;
				if (temp->src.arr[i].port_num) {
					tmp_proto = get_proto(temp->src.arr[i].proto);
					if (tmp_proto == IOSS_IPPROTO_INVALID_PROTO) {
						ioss_qos_dev_err(NULL, "[ioss qos] Invalid src proto for TC = %d\n", temp->tc_prio);
						continue;
					} else if (tmp_proto == IOSS_IPPROTO_TCP_UDP) {
						/* Add filter for TCP */
						filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
						if (!filter_node)
							goto alloc_err;

						filter_node->src_port.port_num = temp->src.arr[i].port_num;
						filter_node->src_port.proto = IOSS_IPPROTO_TCP;
						filter_node->dma_ch = filter_map->channel;
						filter_node->tc_prio = temp->tc_prio;
						INIT_LIST_HEAD(&filter_node->node);
						list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
						/* Add filter for UDP */
						filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
						if (!filter_node)
							goto alloc_err;

						filter_node->src_port.port_num = temp->src.arr[i].port_num;
						filter_node->src_port.proto = IOSS_IPPROTO_UDP;
						filter_node->dma_ch = filter_map->channel;
						filter_node->tc_prio = temp->tc_prio;
						INIT_LIST_HEAD(&filter_node->node);
						list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
					} else {
						filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
						if (!filter_node)
							goto alloc_err;

						filter_node->src_port.port_num = temp->src.arr[i].port_num;
						filter_node->src_port.proto = tmp_proto;

						filter_node->dma_ch = filter_map->channel;
						filter_node->tc_prio = temp->tc_prio;
						INIT_LIST_HEAD(&filter_node->node);
						list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
					}

				} else {
					cnt1++;
				}
			}
			if (cnt == cnt1) {
				not_a_filter = true;
				break;
			} else {
				not_a_filter = false;
				cnt = 0;
				cnt1 = 0;
			}
		} else {
			not_a_filter = true;
			break;
		}
	}

	if (not_a_filter)
		unique = false;
	else
		unique = is_param_unique(&qos_tables.dma_filter_table, SRC_PORT);
	if (unique) {
		filter = SRC_PORT;
		ioss_qos_dev_log(NULL, "[ioss qos] Unique filter SRC_PORT\n");
		goto filter_found;
	} else {
		/* do cleanup*/
		delete_filter_table(&qos_tables.dma_filter_table);
	}

	/*check if vlan is unique in filter table*/
	count = 0;
	not_a_filter = false;
	list_for_each_entry(temp, qos_rx, node) {
		/* If TC is for HW path and no IPA ch are available don't add to the filter table*/
		if (temp->action == IOSS_QOS_HW_PATH && !qos_tables.ipa_qos_rx_ch)
			continue;

		/* if queue is assigned don't do dma filtering */
		filter_map = (struct filter_map_info *)(temp->filter_info);
		if (filter_map->queue)
			continue;
		if (temp->vlan_ids.len) {
			for (i = 0; i< temp->vlan_ids.len; i++) {
				if (temp->vlan_ids.arr[i]) {
					filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
					if(!filter_node)
						goto alloc_err;

					filter_node->vlan_id = temp->vlan_ids.arr[i];
					filter_map = (struct filter_map_info *)(temp->filter_info);
					filter_node->dma_ch = filter_map->channel;
					filter_node->tc_prio = temp->tc_prio;
					INIT_LIST_HEAD(&filter_node->node);
					list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
				}
			}
		} else {
			not_a_filter = true;
			break;
		}
	}

	if (not_a_filter)
		unique = false;
	else
		unique = is_param_unique(&qos_tables.dma_filter_table, VLAN_ID);
	if (unique) {
		filter = VLAN_ID;
		ioss_qos_dev_log(NULL, "[ioss qos] Unique filter VLAN_ID\n");
		goto filter_found;
	} else {
		/* do cleanup*/
		delete_filter_table(&qos_tables.dma_filter_table);
	}

	/*check if dst ip addr is unique in filter table*/
	cnt = 0;
	cnt1 = 0;
	count = 0;
	not_a_filter = false;
	list_for_each_entry(temp, qos_rx, node) {
		/* If TC is for HW path and no IPA ch are available don't add to the filter table*/
		if (temp->action == IOSS_QOS_HW_PATH && !qos_tables.ipa_qos_rx_ch)
			continue;

		/* if queue is assigned don't do dma filtering */
		filter_map = (struct filter_map_info *)(temp->filter_info);
		if (filter_map->queue)
			continue;
		if (temp->dst.len) {
			for (i = 0; i< temp->dst.len; i++) {
				cnt++;
				if (temp->dst.arr[i].address.ss_family) {
					filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
					if (!filter_node)
						goto alloc_err;

					convert_ip_addr_to_str(&(temp->dst.arr[i].address), &ipv4_addr, ipv6_addr);
					filter_node->ip_dest.dst_mask_length = temp->dst.arr[i].mask_length;
					if (temp->dst.arr[i].address.ss_family == AF_INET6) {
						for (i = 0; i < 16; i++)
							filter_node->ip_dest.ipv6_dst_addr[i] = ipv6_addr[i];
						filter_node->ip_dest.ipv6_dst = true;
					} else {
						filter_node->ip_dest.ipv4_dst_addr = ipv4_addr;
						filter_node->ip_dest.ipv6_dst = false;
					}

					filter_node->dma_ch = filter_map->channel;
					filter_node->tc_prio = temp->tc_prio;
					INIT_LIST_HEAD(&filter_node->node);
					list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
				} else {
					cnt1++;
				}
			}
			if (cnt == cnt1) {
				not_a_filter = true;
				break;
			} else {
				not_a_filter = false;
				cnt = 0;
				cnt1 = 0;
			}
		} else {
			not_a_filter = true;
			break;
		}
	}

	if (not_a_filter)
		unique = false;
	else
		unique = is_param_unique(&qos_tables.dma_filter_table, DEST_IP);
	if (unique) {
		filter = DEST_IP;
		ioss_qos_dev_log(NULL, "[ioss qos] Unique filter DEST_IP\n");
		goto filter_found;
	} else {
		/* do cleanup*/
		delete_filter_table(&qos_tables.dma_filter_table);
	}

	/*check if src ip addr is unique in filter table*/
	cnt = 0;
	cnt1 = 0;
	count = 0;
	not_a_filter = false;
	list_for_each_entry(temp, qos_rx, node) {
		/* If TC is for HW path and no IPA ch are available don't add to the filter table*/
		if (temp->action == IOSS_QOS_HW_PATH && !qos_tables.ipa_qos_rx_ch)
			continue;

		/* if queue is assigned don't do dma filtering */
		filter_map = (struct filter_map_info *)(temp->filter_info);
		if (filter_map->queue)
			continue;
		if (temp->src.len) {
			for(i = 0; i < temp->src.len; i++) {
				cnt++;
				if (temp->src.arr[i].address.ss_family) {
					filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
					if (!filter_node)
						goto alloc_err;

					convert_ip_addr_to_str(&(temp->src.arr[i].address), &ipv4_addr, ipv6_addr);
					filter_node->ip_src.src_mask_length = temp->src.arr[i].mask_length;
					if (temp->src.arr[i].address.ss_family == AF_INET6) {
						for (i = 0; i < 16; i++)
							filter_node->ip_src.ipv6_src_addr[i] = ipv6_addr[i];
						filter_node->ip_src.ipv6_src = true;
					} else {
						filter_node->ip_src.ipv4_src_addr = ipv4_addr;
						filter_node->ip_src.ipv6_src = false;
					}
					filter_map = (struct filter_map_info *)(temp->filter_info);
					filter_node->dma_ch = filter_map->channel;
					filter_node->tc_prio = temp->tc_prio;
					INIT_LIST_HEAD(&filter_node->node);
					list_add_tail(&filter_node->node, &qos_tables.dma_filter_table);
				} else {
					cnt1++;
				}
			}
			if (cnt1 == cnt) {
				not_a_filter = true;
				break;
			} else {
				not_a_filter = false;
				cnt = 0;
				cnt1 = 0;
			}
		} else {
			not_a_filter = true;
			break;
		}
	}

	if (not_a_filter)
		unique = false;
	else
		unique = is_param_unique(&qos_tables.dma_filter_table, SRC_IP);
	if (unique) {
		filter = SRC_IP;
		ioss_qos_dev_log(NULL, "[ioss qos] Unique filter SRC_IP\n");
		goto filter_found;
	} else {
		/* do cleanup*/
		delete_filter_table(&qos_tables.dma_filter_table);
	}

	kfree(filter_map);
	return filter;

filter_found:
	if (filter != PCP) {
		list_for_each_entry(filter_node_temp, &qos_tables.dma_filter_table, node) {
			qos_tables.filter_cnt++;
		}
	}
	ioss_qos_dev_log(NULL, "[ioss qos] No. of filters to be installed = %d\n\n", qos_tables.filter_cnt);
	kfree(filter_map);
	return filter;

alloc_err:
	ioss_qos_dev_err(NULL, "Filter node allocation failed");
	delete_filter_table(&qos_tables.dma_filter_table);
	kfree(filter_map);
	return INVALID_FILTER;
}

inline bool stmmac_is_phy_link_up(struct stmmac_priv *priv)
{
	if (priv->plat->mac2mac_en || priv->plat->fixed_phy_mode)
		return priv->plat->mac2mac_link;
	else
		return (priv->dev->phydev &&
			priv->dev->phydev->link);
}

static int stmmac_get_cbs_avail_bw(int speed)
{
	int bw_avail;

	switch (speed) {
	case SPEED_10000:
		bw_avail = 9500;
		break;
	case SPEED_5000:
		bw_avail = 4750;
		break;
	case SPEED_2500:
		bw_avail = 2350;
		break;
	case SPEED_1000:
		bw_avail = 950;
		break;
	case SPEED_100:
		bw_avail = 90;
		break;
	case SPEED_10:
		bw_avail = -1;
		break;
	default:
		bw_avail = 0;
	}
	return bw_avail;
}


const char* idx_action_to_string(enum idx_action action)
{
	switch (action) {
	case IDX_UNUSED:
		return "IDX_UNUSED";
	case IDX_USED:
		return "IDX_USED";
	case IDX_CLEAR:
		return "IDX_CLEAR";
	default:
		return "UNKNOWN";
	}
}

void dump_qos_priv(struct ioss_device* idev, struct stmmac_priv *priv)
{
	int i;
	ioss_qos_dev_log(idev, "### START PRIV DUMP");
	ioss_qos_dev_log(idev, "\tpriv->unique_filter_old = %d", priv->unique_filter_old);
	ioss_qos_dev_log(idev, "\tpriv->unique_filter_new = %d", priv->unique_filter_new);
	ioss_qos_dev_log(idev, "\tpriv->max_filters_old = %d", priv->max_filters_old);
	ioss_qos_dev_log(idev, "\tpriv->max_filters_new = %d", priv->max_filters_new);

	for (i = 0; i < MTL_MAX_RX_QUEUES; i++) {
		ioss_qos_dev_log(idev, "\tpriv->queue_dis[%d] = %d", i, priv->queue_dis[i]);
	}
	for (i = 0; i < 32; i++) {
		ioss_qos_dev_log(idev, "\tpriv->app_filters[%d].action = %s  ,  DMA_CH = %u",
					i, idx_action_to_string(priv->app_filters[i].action),
					priv->app_filters[i].dma_ch);
	}

	for (i = 0; i < MTL_MAX_RX_QUEUES; i++)
		ioss_qos_dev_log(idev, "\tpriv->is_rx_sw[%d] = %d", i, priv->is_rx_sw[i]);
	for (i = 0; i < MTL_MAX_TX_QUEUES; i++)
		ioss_qos_dev_log(idev, "\tpriv->is_tx_sw[%d] = %d", i, priv->is_tx_sw[i]);
	for (i = 0; i < MTL_MAX_RX_QUEUES; i++)
		ioss_qos_dev_log(idev, "\tpriv->queue_pcp_map[%d] = %u", i, priv->queue_pcp_map[i]);
	for (i = 0; i < MTL_MAX_TX_QUEUES; i++)
		ioss_qos_dev_log(idev, "\tpriv->tx_queue_pcp_map[%d] = %u", i, priv->tx_queue_pcp_map[i]);
	for (i = 0; i < MTL_MAX_TX_QUEUES; i++)
		ioss_qos_dev_log(idev, "\tpriv->tx_ch_bw[%d] = %u", i, priv->tx_ch_bw[i]);
	ioss_qos_dev_log(idev, "END PRIV DUMP ###");

}

static struct response stmmac_prepare_qos_info(struct ioss_device *idev, struct list_head *qos_rx, struct list_head *qos_tx)
{
	struct stmmac_priv *priv = netdev_priv(idev->net_dev);
	struct filter_map_info *filter_info;
	int i = 0, j = 0, k = 0;
	int num_rx_tc = 0, num_tx_tc = 0;
	int num_rx_sw_tc = 0, num_tx_sw_tc = 0;
	int num_rx_hw_tc = 0, num_tx_hw_tc = 0;
	u8 sw_ch = 0, hw_ch = 0, rx_ch_avail = 0, tx_avail = 0, rx_queue_avail = 0;
	u8 sw_queue = 0, hw_queue = 0;
	u8 channel = 0;
	struct qos_routing_rx *temp_rx;
	struct qos_routing_tx *temp_tx, *temp_tx_next;
	int qos_rx_queues = priv->plat->rx_qos_queues_to_use;
	int qos_tx_queues = priv->plat->tx_qos_queues_to_use;
	u8 pcp_mask = 0, pcp_mask_old = 0;
	bool min_bw_exceed = false;
	int bw_total_min = 0;
	int bw_avail = 1000;
	int aggr_bw = 0;
	struct response map_info;
	struct filter_map_info *filter_map;
	struct pcp_routing *filter_node_pcp, *temp_pcp_node;
	struct dma_filter_table *temp_filter_node, *filter_node;
	bool pcp_not_unique = false;
	bool flt_appd = false;
	u8 tx_hw_qos_ch_available = 0;
	u8 old_tx_queue_pcp_map[MTL_MAX_TX_QUEUES];
	ssize_t intf_width;

        if (!priv->plat->qos_supported) {
                map_info.qos_response_status  = QOS_COMMIT_FAIL;
                ioss_qos_dev_err(idev, "EMAC QOS not enabled");
                return map_info;
        }

	/* Cleanup the used tables */
	if (priv->plat->qos_active) {
		priv->unique_filter_old = priv->unique_filter_new;
		priv->max_filters_old = priv->max_filters_new;
		if (priv->unique_filter_old != PCP) {
			if (&qos_tables.dma_filter_table)
				delete_filter_table(&qos_tables.dma_filter_table);
			if (&qos_tables.flt_to_app)
				delete_filter_table(&qos_tables.flt_to_app);
		}
		if (&qos_tables.pcp_route_table)
			delete_route_table(&qos_tables.pcp_route_table);
	}

	memset(&qos_tables, 0, sizeof(struct qos_struct));
	INIT_LIST_HEAD(&qos_tables.pcp_route_table);
	INIT_LIST_HEAD(&qos_tables.flt_to_app);
	map_info.qos_response_status = QOS_COMMIT_EMPTY;
	/* First time initialization before enabling qos (after clear qos) */
	if (!priv->plat->qos_active) {
		priv->max_filters_old = 32;
		for (i = 0; i < priv->plat->rx_qos_queues_to_use; i++) {
			if (i == 0)
				priv->is_rx_sw[i] = 0;
			else
				priv->is_rx_sw[i] = 1;
		}

		for (i = 0; i < priv->plat->tx_qos_queues_to_use; i++) {
			if (i == 0)
				priv->is_tx_sw[i] = 0;
			else
				priv->is_tx_sw[i] = 1;
		}
	}
	/*Initialize channel info*/
	for (i = 0; i < priv->plat->rx_qos_queues_to_use; i++) {
		if (i == 0)
			qos_tables.rx_channel_info[i] = IOSS_QOS_HW_PATH;
		else
			qos_tables.rx_channel_info[i] = IOSS_QOS_SW_PATH;
	}

	for (i = 0; i < priv->plat->tx_qos_queues_to_use; i++) {
		old_tx_queue_pcp_map[i] = priv->tx_queue_pcp_map[i];
		priv->tx_queue_pcp_map[i] = 0;
		if (i == 0)
			qos_tables.tx_channel_info[i] = IOSS_QOS_HW_PATH;
		else
			qos_tables.tx_channel_info[i] = IOSS_QOS_SW_PATH;
	}


	if (qos_rx_queues < 3 || qos_tx_queues < 3) {
		ioss_qos_dev_err(idev, "No. of TX/RX queues not sufficient for QOS\n");
		map_info.qos_response_status = QOS_COMMIT_FAIL;
	} else {
		/*start rx aggr and filter*/
		/* Get no. of qos queues available and aggregation logic */
		qos_tables.ipa_qos_rx_ch = idev->qos_rx_channels;
		num_rx_tc = stmmac_get_rx_tc_info(qos_rx, SW_HW_PATH);
		ioss_qos_dev_log(idev, "num_rx_tc = %d\n", num_rx_tc);
		num_rx_sw_tc = stmmac_get_rx_tc_info(qos_rx, SW_PATH);
		ioss_qos_dev_log(idev, "num_rx_sw_tc = %d\n", num_rx_sw_tc);
		num_rx_hw_tc = stmmac_get_rx_tc_info(qos_rx, HW_PATH);
		ioss_qos_dev_log(idev, "num_rx_hw_tc = %d\n", num_rx_hw_tc);

		/* Defualt pcp 0 goes to queue 0. We will be left with 4 queues. We need to
		 * seperate these 4 queues among all HW and SW TCs.
		 * Assumptions:
		 * 1.Each TC can have multiple PCP's
		 * 2.Multiple TC can share same PCP
		 * 3.HW and SW TC's will not share same PCP value
		 */

		rx_ch_avail = qos_rx_queues - 2;
		rx_queue_avail = qos_rx_queues - 2; // Q0 reserved for BE, Q1 reserved for HW TC

		list_for_each_entry(temp_rx, qos_rx, node)	{
			if (!qos_tables.ipa_qos_rx_ch && temp_rx->action == IOSS_QOS_HW_PATH)
				continue;

			filter_info = kzalloc(sizeof(struct filter_map_info), GFP_KERNEL);
			filter_node_pcp = kzalloc(sizeof(struct pcp_routing), GFP_KERNEL);
			if (rx_queue_avail) {
				if (temp_rx->pcp.len) {
					/*PCP exists, check if its non-zero*/
					pcp_mask_old = pcp_mask;
					for (i = 0; i < temp_rx->pcp.len; i++) {
						if (temp_rx->pcp.arr[i]) {
							if (pcp_mask & (1 << temp_rx->pcp.arr[i])) {
								ioss_qos_dev_log(idev, "pcp %d for TC %d already used by other high priority TC\n",
									    		 temp_rx->pcp.arr[i], temp_rx->tc_prio);
							} else {
								pcp_mask |= 1 << temp_rx->pcp.arr[i];
							}
							filter_node_pcp->pcp |= 1 << temp_rx->pcp.arr[i];
						} else {
							pcp_not_unique = true;
							if (temp_rx->action == IOSS_QOS_SW_PATH)
								filter_info->channel = 3;
							else if (temp_rx->action == IOSS_QOS_HW_PATH)
								filter_info->channel = 2;
						}
					}

					/* Assign queue if pcp is not used already */
					if (pcp_mask != pcp_mask_old) {
						if (temp_rx->action == IOSS_QOS_HW_PATH && qos_tables.ipa_qos_rx_ch) {
							/* HW TC always goes to Q1 and CH2 */
							filter_info->queue = 1;
							filter_info->channel = 2;
							filter_node_pcp->path = IOSS_QOS_HW_PATH;
						} else if (temp_rx->action == IOSS_QOS_SW_PATH) {
							if (rx_queue_avail == 1) {
								/* No more queues are available, assign to Q2 */
								sw_queue = 2;
							} else if (rx_queue_avail > 1) {
								/* queues are available, so assign one (range from Q2 to Qx) */
								sw_queue = rx_queue_avail + 1;
								rx_queue_avail--;
							}
							filter_info->queue = sw_queue;
							if (qos_tables.queue_to_ch_map[sw_queue]) {
								for (i = 1; i < priv->plat->rx_qos_queues_to_use; i++) {
									if (i == filter_info->queue ) {
										filter_info->channel = qos_tables.queue_to_ch_map[i];
									}
								}
							}
							filter_info->channel = rx_ch_avail + 1;
							/* SW TC should use CH3 and above*/
							if (rx_ch_avail >= 3 ) {
								rx_ch_avail--;
							}
							filter_node_pcp->path = IOSS_QOS_SW_PATH;
						}
					} else {
						/* go over existing pcp route table to match the pcp mask
						 * and assign queue-ch info accordingly for the tc*/
						 list_for_each_entry(temp_pcp_node, &qos_tables.pcp_route_table, node) {
							if (temp_pcp_node->pcp & filter_node_pcp->pcp) {
								filter_info->queue = temp_pcp_node->queue;
								filter_info->channel = temp_pcp_node->dma_ch;
								filter_node_pcp->path = temp_pcp_node->path;
								break;
							}
						 }
					}
				} else {
					/* for a TC if PCP is not given queue will be zero
					 * if HW TC, we can assign CH2, for SW TC assign CH 3
					 * as no priority at CH level for SW traffic
					 */
					filter_info->queue = 0;
					if (temp_rx->action == IOSS_QOS_HW_PATH && qos_tables.ipa_qos_rx_ch) {
						filter_info->channel = 2;
					} else if (temp_rx->action == IOSS_QOS_SW_PATH) {
						filter_info->channel = 3;
					}
					pcp_not_unique = true;
				}
			}
			qos_tables.pipe_map.pipe_to_tc_mapping_rx[filter_info->channel] |= (1 << temp_rx->tc_prio);
			qos_tables.rx_channel_info[filter_info->channel] = temp_rx->action;
			qos_tables.queue_to_ch_map[filter_info->queue] = filter_info->channel;
			temp_rx->filter_info = (void *)(filter_info);
			filter_node_pcp->queue = filter_info->queue;
			filter_node_pcp->dma_ch = filter_info->channel;
			filter_node_pcp->tc_prio = temp_rx->tc_prio;
			INIT_LIST_HEAD(&filter_node_pcp->node);
			list_add_tail(&filter_node_pcp->node, &qos_tables.pcp_route_table);
		}

		list_for_each_entry(temp_rx, qos_rx, node) {
			if (!qos_tables.ipa_qos_rx_ch && temp_rx->action == IOSS_QOS_HW_PATH) {
				ioss_qos_dev_log(idev, "RX TC = %d skipped <IPA HW QOS Channel unavailable>\n", temp_rx->tc_prio);
				continue;
			}

			filter_map = (struct filter_map_info *)(temp_rx->filter_info);
			qos_tables.tc_to_queue_map[temp_rx->tc_prio] = filter_map->queue;

			ioss_qos_dev_log(idev, "TC = %d, action = %d, queue = %d, ch = %d\n",
							 temp_rx->tc_prio, temp_rx->action,
							 filter_map->queue, filter_map->channel);
		}

		/*prepare queue_to_ch map*/
		list_for_each_entry(temp_pcp_node, &qos_tables.pcp_route_table, node) {
			qos_tables.queue_to_ch_map[temp_pcp_node->queue] = temp_pcp_node->dma_ch;
		}

		ioss_qos_dev_log(idev, "RX aggregation done\n");

		if (pcp_not_unique) {
			priv->unique_filter_new = find_filter(qos_rx);
		} else {
			priv->unique_filter_new = PCP;
		}
		if (priv->unique_filter_new == INVALID_FILTER) {
			map_info.qos_response_status = QOS_COMMIT_FAIL;
			return map_info;
		}

		/*start tx allocation*/
		qos_tables.ipa_qos_tx_ch = idev->qos_tx_channels;
		tx_hw_qos_ch_available = qos_tables.ipa_qos_tx_ch;

		num_tx_tc = stmmac_get_tx_tc_count(qos_tx, SW_HW_PATH);
		ioss_qos_dev_log(idev, "num_tx_tc = %d\n", num_tx_tc);
		num_tx_sw_tc = stmmac_get_tx_tc_count(qos_tx, SW_PATH);
		ioss_qos_dev_log(idev, "num_tx_sw_tc = %d\n", num_tx_sw_tc);
		num_tx_hw_tc = stmmac_get_tx_tc_count(qos_tx, HW_PATH);
		ioss_qos_dev_log(idev, "num_tx_hw_tc = %d\n", num_tx_hw_tc);

		tx_avail = qos_tx_queues - 1;

		if(!stmmac_is_phy_link_up(priv)) {
			ioss_qos_dev_log(idev, "Link is down : CBS Params can't be calculated \n");
			map_info.qos_response_status = QOS_COMMIT_LINK_DOWN;
			goto err_inval_speed;
		}

		list_for_each_entry(temp_tx, qos_tx, node) {
			if ((tx_hw_qos_ch_available <= 0) && (temp_tx->action == IOSS_QOS_HW_PATH)) {
				ioss_qos_dev_err(idev, "Not enough HW pipes, skipping TC %d\n",temp_tx->tc_prio);
				map_info.qos_response_status = QOS_COMMIT_FAIL;
				goto err_queue_exhaust;
			}
			if (temp_tx->action == IOSS_QOS_HW_PATH)
				tx_hw_qos_ch_available--;
		}

		/*CBS claculation*/
		bw_avail = stmmac_get_cbs_avail_bw(priv->speed);
		if (bw_avail < 0 ) {
			map_info.qos_response_status = QOS_COMMIT_FAIL;
			ioss_qos_dev_err(idev, "Invalid Speed for QOS\n");
			goto err_inval_speed;
		}

		// Check if minimum bw requirement can be sufficed for all TC
		list_for_each_entry_safe(temp_tx, temp_tx_next, qos_tx, node) {
			bw_total_min += temp_tx->cbs_bw.low_bw;
			if (bw_total_min > bw_avail) {
				min_bw_exceed = true;
				temp_tx->disabled = true;
			} else {
				temp_tx->disabled = false;
			}
		}

		qos_adjust_txq_cbs_bw(qos_tx, bw_avail);
		list_for_each_entry(temp_tx, qos_tx, node) {
			if(temp_tx->disabled)
				continue;
			if (temp_tx->bw_allocated < temp_tx->cbs_bw.low_bw) {
				map_info.qos_response_status = QOS_COMMIT_BW_EXHAUST;
				ioss_qos_dev_err(idev, "BW EXHAUSTED for TX TC %d\n",temp_tx->tc_prio);
				goto err_bw_exhaust;
			}

			if (tx_avail > 1) {
				channel = tx_avail;
				tx_avail--;
			}
			ioss_qos_dev_log(idev, "allocated bw for tc = %d, ch %d = %d\n", temp_tx->tc_prio, channel, temp_tx->bw_allocated);
			qos_tables.pipe_map.pipe_to_tc_mapping_tx[channel] = 1 << temp_tx->tc_prio;
			qos_tables.tx_routing_info[channel].acc_bw = temp_tx->bw_allocated;
			/* As qos_tables is global and we are memsetting to 0 after clear, default mode to use should be MTL_QUEUE_AVB*/
			if (qos_tables.tx_routing_info[channel].acc_bw &&
			    (qos_tables.tx_routing_info[channel].mode_to_use != MTL_QUEUE_DCB))
				qos_tables.tx_routing_info[channel].mode_to_use = MTL_QUEUE_AVB;
			else
				qos_tables.tx_routing_info[channel].mode_to_use = MTL_QUEUE_DCB;
			temp_tx->tx_param_info = (void *)(channel);
			qos_tables.tx_channel_info[channel] = temp_tx->action;
			if (temp_tx->action == IOSS_QOS_SW_PATH) {
				for (i = 0; i < temp_tx->pcp.len; i++) {
					priv->tx_queue_pcp_map[channel] |= 1 << temp_tx->pcp.arr[i];
                                  	ioss_qos_dev_log(idev, "tx queue = %d, pcp = %d\n",
                                                         channel, priv->tx_queue_pcp_map[channel]);
				}

				if (priv->tx_queue_pcp_map[channel] != old_tx_queue_pcp_map[channel]) {
					map_info.qos_response_status = QOS_COMMIT_SUCCESS;
					ioss_qos_dev_log(idev, "Response TC TX ch SW PCP change = %d\n", map_info.qos_response_status);
				}

			}
		}

		if (min_bw_exceed) {
			map_info.qos_response_status = QOS_COMMIT_BW_EXHAUST;
			ioss_qos_dev_err(idev, "BW EXHAUSTED. Cannot suffice minimun bw requirement");
		}

		intf_width = stmmac_intf_width(priv);
		if (intf_width < 0) {
			ioss_qos_dev_err(idev, "Unable to determine MAC-PCS interface width. Interface: %d, Speed: %d\n",
					 priv->plat->interface, priv->speed);
			map_info.qos_response_status = QOS_COMMIT_FAIL;
			goto err_inval_interface;
		}

		for (i = priv->plat->tx_queues_to_use - 1; i > 1; i--) {
			if (qos_tables.tx_routing_info[i].mode_to_use == MTL_QUEUE_AVB) {
				qos_tables.tx_routing_info[i].idle_slope = intf_width*1024*qos_tables.tx_routing_info[i].acc_bw/priv->speed;
				qos_tables.tx_routing_info[i].send_slope = (priv->speed - qos_tables.tx_routing_info[i].acc_bw)*intf_width*1024/priv->speed;
				qos_tables.tx_routing_info[i].hi_credit = MAX_INTERFERENCE_SIZE*8*1024;
				qos_tables.tx_routing_info[i].low_credit = (-1)*MAX_FRAME_SIZE*8*1024;
			}
		}
		/*end tx allocation*/

		ioss_qos_dev_log(idev, "[iemac qos]: send_slope %d idle_slope = %d hi_credit = %d low_credit = %d\n",
			         		 qos_tables.tx_routing_info[i].send_slope,
							 qos_tables.tx_routing_info[i].idle_slope,
							 qos_tables.tx_routing_info[i].hi_credit,
					  		 qos_tables.tx_routing_info[i].low_credit);

		if (!priv->plat->qos_active)
			stmmac_backup_pcp(priv, &qos_tables);

		for (i = 0; i < priv->plat->tx_queues_to_use; i++) {
			if (i < 2) {
				map_info.qos_pipe_mapping.pipe_to_tc_mapping_tx[0] = 0;
				map_info.qos_pipe_mapping.pipe_to_tc_mapping_tx[1] = 0;
				map_info.qos_pipe_mapping.is_tx_tc_sw[0] = 0;
				map_info.qos_pipe_mapping.is_tx_tc_sw[1] = 1;
			} else {
				map_info.qos_pipe_mapping.pipe_to_tc_mapping_tx[i] = qos_tables.pipe_map.pipe_to_tc_mapping_tx[i];
				map_info.qos_pipe_mapping.is_tx_tc_sw[i] = qos_tables.tx_channel_info[i] == IOSS_QOS_HW_PATH? 0: 1;
				priv->plat->qos_ch_map.ch_to_tc_map_tx[i] = qos_tables.pipe_map.pipe_to_tc_mapping_tx[i];
				priv->plat->qos_ch_map.tc_tx_info[i] = qos_tables.tx_channel_info[i] == IOSS_QOS_SW_PATH? 1: 0;
			}

			ioss_qos_dev_log(idev, "tx ch = %d pipe map = %d\n",
							 i, map_info.qos_pipe_mapping.pipe_to_tc_mapping_tx[i]);
			ioss_qos_dev_log(idev, "tx ch = %d\n info = %d\n\n",
							 i, map_info.qos_pipe_mapping.is_tx_tc_sw[i]);
		}

		for (i = 0; i < priv->plat->rx_queues_to_use; i++){
			if (i < 2) {
				map_info.qos_pipe_mapping.pipe_to_tc_mapping_rx[0] = 0;
				map_info.qos_pipe_mapping.pipe_to_tc_mapping_rx[1] = 0;
				map_info.qos_pipe_mapping.is_rx_tc_sw[0] = 0;
				map_info.qos_pipe_mapping.is_rx_tc_sw[1] = 1;
			} else {
				map_info.qos_pipe_mapping.pipe_to_tc_mapping_rx[i] = qos_tables.pipe_map.pipe_to_tc_mapping_rx[i];
				map_info.qos_pipe_mapping.is_rx_tc_sw[i] = qos_tables.rx_channel_info[i] == IOSS_QOS_HW_PATH? 0: 1;
				priv->plat->qos_ch_map.ch_to_tc_map_rx[i] = qos_tables.pipe_map.pipe_to_tc_mapping_rx[i];
				priv->plat->qos_ch_map.tc_rx_info[i] = qos_tables.rx_channel_info[i] == IOSS_QOS_SW_PATH? 1: 0;
			}
			ioss_qos_dev_log(idev, "rx ch = %d pipe map = %d\n",
							 i, map_info.qos_pipe_mapping.pipe_to_tc_mapping_rx[i]);
			ioss_qos_dev_log(idev, "rx ch = %d info = %d\n",
							 i, map_info.qos_pipe_mapping.is_rx_tc_sw[i]);
		}

		map_info.num_tx_pipes = qos_tx_queues;
		map_info.num_rx_pipes = qos_rx_queues;

	}

	/* Add comparison table here */
	/* first check SW/HW channels changed which needs pipe connect and disconnect */
	/* Alloc and dealloc in request ch */
	/* comment: add error code enum for map_info.err*/
	for (i = 2; i < priv->plat->rx_queues_to_use; i++) {
		/* A new RX TC might be added or deleted*/
		if ((map_info.qos_pipe_mapping.is_rx_tc_sw[i] != priv->is_rx_sw[i]) &&
		    map_info.qos_pipe_mapping.pipe_to_tc_mapping_rx[i]) {
			map_info.qos_response_status = QOS_COMMIT_SUCCESS;
			ioss_qos_dev_log(idev, "Response TC change for RX = %d, acc = %d, curr = %d, idx = %d\n",
							 map_info.qos_response_status, priv->is_rx_sw[i],
							 map_info.qos_pipe_mapping.is_rx_tc_sw[i], i);
			break;
		}
	}
	for (i = 2; i < priv->plat->tx_queues_to_use; i++) {
		/* A new TX TC might be added or deleted*/
		if (map_info.qos_pipe_mapping.is_tx_tc_sw[i] != priv->is_tx_sw[i] &&
		    map_info.qos_pipe_mapping.pipe_to_tc_mapping_tx[i])  {
			map_info.qos_response_status = QOS_COMMIT_SUCCESS;
			ioss_qos_dev_log(idev, "Response TC change TX = %d\n", map_info.qos_response_status);
			break;
		}
		if (qos_tables.tx_routing_info[i].mode_to_use != priv->plat->tx_queues_cfg[i].mode_to_use) {
			map_info.qos_response_status = QOS_COMMIT_SUCCESS;
			ioss_qos_dev_log(idev, "Response TC TX ch mode change for queue = %d\n", i);
			ioss_qos_dev_log(idev, "queue = %d cur mode to use = %d, old mode to use = %d,",i,
					 qos_tables.tx_routing_info[i].mode_to_use,
					 priv->plat->tx_queues_cfg[i].mode_to_use);
			break;
		} else {
			if (qos_tables.tx_routing_info[i].acc_bw != priv->tx_ch_bw[i]) {
				map_info.qos_response_status = QOS_COMMIT_SUCCESS;
				ioss_qos_dev_log(idev, "Response TC TX ch BW change = %d\n", map_info.qos_response_status);
			}
		}
	}
	/* Prepare queue to pcp mapping and check if PCP table has changed*/
	pcp_mask_old = 0;
	pcp_mask = pcp_mask_old;
	for (i = priv->plat->rx_qos_queues_to_use - 1; i > 0; i--) {
		list_for_each_entry(temp_pcp_node, &qos_tables.pcp_route_table, node) {
			if (temp_pcp_node->queue == i) {
				pcp_mask |= temp_pcp_node->pcp;
				if (pcp_mask != pcp_mask_old) {
					/* TC having multiple PCP's but atleast one pcp is common*/
					if (pcp_mask_old & temp_pcp_node->pcp) {
						temp_pcp_node->pcp = pcp_mask - pcp_mask_old;
					}
					ioss_qos_dev_log(idev, "prio = %d, pcp = %d, queue = %d\n",
							 temp_pcp_node->tc_prio, temp_pcp_node->pcp, temp_pcp_node->queue);
					qos_tables.queue_to_pcp_map[i] |= temp_pcp_node->pcp;
				}
				pcp_mask_old = pcp_mask;
			}
		}
		ioss_qos_dev_log(idev, "queue_pcp_map[%d] = %d\n", i, qos_tables.queue_to_pcp_map[i]);
	}
	for (i = 1; i < priv->plat->rx_qos_queues_to_use; i++) {
		if (qos_tables.queue_to_pcp_map[i] != priv->queue_pcp_map[i]) {
			map_info.qos_response_status = QOS_COMMIT_SUCCESS;
			ioss_qos_dev_log(idev, "Response pcp map change = %d\n", map_info.qos_response_status);
			break;
		}
	}

	/* Check if dma filter table changed */
	/* IF filter table changed find the filters to be applied and deleted/modified */
	if (priv->unique_filter_new != PCP || priv->unique_filter_old != PCP) {
		if(priv->unique_filter_new == VLAN_ID)
			priv->max_filters_new = priv->dma_cap.nrvf_num;
		else if(priv->unique_filter_new >= SRC_IP && priv->unique_filter_new <= DEST_PORT)
			priv->max_filters_new = priv->dma_cap.l3l4fnum;
		else
			priv->max_filters_new = 0;

		ioss_qos_dev_log(idev, "Max filters new = %d, Max filters old = %d\n", priv->max_filters_new, priv->max_filters_old);
		ioss_qos_dev_log(idev, "Printing new dma_filter_table\n");
		if (priv->max_filters_new) {
			list_for_each_entry(temp_filter_node, &qos_tables.dma_filter_table, node) {
				switch (priv->unique_filter_new) {
				case VLAN_ID:
					ioss_qos_dev_log(idev, "vlanid %d - ch %d\n", temp_filter_node->vlan_id, temp_filter_node->dma_ch);
					break;
				case SRC_IP:
					if (!temp_filter_node->ip_src.ipv6_src) {
						ioss_qos_dev_log(idev, "SRC_IP/MASK = %pI4l/%d - ch %d\n", &temp_filter_node->ip_src.ipv4_src_addr,
								 temp_filter_node->ip_src.src_mask_length, temp_filter_node->dma_ch);
					}
					break;
				case DEST_IP:
					if (!temp_filter_node->ip_dest.ipv6_dst) {
						ioss_qos_dev_log(idev, "DST_IP/MASK = %pI4l/%d - ch = %d\n",&temp_filter_node->ip_dest.ipv4_dst_addr,
								 temp_filter_node->ip_dest.dst_mask_length, temp_filter_node->dma_ch);
					}
					break;
				case SRC_PORT:
					ioss_qos_dev_log(idev, "SRC_PORT/PROTO = %d/%d - ch = %d\n", temp_filter_node->src_port.port_num,
							 temp_filter_node->src_port.proto, temp_filter_node->dma_ch);
					break;
				case DEST_PORT:
					ioss_qos_dev_log(idev, "DST_PORT/PROTO = %d/%d - ch = %d\n", temp_filter_node->dst_port.port_num,
							 temp_filter_node->dst_port.proto, temp_filter_node->dma_ch);
					break;
				default:
					break;
				}
			}

			/* Add filters to be installed */
			/* Check if new unique filter is different */
			ioss_qos_dev_log(idev, "New filter = %d, Old filter = %d \n", priv->unique_filter_new, priv->unique_filter_old);
			if (priv->unique_filter_new != priv->unique_filter_old)  {
				/* Delete existing HW filter table as complete filter table changed */
				memset(&priv->app_filters, 0, priv->max_filters_old*sizeof(struct dma_flt));
				/* Assign new filter table to new flt_to_app as both will be same */
				map_info.qos_response_status = QOS_COMMIT_SUCCESS;
				ioss_qos_dev_log(idev, "Response filter change = %d\n", map_info.qos_response_status);
			} else {
				/* Search for filters in existing table and add the filters whichever are required */
				/* Check DMA CH as well */
				list_for_each_entry(temp_filter_node, &qos_tables.dma_filter_table, node) {
					for (i = 0; i < priv->max_filters_new; i++) {
						if (priv->app_filters[i].action == IDX_UNUSED && i != (priv->max_filters_new - 1))
							continue;
						switch (priv->unique_filter_new) {
						case VLAN_ID:
							if (temp_filter_node->vlan_id == priv->app_filters[i].vlan_id &&
							    temp_filter_node->dma_ch==  priv->app_filters[i].dma_ch) {
								ioss_qos_dev_log(idev, "filter is already applied and at index = %d", i);
								flt_appd = true;
							} else {
								/* The new filter is not present, add it to the flt_app list*/
								if (i == priv->max_filters_new - 1) {
									filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
									filter_node->vlan_id = temp_filter_node->vlan_id;
								}
							}
							break;
						case SRC_IP:
							if (temp_filter_node->ip_src.ipv6_src) {
								for (j = 0; j < 16; j++) {
									if (priv->app_filters[i].ip_src.ipv6_src_addr[j] == temp_filter_node->ip_src.ipv6_src_addr[j])
										k++;
								}
								if (k == 16 && priv->app_filters[i].dma_ch == temp_filter_node->dma_ch) {
									ioss_qos_dev_log(idev, "filter already applied at index = %d\n", i);
									k = 0;
									flt_appd = true;
								} else {
									if(i == priv->max_filters_new - 1) {
										filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
										for (j = 0; j < 16; j++)
											filter_node->ip_src.ipv6_src_addr[j] = temp_filter_node->ip_src.ipv6_src_addr[j];
										filter_node->ip_src.src_mask_length = temp_filter_node->ip_src.src_mask_length;
										filter_node->ip_src.ipv6_src = true;
									}
								}
							} else {
								if (temp_filter_node->ip_src.ipv4_src_addr == priv->app_filters[i].ip_src.ipv4_src_addr &&
								    temp_filter_node->dma_ch == priv->app_filters[i].dma_ch) {
									ioss_qos_dev_log(idev, "filter is already applied at index = %d\n", i);
									flt_appd = true;
								} else {
									if (i == priv->max_filters_new - 1) {
										filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
										filter_node->ip_src.ipv4_src_addr = temp_filter_node->ip_src.ipv4_src_addr;
										filter_node->ip_src.src_mask_length = temp_filter_node->ip_src.src_mask_length;
										filter_node->ip_src.ipv6_src = false;
									}
								}
							}
							break;
						case DEST_IP:
							if (temp_filter_node->ip_dest.ipv6_dst) {
								for (j = 0; j < 16; j++) {
									if (priv->app_filters[i].ip_dest.ipv6_dst_addr[j] == temp_filter_node->ip_dest.ipv6_dst_addr[j])
										k++;
								}
								if (k == 16 && priv->app_filters[i].dma_ch == temp_filter_node->dma_ch) {
									ioss_qos_dev_log(idev, "filter is already applied at index = %d\n", i);
									k = 0;
									flt_appd = true;
								} else {
									if (i == priv->max_filters_new - 1) {
										filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
										for (j = 0; j < 16; j++)
											filter_node->ip_dest.ipv6_dst_addr[j] = temp_filter_node->ip_dest.ipv6_dst_addr[j];
										filter_node->ip_dest.dst_mask_length = temp_filter_node->ip_dest.dst_mask_length;
										filter_node->ip_dest.ipv6_dst = true;

									}
								}
							} else {
								if (temp_filter_node->ip_dest.ipv4_dst_addr == priv->app_filters[i].ip_dest.ipv4_dst_addr &&
								    temp_filter_node->dma_ch == priv->app_filters[i].dma_ch) {
									ioss_qos_dev_log(idev, "filter is already applied at index = %d\n", i);
									flt_appd = true;
								} else {
									if (i == priv->max_filters_new - 1) {
										filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
										filter_node->ip_dest.ipv4_dst_addr = temp_filter_node->ip_dest.ipv4_dst_addr;
										filter_node->ip_dest.dst_mask_length = temp_filter_node->ip_dest.dst_mask_length;
										filter_node->ip_dest.ipv6_dst = false;
									}
								}
							}
							break;
						case SRC_PORT:
							if (temp_filter_node->src_port.proto == priv->app_filters[i].src_port.proto &&
							    temp_filter_node->src_port.port_num == priv->app_filters[i].src_port.port_num &&
							    temp_filter_node->dma_ch==  priv->app_filters[i].dma_ch) {
								ioss_qos_dev_log(idev, "filter is already applied at index = %d\n", i);
								flt_appd = true;
							} else {
								/* The new filter is not present, add it to the flt_app list*/
								if(i == priv->max_filters_new - 1) {
									filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
									filter_node->src_port.port_num = temp_filter_node->src_port.port_num;
									filter_node->src_port.proto = temp_filter_node->src_port.proto;
								}
							}
							break;
						case DEST_PORT:
							if (temp_filter_node->dst_port.proto == priv->app_filters[i].dst_port.proto &&
							    temp_filter_node->dst_port.port_num == priv->app_filters[i].dst_port.port_num &&
							    temp_filter_node->dma_ch==  priv->app_filters[i].dma_ch) {
								ioss_qos_dev_log(idev, "filter is already applied at index = %d\n", i);
								flt_appd = true;
							} else {
								/* The new filter is not present, add it to the flt_app list*/
								if(i == priv->max_filters_new - 1) {
									filter_node = kzalloc(sizeof(struct dma_filter_table), GFP_KERNEL);
									filter_node->dst_port.port_num = temp_filter_node->dst_port.port_num;
									filter_node->dst_port.proto = temp_filter_node->dst_port.proto;
								}
							}
							break;
						default:
							break;
						}
						if (flt_appd) {
							flt_appd = false;
							break;
						}
					}
					if (i == priv->max_filters_new) {
						filter_node->dma_ch = temp_filter_node->dma_ch;
						filter_node->tc_prio = temp_filter_node->tc_prio;
						list_add_tail(&filter_node->node, &qos_tables.flt_to_app);
						map_info.qos_response_status = QOS_COMMIT_SUCCESS;
					}
				}
			}
		}
		/* Add filters to be deleted */
		/* Set action flag to IDX_CLEAR to delete them later */
		if (priv->unique_filter_new != priv->unique_filter_old) {
			/*If unique filter changed, need to delete all filters*/
			for (i = 0; i < priv->max_filters_old; i++) {
				if (priv->app_filters[i].action == IDX_USED)
					priv->app_filters[i].action = IDX_CLEAR;
			}
			map_info.qos_response_status = QOS_COMMIT_SUCCESS;
			ioss_qos_dev_log(idev, "Response filter change = %d\n", map_info.qos_response_status);
		} else {
			for (i = 0; i < priv->max_filters_new; i++) {
				if (priv->app_filters[i].action == IDX_UNUSED)
					continue;
				list_for_each_entry(temp_filter_node, &qos_tables.dma_filter_table, node) {
					switch (priv->unique_filter_new) {
					case VLAN_ID:
						if (temp_filter_node->vlan_id == priv->app_filters[i].vlan_id &&
							temp_filter_node->dma_ch ==  priv->app_filters[i].dma_ch) {
							flt_appd = true;
						} else {
							/* The applied filter is not needed, clear it*/
							if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
								priv->app_filters[i].action = IDX_CLEAR;
						}
						break;
					case SRC_IP:
						if (temp_filter_node->ip_src.ipv6_src) {
							for (j = 0; j < 16; j++) {
								if (priv->app_filters[i].ip_src.ipv6_src_addr[j] == temp_filter_node->ip_src.ipv6_src_addr[j])
									k++;
							}
							if (k == 16 && priv->app_filters[i].dma_ch == temp_filter_node->dma_ch) {
								k = 0;
								flt_appd = true;
							} else {
								/* The applied filter is not needed, clear it*/
								if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
									priv->app_filters[i].action = IDX_CLEAR;
							}

						} else {
							if (temp_filter_node->ip_src.ipv4_src_addr == priv->app_filters[i].ip_src.ipv4_src_addr &&
							    temp_filter_node->dma_ch == priv->app_filters[i].dma_ch) {
								flt_appd = true;
							} else {
								/* The applied filter is not needed, clear it*/
								if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
									priv->app_filters[i].action = IDX_CLEAR;
							}
						}
						break;
					case DEST_IP:
						if (temp_filter_node->ip_dest.ipv6_dst) {
							for (j = 0; j < 16; j++) {
								if (priv->app_filters[i].ip_dest.ipv6_dst_addr[j] == temp_filter_node->ip_dest.ipv6_dst_addr[j])
									k++;
							}
							if (k == 16 && priv->app_filters[i].dma_ch == temp_filter_node->dma_ch) {
								k = 0;
								flt_appd = true;
							} else {
								/* The applied filter is not needed, clear it*/
								if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
									priv->app_filters[i].action = IDX_CLEAR;
							}

						} else {
							if (temp_filter_node->ip_dest.ipv4_dst_addr == priv->app_filters[i].ip_dest.ipv4_dst_addr &&
							    temp_filter_node->dma_ch == priv->app_filters[i].dma_ch) {
								flt_appd = true;
							} else {
								/* The applied filter is not needed, clear it*/
								if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
									priv->app_filters[i].action = IDX_CLEAR;
							}
						}
						break;
					case SRC_PORT:
						if (temp_filter_node->src_port.proto == priv->app_filters[i].src_port.proto &&
						    temp_filter_node->src_port.port_num == priv->app_filters[i].src_port.port_num &&
						    temp_filter_node->dma_ch==  priv->app_filters[i].dma_ch) {
							flt_appd = true;
						} else {
							/* The applied filter is not needed, clear it*/
							if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
								priv->app_filters[i].action = IDX_CLEAR;
						}
						break;
					case DEST_PORT:
						if (temp_filter_node->dst_port.proto == priv->app_filters[i].dst_port.proto &&
						    temp_filter_node->dst_port.port_num == priv->app_filters[i].dst_port.port_num &&
						    temp_filter_node->dma_ch==  priv->app_filters[i].dma_ch) {
							flt_appd = true;
						} else {
							/* The applied filter is not needed, clear it*/
							if (list_is_last(&temp_filter_node->node, &qos_tables.dma_filter_table))
								priv->app_filters[i].action = IDX_CLEAR;
						}
						break;
					default:
						break;
					}
					if (flt_appd) {
						flt_appd = false;
						break;
					} else {
						map_info.qos_response_status = QOS_COMMIT_SUCCESS;
					}
				}
			}
		}
		/*debug code to print hw filters*/
		ioss_qos_dev_log(idev, "Printing existing HW filter\n");
		for (i = 0; i < priv->max_filters_old; i++)
		{
			if (priv->app_filters[i].action == IDX_UNUSED)
				continue;
			switch (priv->unique_filter_old) {
			case VLAN_ID:
				ioss_qos_dev_log(idev, "i = %d: vlanid = %d - ch =%d\n", i,
								 priv->app_filters[i].vlan_id, priv->app_filters[i].dma_ch);
				break;
			case SRC_IP:
				if (!priv->app_filters[i].ip_src.ipv6_src) {
					ioss_qos_dev_log(idev, "i = %d: src_ip/mask = %pI4l/%d - ch = %d\n", i,
							 &priv->app_filters[i].ip_src.ipv4_src_addr,
							 priv->app_filters[i].ip_src.src_mask_length, priv->app_filters[i].dma_ch);
				}
				break;
			case DEST_IP:
				if (!priv->app_filters[i].ip_dest.ipv6_dst) {
					ioss_qos_dev_log(idev, "i = %d: dst_ip/mask = %pI4l/%d - ch = %d\n", i,
							 &priv->app_filters[i].ip_dest.ipv4_dst_addr,
							 priv->app_filters[i].ip_dest.dst_mask_length, priv->app_filters[i].dma_ch);
				}
				break;
			case SRC_PORT:
				ioss_qos_dev_log(idev, "i = %d: src_port/proto = %d/%d - ch = %d\n", i, priv->app_filters[i].src_port.port_num,
								 priv->app_filters[i].src_port.proto, priv->app_filters[i].dma_ch);
				break;
			case DEST_PORT:
				ioss_qos_dev_log(idev, "i = %d: dst_port/proto = %d/%d - ch = %d\n", i, priv->app_filters[i].dst_port.port_num,
								 priv->app_filters[i].dst_port.proto, priv->app_filters[i].dma_ch);
				break;
			default:
				break;
			}
		}

		if (priv->max_filters_new) {
			ioss_qos_dev_log(idev, "Printing new filters to be installed\n");
			list_for_each_entry(temp_filter_node, &qos_tables.flt_to_app, node) {
				switch (priv->unique_filter_new) {
				case VLAN_ID:
					ioss_qos_dev_log(idev, "vlanid = %d - ch = %d\n", temp_filter_node->vlan_id, temp_filter_node->dma_ch);
					break;
				case SRC_IP:
					if (!temp_filter_node->ip_src.ipv6_src) {
						ioss_qos_dev_log(idev, "src_addr/mask = %pI4l/%d - ch = %d\n",
								 &temp_filter_node->ip_src.ipv4_src_addr,
								 temp_filter_node->ip_src.src_mask_length, temp_filter_node->dma_ch);
					}
					break;
				case DEST_IP:
					if (!temp_filter_node->ip_dest.ipv6_dst) {
						ioss_qos_dev_log(idev, "dst_addr/mask = %pI4l/%d - ch = %d\n",
								 &temp_filter_node->ip_dest.ipv4_dst_addr,
								 temp_filter_node->ip_dest.dst_mask_length, temp_filter_node->dma_ch);
					}
					break;
				case SRC_PORT:
					ioss_qos_dev_log(idev, "src_port/proto = %d/%d - ch = %d\n", temp_filter_node->src_port.port_num,
							 temp_filter_node->src_port.proto, temp_filter_node->dma_ch);
					break;
				case DEST_PORT:
					ioss_qos_dev_log(idev, "dst_port/proto = %d/%d - ch = %d\n", temp_filter_node->dst_port.port_num,
							 temp_filter_node->dst_port.proto, temp_filter_node->dma_ch);
					break;
				default:
					break;
				}
			}
		}
	}

	ioss_qos_dev_log(idev, "Response = %d\n", map_info.qos_response_status);
	dump_qos_priv(idev, priv);
	return map_info;

	/* check for any other memory leaks*/
err_queue_exhaust:
err_inval_speed:
err_inval_interface:
err_bw_exhaust:
	if (priv->unique_filter_new != PCP && priv->unique_filter_new != INVALID_FILTER)
			delete_filter_table(&qos_tables.dma_filter_table);
	delete_route_table(&qos_tables.pcp_route_table);
	delete_filter_table(&qos_tables.flt_to_app);
	dump_qos_priv(idev, priv);
	return map_info;
}

static int stmmac_request_qos(struct ioss_device *idev)
{
	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);
	int i = 0;
	bool is_sw = true;

	if (priv->plat->rx_qos_queues_to_use >= 3)  {
		/* Check if we need to configure a SW channel to HW */
		for (i = 1; i < priv->plat->rx_qos_queues_to_use; i++) {
			ioss_qos_dev_log(idev, "ch_info_cur = %d, ch_info_new = %d\n",
							 priv->is_rx_sw[i], qos_tables.rx_channel_info[i]);
			if (!qos_tables.rx_channel_info[i])
				continue;
			is_sw = qos_tables.rx_channel_info[i] == IOSS_QOS_SW_PATH ? 1: 0;
			if (is_sw != priv->is_rx_sw[i]) {
				/* configure rx channel as HW */
				if (!is_sw) {
					if (!priv->plat->rx_queues_cfg[i].skip_sw) {
						config_rx_queue_path(ndev, i, true);
						ioss_qos_dev_log(idev, "Config channel %d as HW\n", i);
					}
				} else {
					config_rx_queue_path(ndev, i, false);
					ioss_qos_dev_log(idev, "Config channel %d as SW\n", i);
				}
				priv->is_rx_sw[i] = is_sw;
			}
		}
		/* Check if we need to configure a SW channel to HW */
		for (i = 2; i < priv->plat->tx_qos_queues_to_use; i++) {
			if (!qos_tables.tx_channel_info[i])
				continue;
			is_sw = qos_tables.tx_channel_info[i] == IOSS_QOS_SW_PATH ? 1: 0;
			if (is_sw != priv->is_tx_sw[i]) {
				/* configure tx channel as HW */
				if (!is_sw) {
					if (!priv->plat->tx_queues_cfg[i].skip_sw)
						config_tx_queue_path(ndev, i, true);
				}
				else {
					config_tx_queue_path(ndev, i, false);
				}
				priv->is_tx_sw[i] = is_sw;
			}
			if (qos_tables.tx_routing_info[i].mode_to_use != priv->plat->tx_queues_cfg[i].mode_to_use) {
				/*Change mode to use for TX queues*/
				priv->plat->tx_queues_cfg[i].mode_to_use = qos_tables.tx_routing_info[i].mode_to_use;
				stmmac_configure_tx_queue(priv, i, priv->plat->tx_queues_cfg[i].mode_to_use);
			}
		}
	}

	return 0;
}

static int stmmac_enable_qos(struct ioss_device *idev)
{
	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);

	ioss_qos_dev_log(idev, "Enter");

	if (priv->plat->rx_qos_queues_to_use >= 3) {
		stmmac_enable_qos_queue_cfg(priv, &qos_tables);
		if (priv->unique_filter_new != PCP) {
			stmmac_enable_qos_filtering(ndev, &qos_tables);
		} else if (priv->unique_filter_old != PCP) {
			stmmac_remove_qos_filtering(ndev, priv->unique_filter_old, IDX_USED);
			stmmac_remove_qos_filtering(ndev, priv->unique_filter_old, IDX_CLEAR);
		}
		/*cbs routing*/
		stmmac_config_qos_cbs(priv, &qos_tables);
		qos_tables.filter_cnt = 0;
		priv->plat->qos_active = true;
	}

	return 0;
}

static int stmmac_clear_qos(struct ioss_device *idev)
{

	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct dma_filter_table *dma_filter_node;
	int i = 0;

	ioss_qos_dev_log(idev, "Enter");

	if (priv->plat->qos_active)
		priv->plat->qos_active = false;
	else
		return 0;

	/*Go to previous pcp routing*/
	if (priv->plat->rx_qos_queues_to_use >= 3) {
		stmmac_restore_qos_queue_cfg(priv, &qos_tables);
		if (priv->unique_filter_new != PCP) {
			stmmac_remove_qos_filtering(ndev, priv->unique_filter_new, IDX_USED);
		}
		stmmac_restore_dma_config(ndev, &qos_tables);
		/* Cleanup the used tables */
		priv->unique_filter_old = priv->unique_filter_new;
		priv->max_filters_old = priv->max_filters_new;
		if (priv->unique_filter_old != PCP) {
			if (&qos_tables.dma_filter_table)
				delete_filter_table(&qos_tables.dma_filter_table);
			if (&qos_tables.flt_to_app)
				delete_filter_table(&qos_tables.flt_to_app);
		}
		if (&qos_tables.pcp_route_table)
			delete_route_table(&qos_tables.pcp_route_table);

		memset(&qos_tables, 0, sizeof(struct qos_struct));
	}

	for (i = 0; i < priv->plat->tx_queues_to_use; i++)
		priv->tx_queue_pcp_map[i] = 0;

	return 0;
}

static int stmmac_clear_qos_cache(struct ioss_device *idev)
{
	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);
	int i = 0;

	ioss_qos_dev_log(idev, "Enter");

	if (priv->plat->qos_active) {
		for (i = 0; i < 32; i++) {
			priv->app_filters[i].action = IDX_UNUSED;
		}

		bool is_tx_sw[MTL_MAX_TX_QUEUES];
		u32 tx_ch_bw[MTL_MAX_TX_QUEUES];

		for (i = 0; i < priv->plat->rx_qos_queues_to_use; i++) {
			priv->queue_dis[i] = false;
			priv->queue_pcp_map[i] = qos_tables.backup_pcp_map[i];
			if (i == 0)
				priv->is_rx_sw[i] = false;
			else
				priv->is_rx_sw[i] = true;
		}

		for (i = 0; i < priv->plat->tx_qos_queues_to_use; i++) {
			priv->tx_ch_bw[i] = 0;
			if (i == 0)
				priv->is_tx_sw[i] = false;
			else
				priv->is_tx_sw[i] = true;
		}
		priv->unique_filter_old = INVALID_FILTER;
	}
	return 0;
}

static int stmmac_validate_tx_tc(struct ioss_device *idev, struct list_head *qos_tx, struct qos_routing_tx *tx_node)
{
	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);
	int tx_tc_cnt;
	int max_tx_tc_cnt = priv->plat->tx_qos_queues_to_use - 2;
	int tc_cnt = 0;

	/* iterate over qos_tx pending table to find number of existing tc's
	 * If number of TCs are more than number of qos queues return -EINVAL
	 */
	tx_tc_cnt = stmmac_get_tx_tc_count(qos_tx, SW_HW_PATH);
	if (tx_tc_cnt >= max_tx_tc_cnt)
		return -EINVAL;

	return 0;
}

static void find_tc_queue_channel(struct qos_struct *qos_table, u8 tc_prio, u8 *queue, u8 *channel, bool dir_rx)
{
	int i;
	struct pcp_routing *ptr;

	ioss_qos_dev_log(NULL, "Enter");

	if (dir_rx) {
		for (i = 0; i < ARRAY_SIZE(qos_table->pipe_map.pipe_to_tc_mapping_rx); i++) {
			if ((qos_table->pipe_map.pipe_to_tc_mapping_rx[i]) & (1 << tc_prio)) {
				*channel = i;
				break;
			}
		}
		if (tc_prio < ARRAY_SIZE(qos_table->tc_to_queue_map))
			*queue = qos_table->tc_to_queue_map[tc_prio];
		else
			*queue = 0;
	}
	else {
		for (i = 0; i < ARRAY_SIZE(qos_table->pipe_map.pipe_to_tc_mapping_tx); i++) {
			if ((qos_table->pipe_map.pipe_to_tc_mapping_tx[i]) & (1 << tc_prio)) {
				*queue = i;
				*channel = i;
				return;
			}
		}
	}
}

static bool is_vlan_id_filter_applied(struct stmmac_priv *priv, void* filter_value)
{
	int i;

	ioss_qos_dev_log(NULL, "Enter");

	for (i = 0; i < ARRAY_SIZE(priv->app_filters); i++) {
		if (priv->app_filters[i].vlan_id == *(u16 *)(filter_value))
			return true;
	}

	return false;
}

static bool is_src_ip_filter_applied(struct stmmac_priv *priv, void *filter_value)
{
	int i, j;
	bool equal;
	u32 ipv4_addr;
	struct src_ip filter_node;
	unsigned char ipv6_addr[16];
	struct qos_filters original_node = *(struct qos_filters *)(filter_value);

	ioss_qos_dev_log(NULL, "Enter");

	convert_ip_addr_to_str(&original_node.address, &ipv4_addr, ipv6_addr);
	filter_node.src_mask_length = original_node.mask_length;
	if (original_node.address.ss_family == AF_INET6) {
		for (i = 0; i < 16; i++)
			filter_node.ipv6_src_addr[i] = ipv6_addr[i];
		filter_node.ipv6_src = true;
	} else {
		filter_node.ipv4_src_addr = ipv4_addr;
		filter_node.ipv6_src = false;
	}

	for (i = 0; i < ARRAY_SIZE(priv->app_filters); i++) {
		equal = true;
		if (priv->app_filters[i].ip_src.ipv6_src != filter_node.ipv6_src)
			equal = false;
		else if (priv->app_filters[i].ip_src.src_mask_length != filter_node.src_mask_length)
			equal = false;
		else if (priv->app_filters[i].ip_src.ipv6_src) {
			for (j = 0; j < ARRAY_SIZE(filter_node.ipv6_src_addr); j++) {
				if (priv->app_filters[i].ip_src.ipv6_src_addr[j] != filter_node.ipv6_src_addr[j])
					equal = false;
			}
		}
		else {
			if (priv->app_filters[i].ip_src.ipv4_src_addr != filter_node.ipv4_src_addr)
				equal = false;
		}

		if (equal)
			return true;
	}

	return false;
}

static bool is_dest_ip_filter_applied(struct stmmac_priv *priv, void *filter_value)
{
	int i, j;
	bool equal;
	u32 ipv4_addr;
	struct dest_ip filter_node;
	unsigned char ipv6_addr[16];
	struct qos_filters original_node = *(struct qos_filters *)(filter_value);

	ioss_qos_dev_log(NULL, "Enter");

	convert_ip_addr_to_str(&original_node.address, &ipv4_addr, ipv6_addr);
	filter_node.dst_mask_length = original_node.mask_length;
	if (original_node.address.ss_family == AF_INET6) {
		for (i = 0; i < 16; i++)
			filter_node.ipv6_dst_addr[i] = ipv6_addr[i];
		filter_node.ipv6_dst = true;
	} else {
		filter_node.ipv4_dst_addr = ipv4_addr;
		filter_node.ipv6_dst = false;
	}

	for (i = 0; i < ARRAY_SIZE(priv->app_filters); i++) {
		equal = true;
		if (priv->app_filters[i].ip_dest.ipv6_dst != filter_node.ipv6_dst)
			equal = false;
		else if (priv->app_filters[i].ip_dest.dst_mask_length != filter_node.dst_mask_length)
			equal = false;
		else if (priv->app_filters[i].ip_dest.ipv6_dst) {
			for (j = 0; j < ARRAY_SIZE(filter_node.ipv6_dst_addr); j++) {
				if (priv->app_filters[i].ip_dest.ipv6_dst_addr[j] != filter_node.ipv6_dst_addr[j])
					equal = false;
			}
		}
		else {
			if (priv->app_filters[i].ip_dest.ipv4_dst_addr != filter_node.ipv4_dst_addr)
				equal = false;
		}

		if (equal)
			return true;
	}

	return false;
}

static bool is_src_port_filter_applied(struct stmmac_priv *priv, void *filter_value)
{
	struct qos_filters original_node = *(struct qos_filters *)(filter_value);
	struct port filter_node;
	int i;

	ioss_qos_dev_log(NULL, "Enter");

	filter_node.port_num = original_node.port_num;
	filter_node.proto = get_proto(original_node.proto);

	if (filter_node.proto == IOSS_IPPROTO_INVALID_PROTO) {
		return false;
	}

	for (i = 0; i < ARRAY_SIZE(priv->app_filters); i++) {
		if (filter_node.proto == IOSS_IPPROTO_TCP_UDP) {
			if (priv->app_filters[i].src_port.port_num == filter_node.port_num)
				return true;
		} else {
			if (priv->app_filters[i].src_port.port_num == filter_node.port_num
			    && priv->app_filters[i].src_port.proto == filter_node.proto)
				return true;
		}
	}

	return false;
}


static bool is_dst_port_filter_applied(struct stmmac_priv *priv, void *filter_value)
{
	struct qos_filters original_node = *(struct qos_filters *)(filter_value);
	struct port filter_node;
	int i;

	ioss_qos_dev_log(NULL, "Enter");

	filter_node.port_num = original_node.port_num;
	filter_node.proto = get_proto(original_node.proto);

	if (filter_node.proto == IOSS_IPPROTO_INVALID_PROTO) {
		return false;
	}

	for (i = 0; i < ARRAY_SIZE(priv->app_filters); i++) {
		if (filter_node.proto == IOSS_IPPROTO_TCP_UDP) {
			if (priv->app_filters[i].dst_port.port_num == filter_node.port_num)
				return true;
		} else {
			if (priv->app_filters[i].dst_port.port_num == filter_node.port_num
			    && priv->app_filters[i].dst_port.proto == filter_node.proto)
				return true;
		}
	}

	return false;
}

static bool is_filter_applied(struct stmmac_priv *priv, enum qos_filter_type filter_type, void *filter_value)
{
	ioss_qos_dev_log(NULL, "Enter unique_filter_new = %d", priv->unique_filter_new);

	if (priv->unique_filter_new != filter_type)
		return false;

	if (priv->unique_filter_new == PCP)
		return false;

	switch (filter_type) {
		case VLAN_ID:
			if (is_vlan_id_filter_applied(priv, filter_value))
				return true;
			break;

		case SRC_IP:
			if (is_src_ip_filter_applied(priv, filter_value))
				return true;
			break;

		case DEST_IP:
			if (is_dest_ip_filter_applied(priv, filter_value))
				return true;
			break;

		case SRC_PORT:
			if (is_src_port_filter_applied(priv, filter_value))
				return true;
			break;

		case DEST_PORT:
			if (is_dst_port_filter_applied(priv, filter_value))
				return true;
		default:
			return false;
	}

	return false;
}

#define QOS_ROW_PREFIX(priv, filter_type, filter_value)\
	(is_filter_applied(priv, filter_type, filter_value))? "  " : "# "

#define TC_SKIP_PREFIX(tc_node, hw_channels)\
	(tc_node->action == IOSS_QOS_HW_PATH && !hw_channels? "# " : "  ")

#define TX_TC_SKIP_PREFIX(tc_node, hw_channels)\
	((tc_node->action == IOSS_QOS_HW_PATH && !hw_channels) || tc_node->disabled ? "# " : "  ")

static ssize_t stmmac_show_qos(struct ioss_device *idev, char* buf, struct list_head *qos_rx, struct list_head *qos_tx)
{
	size_t i;
	ssize_t ret;
	u8 tc_queue   = 0;
	u8 tc_channel = 0;
	u8 hw_rxch = idev->qos_rx_channels;
	u8 hw_txch = idev->qos_tx_channels;
	struct list_head *ptr, *hdl_ptr,*rx_flow_hdl;
	struct sockaddr_storage *ss;
	const int ROW_BUFFER   =   60;
	const int TABLE_BUFFER = 4095;
	const int APPLIED_BUFFER = 1024;
	struct qos_rx_tc *rx_node;
	struct qos_routing_rx_hdl *rx_hdl;
	struct qos_routing_tx *tx_node;

	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);

	char *row = kzalloc(sizeof(char) * ROW_BUFFER, GFP_KERNEL);
	if(!row)
		goto row_alloc_err;
	char *table = kzalloc(sizeof(char) * TABLE_BUFFER, GFP_KERNEL);
	if(!table)
		goto table_alloc_err;
	char *applied = kzalloc(sizeof(char) * APPLIED_BUFFER, GFP_KERNEL);
	if(!applied)
		goto applied_alloc_err;

	ioss_qos_dev_log(idev, "Enter");
	scnprintf(row, ROW_BUFFER, "DL TCs: \n");
	strlcat(table, row, TABLE_BUFFER);
	list_for_each(ptr, qos_tx) {
		tx_node = to_qos_routing_tx(ptr);
		scnprintf(row, ROW_BUFFER,"%s- %u:\n", TX_TC_SKIP_PREFIX(tx_node, hw_txch), tx_node->tc_prio);
		strlcat(table, row, TABLE_BUFFER);

		scnprintf(row, ROW_BUFFER, "  %saction: %s\n", TX_TC_SKIP_PREFIX(tx_node, hw_txch),
			  (tx_node->action == IOSS_QOS_HW_PATH)? "HW" : "SW");
		strlcat(table, row, TABLE_BUFFER);

		scnprintf(row, ROW_BUFFER, "  %sbw: %u:%u\n", TX_TC_SKIP_PREFIX(tx_node, hw_txch),
			  tx_node->cbs_bw.low_bw, tx_node->cbs_bw.high_bw);
		strlcat(table, row, TABLE_BUFFER);

		scnprintf(row, ROW_BUFFER, "    bw allocated: %u\n", qos_tables.bw_allocated[tx_node->tc_prio]);
		strlcat(table, row, TABLE_BUFFER);
		if (tx_node->action == IOSS_QOS_SW_PATH) {
			scnprintf(row, ROW_BUFFER, "    handle: %u\n", tx_node->handle);
			strlcat(table, row, TABLE_BUFFER);

			scnprintf(row, ROW_BUFFER, "    pcp: ");
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < tx_node->pcp.len; i++) {
				scnprintf(row, ROW_BUFFER, "%u ", tx_node->pcp.arr[i]);
				strlcat(table, row, TABLE_BUFFER);
			}
			scnprintf(row, ROW_BUFFER, "\n");
			strlcat(table, row, TABLE_BUFFER);
                }
		find_tc_queue_channel(&qos_tables, tx_node->tc_prio, &tc_queue, &tc_channel, false);
		scnprintf(row, ROW_BUFFER, "  %squeue: %u\n  %schannel: %u\n",
			  TX_TC_SKIP_PREFIX(tx_node, hw_txch), tc_queue, TX_TC_SKIP_PREFIX(tx_node, hw_txch), tc_channel);
		strlcat(table, row, TABLE_BUFFER);
	}

	scnprintf(row, ROW_BUFFER, "UL TCs: \n");
	strlcat(table, row, TABLE_BUFFER);
	list_for_each(ptr, qos_rx) {
		rx_node = to_qos_rx_tc(ptr);
		scnprintf(row, ROW_BUFFER, "%s- %u:\n", TC_SKIP_PREFIX(rx_node, hw_rxch), rx_node->tc_prio);
		strlcat(table, row, TABLE_BUFFER);

		scnprintf(row, ROW_BUFFER, "  %saction: %s\n", TC_SKIP_PREFIX(rx_node, hw_rxch),
			  (rx_node->action == IOSS_QOS_HW_PATH)? "hw" : "sw");
		strlcat(table, row, TABLE_BUFFER);

		find_tc_queue_channel(&qos_tables, rx_node->tc_prio, &tc_queue, &tc_channel, true);
		scnprintf(row, ROW_BUFFER, "  %squeue: %u\n  %schannel: %u\n",
			  TC_SKIP_PREFIX(rx_node, hw_rxch), tc_queue,
			  TC_SKIP_PREFIX(rx_node, hw_rxch), tc_channel);
		strlcat(table, row, TABLE_BUFFER);

		rx_flow_hdl = &rx_node->hdl_node;
		list_for_each(hdl_ptr, rx_flow_hdl) {
			rx_hdl = to_qos_routing_rx_hdl(hdl_ptr);
			scnprintf(row, ROW_BUFFER, "  %s- handle: %u\n",
				  TC_SKIP_PREFIX(rx_node, hw_rxch), rx_hdl->hdl);
			strlcat(table, row, TABLE_BUFFER);
			scnprintf(row, ROW_BUFFER, "    %spcp:\n", TC_SKIP_PREFIX(rx_node, hw_rxch));
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < rx_hdl->pcp.len; i++) {
				scnprintf(row, ROW_BUFFER, "      %s- %u\n",
					  TC_SKIP_PREFIX(rx_node, hw_rxch), rx_hdl->pcp.arr[i]);
				strlcat(table, row, TABLE_BUFFER);
			}

			scnprintf(row, ROW_BUFFER, "    %svlan:\n", TC_SKIP_PREFIX(rx_node, hw_rxch));
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < rx_hdl->vlan_ids.len; i++) {
				scnprintf(row, ROW_BUFFER, "    %s%s- %u\n",
					  TC_SKIP_PREFIX(rx_node, hw_rxch),
					  QOS_ROW_PREFIX(priv, VLAN_ID, &rx_hdl->vlan_ids.arr[i]),
					  rx_hdl->vlan_ids.arr[i]);
				strlcat(table, row, TABLE_BUFFER);
				if (!rx_hdl->hdl_committed)
					rx_hdl->hdl_committed = is_filter_applied(priv, VLAN_ID, &rx_hdl->vlan_ids.arr[i]);
			}

			scnprintf(row, ROW_BUFFER, "    %ssrc:\n", TC_SKIP_PREFIX(rx_node, hw_rxch));
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < rx_hdl->src.len; i++) {
				ss = &rx_hdl->src.arr[i].address;
				if (ss->ss_family == AF_INET) {
					scnprintf(row, ROW_BUFFER, "    %s%s- %pI4/%u\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, SRC_IP, &rx_hdl->src.arr[i]),
						&(((struct sockaddr_in *)ss)->sin_addr),
						rx_hdl->src.arr[i].mask_length);
					if (!rx_hdl->hdl_committed)
						rx_hdl->hdl_committed = is_filter_applied(priv, SRC_IP, &rx_hdl->src.arr[i]);
				}
				else if (ss->ss_family == AF_INET6) {
					scnprintf(row, ROW_BUFFER, "    %s%s- %pI6/%u\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, SRC_IP, &rx_hdl->src.arr[i]),
						&(((struct sockaddr_in6 *)ss)->sin6_addr),
						rx_hdl->src.arr[i].mask_length);
					if (!rx_hdl->hdl_committed)
						rx_hdl->hdl_committed = is_filter_applied(priv, SRC_IP, &rx_hdl->src.arr[i]);
				}
				else {
					scnprintf(row, ROW_BUFFER, "    %s%s- [%s/%u]\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, SRC_PORT, &rx_hdl->src.arr[i]),
						rx_hdl->src.arr[i].proto,
						rx_hdl->src.arr[i].port_num);
					if (!rx_hdl->hdl_committed)
						rx_hdl->hdl_committed = is_filter_applied(priv, SRC_PORT, &rx_hdl->src.arr[i]);
				}

				strlcat(table, row, TABLE_BUFFER);
			}

			scnprintf(row, ROW_BUFFER, "    %sdst:\n", TC_SKIP_PREFIX(rx_node, hw_rxch));
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < rx_hdl->dst.len; i++) {
				ss = &rx_hdl->dst.arr[i].address;
				if (ss->ss_family == AF_INET) {
					scnprintf(row, ROW_BUFFER, "    %s%s- %pI4/%u\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, DEST_IP, &rx_hdl->dst.arr[i]),
						&(((struct sockaddr_in *)ss)->sin_addr),
						rx_hdl->dst.arr[i].mask_length);
					if (!rx_hdl->hdl_committed)
						rx_hdl->hdl_committed = is_filter_applied(priv, DEST_IP, &rx_hdl->dst.arr[i]);
				}
				else if (ss->ss_family == AF_INET6) {
					scnprintf(row, ROW_BUFFER, "    %s%s- %pI6/%u\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, DEST_IP, &rx_hdl->dst.arr[i]),
						&(((struct sockaddr_in6 *)ss)->sin6_addr),
						rx_hdl->dst.arr[i].mask_length);
					if (!rx_hdl->hdl_committed)
						rx_hdl->hdl_committed = is_filter_applied(priv, DEST_IP, &rx_hdl->dst.arr[i]);
				}
				else {
					scnprintf(row, ROW_BUFFER, "    %s%s- [%s/%u]\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, DEST_PORT, &rx_hdl->dst.arr[i]),
						rx_hdl->dst.arr[i].proto,
						rx_hdl->dst.arr[i].port_num);
					if (!rx_hdl->hdl_committed)
						rx_hdl->hdl_committed = is_filter_applied(priv, DEST_PORT, &rx_hdl->dst.arr[i]);
				}

				strlcat(table, row, TABLE_BUFFER);
			}

			scnprintf(row, ROW_BUFFER, "    %ssmac:\n", TC_SKIP_PREFIX(rx_node, hw_rxch));
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < rx_hdl->smac.len; i++) {
				scnprintf(row, ROW_BUFFER, "    %s%s- %02x:%02x:%02x:%02x:%02x:%02x\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, SRC_MAC, &rx_hdl->smac.arr[i]),
						rx_hdl->smac.arr[i][0],
						rx_hdl->smac.arr[i][1],
						rx_hdl->smac.arr[i][2],
						rx_hdl->smac.arr[i][3],
						rx_hdl->smac.arr[i][4],
						rx_hdl->smac.arr[i][5]);
				strlcat(table, row, TABLE_BUFFER);
				if (!rx_hdl->hdl_committed)
					rx_hdl->hdl_committed = is_filter_applied(priv, SRC_MAC,  &rx_hdl->smac.arr[i]);
			}

			scnprintf(row, ROW_BUFFER, "    %sdmac:\n", TC_SKIP_PREFIX(rx_node, hw_rxch));
			strlcat(table, row, TABLE_BUFFER);
			for (i = 0; i < rx_hdl->dmac.len; i++) {
				scnprintf(row, ROW_BUFFER, "     %s%s- %02x:%02x:%02x:%02x:%02x:%02x\n",
						TC_SKIP_PREFIX(rx_node, hw_rxch),
						QOS_ROW_PREFIX(priv, DEST_MAC, &rx_hdl->dmac.arr[i]),
						rx_hdl->dmac.arr[i][0],
						rx_hdl->dmac.arr[i][1],
						rx_hdl->dmac.arr[i][2],
						rx_hdl->dmac.arr[i][3],
						rx_hdl->dmac.arr[i][4],
						rx_hdl->dmac.arr[i][5]);
				strlcat(table, row, TABLE_BUFFER);
				if (!rx_hdl->hdl_committed)
					rx_hdl->hdl_committed = is_filter_applied(priv, DEST_MAC, &rx_hdl->dmac.arr[i]);
			}
			if (rx_hdl->pcp.len)
				rx_hdl->hdl_committed = true;
		}
	}

	scnprintf(row, ROW_BUFFER, "Applied: ");
	strlcat(applied, row, APPLIED_BUFFER);
	list_for_each(ptr, qos_rx) {
		rx_node = to_qos_rx_tc(ptr);
		rx_flow_hdl = &rx_node->hdl_node;
		list_for_each(hdl_ptr, rx_flow_hdl) {
			rx_hdl = to_qos_routing_rx_hdl(hdl_ptr);
			if (rx_hdl->hdl_committed) {
				scnprintf(row, ROW_BUFFER, "%u ", rx_hdl->hdl);
				strlcat(applied, row, APPLIED_BUFFER);
			}
		}
	}
	list_for_each(ptr, qos_tx) {
		tx_node = to_qos_routing_tx(ptr);
		if (tx_node->committed && tx_node->action == IOSS_QOS_SW_PATH) {
			scnprintf(row, ROW_BUFFER, "%u ", tx_node->handle);
			strlcat(applied, row, APPLIED_BUFFER);
		}
	}
	scnprintf(row, ROW_BUFFER, "\n");
	strlcat(applied, row, APPLIED_BUFFER);

	ret = sysfs_emit(buf, "%s%s\n", applied, table);

	kfree(row);
	kfree(table);
	kfree(applied);
	return ret;

applied_alloc_err:
	ioss_qos_dev_err(NULL, "Applied buffer allocation failed");
	kfree(table);
table_alloc_err:
	ioss_qos_dev_err(NULL, "Table buffer allocation failed");
	kfree(row);
row_alloc_err:
	ioss_qos_dev_err(NULL, "Row buffer allocation failed");
	return -ENOMEM;

}

static ssize_t stmmac_get_qos_info(struct ioss_device *idev, char* buf, ssize_t buf_size)
{
	struct net_device *ndev = idev->net_dev;
	struct stmmac_priv *priv = netdev_priv(ndev);
	int bytes_written = 0;

	bytes_written += snprintf(buf + bytes_written, buf_size - bytes_written,
				  "emac_qos_supported: %d\n", priv->plat->qos_supported);
	bytes_written += snprintf(buf + bytes_written, buf_size - bytes_written,
				  "emac_qos_config: %s\n", priv->plat->qoscfg);
	bytes_written += snprintf(buf + bytes_written, buf_size - bytes_written,
				  "emac_rx_queues: %d\n", priv->plat->rx_qos_queues_to_use);
	bytes_written += snprintf(buf + bytes_written, buf_size - bytes_written,
				  "emac_tx_queues: %d\n", priv->plat->tx_qos_queues_to_use);
	bytes_written += snprintf(buf + bytes_written, buf_size - bytes_written,
				  "emac_dl_queue_sel: %s\n", stmmac_is_skprio_routing(ndev)
				  ? "skprio": "pcp");
	bytes_written += snprintf(buf + bytes_written, buf_size - bytes_written,
				  "usable_bw: %d\n", stmmac_get_cbs_avail_bw(priv->speed));
	return bytes_written;
}

static void stmmac_get_hw_cap(struct ioss_device *idev, struct ioss_qos_hw_caps *hw_cap) {
	struct stmmac_priv *priv = netdev_priv(idev->net_dev);

	hw_cap->tx_qos_queues = (priv->plat->tx_qos_queues_to_use - 2);
	hw_cap->max_link_speed = priv->plat->max_supported_speed;
	hw_cap->max_usable_bw = stmmac_get_cbs_avail_bw(priv->plat->max_supported_speed);
}

static struct ioss_qos_ops stmmac_qos_ops = {
	.prepare_qos = stmmac_prepare_qos_info,
	.request_qos = stmmac_request_qos,
	.enable_qos = stmmac_enable_qos,
	.clear_qos = stmmac_clear_qos,
	.show_qos = stmmac_show_qos,
	.clear_qos_cache = stmmac_clear_qos_cache,
	.get_qos_info = stmmac_get_qos_info,
	.validate_tx_tc = stmmac_validate_tx_tc,
	.get_hw_caps = stmmac_get_hw_cap,
};

static struct ioss_driver_ops stmmac_ioss_ops = {
	.open_device = stmmac_ioss_open_device,
	.close_device = stmmac_ioss_close_device,

	.request_channel = stmmac_ioss_request_channel,
	.release_channel = stmmac_ioss_release_channel,

	.enable_channel = stmmac_ioss_enable_channel,
	.disable_channel = stmmac_ioss_disable_channel,

	.request_event = stmmac_ioss_request_event,
	.release_event = stmmac_ioss_release_event,

	.enable_event = stmmac_ioss_enable_event,
	.disable_event = stmmac_ioss_disable_event,

	.get_device_statistics = stmmac_ioss_device_statistics,
	.get_channel_statistics = stmmac_ioss_channel_statistics,
	.get_channel_status = stmmac_ioss_channel_status,
	.update_skb = stmmac_ioss_update_skb,
};

bool stmmac_driver_match(struct device *dev)
{
	bool rc;

	/* Just match the driver name */
	rc = (dev->bus == &platform_bus_type) &&
		!strcmp(to_platform_driver(dev->driver)->driver.name, DRV_NAME);

	ioss_log_dbg(NULL, "MATCH %s, bool = %d, strcmp=%s",
				 dev_name(dev),
				 rc,
				 to_platform_driver(dev->driver)->driver.name);

	return rc;
}

static struct ioss_driver stmmac_ioss_drv = {
	.name = DRV_NAME "_ioss",
	.match = stmmac_driver_match,
	.ops = &stmmac_ioss_ops,
	.qos_ops = &stmmac_qos_ops,
	.filter_types = IOSS_RXF_BE,
};

static int __init stmmac_ioss_init(void)
{
	return ioss_plat_register_driver(&stmmac_ioss_drv, THIS_MODULE);
}
module_init(stmmac_ioss_init);

static void __exit stmmac_ioss_exit(void)
{
	return ioss_plat_unregister_driver(&stmmac_ioss_drv);
}
module_exit(stmmac_ioss_exit);

MODULE_DESCRIPTION("STMMAC IOSS Glue Driver");
MODULE_LICENSE("GPL v2");

