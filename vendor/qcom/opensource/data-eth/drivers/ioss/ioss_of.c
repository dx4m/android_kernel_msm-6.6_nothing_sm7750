/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/of.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "ioss_i.h"

typedef int (*of_parser_t)(struct ioss_device *idev, struct device_node *np);

#define KEY_TRAFFIC_TYPE "qcom,ioss-traffic-type"
#define KEY_IPA_CONFIGS "qcom,compatible-ipa-configs"

static int ioss_of_parse_traffic_type(struct ioss_device *idev,
		struct device_node *np, struct ioss_channel *ch)
{
	const char *str = NULL;
	enum ioss_traffic_type t;

	ch->traffic_type = DEFAULT_IOSS_TRAFFIC_TYPE;

	if (of_property_read_string(np, KEY_TRAFFIC_TYPE, &str))
		return 0; /* traffic type is optional */

	for (t = 0; t < IOSS_TRAFFIC_TYPE_MAX; t++) {
		if (!strcmp(str, ioss_traffic_name(t))) {
			ch->traffic_type = t;
			return 0;
		}
	}

	ioss_dev_bug(idev, "Need to update traffic type map for '%s", str);

	return 0;
}

static int ioss_of_parse_channel(struct ioss_device *idev,
		struct device_node *np)
{
	int ret;
	const char *key;
	struct ioss_channel *ch = NULL;
	struct ioss_interface *iface = &idev->interface;

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return -ENOMEM;

	ch->name = of_node_full_name(np);

	ch->ioss_priv = kzalloc(sizeof(struct ioss_ch_priv), GFP_KERNEL);
	if (!ch->ioss_priv) {
		kvfree_sensitive(ch,ksize(ch));
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&ch->node);
	INIT_LIST_HEAD(&ch->desc_mem);
	INIT_LIST_HEAD(&ch->buff_mem);

	ch->iface = iface;

	if (!!of_find_property(np, "qcom,dir-rx", NULL))
		ch->direction = IOSS_CH_DIR_RX;
	else if (!!of_find_property(np, "qcom,dir-tx", NULL))
		ch->direction = IOSS_CH_DIR_TX;
	else
		goto err;

	key = "qcom,ring-size";
	if (of_property_read_u32(np, key, &ch->default_config.ring_size)) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	key = "qcom,buff-size";
	if (of_property_read_u32(np, key, &ch->default_config.buff_size)) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	key = "qcom,mod-count-min";
	if (of_property_read_u32(np, key, &ch->event.mod_count_min)) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	key = "qcom,mod-count-max";
	if (of_property_read_u32(np, key, &ch->event.mod_count_max)) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	key = "qcom,mod-usecs-min";
	if (of_property_read_u32(np, key, &ch->event.mod_usecs_min)) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	key = "qcom,mod-usecs-max";
	if (of_property_read_u32(np, key, &ch->event.mod_usecs_max)) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	if (of_find_property(np, "qcom,multi_rx_queues", NULL))
		ch->multi_rx_queues = 1;
	else
		ch->multi_rx_queues = 0;

	if (!!of_find_property(np, "qcom,rx-filter-be", NULL))
		ch->filter_types |= IOSS_RXF_F_BE;

	if (!!of_find_property(np, "qcom,rx-filter-ip", NULL))
		ch->filter_types |= IOSS_RXF_F_IP;

	if (ioss_of_parse_traffic_type(idev, np, ch))
		goto err;

	ret = of_property_count_strings(np, KEY_IPA_CONFIGS);
	if (ret > 0) {
		ch->ipa_configs = kmalloc(ret * sizeof(*ch->ipa_configs), GFP_KERNEL);
		if (!ch->ipa_configs)
			goto err;

		ch->num_ipa_configs = of_property_read_string_array(np,
				KEY_IPA_CONFIGS, ch->ipa_configs, ret);
	}

