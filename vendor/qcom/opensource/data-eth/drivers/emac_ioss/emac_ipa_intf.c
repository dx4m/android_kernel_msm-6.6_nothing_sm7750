/* Copyright (C) 2021 Toshiba Electronic Devices & Storage Corporation
 *
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include <linux/dma-mapping.h>
#include <qcom_scm.h>
#include "common.h"
#include "stmmac.h"
#include "dwxgmac2.h"
#include "dwmac4.h"
#include "dwmac4_dma.h"
#include "emac_ipa_intf.h"
#include "dwmac-qcom-ethqos.h"

#define IPA_MAX_BUFFER_SIZE (9 * 1024) /* 9KBytes */
#define IPA_MAX_DESC_CNT    16384 /* XGMAC limit */

#define MAC_ADDR_INDEX 1
#define MAC_ADDR_AE 1
#define MAC_ADDR_MBC 0x3F
#define MAC_ADDR_DCS 0x1
static u8 mac_addr_default[6] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

static bool is_valid_default_ch_config(struct channel_info *channel)
{
	if (!channel->ezmesh_enabled && !channel->tsn_enabled
		&& channel->channel_num == DEFAULT_CHANNEL_RX_TX)
		return true;

	return false;
}

static bool is_valid_easymesh_ch_config(struct channel_info *channel)
{
	if (!channel->ezmesh_enabled)
		return false;

	if (channel->traffic_type_info == IOSS_TRAFFIC_BE
		&& channel->channel_num == EASYMESH_CHANNEL_RX_TX_BE)
		return true;

	if (channel->traffic_type_info == IOSS_TRAFFIC_BE_TAGGED
		&& channel->channel_num == EASYMESH_CHANNEL_RX_TX_BE_TAGGED)
		return true;

	return false;
}

static bool is_valid_tsn_ch_config(struct channel_info *channel)
{
	if (!channel->tsn_enabled)
		return false;

	if (channel->traffic_type_info == IOSS_TRAFFIC_BE) {
		if (channel->direction == CH_DIR_RX && channel->channel_num == TSN_CHANNEL_RX_DEFAULT)
			return true;
		if (channel->direction == CH_DIR_TX && channel->channel_num == TSN_CHANNEL_TX_DEFAULT)
			return true;
	}

	if (channel->traffic_type_info == IOSS_TRAFFIC_LL) {
		if (channel->direction == CH_DIR_TX && channel->channel_num == TSN_CHANNEL_TX_LL)
			return true;
	}

	return false;
}

static bool is_valid_qos_ch_config(struct channel_info *channel)
{
	if (!channel->qos_enabled)
		return false;

	if (channel->traffic_type_info == IOSS_TRAFFIC_QOS)
		return true;

	return false;
}

/*
* Check if the channel config is valid, depending on the enabled feature
*/
static bool valid_ch_config(struct channel_info *channel)
{
	return (is_valid_default_ch_config(channel)
		|| is_valid_easymesh_ch_config(channel)
		|| is_valid_tsn_ch_config(channel)
		|| is_valid_qos_ch_config(channel));
}

/* Local SMC buffer is valid only for HW where IO macro space is moved to TZ.
 * for other configurations it should always be passed as NULL
 */
static int rgmii_readl(struct qcom_ethqos *ethqos, unsigned int offset)
{
	if (ethqos->shm_rgmii_local.vaddr)
		return readl(ethqos->shm_rgmii_local.vaddr + offset);
	else
		return readl(ethqos->rgmii_base + offset);
}

static void rgmii_writel(struct qcom_ethqos *ethqos,
			 int value, unsigned int offset)
{
	writel(value, ethqos->rgmii_base + offset);
}

static void rgmii_updatel(struct qcom_ethqos *ethqos,
			  int mask, int val, unsigned int offset)
{
	unsigned int temp;

	temp =  rgmii_readl(ethqos, offset);
	temp = (temp & ~(mask)) | val;
	rgmii_writel(ethqos, temp, offset);
}

static int enable_ezmesh_vlan_filtering(struct stmmac_priv *priv)
{
	int ret = 0;

	/*	Expected behaviour:
	*	Untagged packets:	Queue0 -> Channel 0
	* 	Tagged packets:		Queue0 -> Channel 2
	*/

	/* Packets which don't match the filter will go to default mapped channel (Channel 0) */
	stmmac_map_mtl_to_dma(priv, priv->hw, EASYMESH_QUEUE_RX_TX_BE_TAGGED,
			      EASYMESH_CHANNEL_RX_TX_BE);

	/*	Enable dynamic dma channel selection for the queue and install inverse VLAN routing filter
	*	to route all VLAN packets to Channel 2 */
	stmmac_enable_queue_dynamic_dma_ch_selection(priv, priv->hw, EASYMESH_QUEUE_RX_TX_BE_TAGGED);
	ret = stmmac_add_hw_vlan_rx_routing_fltr(priv, priv->dev, priv->hw, EZMESH_VID_FILTER,
						 EASYMESH_CHANNEL_RX_TX_BE_TAGGED, true);
	if (ret)
		netdev_err(priv->dev, "Failed to enable Ezmesh VLAN filtering");
	return ret;
}

static int disable_ezmesh_vlan_filtering(struct stmmac_priv *priv)
{
	int ret = 0;

	stmmac_disable_queue_dynamic_dma_ch_selection(priv, priv->hw, EASYMESH_QUEUE_RX_TX_BE_TAGGED);
	ret = stmmac_del_hw_vlan_rx_routing_fltr(priv, priv->dev, priv->hw, EZMESH_VID_FILTER,
						 true);
	if (ret)
		netdev_err(priv->dev, "Failed to disable Ezmesh VLAN filtering");
	return ret;
}

static void free_ipa_tx_resources(struct net_device *ndev,
				  struct channel_info *channel)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_tx_queue *tx_q = &priv->tx_queue[channel->channel_num];
	u32 i;

	ioss_log_msg(NULL, "Start");

	if (channel->ch_flags == STMMAC_CONTIG_BUFS) {
		if (channel->mem_ops) {
			channel->mem_ops->free_descs(ndev,
						     channel->desc_addr.desc_virt_addrs_base,
						     channel->desc_cnt * channel->desc_size,
						     &tx_q->dma_tx_phy,
						     channel->mem_ops, channel);
		} else {
			dma_free_coherent(priv->device,
					  channel->desc_size * channel->desc_cnt,
					  channel->desc_addr.desc_virt_addrs_base,
					  tx_q->dma_tx_phy);
		}
		if (channel->mem_ops) {
			channel->mem_ops->free_buf(ndev,
						   channel->buff_pool_addr.buff_pool_va_addrs_base[0],
						   channel->desc_cnt * channel->buf_size,
						   &tx_q->buff_tx_phy,
						   channel->mem_ops, channel);
		} else {
			dma_free_coherent(priv->device,
					  channel->buf_size * channel->desc_cnt,
					  channel->buff_pool_addr.buff_pool_va_addrs_base[0],
					  tx_q->buff_tx_phy);
		}
	} else {
		for (i = 0; i < channel->desc_cnt; i++) {
			dma_unmap_single(priv->device,
					 tx_q->tx_offload_skbuff_dma[i],
					 channel->buf_size, DMA_TO_DEVICE);

			if (tx_q->tx_offload_skbuff[i])
				dev_kfree_skb_any(tx_q->tx_offload_skbuff[i]);

			channel->buff_pool_addr.buff_pool_dma_addrs_base[i] = 0;
			channel->buff_pool_addr.buff_pool_va_addrs_base[i] = NULL;
		}
		dma_free_coherent(priv->device, channel->desc_size * channel->desc_cnt,
				  channel->desc_addr.desc_virt_addrs_base,
				  tx_q->dma_tx_phy);
		kfree(tx_q->tx_offload_skbuff);
		kfree(tx_q->tx_offload_skbuff_dma);
	}
}

static void free_ipa_rx_resources(struct net_device *ndev,
				  struct channel_info *channel)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_rx_queue *rx_q = &priv->rx_queue[channel->channel_num];
	u32 i;

	if (channel->ch_flags == STMMAC_CONTIG_BUFS) {
		if (channel->mem_ops) {
			channel->mem_ops->free_descs(ndev,
						     channel->desc_addr.desc_virt_addrs_base,
						     channel->desc_cnt * channel->desc_size,
						     &rx_q->dma_rx_phy,
						     channel->mem_ops, channel);
		} else {
			dma_free_coherent(priv->device,
					  channel->desc_size * channel->desc_cnt,
					  channel->desc_addr.desc_virt_addrs_base,
					  rx_q->dma_rx_phy);
		}
		if (channel->mem_ops) {
			channel->mem_ops->free_buf(ndev,
						   channel->buff_pool_addr.buff_pool_va_addrs_base[0],
						   channel->desc_cnt * channel->buf_size,
						   &rx_q->buff_rx_phy,
						   channel->mem_ops, channel);
		} else {
			dma_free_coherent(priv->device,
					  channel->buf_size * channel->desc_cnt,
					  channel->buff_pool_addr.buff_pool_va_addrs_base[0],
					  rx_q->buff_rx_phy);
		}
	} else {
		for (i = 0; i < channel->desc_cnt; i++) {
			dma_unmap_single(priv->device,
					 rx_q->rx_offload_skbuff_dma[i],
					 channel->buf_size, DMA_FROM_DEVICE);

			if (rx_q->rx_offload_skbuff[i])
				dev_kfree_skb_any(rx_q->rx_offload_skbuff[i]);

			channel->buff_pool_addr.buff_pool_dma_addrs_base[i] = 0;
			channel->buff_pool_addr.buff_pool_va_addrs_base[i] = NULL;
		}
		dma_free_coherent(priv->device, channel->desc_size * channel->desc_cnt,
				  channel->desc_addr.desc_virt_addrs_base,
				  rx_q->dma_rx_phy);
		kfree(rx_q->rx_offload_skbuff);
		kfree(rx_q->rx_offload_skbuff_dma);
	}
}

