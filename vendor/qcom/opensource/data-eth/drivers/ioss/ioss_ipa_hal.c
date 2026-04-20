/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/pci.h>

#include "ioss_i.h"

struct ioss_ipa_map {
	bool (*match_dev)(struct device *real_dev);
	int (*fill_si)(enum ipa_eth_client_type ctype,
				struct ioss_channel *ch);
};

/* Realtek R8125 HAL implementation */

static int __fill_r8125_si(struct ioss_channel *ch,
		struct ipa_eth_realtek_setup_info *rtk)
{
	static const int RTL8125_BAR_MMIO = 2;
	static const int RTL8125_TAIL_PTR_BASE = 0x2800;
	static const int RTL8125_TAIL_PTR_NEXT = 4;

	struct ioss_device *idev = ioss_ch_dev(ch);
	struct pci_dev *pdev = to_pci_dev(ioss_idev_to_real(idev));

	rtk->bar_addr = pci_resource_start(pdev, RTL8125_BAR_MMIO);
	rtk->bar_size = pci_resource_len(pdev, RTL8125_BAR_MMIO);
	rtk->queue_number = ch->id;

	if (ch->direction == IOSS_CH_DIR_TX)
		rtk->dest_tail_ptr_offs = RTL8125_TAIL_PTR_BASE +
					(ch->id * RTL8125_TAIL_PTR_NEXT);

	return 0;
}

static int fill_r8125_si(enum ipa_eth_client_type ctype,
		struct ioss_channel *ch)
{
	struct ioss_ch_priv *cp = ch->ioss_priv;
	struct ipa_eth_pipe_setup_info *si = &cp->ipa_pi.info;

	return __fill_r8125_si(ch, &si->client_info.rtk);
}

static bool match_r8125(struct device *real_dev)
{
	return (real_dev->bus == &pci_bus_type) &&
		!strcmp(to_pci_driver(real_dev->driver)->name, "r8125");
}

/* Realtek R8168 HAL implementation */

static int __fill_r8168_si(struct ioss_channel *ch,
		struct ipa_eth_realtek_setup_info *rtk)
{
	static const int RTL8168_BAR_MMIO = 2;

	struct ioss_device *idev = ioss_ch_dev(ch);
	struct pci_dev *pdev = to_pci_dev(ioss_idev_to_real(idev));

	rtk->bar_addr = pci_resource_start(pdev, RTL8168_BAR_MMIO);
	rtk->bar_size = pci_resource_len(pdev, RTL8168_BAR_MMIO);
	rtk->queue_number = ch->id;
	rtk->num_queues_enabled = 2;

	return 0;
}
static int fill_r8168_si(enum ipa_eth_client_type ctype,
		struct ioss_channel *ch)
{
	struct ioss_ch_priv *cp = ch->ioss_priv;
	struct ipa_eth_pipe_setup_info *si = &cp->ipa_pi.info;

	return __fill_r8168_si(ch, &si->client_info.rtk);
}

static bool match_r8168(struct device *real_dev)
{
	return (real_dev->bus == &pci_bus_type) &&
		!strcmp(to_pci_driver(real_dev->driver)->name, "r8168");
}

/* AQC HAL implementation */
#define AQC_RX_TAIL_PTR_OFFSET 0x00005B10
#define AQC_RX_TAIL_PTR(base, idx) \
	((base) + AQC_RX_TAIL_PTR_OFFSET + ((idx) * 0x20))

#define AQC_RX_HEAD_PTR_OFFSET 0x00005B0C
#define AQC_RX_HEAD_PTR(base, idx) \
	((base) + AQC_RX_HEAD_PTR_OFFSET + ((idx) * 0x20))

#define AQC_TX_TAIL_PTR_OFFSET 0x00007C10
#define AQC_TX_TAIL_PTR(base, idx) \
	((base) + AQC_TX_TAIL_PTR_OFFSET + ((idx) * 0x40))

#define AQC_TX_HEAD_PTR_OFFSET 0x00007C0C
#define AQC_TX_HEAD_PTR(base, idx) \
	((base) + AQC_TX_HEAD_PTR_OFFSET + ((idx) * 0x40))

static int fill_aqc_si(enum ipa_eth_client_type ctype, struct ioss_channel *ch)
{

	struct ioss_ch_priv *cp = ch->ioss_priv;
	struct ipa_eth_pipe_setup_info *si = &cp->ipa_pi.info;
	struct ipa_eth_aqc_setup_info *aqc = &si->client_info.aqc;
	static const int AQC_BAR_MMIO = 0;

	struct ioss_device *idev = ioss_ch_dev(ch);
	struct pci_dev *pdev = to_pci_dev(ioss_idev_to_real(idev));

	aqc->bar_addr = pci_resource_start(pdev, AQC_BAR_MMIO);
	aqc->aqc_ch = ch->id;

	ioss_dev_log(idev, "AQC: bar=%pap, q=%u\n",
		&aqc->bar_addr, aqc->aqc_ch);

	if (ch->direction == IOSS_CH_DIR_TX) {
		aqc->head_ptr_offs = AQC_TX_HEAD_PTR(0, ch->id);
		aqc->dest_tail_ptr_offs = AQC_TX_TAIL_PTR(0, ch->id);
	} else {
		aqc->head_ptr_offs = AQC_RX_HEAD_PTR(0, ch->id);
		aqc->dest_tail_ptr_offs = AQC_RX_TAIL_PTR(0, ch->id);
	}

	return 0;
}

