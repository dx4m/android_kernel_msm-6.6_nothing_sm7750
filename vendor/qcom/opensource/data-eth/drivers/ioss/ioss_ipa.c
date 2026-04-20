/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/etherdevice.h>
#include <linux/string.h>

#include "ioss_i.h"

#ifdef CONFIG_MSM_BOOT_TIME_MARKER
#include <soc/qcom/boot_stats.h>
#endif

#define to_ipa_dir(ioss_dir) \
	((ioss_dir == IOSS_CH_DIR_TX)? IPA_ETH_PIPE_DIR_TX : IPA_ETH_PIPE_DIR_RX)

#define to_ioss_dir(ipa_dir) \
	((ipa_dir == IPA_ETH_PIPE_DIR_TX) ? IOSS_CH_DIR_TX : IOSS_CH_DIR_RX)

static enum ipa_eth_pipe_traffic_type to_ipa_traffic_type(enum ioss_traffic_type ioss_type)
{
	static const enum ipa_eth_pipe_traffic_type ipa_map[IOSS_TRAFFIC_TYPE_MAX] = {
		[IOSS_TRAFFIC_BE] = IPA_ETH_PIPE_BEST_EFFORT,
		[IOSS_TRAFFIC_BE_TAGGED] = IPA_ETH_PIPE_BEST_EFFORT_VLAN,
		[IOSS_TRAFFIC_QOS]	=	IPA_ETH_PIPE_TRAFFIC_TYPE_QOS,
		[IOSS_TRAFFIC_LL] = IPA_ETH_PIPE_LOW_LATENCY,
	};

	return ipa_map[ioss_type];
}

static enum ioss_traffic_type to_ioss_traffic(enum ipa_eth_pipe_traffic_type ipa_type)
{
	static const enum ioss_traffic_type ioss_map[IPA_ETH_PIPE_TRAFFIC_TYPE_MAX] = {
		[IPA_ETH_PIPE_BEST_EFFORT] = IOSS_TRAFFIC_BE,
		[IPA_ETH_PIPE_LOW_LATENCY] = IOSS_TRAFFIC_LL,
		[IPA_ETH_PIPE_BEST_EFFORT_VLAN] = IOSS_TRAFFIC_BE_TAGGED,
		[IPA_ETH_PIPE_TRAFFIC_TYPE_QOS]	=	IOSS_TRAFFIC_QOS,
	};

	return ioss_map[ipa_type];
}

static void ioss_ipa_notify_cb(void *priv,
					enum ipa_dp_evt_type evt,
					unsigned long data)
{
	struct ioss_channel *ch = priv;
	struct sk_buff *skb = (struct sk_buff *)data;
	struct ioss_interface *iface = ch->iface;
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	struct rtnl_link_stats64 netdev_stats;
#endif
	struct ioss_device *idev = ioss_ch_dev(ch);

	if (evt != IPA_RECEIVE)
		return;

	iface->exception_stats.rx_packets++;

#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	if(iface->exception_stats.rx_packets == 1) {
		memset(&netdev_stats, 0, sizeof(struct rtnl_link_stats64));
		dev_get_stats(ioss_iface_to_netdev(iface), &netdev_stats);
		if (netdev_stats.rx_packets == 1)
			update_marker("M - Ethernet first packet received");
	}
#endif

	iface->exception_stats.rx_bytes += skb->len;

	skb->protocol = eth_type_trans(skb, ioss_iface_to_netdev(iface));

	ioss_dev_op(idev, update_skb, ch, skb);

	if (netif_rx(skb) == NET_RX_DROP)
		iface->exception_stats.rx_drops++;
}

static int ioss_ipa_fill_pipe_info(struct ioss_channel *ch,
		struct ipa_eth_client_pipe_info *pi)
{
	struct ioss_mem *desc_mem;
	struct ioss_mem *buff_mem;
	struct ipa_eth_buff_smmu_map *sm;
	struct ipa_eth_pipe_setup_info *si = &pi->info;
	struct ioss_device *idev = ioss_ch_dev(ch);