static int alloc_ipa_tx_resources(struct net_device *ndev,
				  struct channel_info *channel, gfp_t flags)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_tx_queue *tx_q;
	struct sk_buff *skb;
	u32 i;

	tx_q = &priv->tx_queue[channel->channel_num];

	channel->desc_addr.desc_virt_addrs_base = (channel->mem_ops) ?
							channel->mem_ops->alloc_descs(ndev,
										      channel->desc_size * channel->desc_cnt,
										      &tx_q->dma_tx_phy,
										      (gfp_t)flags,
										      channel->mem_ops, channel) :
							dma_alloc_coherent(priv->device,
									   channel->desc_size * channel->desc_cnt,
									   &tx_q->dma_tx_phy, flags);

	if (!channel->desc_addr.desc_virt_addrs_base) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_mem;
	}

	tx_q->dma_tx = channel->desc_addr.desc_virt_addrs_base;
	channel->desc_addr.desc_dma_addrs_base = tx_q->dma_tx_phy;

	if (channel->ch_flags == STMMAC_CONTIG_BUFS) {
		channel->buff_pool_addr.buff_pool_va_addrs_base[0] = (channel->mem_ops) ?
							channel->mem_ops->alloc_buf(ndev,
										    channel->buf_size * channel->desc_cnt,
										    &tx_q->buff_tx_phy,
										    (gfp_t)flags,
										    channel->mem_ops, channel) :
							dma_alloc_coherent(priv->device,
									   channel->buf_size * channel->desc_cnt,
									   &tx_q->buff_tx_phy, flags);

		if (!channel->buff_pool_addr.buff_pool_va_addrs_base[0]) {
			netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
			goto err_mem;
		}
		channel->buff_pool_addr.buff_pool_dma_addrs_base[0] = tx_q->buff_tx_phy;
		tx_q->buffer_tx_va_addr = channel->buff_pool_addr.buff_pool_va_addrs_base[0];
		return 0;
	}

	tx_q->tx_offload_skbuff_dma = kcalloc(channel->desc_cnt,
					      sizeof(*tx_q->tx_offload_skbuff_dma), flags);
	if (!tx_q->tx_offload_skbuff_dma) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_mem;
	}

	tx_q->tx_offload_skbuff = kcalloc(channel->desc_cnt, sizeof(struct sk_buff *), flags);
	if (!tx_q->tx_offload_skbuff) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_mem;
	}

	for (i = 0; i < channel->desc_cnt; i++) {
		skb = __netdev_alloc_skb_ip_align(priv->dev, channel->buf_size, flags);

		if (!skb) {
			netdev_err(priv->dev,
				   "%s: Rx init fails; skb is NULL\n", __func__);
			goto err_mem;
		}

		tx_q->tx_offload_skbuff[i] = skb;
		tx_q->tx_offload_skbuff_dma[i] = dma_map_single(priv->device, skb->data,
								channel->buf_size, DMA_TO_DEVICE);

		if (dma_mapping_error(priv->device, tx_q->tx_offload_skbuff_dma[i])) {
			netdev_err(priv->dev, "%s: DMA mapping error\n", __func__);
			dev_kfree_skb_any(skb);
			goto err_mem;
		}

		channel->buff_pool_addr.buff_pool_va_addrs_base[i] = (void *)tx_q->tx_offload_skbuff[i]->data;
		channel->buff_pool_addr.buff_pool_dma_addrs_base[i] = tx_q->tx_offload_skbuff_dma[i];
	}
	return 0;

err_mem:
	free_ipa_tx_resources(ndev, channel);
	return -ENOMEM;
}

static int alloc_ipa_rx_resources(struct net_device *ndev, struct channel_info *channel,
				  gfp_t flags)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_rx_queue *rx_q;
	struct sk_buff *skb;
	u32 i;

	rx_q = &priv->rx_queue[channel->channel_num];

	channel->desc_addr.desc_virt_addrs_base = (channel->mem_ops) ?
							channel->mem_ops->alloc_descs(ndev,
										      channel->desc_size * channel->desc_cnt,
										      &rx_q->dma_rx_phy,
										      (gfp_t)flags,
										      channel->mem_ops, channel) :
							dma_alloc_coherent(priv->device,
									   channel->desc_size * channel->desc_cnt,
									   &rx_q->dma_rx_phy, flags);

	if (!channel->desc_addr.desc_virt_addrs_base) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_mem;
	}

	rx_q->dma_rx = channel->desc_addr.desc_virt_addrs_base;
	channel->desc_addr.desc_dma_addrs_base = rx_q->dma_rx_phy;

	if (channel->ch_flags == STMMAC_CONTIG_BUFS) {
		channel->buff_pool_addr.buff_pool_va_addrs_base[0] = (channel->mem_ops) ?
							channel->mem_ops->alloc_buf(ndev,
										    channel->buf_size * channel->desc_cnt,
										    &rx_q->buff_rx_phy,
										    (gfp_t)flags,
										    channel->mem_ops, channel) :
							dma_alloc_coherent(priv->device,
									   channel->buf_size * channel->desc_cnt,
									   &rx_q->buff_rx_phy, flags);

		if (!channel->buff_pool_addr.buff_pool_va_addrs_base[0]) {
			netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
			goto err_mem;
		}
		channel->buff_pool_addr.buff_pool_dma_addrs_base[0] = rx_q->buff_rx_phy;
		rx_q->buffer_rx_va_addr = channel->buff_pool_addr.buff_pool_va_addrs_base[0];
		return 0;
	}

	rx_q->rx_offload_skbuff_dma = kcalloc(channel->desc_cnt,
					      sizeof(*rx_q->rx_offload_skbuff_dma), flags);

	if (!rx_q->rx_offload_skbuff_dma) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_mem;
	}

	rx_q->rx_offload_skbuff = kcalloc(channel->desc_cnt, sizeof(struct sk_buff *), flags);
	if (!rx_q->rx_offload_skbuff) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_mem;
	}

	for (i = 0; i < channel->desc_cnt; i++) {
		skb = __netdev_alloc_skb_ip_align(priv->dev, channel->buf_size, flags);

		if (!skb) {
			netdev_err(priv->dev,
				   "%s: Rx init fails; skb is NULL\n", __func__);
			goto err_mem;
		}

		rx_q->rx_offload_skbuff[i] = skb;
		rx_q->rx_offload_skbuff_dma[i] = dma_map_single(priv->device, skb->data,
								channel->buf_size, DMA_FROM_DEVICE);

		if (dma_mapping_error(priv->device, rx_q->rx_offload_skbuff_dma[i])) {
			netdev_err(priv->dev, "%s: DMA mapping error\n", __func__);
			dev_kfree_skb_any(skb);
			goto err_mem;
		}

		channel->buff_pool_addr.buff_pool_va_addrs_base[i] = (void *)rx_q->rx_offload_skbuff[i]->data;
		channel->buff_pool_addr.buff_pool_dma_addrs_base[i] = rx_q->rx_offload_skbuff_dma[i];
	}
	return 0;

err_mem:
	free_ipa_rx_resources(ndev, channel);
	return -ENOMEM;
}

static void stmmac_init_ipa_tx_ch(struct stmmac_priv *priv, struct channel_info *channel)
{
	u32 i;
	u32 chan = channel->channel_num;
	struct stmmac_tx_queue *tx_q = &priv->tx_queue[chan];

	for (i = 0; i < channel->desc_cnt; i++) {
		struct dma_desc *p;

		p = tx_q->dma_tx + i;

		stmmac_clear_desc(priv, p);
		if (channel->ch_flags == STMMAC_CONTIG_BUFS) {
			stmmac_set_desc_addr(priv, p,
					     (tx_q->buff_tx_phy + (i * channel->buf_size)));
		} else {
			stmmac_set_desc_addr(priv, p,
					     channel->buff_pool_addr.buff_pool_dma_addrs_base[i]);
		}
	}

	ioss_log_msg(NULL, "dma_tx_phy = 0x%p", tx_q->dma_tx_phy);

	stmmac_init_chan(priv, priv->ioaddr, priv->plat->dma_cfg, chan);
	stmmac_init_tx_chan(priv, priv->ioaddr, priv->plat->dma_cfg,
			    tx_q->dma_tx_phy, chan);

	tx_q->tx_tail_addr = tx_q->dma_tx_phy;
	stmmac_set_tx_tail_ptr(priv, priv->ioaddr,
			       tx_q->tx_tail_addr, chan);

	stmmac_set_tx_ring_len(priv, priv->ioaddr, channel->desc_cnt - 1, chan);
	
	if (priv->plat->tx_queues_cfg[chan].mode_to_use != MTL_QUEUE_AVB)
		stmmac_set_mtl_tx_queue_weight(priv, priv->hw,
					       priv->plat->tx_queues_cfg[chan].weight, chan);

	ioss_log_msg(NULL, "desc_cnt = %d $ buf_size = %d", channel->desc_cnt, channel->buf_size);
}