static bool match_aqc(struct device *real_dev)
{
	return (real_dev->bus == &pci_bus_type) &&
		!strcmp(to_pci_driver(real_dev->driver)->name, "atlantic-fwd");
}

static int fill_ntn3_si(enum ipa_eth_client_type ctype, struct ioss_channel *ch)
{

	struct ioss_ch_priv *cp = ch->ioss_priv;
	struct ipa_eth_pipe_setup_info *si = &cp->ipa_pi.info;
	struct ipa_eth_ntn_setup_info *ntn = &si->client_info.ntn;
	static const int NTN_BAR_MMIO = 4;

	struct ioss_device *idev = ioss_ch_dev(ch);
	struct pci_dev *pdev = to_pci_dev(ioss_idev_to_real(idev));

	ntn->bar_addr = pci_resource_start(pdev, NTN_BAR_MMIO);
	ntn->ioc_mod_threshold = ch->event.mod_count_min;
	ioss_dev_log(idev, "NTN: bar=%pap, q=%u, ioc_threshold=%d\n",
		&ntn->bar_addr, ch->id, ntn->ioc_mod_threshold);

	ntn->tail_ptr_offs = ch->tail_ptr_addr;

	return 0;
}

static bool match_ntn3(struct device *real_dev)
{
	return (real_dev->bus == &pci_bus_type) &&
		!strcmp(to_pci_driver(real_dev->driver)->name, "tc956x_pci-eth");
}

static int fill_stmmac_si(enum ipa_eth_client_type ctype, struct ioss_channel *ch)
{
	struct ioss_ch_priv *cp = ch->ioss_priv;
	struct ipa_eth_pipe_setup_info *si = &cp->ipa_pi.info;
	struct ipa_eth_ntn_setup_info *stm_ipa = &si->client_info.ntn;
	struct resource *resource = NULL;
	int ret = 0;

	struct ioss_device *idev = ioss_ch_dev(ch);
	struct platform_device *pdev = to_platform_device(ioss_idev_to_real(idev));

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource) {
		ioss_dev_err(idev, "Failed to get emac-base resource");
		ret = -ENODEV;
		return ret;
	}

	stm_ipa->bar_addr = resource->start;
	stm_ipa->tail_ptr_offs = ch->tail_ptr_addr;
	stm_ipa->ioc_mod_threshold = ch->event.mod_count_min;

	ioss_dev_log(idev, "stm_ipa: bar=%pap, q=%u, ioc_threshold=%d\n",
		     &stm_ipa->bar_addr, ch->id, stm_ipa->ioc_mod_threshold);

	return 0;
}

static bool match_stmmac(struct device *real_dev)
{
	return (real_dev->bus == &platform_bus_type) &&
		!strcmp(to_platform_driver(real_dev->driver)->driver.name, "qcom-ethqos");
}

struct ioss_ipa_map ioss_ipa_map_table[IPA_ETH_CLIENT_MAX] = {
	[IPA_ETH_CLIENT_RTK8125B] = { match_r8125, fill_r8125_si },
	[IPA_ETH_CLIENT_RTK8111K] = { match_r8168, fill_r8168_si },
	[IPA_ETH_CLIENT_AQC107] = { match_aqc, fill_aqc_si },
	[IPA_ETH_CLIENT_NTN3] = { match_ntn3, fill_ntn3_si },
	[IPA_ETH_CLIENT_IEMAC] = { match_stmmac, fill_stmmac_si },
};

enum ipa_eth_client_type ioss_ipa_hal_get_ctype(struct ioss_device *idev)
{
	enum ipa_eth_client_type ctype;
	struct device *real_dev = ioss_idev_to_real(idev);

	for (ctype = 0; ctype < IPA_ETH_CLIENT_MAX; ctype++) {
		if (!ioss_ipa_map_table[ctype].match_dev)
			continue;

		if (ioss_ipa_map_table[ctype].match_dev(real_dev))
			break;
	}

	return ctype;
}

int ioss_ipa_hal_fill_si(struct ioss_channel *ch)
{
	enum ipa_eth_client_type ctype =
			ioss_ipa_hal_get_ctype(ioss_ch_dev(ch));

	if (ctype < IPA_ETH_CLIENT_MAX && ioss_ipa_map_table[ctype].fill_si)
		return ioss_ipa_map_table[ctype].fill_si(ctype, ch);

	return -EINVAL;
}