	pi->dir = to_ipa_dir(ch->direction);
	pi->traffic_type = to_ipa_traffic_type(ch->traffic_type);
	pi->tc_bmap = ch->tc_mapping;

	desc_mem = list_first_entry_or_null(
			&ch->desc_mem, typeof(*desc_mem), node);
	if (!desc_mem) {
		ioss_dev_err(idev, "Descriptor memory not found");
		return -EINVAL;
	}

	si->is_transfer_ring_valid = true;
	si->transfer_ring_base = desc_mem->daddr;
	si->transfer_ring_sgt = &desc_mem->sgt;

	if (ch->multi_rx_queues)
		si->transfer_ring_size = desc_mem->size*2;
	else
		si->transfer_ring_size = desc_mem->size;

	si->data_buff_list_size = ch->config.ring_size;
	si->data_buff_list = sm = kcalloc(si->data_buff_list_size,
			sizeof(*si->data_buff_list), GFP_KERNEL);
	if (!sm) {
		ioss_dev_err(idev,
			"Kmalloc failed for data buff list");
		return -EINVAL;
	}
	si->fix_buffer_size = ch->config.buff_size;

	list_for_each_entry(buff_mem, &ch->buff_mem, node) {
		sm->iova = buff_mem->daddr;
		sm->pa = ch->config.buff_alctr->pa(ioss_ch_dev(ch),
				buff_mem->addr, buff_mem->daddr,
				ch->config.buff_alctr);
		sm++;
	}

	if (pi->dir == IPA_ETH_PIPE_DIR_RX) {
		si->notify = ioss_ipa_notify_cb;
		si->priv = ch;
	}

	if (ioss_ipa_hal_fill_si(ch)) {
		ioss_dev_err(idev,
			"Failed to fill IPA pipe setup info");
		return -EINVAL;
	}

	return 0;
}

static void ioss_ipa_clear_pipe_info(struct ipa_eth_client_pipe_info *pi) {
	struct ipa_eth_pipe_setup_info *si = &pi->info;
	kfree(si->data_buff_list);
}

static int ioss_ipa_vote_bw(struct ioss_interface *iface)
{
	struct ipa_eth_perf_profile profile;
	struct ioss_iface_priv *ifp = iface->ioss_priv;
	struct ipa_eth_client *ec = &ifp->ipa_ec;
	struct ioss_device *idev = ioss_iface_dev(iface);

	ioss_dev_dbg(idev,
		"Voting for IPA bandwidth of %u Mbps", iface->link_speed);

	memset(&profile, 0, sizeof(profile));
	profile.max_supported_bw_mbps = iface->link_speed;

	if (ipa_eth_client_set_perf_profile(ec, &profile)) {
		ioss_dev_err(idev,
				"Failed to set IPA perf profile");
		return -EINVAL;
	}

	ioss_dev_cfg(idev,
		"Voted for IPA bandwidth of %u Mbps", iface->link_speed);

	return 0;
}

#if IPA_ETH_API_VER > 4
int ioss_ipa_enable_pipes(struct ioss_interface *iface)
{
	struct ioss_iface_priv *ifp = iface->ioss_priv;
	struct ipa_eth_client *ec = &ifp->ipa_ec;
	struct ioss_device *idev = ioss_iface_dev(iface);

	if (ipa_eth_client_enable_pipes(ec)) {
		ioss_dev_err(idev, "Failed to enable pipes");
		return -EINVAL;
	}
	ioss_dev_dbg(idev, "Enabled IPA pipes");

	return 0;
}

int ioss_ipa_disable_pipes(struct ioss_interface *iface)
{
	struct ioss_iface_priv *ifp = iface->ioss_priv;
	struct ipa_eth_client *ec = &ifp->ipa_ec;
	struct ioss_device *idev = ioss_iface_dev(iface);

	if (ipa_eth_client_disable_pipes(ec)) {
		ioss_dev_err(idev, "Failed to disable pipes");
		return -EINVAL;
	}
	ioss_dev_dbg(idev, "Disabled IPA pipes");

	return 0;
}
#endif