static void stmmac_init_ipa_rx_ch(struct stmmac_priv *priv, struct channel_info *channel)
{
	u32 i;
	u32 chan = channel->channel_num;
	struct stmmac_rx_queue *rx_q = &priv->rx_queue[chan];

	for (i = 0; i < channel->desc_cnt; i++) {
		struct dma_desc *p;

		p = rx_q->dma_rx + i;

		stmmac_init_rx_desc(priv, &rx_q->dma_rx[i],
				    priv->use_riwt, priv->mode,
				    (i == channel->desc_cnt - 1),
				    channel->buf_size);

		if (channel->ch_flags == STMMAC_CONTIG_BUFS) {
			stmmac_set_desc_addr(priv, p, (rx_q->buff_rx_phy + (i * channel->buf_size)));
		} else {
			stmmac_set_desc_addr(priv, p,
					     channel->buff_pool_addr.buff_pool_dma_addrs_base[i]);
		}
	}
	ioss_log_msg(NULL, "dma_rx_phy = 0x%p", rx_q->dma_rx_phy);

	stmmac_init_chan(priv, priv->ioaddr, priv->plat->dma_cfg, chan);
	stmmac_init_rx_chan(priv, priv->ioaddr, priv->plat->dma_cfg,
			    rx_q->dma_rx_phy, chan);

	rx_q->rx_tail_addr = rx_q->dma_rx_phy +
				 (channel->desc_cnt * sizeof(struct dma_desc));
	stmmac_set_rx_tail_ptr(priv, priv->ioaddr, rx_q->rx_tail_addr, chan);

	stmmac_set_rx_ring_len(priv, priv->ioaddr, (channel->desc_cnt - 1), chan);
	stmmac_set_dma_bfsize(priv, priv->ioaddr, (channel->buf_size/8), chan);

	ioss_log_msg(NULL, "desc_cnt = %d $ buf_size = %d",
				channel->desc_cnt,
				channel->buf_size);
}

static void dealloc_ipa_tx_resources(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_tx_queue *tx_q;
	u32 ch = channel->channel_num;

	tx_q = &priv->tx_queue[ch];
	tx_q->priv_data = NULL;

	free_ipa_tx_resources(ndev, channel);

	tx_q->dma_tx = NULL;
}

static void dealloc_ipa_rx_resources(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_rx_queue *rx_q;
	u32 ch = channel->channel_num;

	rx_q = &priv->rx_queue[ch];
	rx_q->priv_data = NULL;

	free_ipa_rx_resources(ndev, channel);

	rx_q->dma_rx = NULL;
}

/*!
 * \brief API to allocate a channel for IPA  Tx/Rx datapath,
 *	  allocate memory and buffers for the DMA channel, setup the
 *	  descriptors and configure the required registers.
 *
 *	  The API will check for NULL pointers and Invalid arguments such as,
 *	  out of bounds buf size > 9K bytes, descriptor count > 512
 *
 * \param[in] channel_input : data structure specifying all input needed to request a channel
 *
 * \return channel_info : Allocate memory for channel_info structure and initialize the structure members
 *			  NULL on fail
 * \remarks :In case of Tx, only TDES0 and TDES1 will be updated with buffer addresses. TDES2 and TDES3
 *	    must be updated by the offloading driver.
 */
struct channel_info *request_channel(struct request_channel_input *channel_input)
{
	struct channel_info *channel;
	struct stmmac_priv *priv;
	struct qcom_ethqos *ethqos;
	unsigned int queue_num;

	if (!channel_input->ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return NULL;
	}

	priv = netdev_priv(channel_input->ndev);
        if (!priv) {
                pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
                return NULL;
        }

	if ((channel_input->ch_dir != CH_DIR_RX) &&
		(channel_input->ch_dir != CH_DIR_TX)) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid Channel direction\n", __func__);
		return NULL;
	}

	ethqos = priv->plat->bsp_priv;
	if (!ethqos) {
		ETHQOSERR("ethqos is NULL\n");
		return NULL;
	}

	if (channel_input->desc_cnt > IPA_MAX_DESC_CNT) {
		netdev_err(priv->dev,
			   "%s: ERROR: Descriptor count greater than %d\n", __func__, IPA_MAX_DESC_CNT);
		return NULL;
	}

	if (channel_input->buf_size > IPA_MAX_BUFFER_SIZE) {
		netdev_err(priv->dev,
			   "%s: ERROR: Buffer size greater than %d bytes\n", __func__, IPA_MAX_BUFFER_SIZE);

		return NULL;
	}

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		return NULL;
	}

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
	/*
	  To increase the power consumer count so that to avoid disabling
	  the power before IPA channels gets disconnected.
	 */
	 if (priv->plat->enable_power_saving)
		priv->plat->enable_power_saving(channel_input->ndev, false);
#endif

	channel->buf_size = channel_input->buf_size;
	channel->client_ch_priv = channel_input->client_ch_priv;
	channel->desc_cnt = channel_input->desc_cnt;
	channel->desc_size = sizeof(struct dma_desc);
	channel->direction = channel_input->ch_dir;
	channel->mem_ops = channel_input->mem_ops;
	channel->ch_flags = channel_input->ch_flags;
	channel->traffic_type_info = channel_input->traffic_type_info;

	if (!strcmp(channel_input->ipa_config_info, "ezmesh"))
		channel->ezmesh_enabled = true;

	if (!strcmp(channel_input->ipa_config_info, "tsn"))
		channel->tsn_enabled = true;

	if (!strcmp(channel_input->ipa_config_info, "qos"))
		channel->qos_enabled = true;

	if ((channel->ezmesh_enabled) && (channel->traffic_type_info == IOSS_TRAFFIC_BE_TAGGED))
	{
		channel->channel_num = EASYMESH_CHANNEL_RX_TX_BE_TAGGED;
		queue_num = EASYMESH_QUEUE_RX_TX_BE_TAGGED;
	}
	else if ((channel->tsn_enabled) && (channel->traffic_type_info == IOSS_TRAFFIC_LL)
			&& (channel->direction == CH_DIR_TX)) {
		channel->channel_num = TSN_CHANNEL_TX_LL;
		queue_num = TSN_QUEUE_TX_LL;
	}
	else if (channel->qos_enabled) {
		channel->channel_num = channel_input->channel_num;
		queue_num = channel->channel_num;
	}
	else {
		channel->channel_num = DEFAULT_CHANNEL_RX_TX;
		queue_num = DEFAULT_QUEUE_RX_TX;
	}

	channel->buff_pool_addr.buff_pool_va_addrs_base = kcalloc(channel_input->desc_cnt,
								  sizeof(void *),
								  (gfp_t)channel_input->flags);

	if (!channel->buff_pool_addr.buff_pool_va_addrs_base) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_buff_pool_va_mem_alloc;
	}

	channel->buff_pool_addr.buff_pool_dma_addrs_base = kcalloc(channel_input->desc_cnt,
								   sizeof(dma_addr_t),
								   (gfp_t)channel_input->flags);

	if (!channel->buff_pool_addr.buff_pool_dma_addrs_base) {
		netdev_err(priv->dev, "%s: ERROR: allocating memory\n", __func__);
		goto err_buff_pool_dma_mem_alloc;
	}

	channel->dma_pdev = (struct pci_dev *)priv->device;

	/* Allocate resources for descriptor and buffer */
	if (channel_input->ch_dir == CH_DIR_TX) {
		if (alloc_ipa_tx_resources(channel_input->ndev, channel,
					   (gfp_t)channel_input->flags)) {
			netdev_err(priv->dev,
				   "%s: ERROR: allocating Tx resources\n", __func__);
			goto err_buff_dma_mem_alloc;
		}
	} else if (channel_input->ch_dir == CH_DIR_RX) {
		if (alloc_ipa_rx_resources(channel_input->ndev,
					   channel, (gfp_t)channel_input->flags)) {
			netdev_err(priv->dev,
				   "%s: ERROR: allocating Rx resources\n", __func__);
			goto err_buff_dma_mem_alloc;
		}
	}

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_SCM)
	if (ethqos->emac_ver == EMAC_HW_v4_0_0) {

		/* Disabling interrupts on all channels */
		qcom_scm_call_ipa_intr_config(ethqos->rgmii_phy_base, EMAC_SELECT_ALLCH);
	}
#else
	/* Disabling interrupts on all channels */
	rgmii_updatel(ethqos, EMAC_SELECT_ALLCH, 0, EMAC0_EMAC_INTERRUPT_ENABLE);
#endif
	/* Configure DMA registers */
	if (channel_input->ch_dir == CH_DIR_TX) {
		stmmac_stop_tx(priv, priv->ioaddr, channel->channel_num);
		stmmac_init_ipa_tx_ch(priv, channel);

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
		channel_input->tail_ptr_addr = XGMAC_DMA_CH_TxDESC_TAIL_LPTR(channel->channel_num);
#else
		channel_input->tail_ptr_addr = DMA_CHAN_TX_END_ADDR(channel->channel_num);
#endif
	} else if (channel_input->ch_dir == CH_DIR_RX) {
		stmmac_stop_rx(priv, priv->ioaddr, channel->channel_num);
		stmmac_init_ipa_rx_ch(priv, channel);
#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
		channel_input->tail_ptr_addr = XGMAC_DMA_CH_RxDESC_TAIL_LPTR(channel->channel_num);
#else
		channel_input->tail_ptr_addr = DMA_CHAN_RX_END_ADDR(channel->channel_num);
#endif

		stmmac_map_mtl_to_dma(priv, priv->hw, queue_num, channel->channel_num);
		if ((queue_num == EASYMESH_QUEUE_RX_TX_BE) && (channel->ezmesh_enabled))
			enable_ezmesh_vlan_filtering(priv);
		if ((queue_num == QOS_RX_PCP0_QUEUE) && (channel->qos_enabled) && (priv->unique_filter_new != PCP))
			stmmac_enable_queue_dynamic_dma_ch_selection(priv, priv->hw, QOS_RX_PCP0_QUEUE);
	}

	return channel;