	ch->default_config.desc_alctr = &ioss_default_alctr;
	ch->default_config.buff_alctr = &ioss_default_alctr;

	list_add_tail(&ch->node, &iface->invalid_channels);

	return 0;

err:
	kvfree_sensitive(ch->ioss_priv,ksize(ch->ioss_priv));
	kvfree_sensitive(ch,ksize(ch));

	return -EINVAL;
}

static int ioss_of_parse_v2(struct ioss_device *idev, struct device_node *np)
{
	int i, count;
	const char *key;
	struct ioss_interface *iface = &idev->interface;

	iface->ioss_priv = kzalloc(sizeof(struct ioss_iface_priv), GFP_KERNEL);
	if (!iface->ioss_priv)
		return -ENOMEM;

	key = "qcom,ioss_instance";
	if (of_property_read_u32(np, key, &iface->instance_id)) {
		ioss_dev_log(idev, "Failed to parse key %s", key);
		goto err;
	}

	/*To disable auto resume of ioss during PM*/
	key = "qcom,ioss_auto_resume_disabled";
	iface->auto_resume_disabled = of_property_read_bool(np, key);

	/* Parse channels */
	key = "qcom,ioss_channels";
	count = of_count_phandle_with_args(np, "qcom,ioss_channels", NULL);
	if (count < 0) {
		ioss_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}

	/* Get channels */
	for (i = 0; i < count; i++) {
		struct device_node *n = of_parse_phandle(np,
						"qcom,ioss_channels", i);

		if (ioss_of_parse_channel(idev, n)) {
			ioss_dev_err(idev, "Failed to parse channel[%d]", i);
			goto err;
		}
	}

	if(of_property_read_bool(np, "pci_device"))
		iface->is_pci_device = true;

	if (!!of_find_property(np, "qcom,ioss-wol-phy", NULL))
		idev->wol.wolopts |= WAKE_PHY;

	if (!!of_find_property(np, "qcom,ioss-wol-magic", NULL))
		idev->wol.wolopts |= WAKE_MAGIC;

	return 0;

err:
	{
		struct ioss_channel *ch, *tmp_ch;

		list_for_each_entry_safe(ch, tmp_ch, &iface->invalid_channels, node) {
			list_del(&ch->node);
			kfree(ch->ipa_configs);
			kvfree_sensitive(ch->ioss_priv,ksize(ch->ioss_priv));
			kvfree_sensitive(ch,ksize(ch));
		}
	}

	kvfree_sensitive(iface->ioss_priv,ksize(iface->ioss_priv));

	return -EINVAL;
}

static const struct of_device_id ioss_dev_match_table[] = {
	{ .compatible = "qcom,ioss-v2-device", .data = ioss_of_parse_v2, },
	{},
};

static int __ioss_of_parse(struct ioss_device *idev, struct device_node *np)
{
	const struct of_device_id *match =
		of_match_node(ioss_dev_match_table, np);

	if (match) {
		of_parser_t parser = match->data;

		return parser(idev, np);
	}

	return -EINVAL;
}

int ioss_of_parse(struct ioss_device *idev)
{
	const char *key;
	struct device_node *of_root;
	struct platform_device *pdev;
	struct device_node *np = dev_of_node(ioss_idev_to_real(idev));

	if (!np)
		return -EINVAL;

	/* Get ioss root */
	key = "qcom,ioss";
	of_root = of_parse_phandle(np, key, 0);
	if (!of_root) {
		ioss_dev_dbg(idev, "DT prop %s not found, skipping.", key);
		return -ENOENT;
	}

	pdev = ioss_find_dev_from_of_node(of_root);
	if (!pdev) {
		ioss_dev_err(idev, "Failed to find ioss root node");
		return -EFAULT;
	}

	if (idev->root->pdev != pdev) {
		ioss_dev_dbg(idev, "Device belongs to a different root");
		return -ENOENT;
	}

	return __ioss_of_parse(idev, np);
}