int ioss_ipa_register(struct ioss_interface *iface)
{
	int i;
	struct ioss_iface_priv *ifp = iface->ioss_priv;
	struct ipa_eth_client *ec = &ifp->ipa_ec;
	struct ipa_eth_intf_info *ii = &ifp->ipa_ii;
	struct ioss_channel *ch;
	struct ioss_device *idev = ioss_iface_dev(iface);

	ec->priv = iface;
	ec->inst_id = iface->instance_id;
	ec->client_type = ioss_ipa_hal_get_ctype(idev);

	if (ec->client_type == IPA_ETH_CLIENT_MAX) {
		ioss_dev_err(idev, "Failed to determine client type");
		return -EFAULT;
	}

	INIT_LIST_HEAD(&ec->pipe_list);

	ioss_for_each_channel(ch, iface) {
		struct ioss_ch_priv *cp = ch->ioss_priv;

		cp->ipa_pi.client_info = ec;

		if (ioss_ipa_fill_pipe_info(ch, &cp->ipa_pi)) {
			ioss_dev_err(idev, "Failed to fill pipe info");
			return -EFAULT;
		}

		list_add_tail(&cp->ipa_pi.link, &ec->pipe_list);
	}

	/* connect pipes */
	ec->net_dev = ioss_iface_to_netdev(iface);

	if (ipa_eth_client_conn_pipes(ec)) {
		ioss_dev_err(idev, "Failed to connect pipes");
		return -EINVAL;
	}

	/* vote for bw */
	if (ioss_ipa_vote_bw(iface)) {
		ioss_dev_err(idev, "Failed to vote for bandwidth");
		return -EINVAL;
	}

	/* Retrieve output from conn_pipes call */
	i = 0;
	ioss_for_each_channel(ch, iface) {
		struct ioss_ch_priv *cp = ch->ioss_priv;
		struct ipa_eth_client_pipe_info *pi = &cp->ipa_pi;
		struct ipa_eth_pipe_setup_info *si = &pi->info;

		ch->event.paddr = si->db_pa;
		ch->event.data = si->db_val;

	}

	/* register interface */
	ii->client = ec;
	ii->is_conn_evt = true;

	if (ipa_eth_client_reg_intf(ii)) {
		ioss_dev_err(idev, "Failed to register interface");
		return -EINVAL;
	}

	return 0;
}

int ioss_ipa_unregister(struct ioss_interface *iface)
{
	int rc;
	struct ioss_channel *ch;
	struct ioss_iface_priv *ifp = iface->ioss_priv;
	struct ipa_eth_client *ec = &ifp->ipa_ec;
	struct ipa_eth_intf_info *ii = &ifp->ipa_ii;
	struct ioss_device *idev = ioss_iface_dev(iface);

	/* connect pipes */
	rc = ipa_eth_client_disconn_pipes(ec);
	if (rc) {
		ioss_dev_err(idev, "Failed to disconnect pipes");
		return rc;
	}

	/* unregister interface */
	rc = ipa_eth_client_unreg_intf(ii);
	if (rc) {
		ioss_dev_err(idev, "Failed to unregister interface");
		return rc;
	}

	memset(ii, 0, sizeof(*ii));

	ioss_for_each_channel(ch, iface) {
		struct ioss_ch_priv *cp = ch->ioss_priv;

		ch->event.paddr = 0;
		ch->event.data = 0;
		ioss_ipa_clear_pipe_info(&cp->ipa_pi);

		memset(&cp->ipa_pi, 0, sizeof(cp->ipa_pi));
	}

	memset(ec, 0, sizeof(*ec));

	return 0;
}

static bool channel_match_ipa_config(struct ioss_channel *ch, const char *ipa_config)
{
	int i;

	if (!ipa_config)
		return false;

	/* A channel config (ex. legacy) that does not explicitly list any compatible IPA configs
	 * is assumed to be compatible only with the default IPA config.
	 */
	if (!ch->num_ipa_configs)
		return !strcmp(ipa_config, DEFAULT_IPA_CONFIG);

	for (i = 0; i < ch->num_ipa_configs; i++)
		if (!strcmp(ipa_config, ch->ipa_configs[i]))
			return true;

	return false;
}