err_buff_dma_mem_alloc:
	kfree(channel->buff_pool_addr.buff_pool_dma_addrs_base);
err_buff_pool_dma_mem_alloc:
	kfree(channel->buff_pool_addr.buff_pool_va_addrs_base);
err_buff_pool_va_mem_alloc:
	channel->desc_addr.desc_dma_addrs_base = 0;
	kfree(channel);

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
	//This will decrement the power consumer count.
	if(priv->plat->enable_power_saving)
		priv->plat->enable_power_saving(priv->dev, true);
#endif

	return NULL;
}
EXPORT_SYMBOL_GPL(request_channel);

/*!
 * \brief Release the resources associated with the channel
 *	  and reset the descriptors and registers
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer or memory buffers in channel pointer are NULL
 *
 * \remarks : DMA Channel has to be stopped prior to invoking this API
 */
int release_channel(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;
	struct mem_ops *mem_ops;

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!channel) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if ((channel->direction != CH_DIR_RX) &&
	    (channel->direction != CH_DIR_TX)) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid Channel direction\n", __func__);
		return -EINVAL;
	}

	if (!channel->desc_addr.desc_virt_addrs_base ||
	    !channel->desc_addr.desc_dma_addrs_base ||
	    !channel->buff_pool_addr.buff_pool_dma_addrs_base ||
	    !channel->buff_pool_addr.buff_pool_va_addrs_base) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid memory pointers\n", __func__);
		return -EINVAL;
	}

	if (!valid_ch_config(channel)){
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel config\n", __func__);
			return -EINVAL;
	}

	priv = netdev_priv(ndev);
	mem_ops = channel->mem_ops;

	if (channel->direction == CH_DIR_TX) {
		stmmac_stop_tx(priv, priv->ioaddr, channel->channel_num);
		stmmac_flush_tx_mtl(priv, priv->hw, channel->channel_num);
		dealloc_ipa_tx_resources(ndev, channel);
	} else if (channel->direction == CH_DIR_RX) {
		stmmac_stop_rx(priv, priv->ioaddr, channel->channel_num);
		dealloc_ipa_rx_resources(ndev, channel);
	}

	kfree(channel->buff_pool_addr.buff_pool_va_addrs_base);
	kfree(channel->buff_pool_addr.buff_pool_dma_addrs_base);
	channel->desc_addr.desc_dma_addrs_base = 0;
	kfree(channel);

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
	//This will decrement the power consumer count.
	if(priv->plat->enable_power_saving)
		priv->plat->enable_power_saving(priv->dev, true);
#endif

	return 0;
}
EXPORT_SYMBOL_GPL(release_channel);

static int enable_dma_interrupt_fields(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;
	u32 reg;

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
	unsigned long DMA_TX_INT_MASK = 0xFC07;
	unsigned long DMA_TX_INT_RESET_MASK = 0xFBC0;
	unsigned long DMA_RX_INT_MASK = 0xFBC0;
	unsigned long DMA_RX_INT_RESET_MASK = 0xF087;
#else
	unsigned long DMA_TX_INT_MASK = 0xFC07;
	unsigned long DMA_TX_INT_RESET_MASK = 0xFBC0;
	unsigned long DMA_RX_INT_MASK = 0xFBC0;
	unsigned long DMA_RX_INT_RESET_MASK = 0xF407;
#endif

	priv = netdev_priv(ndev);

	ioss_log_msg(NULL, "%s: Start", __func__);

	if (channel->direction == CH_DIR_TX) {

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
		/* clear all the interrupts which are set */
		reg = readl(priv->ioaddr + XGMAC_DMA_CH_STATUS(channel->channel_num));
		reg &= DMA_TX_INT_MASK;
		writel(reg, priv->ioaddr + XGMAC_DMA_CH_STATUS(channel->channel_num));
#else
		/* clear all the interrupts which are set */
		reg = readl(priv->ioaddr + DMA_CHAN_STATUS(channel->channel_num));

		reg &= DMA_TX_INT_MASK;
		writel(reg, priv->ioaddr + DMA_CHAN_STATUS(channel->channel_num));
#endif
		/* Enable following interrupts for Queue */
		/* NIE - Normal Interrupt Summary Enable */
		/* AIE - Abnormal Interrupt Summary Enable */
		/* FBE - Fatal Bus Error Enable */

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)

		reg = readl(priv->ioaddr + XGMAC_DMA_CH_INT_EN(channel->channel_num));
		reg &= DMA_TX_INT_RESET_MASK;
		reg |= ((0x1) << 12) | ((0x1) << 14) | ((0x1) << 15) | ((0x1) << 7);

		ioss_log_msg(NULL, "ch = %d Interrupt Enabled = 0x%x",
					channel->channel_num,
					reg);

		writel(reg, priv->ioaddr + XGMAC_DMA_CH_INT_EN(channel->channel_num));
#else
		reg = readl(priv->ioaddr + DMA_CHAN_INTR_ENA(channel->channel_num));
		reg &= DMA_TX_INT_RESET_MASK;
		reg |= ((0x1) << 12) | ((0x1) << 14) | ((0x1) << 15);

		ioss_log_msg(NULL, "ch = %d Interrupt Enabled = 0x%x",
					channel->channel_num,
					reg);

		writel(reg, priv->ioaddr + DMA_CHAN_INTR_ENA(channel->channel_num));
#endif

	} else if (channel->direction == CH_DIR_RX) {

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
		/* clear all the interrupts which are set */
		reg = readl(priv->ioaddr + XGMAC_DMA_CH_STATUS(channel->channel_num));
		reg &= DMA_RX_INT_MASK;
		writel(reg, priv->ioaddr + XGMAC_DMA_CH_STATUS(channel->channel_num));
#else
		/* clear all the interrupts which are set */
		reg = readl(priv->ioaddr + DMA_CHAN_STATUS(channel->channel_num));
		reg &= DMA_RX_INT_MASK;
		writel(reg, priv->ioaddr + DMA_CHAN_STATUS(channel->channel_num));
#endif
		/* Enable following interrupts for Queue */
		/* NIE - Normal Interrupt Summary Enable */
		/* AIE - Abnormal Interrupt Summary Enable */
		/* FBE - Fatal Bus Error Enable */

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
		reg = readl(priv->ioaddr + XGMAC_DMA_CH_INT_EN(channel->channel_num));
		reg &= (unsigned long)DMA_RX_INT_RESET_MASK;
		reg |= ((0x1) << 12) | ((0x1) << 14) | ((0x1) << 15) | ((0x1) << 7);

		ioss_log_msg(NULL, "ch = %d Interrupt Enabled = 0x%x",
					channel->channel_num,
					reg);

		writel(reg, priv->ioaddr + XGMAC_DMA_CH_INT_EN(channel->channel_num));
#else
		reg = readl(priv->ioaddr + DMA_CHAN_INTR_ENA(channel->channel_num));
		reg &= (unsigned long)DMA_RX_INT_RESET_MASK;
		reg |= ((0x1) << 12) | ((0x1) << 14) | ((0x1) << 15);

		ioss_log_msg(NULL, "ch = %d Interrupt Enabled = 0x%x",
					channel->channel_num,
					reg);

		writel(reg, priv->ioaddr + DMA_CHAN_INTR_ENA(channel->channel_num));
#endif
	} else {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel direction\n", __func__);
		return -EINVAL;
	}
	return 0;
}

/*!
 * \brief
 *
 *	  The API will check for NULL pointers and Invalid arguments such as,
 *	  non IPA channel.
 *
 * \param[in] ndev : Stmmac netdev  data structure
 * \param[in] channel : Pointer to channel info containing the channel information
 * \param[in] addr :  Address location to which the  write is to be performed
 * \param[in] data :  Address location to which the  write is to be performed
 *
 * \return : O for success
 *	     -EPERM if non IPA channels are accessed, out of range PCIe access location for CM3
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL
 *
 */
int request_event(struct net_device *ndev, struct channel_info *channel, dma_addr_t addr, u64 data)
{
	struct stmmac_priv *priv;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!channel) {
		netdev_err(priv->dev,
				"%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	channel->dma_map_dbaddr = addr;

	if (!valid_ch_config(channel)){

		netdev_err(priv->dev,
				"%s: ERROR: Invalid channel\n", __func__);
			goto error;
	}

	return 0;

error:

	channel->dma_map_dbaddr = 0;

	return -EPERM;
}
EXPORT_SYMBOL_GPL(request_event);

/*!
 * \brief
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL
 */
int release_event(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!channel) {
		netdev_err(priv->dev,
				"%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if (!valid_ch_config(channel)){
		netdev_err(priv->dev,
				"%s: ERROR: Invalid channel\n", __func__);
			return -EPERM;
	}

	channel->dma_map_dbaddr = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(release_event);

/*!
 * \brief Enable interrupt generation for given channel
 *
 * The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL
 */
int enable_event(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;
	struct qcom_ethqos *ethqos;
	int ret;
	u32 reg = 0;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	ethqos = priv->plat->bsp_priv;
	if (!ethqos) {
		ETHQOSERR("ethqos is NULL\n");
		return -EINVAL;
	}

	if (!channel) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if (!valid_ch_config(channel)){
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel\n", __func__);
			return -EPERM;
	}

	if (channel->direction == CH_DIR_TX) {
#if IS_ENABLED(CONFIG_ETHQOS_QCOM_SCM)
		if (channel->ezmesh_enabled)
		{
			reg |= (EMAC_CHANNEL_INTR_EN | EMAC0_IPA_TX_INTR_EN | EMAC0_IPA_TX2_INTR_EN);
		}
		else if (channel->tsn_enabled)
		{
			reg |= (EMAC_CHANNEL_INTR_EN | EMAC0_IPA_TX_INTR_EN | EMAC0_IPA_TX3_INTR_EN);
		}
		else
		{
			reg |= (EMAC_CHANNEL_INTR_EN | (EMAC0_IPA_TX_INTR_EN << channel->channel_num));
		}
		if (ethqos->emac_ver == EMAC_HW_v4_0_0) {
			/* Disabling interrupts on all channels */
			qcom_scm_call_ipa_intr_config(ethqos->rgmii_phy_base, reg);
		}
#else
		reg |= (EMAC0_IPA_TX_INTR_EN << channel->channel_num);
		/* Disabling interrupts on all channels */
		rgmii_updatel(ethqos, reg, reg, EMAC0_EMAC_INTERRUPT_ENABLE);
#endif

	} else if (channel->direction == CH_DIR_RX) {
#if IS_ENABLED(CONFIG_ETHQOS_QCOM_SCM)
		if (channel->ezmesh_enabled)
		{
			reg |= (EMAC_CHANNEL_INTR_EN | EMAC0_IPA_RX_INTR_EN | EMAC0_IPA_RX2_INTR_EN);
		}
		else
		{
			reg |= (EMAC_CHANNEL_INTR_EN | (EMAC0_IPA_RX_INTR_EN << channel->channel_num));
		}
		if (ethqos->emac_ver == EMAC_HW_v4_0_0) {
			/* Disabling interrupts on all channels */
			qcom_scm_call_ipa_intr_config(ethqos->rgmii_phy_base, reg);
		}
#else
		reg |= (EMAC0_IPA_RX_INTR_EN << channel->channel_num);
		rgmii_updatel(ethqos, reg, reg, EMAC0_EMAC_INTERRUPT_ENABLE);
#endif
	} else {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel direction\n", __func__);
		return -EINVAL;
	}

	ret = enable_dma_interrupt_fields(ndev, channel);

	return ret;
}
EXPORT_SYMBOL_GPL(enable_event);

/*!
 * \brief Disable interrupt generation for given channel
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL
 */
int disable_event(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;
	struct qcom_ethqos *ethqos;
	u32 reg = 0;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	ethqos = priv->plat->bsp_priv;
	if (!ethqos) {
		ETHQOSERR("ethqos is NULL\n");
		return -EINVAL;
	}

	if (!channel) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if (!valid_ch_config(channel)){
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel\n", __func__);
			return -EPERM;
	}

	if (channel->direction == CH_DIR_TX) {
		if (channel->ezmesh_enabled)
		{
			reg |= (EMAC0_IPA_TX_INTR_EN | EMAC0_IPA_TX2_INTR_EN);
		}
		else if (channel->tsn_enabled)
		{
			reg |= (EMAC0_IPA_TX_INTR_EN | EMAC0_IPA_TX3_INTR_EN);
		}
		else
		{
			reg = EMAC0_IPA_TX_INTR_EN;
		}
#if IS_ENABLED(CONFIG_ETHQOS_QCOM_SCM)
		if (ethqos->emac_ver == EMAC_HW_v4_0_0)
			qcom_scm_call_ipa_intr_config(ethqos->rgmii_phy_base, reg);
#else
		rgmii_updatel(ethqos, reg, 0, EMAC0_EMAC_INTERRUPT_ENABLE);
#endif
	} else if (channel->direction == CH_DIR_RX) {
		if (channel->ezmesh_enabled)
		{
			reg |= (EMAC0_IPA_RX_INTR_EN | EMAC0_IPA_RX2_INTR_EN);
		}
		else
		{
			reg = EMAC0_IPA_RX_INTR_EN;
		}
#if IS_ENABLED(CONFIG_ETHQOS_QCOM_SCM)
		if (ethqos->emac_ver == EMAC_HW_v4_0_0)
			qcom_scm_call_ipa_intr_config(ethqos->rgmii_phy_base, reg);
#else
		rgmii_updatel(ethqos, reg, 0, EMAC0_EMAC_INTERRUPT_ENABLE);
#endif
	} else {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel direction\n", __func__);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(disable_event);


/*!
 * \brief Control the Rx DMA interrupt generation by modfying the Rx WDT timer
 *
 *	  The API will check for NULL pointers and Invalid arguments such as,
 *	  non IPA channel, event moderation for Tx path
 *
 * \param[in] ndev : STMMAC netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 * \param[in] wdt : Watchdog timeout value in clock cycles
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed, IPA Tx channel
 *	     -ENODEV if ndev is NULL, tc956xmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL
 */
int set_event_mod(struct net_device *ndev, struct channel_info *channel, unsigned int wdt)
{
	struct stmmac_priv *priv;
	u32 rx_cnt;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!channel) {
		netdev_err(priv->dev,
				"%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if ((channel->direction == CH_DIR_TX) ||
		(!valid_ch_config(channel))){

		netdev_err(priv->dev,
				"%s: ERROR: Invalid channel\n", __func__);
		return -EPERM;
	}

	if (wdt > MAX_DMA_RIWT) {
		netdev_err(priv->dev,
				"%s: ERROR: Timeout value Out of range\n", __func__);
		return -EINVAL;
	}

	rx_cnt = priv->plat->rx_queues_to_use;

	if ((priv->use_riwt) && (priv->hw->dma->rx_watchdog)) {
		priv->rx_riwt[channel->channel_num] = wdt;
		priv->hw->dma->rx_watchdog(priv->ioaddr, wdt, channel->channel_num);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(set_event_mod);

/*!
 * \brief This API will configure the FRP table with the parameters passed through rx_filter_entry.
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel.
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] filter_params: filter_params containig the parameters based on which packet will pass or drop
 * \return : Return 0 on success, -ve value on error
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL filter_params, if number of entries > 72
 *
 * \remarks : The entries should be prepared considering the filtering and routing to CortexA also
 *	      MAC Rx will be stopped while updating FRP table dynamically.
 */

int set_rx_filter(struct net_device *ndev, struct rxp_filter_entry *filter_entries)
{
	struct stmmac_priv *priv;
	u32 ret = -EINVAL;
	int i;

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!filter_entries) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid filter parameter entries\n", __func__);
		return -EINVAL;
	}

	memset(priv->tc_entries, 0, sizeof(*priv->tc_entries));

	for (i = 0; i < priv->tc_entries_max; i++) {
		priv->tc_entries[i].in_use = true;
		priv->tc_entries[i].is_last = true;
		priv->tc_entries[i].is_frag = false;
		priv->tc_entries[i].prio = ~0x0;
		priv->tc_entries[i].handle = 0;

		memcpy(&priv->tc_entries[i].val, &filter_entries[i], sizeof(filter_entries[0]));
	}

	ret = stmmac_rxp_config(priv, priv->ioaddr, priv->tc_entries, priv->tc_entries_max);

	return ret;
}
EXPORT_SYMBOL_GPL(set_rx_filter);

/*!
 * \brief This API will clear the FRP filters and route all packets to RxCh0
 *
 *	 The API will check for NULL pointers
 *
 * \param[in] ndev : STMMAC netdev data structure
 * \return : Return 0 on success, -ve value on error
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *
 * \remarks : MAC Rx will be stopped while updating FRP table dynamically.

 */

int clear_rx_filter(struct net_device *ndev)
{
	struct stmmac_priv *priv;
	struct rxp_filter_entry filter_entries;
	u32 ret = -EINVAL;
	int i;

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	/* Create FRP entries to route all packets to RxCh0 */
	filter_entries.match_data = 0x00000000;
	filter_entries.match_en = 0x00000000;
	filter_entries.af = 1;
	filter_entries.rf = 0;
	filter_entries.im = 0;
	filter_entries.nc = 0;
	filter_entries.res1 = 0;
	filter_entries.frame_offset = 0;
	filter_entries.ok_index = 0;
	filter_entries.dma_ch_no = 1;
	filter_entries.res2 = 0;

	for (i = 0; i < priv->tc_entries_max; i++) {
		priv->tc_entries[i].in_use = true;
		priv->tc_entries[i].is_last = true;
		priv->tc_entries[i].is_frag = false;
		priv->tc_entries[i].prio = ~0x0;
		priv->tc_entries[i].handle = 0;

		memcpy(&priv->tc_entries[i].val, &filter_entries,
		       sizeof(struct rxp_filter_entry));
	}

	ret = stmmac_rxp_config(priv, priv->ioaddr, priv->tc_entries, priv->tc_entries_max);

	return ret;
}
EXPORT_SYMBOL_GPL(clear_rx_filter);

/*!
 * \brief Start the DMA channel. channel_dir member variable
 *	  will be used to start the Tx/Rx channel
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL

 */
int start_channel(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;
	struct mac_addr_list mac_addr;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!channel) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if (!valid_ch_config(channel))
	{
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel\n", __func__);
			return -EPERM;
	}

	if (channel->direction == CH_DIR_TX) {
		netdev_dbg(priv->dev, "DMA Tx process started in channel = %d\n", channel->channel_num);
		stmmac_start_tx(priv, priv->ioaddr, channel->channel_num);
	} else if (channel->direction == CH_DIR_RX) {
		mac_addr.ae = MAC_ADDR_AE;
		mac_addr.mbc = MAC_ADDR_MBC;
		mac_addr.dcs = MAC_ADDR_DCS;
		memcpy(&mac_addr.addr[0], &mac_addr_default[0], sizeof(mac_addr_default));
		set_mac_addr(ndev, &mac_addr, MAC_ADDR_INDEX);

		netdev_dbg(priv->dev, "DMA Rx process started in channel = %d\n", channel->channel_num);
		stmmac_start_rx(priv, priv->ioaddr, channel->channel_num);
	} else {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel\n", __func__);
		return -EPERM;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(start_channel);

/*!
 * \brief Stop the DMA channel. channel_dir member variable will be
 *	  used to stop the Tx/Rx channel. In case of Rx, clear the
 *	  MTL queue associated with the channel and this will result in packet drops
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer  NULL
 */
int stop_channel(struct net_device *ndev, struct channel_info *channel)
{
	struct stmmac_priv *priv;
	u32 sw_chan = 1;
	int i = 0;
	u32 reg = 0, value = 0;
	u32 ch_mask = 0;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!channel) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel info structure\n", __func__);
		return -EINVAL;
	}

	if (!valid_ch_config(channel)){
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel\n", __func__);
			return -EPERM;
	}

	if (channel->direction == CH_DIR_TX) {
		netdev_dbg(priv->dev, "DMA Tx process stopped in channel = %d\n",
			   channel->channel_num);
		stmmac_stop_tx(priv, priv->ioaddr, channel->channel_num);
	} else if (channel->direction == CH_DIR_RX) {
		netdev_dbg(priv->dev, "DMA Rx process stopped in channel = %d\n",
			   channel->channel_num);
		stmmac_stop_rx(priv, priv->ioaddr, channel->channel_num);
		if (channel->ezmesh_enabled)
			disable_ezmesh_vlan_filtering(priv);
		if (channel->channel_num == 0) {
			stmmac_map_mtl_to_dma(priv, priv->hw, channel->channel_num, sw_chan);
		} else {
			for (i = 1; i < priv->plat->rx_queues_to_use; i++) {
				reg = (i < 4) ? XGMAC_MTL_RXQ_DMA_MAP0 : XGMAC_MTL_RXQ_DMA_MAP1;
				value = readl(priv->ioaddr + reg);
				if (i < 4)
					ch_mask = (channel->channel_num << XGMAC_QxMDMACH_SHIFT(i)) & (XGMAC_QxMDMACH(i));
				else
					ch_mask = channel->channel_num;

				if (value & ch_mask == ch_mask)
					stmmac_map_mtl_to_dma(priv, priv->hw, i, sw_chan);
			}
		}
	} else {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid channel\n", __func__);
		return -EPERM;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(stop_channel);

/*!
 * \brief Configure MAC registers at a particular index in the MAC Address list
 *
 * \param[in] ndev : Stmmac netdev data structure
 * \param[in] mac_addr : Pointer to structure containing mac_addr_list that needs to updated
 *		     in MAC_Address_High and MAC_Address_Low registers
 * \param[in] index : Index in the MAC Address Register list
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if index 0 used
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if mac_addr NULL
 *
 * \remarks : Do not use the API to set register at index 0.
 *	      There is possibilty of kernel network subsytem overwriting these registers
 *	      when " tc956xmac_set_rx_mode" is invoked via "ndo_set_rx_mode" callback.
 */
int set_mac_addr(struct net_device *ndev, struct mac_addr_list *mac_addr, u8 index)
{
	struct stmmac_priv *priv;
	u32 data;

	ioss_log_msg(NULL, "Start");

	if (!ndev) {
		pr_err("%s: ERROR: Invalid netdevice pointer\n", __func__);
		return -ENODEV;
	}

	priv = netdev_priv(ndev);
	if (!priv) {
		pr_err("%s: ERROR: Invalid private data pointer\n", __func__);
		return -ENODEV;
	}

	if (!mac_addr) {
		netdev_err(priv->dev,
			   "%s: ERROR: Invalid mac addr list structure\n", __func__);
		return -EINVAL;
	}

	if (index == 0) {
		netdev_err(priv->dev,
			   "%s: ERROR: Do not use index 0\n", __func__);
		return -EPERM;
	}

#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
	data = (mac_addr->addr[5] << 8) | (mac_addr->addr[4]) |
		(mac_addr->ae << XGMAC_AE_SHIFT) | (mac_addr->mbc << XGMAC_MBC_SHIFT);
	writel(data, priv->ioaddr + XGMAC_ADDRx_HIGH(index));

	data = (mac_addr->addr[3] << 24) | (mac_addr->addr[2] << 16) |
		(mac_addr->addr[1] << 8) | mac_addr->addr[0];
	writel(data, priv->ioaddr + XGMAC_ADDRx_LOW(index));
#else
	data = (mac_addr->addr[5] << 8) | (mac_addr->addr[4]) |
			(mac_addr->ae << GMAC_AE_SHIFT) | (mac_addr->mbc << GMAC_MBC_SHIFT);
	writel(data, priv->ioaddr + GMAC_ADDR_HIGH(index));

	data = (mac_addr->addr[3] << 24) | (mac_addr->addr[2] << 16) |
		(mac_addr->addr[1] << 8) | mac_addr->addr[0];
	writel(data, priv->ioaddr + GMAC_ADDR_LOW(index));

#endif

	return 0;
}
EXPORT_SYMBOL_GPL(set_mac_addr);


#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
ssize_t stmmac_intf_width(struct stmmac_priv *priv)
{
	int intf_width = -EOPNOTSUPP;

	switch (priv->speed) {
	case SPEED_10000:
		intf_width = 32;
		break;
	case SPEED_5000:
		intf_width = 32;
		break;
	case SPEED_2500:
		/* sgmii+ supports GMII interface(width 8)
		while USXGMII and 2500 basex use xgmii interface (width 32)*/
		if (priv->plat->interface == PHY_INTERFACE_MODE_SGMII)
			intf_width = 8;
		else
			intf_width = 32;
		break;
	case SPEED_1000:
		intf_width = 8;
		break;
	case SPEED_100:
		intf_width = 4;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return intf_width;
}
EXPORT_SYMBOL_GPL(stmmac_intf_width);
#else
ssize_t stmmac_intf_width(struct stmmac_priv *priv)
{
    return -EOPNOTSUPP;
}
#endif

void stmmac_config_qos_cbs(struct stmmac_priv *priv, struct qos_struct *qos_table_info)
{
	int i = 0;

	for (i = 2; i < priv->plat->tx_queues_to_use; i++) {
		/* Configure queues for CBS*/
		ioss_qos_dev_log(NULL, "[iemac qos]: queue %d config = %d\n",
			         i, qos_table_info->tx_routing_info[i].mode_to_use);
		if (qos_table_info->tx_routing_info[i].mode_to_use == MTL_QUEUE_AVB) {
			stmmac_config_cbs(priv, priv->hw,
					  qos_table_info->tx_routing_info[i].send_slope,
					  qos_table_info->tx_routing_info[i].idle_slope,
					  qos_table_info->tx_routing_info[i].hi_credit,
					  qos_table_info->tx_routing_info[i].low_credit,
					  i);
			priv->plat->tx_queues_cfg[i].send_slope = qos_table_info->tx_routing_info[i].send_slope;
			priv->plat->tx_queues_cfg[i].idle_slope = qos_table_info->tx_routing_info[i].idle_slope;
			priv->plat->tx_queues_cfg[i].high_credit = qos_table_info->tx_routing_info[i].hi_credit;
			priv->plat->tx_queues_cfg[i].low_credit = qos_table_info->tx_routing_info[i].low_credit;
			priv->tx_ch_bw[i] = qos_table_info->tx_routing_info[i].acc_bw;
			ioss_qos_dev_log(NULL, "[iemac qos]: send_slope %d idle_slope = %d hi_credit = %d low_credit = %d\n",
			         		 qos_table_info->tx_routing_info[i].send_slope,
							 qos_table_info->tx_routing_info[i].idle_slope,
							 qos_table_info->tx_routing_info[i].hi_credit,
					  		 qos_table_info->tx_routing_info[i].low_credit);
			ioss_qos_dev_log(NULL, "[iemac qos]: queue %d bw applied = %d\n",
			         i, qos_table_info->tx_routing_info[i].acc_bw);
		}
	}
}
EXPORT_SYMBOL_GPL(stmmac_config_qos_cbs);

void stmmac_restore_qos_queue_cfg(struct stmmac_priv *priv, struct qos_struct *qos_table_info)
{
	int queue = 0;
	u32 rxmode = priv->plat->rx_queues_cfg[queue].mode_to_use;
	for (queue = 0; queue < 5; queue++) {
		/* Enable the disabled queues */
		if (priv->queue_dis[queue]) {
			stmmac_rx_queue_enable(priv, priv->hw, rxmode, queue);
			priv->queue_dis[queue] = false;
		}
		/* Restore the pcp queue routing */
		priv->plat->rx_queues_cfg[queue].prio = qos_table_info->backup_pcp_map[queue];
		ioss_qos_dev_log(NULL, "[iemac qos]: Restore pcp routing pcp = %d, queue = %d\n",
			       	 getbitpos(qos_table_info->backup_pcp_map[queue]), queue);
		priv->queue_pcp_map[queue] = qos_table_info->backup_pcp_map[queue];
		stmmac_rx_queue_prio(priv, priv->hw, qos_table_info->backup_pcp_map[queue], queue);
		stmmac_map_mtl_to_dma(priv, priv->hw, queue, queue);
	}
}
EXPORT_SYMBOL_GPL(stmmac_restore_qos_queue_cfg);

void stmmac_enable_qos_queue_cfg(struct stmmac_priv *priv, struct qos_struct *qos_table_info)
{
	u8 prio = 0;
	u32 queue = 0;
	u32 read_value = 0;
	int i = 0;

	ioss_qos_dev_log(NULL, "[iemac qos] : Enter");

	if (priv->plat->enable_pfc)
		stmmac_mac_config_pfc(priv);

	/*pcp routing*/
	for (queue = 0; queue < priv->plat->rx_qos_queues_to_use; queue++) {
		if (queue == 0) {
			if (priv->unique_filter_new != PCP) {
				/*enable dynamic mapping for queue0*/
				read_value = (u32)readl_relaxed(priv->ioaddr + XGMAC_MTL_RXQ_DMA_MAP0);
				read_value |= XGMAC_QDDMACH;
				writel(read_value, priv->ioaddr + XGMAC_MTL_RXQ_DMA_MAP0);
			}
			continue;
		}

		if (qos_table_info->queue_to_pcp_map[queue] != priv->queue_pcp_map[queue]) {
			stmmac_rx_queue_prio(priv, priv->hw, qos_table_info->queue_to_pcp_map[queue], queue);
			for (i = 0; i <= PCP_MAX_VALUE; i++) {
				if (qos_table_info->queue_to_pcp_map[queue] & BIT(i)) {
					stmmac_pfc_tx_flow_ctrl(priv, i);
				}
			}
			/* Copy new pcp_map to priv */
			priv->queue_pcp_map[queue] = qos_table_info->queue_to_pcp_map[queue];
			ioss_qos_dev_log(NULL, "[iemac qos]: Install pcp routing pcp = 0x%01x, queue = %d\n",
						qos_table_info->queue_to_pcp_map[queue], queue);
		}

			stmmac_map_mtl_to_dma(priv, priv->hw, queue, qos_table_info->queue_to_ch_map[queue]);
	}
}
EXPORT_SYMBOL_GPL(stmmac_enable_qos_queue_cfg);

void stmmac_enable_qos_filtering(struct net_device *ndev, struct qos_struct *qos_table_info)
{
	int i = 0, j = 0;
	int ret = 0;
	u32 read_value = 0;
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct dma_filter_table *dma_filter_node;
	struct list_head *filter_ptr;

	ioss_qos_dev_log(NULL, "[iemac qos] : Enter");
	/* Clear the filters which aren't needed */
	stmmac_remove_qos_filtering(ndev, priv->unique_filter_old, IDX_CLEAR);

	/* Apply the new filters to be installed */
	if (priv->unique_filter_new != priv->unique_filter_old)
		filter_ptr = &qos_table_info->dma_filter_table;
	else
		filter_ptr = &qos_table_info->flt_to_app;

	/*config to receive unmatched packets too*/
	read_value = (u32)readl(priv->ioaddr + XGMAC_PACKET_FILTER);
	read_value |= XGMAC_FILTER_RA;
	writel(read_value, priv->ioaddr + XGMAC_PACKET_FILTER);

	list_for_each_entry(dma_filter_node, filter_ptr, node) {
		switch (priv->unique_filter_new) {
		case VLAN_ID:
			ret = stmmac_add_hw_vlan_rx_routing_fltr(priv, priv->dev, priv->hw, dma_filter_node->vlan_id,
								 dma_filter_node->dma_ch, false);
			if (ret) {
				ioss_qos_dev_err(NULL, "[iemac qos]: couldn't apply vlan filter %d\n",
					       	 dma_filter_node->vlan_id);
				dma_filter_node->applied = false;
			} else {
				dma_filter_node->applied = true;
				ioss_qos_dev_log(NULL, "[iemac qos]: vlan filter %d applied, ch = %d\n",
					         dma_filter_node->vlan_id, dma_filter_node->dma_ch);
				for (i = 0; i < priv->dma_cap.nrvf_num; i++) {
					if(priv->app_filters[i].action == IDX_UNUSED) {
						priv->app_filters[i].vlan_id = dma_filter_node->vlan_id;
						priv->app_filters[i].dma_ch = dma_filter_node->dma_ch;
						priv->app_filters[i].action = IDX_USED;
						break;
					}
				}
			}
			break;
		case SRC_IP:
			for (i = priv->app_l3_l4_filters; i < priv->dma_cap.l3l4fnum; i++) {
				if (priv->app_filters[i].action == IDX_UNUSED) {
					break;
				}
			}
			if (i < priv->dma_cap.l3l4fnum) {
				ioss_qos_dev_log(NULL, "[iemac qos]: SRC_IP filter num = %d, is_ipv6 = %d, mask_len = %d, dma_ch = %d, idx = %d\n",
							priv->qos_l3_l4_filters,
						 dma_filter_node->ip_src.ipv6_src, dma_filter_node->ip_src.src_mask_length,
						 dma_filter_node->dma_ch, i);
				ret = priv->hw->mac->config_l3_filter_with_mask(priv->hw, i, true,
									  dma_filter_node->ip_src.ipv6_src, true, false,
									  dma_filter_node->ip_src.ipv4_src_addr, dma_filter_node->ip_src.ipv6_src_addr,
									  dma_filter_node->ip_src.src_mask_length, dma_filter_node->dma_ch);
				if (ret) {
					ioss_qos_dev_log(NULL, "[iemac qos]: Apply src ip filter failed\n");
				} else {
					priv->qos_l3_l4_filters++;
					dma_filter_node->applied = true;
					ioss_qos_dev_log(NULL, "[iemac qos]: src ip filter applied\n");

					if (dma_filter_node->ip_src.ipv6_src) {
						for (j = 0; j < 16; j++)
							priv->app_filters[i].ip_src.ipv6_src_addr[j] = dma_filter_node->ip_src.ipv6_src_addr[j];
					} else {
						priv->app_filters[i].ip_src.ipv4_src_addr = dma_filter_node->ip_src.ipv4_src_addr;
					}
					priv->app_filters[i].ip_src.ipv6_src = dma_filter_node->ip_src.ipv6_src;
					priv->app_filters[i].ip_src.src_mask_length = dma_filter_node->ip_src.src_mask_length;
					priv->app_filters[i].dma_ch = dma_filter_node->dma_ch;
					priv->app_filters[i].action = IDX_USED;
				}
			} else {
				dma_filter_node->applied = false;
				ioss_qos_dev_err(NULL, "[iemac qos]: src ip filters exhausted\n");
			}
			break;
		case DEST_IP:
			for (i = priv->app_l3_l4_filters; i < priv->dma_cap.l3l4fnum; i++) {
				if (priv->app_filters[i].action == IDX_UNUSED) {
					break;
				}
			}
			if (i < priv->dma_cap.l3l4fnum) {
				ioss_qos_dev_log(NULL, "[iemac qos]: DEST_IP filter num = %d, is_ipv6 = %d, mask_len = %d, dma_ch = %d, idx = %d\n",
							priv->qos_l3_l4_filters,
						 dma_filter_node->ip_dest.ipv6_dst, dma_filter_node->ip_dest.dst_mask_length,
						 dma_filter_node->dma_ch, i);
				ret = priv->hw->mac->config_l3_filter_with_mask(priv->hw, i, true,
									  dma_filter_node->ip_dest.ipv6_dst, false, false,
									  dma_filter_node->ip_dest.ipv4_dst_addr, dma_filter_node->ip_dest.ipv6_dst_addr,
									  dma_filter_node->ip_dest.dst_mask_length, dma_filter_node->dma_ch);
				if (ret) {
					ioss_qos_dev_log(NULL, "[iemac qos]: Apply dest ip filter failed\n");
				} else {
					priv->qos_l3_l4_filters++;
					dma_filter_node->applied = true;
					ioss_qos_dev_log(NULL, "[iemac qos]: dest ip filter applied\n");

					if (dma_filter_node->ip_dest.ipv6_dst) {
						for (j = 0; j < 16; j++)
							priv->app_filters[i].ip_dest.ipv6_dst_addr[j] = dma_filter_node->ip_dest.ipv6_dst_addr[j];
					} else {
						priv->app_filters[i].ip_dest.ipv4_dst_addr = dma_filter_node->ip_dest.ipv4_dst_addr;
					}
					priv->app_filters[i].ip_dest.ipv6_dst = dma_filter_node->ip_dest.ipv6_dst;
					priv->app_filters[i].ip_dest.dst_mask_length = dma_filter_node->ip_dest.dst_mask_length;
					priv->app_filters[i].dma_ch = dma_filter_node->dma_ch;
					priv->app_filters[i].action = IDX_USED;
				}
			} else {
				dma_filter_node->applied = false;
				ioss_qos_dev_err(NULL, "[iemac qos]: dest ip filters exhausted\n");
			}
			break;
		case SRC_PORT:
			for (i = priv->app_l3_l4_filters; i < priv->dma_cap.l3l4fnum; i++) {
				if (priv->app_filters[i].action == IDX_UNUSED) {
					break;
				}
			}
			if (i < priv->dma_cap.l3l4fnum) {
				ret = priv->hw->mac->config_l4_filter_with_route(priv->hw, i, true, dma_filter_node->src_port.proto,
									   true, false, dma_filter_node->src_port.port_num,
									   dma_filter_node->dma_ch);
				if (ret) {
					ioss_qos_dev_log(NULL, "[iemac qos]: Apply src port filter failed\n");
				} else {
					priv->qos_l3_l4_filters++;
					dma_filter_node->applied = true;
					ioss_qos_dev_log(NULL, "[iemac qos]: applied src port %d filter, proto = %d, ch = %d, idx = %d\n",
							 dma_filter_node->src_port.port_num, dma_filter_node->src_port.proto,
							 dma_filter_node->dma_ch, i);

					priv->app_filters[i].src_port.port_num = dma_filter_node->src_port.port_num;
					priv->app_filters[i].src_port.proto = dma_filter_node->src_port.proto;
					priv->app_filters[i].dma_ch = dma_filter_node->dma_ch;
					priv->app_filters[i].action = IDX_USED;
				}
			} else {
				dma_filter_node->applied = false;
				ioss_qos_dev_err(NULL, "[iemac qos]: filters exhausted, couldn't apply filter for src port = %d, proto = %d, ch = %d\n",
						dma_filter_node->src_port.port_num, dma_filter_node->src_port.proto, dma_filter_node->dma_ch);
			}
			break;
		case DEST_PORT:
			for (i = priv->app_l3_l4_filters; i < priv->dma_cap.l3l4fnum; i++) {
				if (priv->app_filters[i].action == IDX_UNUSED) {
					break;
				}
			}
			if (i < priv->dma_cap.l3l4fnum) {
				ret = priv->hw->mac->config_l4_filter_with_route(priv->hw, i, true, dma_filter_node->dst_port.proto,
									   false, false, dma_filter_node->dst_port.port_num,
									   dma_filter_node->dma_ch);
				if (ret) {
					ioss_qos_dev_log(NULL, "[iemac qos]: Apply dest port filter failed at index i = %d\n", i);
				} else {
					priv->qos_l3_l4_filters++;
					dma_filter_node->applied = true;
					ioss_qos_dev_log(NULL, "[iemac qos]: applied dest port %d filter, proto = %d, ch = %d idx = %d\n",
							 dma_filter_node->dst_port.port_num, dma_filter_node->dst_port.proto,
							 dma_filter_node->dma_ch, i);

					priv->app_filters[i].dst_port.port_num = dma_filter_node->dst_port.port_num;
					priv->app_filters[i].dst_port.proto = dma_filter_node->dst_port.proto;
					priv->app_filters[i].dma_ch = dma_filter_node->dma_ch;
					priv->app_filters[i].action = IDX_USED;
				}
			} else {
				ioss_qos_dev_err(NULL, "[iemac qos]: filters exhausted, couldn't apply filter for dest port %d\n",
					       	 dma_filter_node->dst_port.port_num);
			}
			break;
		case INVALID_FILTER:
		default:
			break;
		}
	}

}
EXPORT_SYMBOL_GPL(stmmac_enable_qos_filtering);

void stmmac_remove_qos_filtering(struct net_device *ndev, int filter_type, enum idx_action action)
{
	int i = 0;
	int ret = 0;
	struct stmmac_priv *priv = netdev_priv(ndev);

	/* Clear the filters which aren't needed */
	for (i = 0; i < 32; i++) {
		if (priv->app_filters[i].action != action)
			continue;
		switch (filter_type) {
		case VLAN_ID:
			if (i > priv->dma_cap.nrvf_num)
				break;
			ret = stmmac_del_hw_vlan_rx_routing_fltr(priv, priv->dev, priv->hw,
								 priv->app_filters[i].vlan_id, false);
			if (ret) {
				ioss_qos_dev_err(NULL, "[iemac qos]: Deleting vlan %d filter failed\n",
					        	priv->app_filters[i].vlan_id);
			} else {
				priv->app_filters[i].action = IDX_UNUSED;
				ioss_qos_dev_log(NULL, "[iemac qos]: vlan filter %d deleted, ch = %d\n",
					         priv->app_filters[i].vlan_id, priv->app_filters[i].dma_ch);
			}
			break;
		case SRC_IP:
			if (i > priv->dma_cap.l3l4fnum)
				break;
			priv->qos_l3_l4_filters--;
			ret = priv->hw->mac->config_l3_filter_with_mask(priv->hw, i, false,
									priv->app_filters[i].ip_src.ipv6_src, true, false,
									priv->app_filters[i].ip_src.ipv4_src_addr, priv->app_filters[i].ip_src.ipv6_src_addr,
									priv->app_filters[i].ip_src.src_mask_length, priv->app_filters[i].dma_ch);
			if(ret) {
				ioss_qos_dev_err(NULL, "[iemac qos]: Deleting src ip filter failed\n");
			} else {
				priv->app_filters[i].action = IDX_UNUSED;
				ioss_qos_dev_log(NULL, "[iemac qos]: src ip filter deleted\n");
			}
			break;
		case DEST_IP:
			if (i > priv->dma_cap.l3l4fnum)
				break;
			priv->qos_l3_l4_filters--;
			ret = priv->hw->mac->config_l3_filter_with_mask(priv->hw, i, false,
									priv->app_filters[i].ip_dest.ipv6_dst, false, false,
									priv->app_filters[i].ip_dest.ipv4_dst_addr, priv->app_filters[i].ip_dest.ipv6_dst_addr,
									priv->app_filters[i].ip_dest.dst_mask_length, priv->app_filters[i].dma_ch);
			if(ret) {
				ioss_qos_dev_err(NULL, "[iemac qos]: Deleting dest ip filter failed\n");
			} else {
				priv->app_filters[i].action = IDX_UNUSED;
				ioss_qos_dev_log(NULL, "[iemac qos]: dest ip filter deleted\n");
			}
			break;
		case SRC_PORT:
			if (i > priv->dma_cap.l3l4fnum)
				break;
			priv->qos_l3_l4_filters--;
			ret = priv->hw->mac->config_l4_filter_with_route(priv->hw, i, false, priv->app_filters[i].src_port.proto,
									 true, false, priv->app_filters[i].src_port.port_num,
									 priv->app_filters[i].dma_ch);

			if(ret) {
				ioss_qos_dev_err(NULL, "[iemac qos]: Deleting src port filter failed\n");
			} else {
				priv->app_filters[i].action = IDX_UNUSED;
				ioss_qos_dev_log(NULL, "[iemac qos]: src port filter deleted\n");
			}
			break;
		case DEST_PORT:
			if (i > priv->dma_cap.l3l4fnum)
				break;
			priv->qos_l3_l4_filters--;
			ret = priv->hw->mac->config_l4_filter_with_route(priv->hw, i, false, priv->app_filters[i].dst_port.proto,
									 false, false, priv->app_filters[i].dst_port.port_num,
									 priv->app_filters[i].dma_ch);
			if(ret) {
				ioss_qos_dev_err(NULL, "[iemac qos]: Deleting dest port filter failed at index %d, ret = %d\n",i, ret);
			} else {
				priv->app_filters[i].action = IDX_UNUSED;
				ioss_qos_dev_log(NULL, "[iemac qos]: dest port filter deleted\n");
			}
			break;
		case INVALID_FILTER:
		default:
			break;
		}
	}

}
EXPORT_SYMBOL_GPL(stmmac_remove_qos_filtering);

void stmmac_restore_dma_config(struct net_device *ndev, struct qos_struct *qos_table_info)
{
	int i = 0;
	struct stmmac_priv *priv = netdev_priv(ndev);

	for (i = 1; i < priv->plat->rx_queues_to_use; i++) {
		if (!priv->is_rx_sw[i]) {
			ioss_qos_dev_log(NULL, "[iemac qos]: Move CH %d to SW\n", i);
			stmmac_config_rx_queue(ndev, i, false);
		}
          	stmmac_map_mtl_to_dma(priv, priv->hw, i, i);
		priv->is_rx_sw[i] = true;
	}

	for (i = 2; i < priv->plat->tx_queues_to_use; i++) {
		if (!priv->is_tx_sw[i])
			stmmac_config_tx_queue(ndev, i, false);

		/*Change mode to use for TX queues*/
		priv->plat->tx_queues_cfg[i].mode_to_use = MTL_QUEUE_AVB;

		stmmac_configure_tx_queue(priv, i, priv->plat->tx_queues_cfg[i].mode_to_use);
		priv->is_tx_sw[i] = true;
		priv->tx_ch_bw[i] = 0;
	}
}
EXPORT_SYMBOL_GPL(stmmac_restore_dma_config);

void stmmac_backup_pcp(struct stmmac_priv *priv, struct qos_struct *qos_table_info)
{
	u32 reg_val = 0;

	reg_val = (u32)readl(priv->ioaddr + XGMAC_RXQ_CTRL2);
	qos_table_info->backup_pcp_map[0] = (u8)(reg_val & 0xFF);
	qos_table_info->backup_pcp_map[1] = (u8)((reg_val & 0xFF00) >> 8);
	qos_table_info->backup_pcp_map[2] = (u8)((reg_val & 0xFF0000) >> 16);
	qos_table_info->backup_pcp_map[3] = (u8)((reg_val & 0xFF000000) >> 24);
	reg_val = (u32)readl(priv->ioaddr + XGMAC_RXQ_CTRL3);
	qos_table_info->backup_pcp_map[4] = (u8)(reg_val & 0xFF);
}
EXPORT_SYMBOL_GPL(stmmac_backup_pcp);

void stmmac_configure_tx_queue(struct stmmac_priv *priv, u8 queue, u8 txmode)
{
	int txfifosz = priv->plat->tx_fifo_size/priv->plat->tx_queues_to_use;

	stmmac_dma_tx_mode(priv, priv->ioaddr, SF_DMA_MODE, queue, txfifosz, txmode);
}
EXPORT_SYMBOL_GPL(stmmac_configure_tx_queue);

bool stmmac_is_skprio_routing (struct net_device *ndev)
{
	struct stmmac_priv *priv = netdev_priv(ndev);
	return priv->plat->is_skprio_routing(priv);
}
EXPORT_SYMBOL_GPL(stmmac_is_skprio_routing);

int config_rx_queue_path(struct net_device *ndev, u32 queue, bool skip_sw)
{
	int ret = 0;
	ret = stmmac_config_rx_queue(ndev, queue, skip_sw);

	return ret;
}
EXPORT_SYMBOL_GPL(config_rx_queue_path);

int config_tx_queue_path(struct net_device *ndev, u32 queue, bool skip_sw)
{
  	int ret = 0;

	stmmac_config_tx_queue(ndev, queue, skip_sw);

	return ret;
}
EXPORT_SYMBOL_GPL(config_tx_queue_path);