static bool validate_channel(struct ioss_channel *ch,
		enum ioss_channel_dir dir, enum ioss_traffic_type traffic)
{
	if (ch->direction != dir)
		return false;

	if (traffic == IOSS_TRAFFIC_QOS) {
		ch->traffic_type = IOSS_TRAFFIC_QOS;
		return true;
	}

	if (ch->traffic_type != traffic)
		return false;

	return channel_match_ipa_config(ch, ch->iface->ipa_config);
}

/* Recursively select channels as per the channel config list provided by IPA */
static int ioss_ipa_validate_one_channel(struct ioss_interface *iface, struct ipa_eth_dma_ch_config *ch_list, int num_channels)
{
	int ret = -ENOENT;
	enum ioss_channel_dir dir;
	enum ioss_traffic_type traffic;
	struct ioss_channel *ch, *tmp_ch;
	const char *ipa_config = iface->ipa_config;
	struct ioss_device *idev = ioss_iface_dev(iface);

	if (!num_channels)
		return 0;

	dir = to_ioss_dir(ch_list->dir);
	traffic = to_ioss_traffic(ch_list->traffic_type);

	list_for_each_entry_safe(ch, tmp_ch, &iface->invalid_channels, node) {
		struct ioss_ch_priv *ch_priv = ch->ioss_priv;

		if (!validate_channel(ch, dir, traffic))
			continue;

		ioss_dev_log(idev, "Selected channel '%s' for %s '%s' traffic",
			ch->name, ioss_ch_dir_name(dir), ioss_traffic_name(traffic));

		list_move(&ch->node, &iface->valid_channels);

		ret = ioss_ipa_validate_one_channel(iface, ch_list + 1, num_channels - 1);
		if (!ret)
			ch_priv->ipa_ch_config = ch_list; /* for debug purposes */
		else
			list_move(&ch->node, &iface->invalid_channels);

		return ret;
	}

	ioss_dev_err(idev,
		"Failed to find (%u more needed) a matching channel for IPA DMA config: "
		"dir=%u traffic=%u config='%s'",
		num_channels, dir, traffic, ipa_config);

	return ret;
}

int ioss_ipa_validate_channels(struct ioss_interface *iface)
{
	int ret;
	int required_channels = 0;
	struct ioss_device *idev = ioss_iface_dev(iface);
	u32 inst_id = iface->instance_id;
	enum ipa_eth_client_type ct = ioss_ipa_hal_get_ctype(idev);
	struct ioss_iface_priv *ifp = iface->ioss_priv;
	struct ipa_eth_config *ipa_config = &ifp->ipa_config;

	iface->ipa_config = NULL;

	memset(ipa_config, 0, sizeof(*ipa_config));

	ret = ipa_eth_get_config_type(ct, inst_id, ipa_config);
	if (ret) {
		ioss_dev_err(idev, "Failed to get IPA config for %u.%u", ct, inst_id);
		return ret;
	}

	iface->ipa_config = ipa_config->config;

	ioss_dev_log(idev,
			"IPA config for %u.%u is '%s' and uses %u channels",
			ct, inst_id, ipa_config->config, ipa_config->num_dma_channel);

	ioss_ipa_invalidate_channels(iface);

	required_channels = ipa_config->num_dma_channel;
	if (!strcmp(ipa_config->config, "qos"))
		required_channels = 2;

	return ioss_ipa_validate_one_channel(
			iface, ipa_config->dma_config, required_channels);
}

void ioss_ipa_invalidate_channels(struct ioss_interface *iface)
{
	struct ioss_channel *ch, *tmp_ch;

	list_for_each_entry_safe(ch, tmp_ch, &iface->valid_channels, node) {
		ioss_dev_log(ioss_iface_dev(iface), "Ch : %d, id : %d, dir : %d, traffic : %d, tc_mapping : %d",
			     ch->channel_num, ch->id, ch->direction, ch->traffic_type, ch->tc_mapping);
		list_move(&ch->node, &iface->invalid_channels);
	}
}
