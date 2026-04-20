/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/inet.h>
#include <linux/of.h>

#include "ioss.h"
#include "ioss_qos.h"

#include "ioss_i.h"

#define __create_sysfs(idev, qos_kobj, qos_node, uid, gid, qos_sysfs_err) \
	do { \
		if (sysfs_create_file(qos_kobj, &dev_attr_##qos_node.attr)) { \
			ioss_dev_err(idev, "unable to create " #qos_node " node"); \
			goto qos_sysfs_err; \
		} \
		if (sysfs_file_change_owner(qos_kobj, #qos_node, KUIDT_INIT(uid), KGIDT_INIT(gid))) { \
			ioss_dev_err(idev, "unable to change owner of " #qos_node " sysfs node"); \
			goto qos_sysfs_err; \
		} \
	} while(0)


static struct kobject *real_kobj_from_dev(struct device *dev, int depth)
{
	struct kobject* kobj;
	int i;

	kobj = dev->kobj.parent;

	for (i = 1; i < depth; i++)
		kobj = kobj->parent;

	return kobj;
}
static struct ioss_device *ioss_dev_from_kobj(struct kobject* kobj)
{
	struct device *parent = NULL;
	struct net_device *net_dev = NULL;
	struct ioss_interface *iface = NULL;
	struct ioss_device *idev = NULL;

	parent = kobj_to_dev(kobj);
	if (!parent) {
		ioss_qos_dev_err(idev, "parent is NULL");
		return NULL;
	}

	net_dev = to_net_dev(parent);
	if (!net_dev) {
		ioss_qos_dev_err(idev, "net_dev is NULL");
		return NULL;
	}

	iface = ioss_netdev_to_iface(net_dev);
	if (!iface) {
		ioss_qos_dev_err(idev, "iface is NULL");
		return NULL;
	}

	idev = ioss_iface_dev(iface);
	if(!idev)
		ioss_qos_dev_err(idev, "idev is NULL");

	return idev;
}

static u16 get_num_arguments(char** buff, const char* delim)
{
	u16 len = 0;
	char* token;

	while ((token = strsep(buff, delim)))
		if (strlen(token)) len++;

	return len;
}

static bool is_valid_pcp(u8 pcp)
{
	return (pcp >= PCP_LOWER_LIMIT && pcp <= PCP_UPPER_LIMIT);
}

static bool is_valid_vlan_id(u16 vlan_id)
{
	return (vlan_id >= VLAN_LOWER_LIMIT && vlan_id <= VLAN_UPPER_LIMIT);
}

static bool is_valid_bw(u16 bw)
{
	return (bw >= BW_LOWER_LIMIT && bw <= BW_UPPER_LIMIT);
}

static int extract_ip_mask(char *src, struct qos_filters *addr)
{
	int i = 0;
	int ret = 0;
	char *token;

	while ( (token = strsep(&src, "/")) ) {
		if (i > 1)
			return -1;
		if (i == 0) {
			ret = inet_pton_with_scope(&init_net, AF_UNSPEC, token, NULL, &(addr->address));
			if (ret) {
				ioss_qos_dev_err(NULL, "[ioss qos] Invalid IP address entered\n");
				return -EINVAL;
			}
		}
		else {
			if (kstrtou8(token, 10, &(addr->mask_length)) < 0)
				return -EINVAL;
		}
		i++;
	}

	if (addr->address.ss_family == AF_INET) {
		if (addr->mask_length > 32) {
			ioss_qos_dev_err(NULL, "[ioss qos] : Mask must be <= 32 for IPv4 address\n");
			return -EINVAL;
		}
		else if (addr->mask_length == 0) {
			addr->mask_length = 32;
		}
	}
	else if (addr->address.ss_family == AF_INET6) {
		if (addr->mask_length > 128) {
			ioss_qos_dev_err(NULL, "[ioss qos] : Mask must be <= 128 for IPv6 address\n");
			return -EINVAL;
		}
		else if (addr->mask_length == 0) {
			addr->mask_length = 128;
		}
	}
	else {
		ioss_qos_dev_err(NULL, "[ioss qos] : Neither IPv4 or IPv6 address\n");
		return -EINVAL;
	}

	return 0;
}

static int extract_proto_port(char *src, struct qos_filters *addr)
{
	char *token;

	/*
	* Currently, src = <Protocol>/<Port>]
	* Remove trailing ] and split into <Protocol> and <Port>
	*/
	src[strlen(src) - 1] = '\0';
	while ( (token = strsep(&src, "/")) ) {
		if (isalpha(token[0])) {
			addr->proto = kstrdup(token, GFP_KERNEL);
		}
		else {
			if (kstrtou32(token, 10, &(addr->port_num)) < 0)
				return -EINVAL;
			if (addr->port_num < 1 || addr->port_num > 65535)
				return -EINVAL;
		}
	}

	return 0;
}

static int extract_qos_filters(char *src, struct qos_filters *addr)
{
	char *token;

	/*
	* Separate <IP>/<Mask>[<Protocol>/<Port>]
	* into <IP>/<Mask>  and  <Protocol>/<Port>]
	*/
	while ( (token = strsep(&src, "[")) ) {
		if (0 == strlen(token)) // Fix for invalid ip
			continue;
		if (token[strlen(token) - 1] == ']') {
			if (extract_proto_port(token, addr))
				return -EINVAL;
		}
		else {
			if (extract_ip_mask(token, addr))
				return -EINVAL;
		}
	}

	return 0;
}

static bool rx_tc_already_exists(struct ioss_device *idev, u16 prio)
{
	struct list_head *ptr;
	struct qos_rx_tc *entry;

	for (ptr = idev->ioss_qos_table.qos_rx_pending_table.next; ptr != &idev->ioss_qos_table.qos_rx_pending_table; ptr = ptr->next) {
		entry = to_qos_rx_tc(ptr);
		if (entry->tc_prio == prio)
			return true;
	}

	return false;
}

static bool tx_tc_already_exists(struct ioss_device *idev, u8 prio)
{
	struct list_head *ptr;
	struct qos_routing_tx *entry;

	for (ptr = idev->ioss_qos_table.qos_tx_pending_table.next; ptr != &idev->ioss_qos_table.qos_tx_pending_table; ptr = ptr->next) {
		entry = to_qos_routing_tx(ptr);
		if (entry->tc_prio == prio)
			return true;
	}

	return false;
}

static void add_rx_handle(struct ioss_device *idev, struct qos_routing_rx_hdl *rx_node)
{

	list_add_tail(&rx_node->node, &idev->ioss_qos_new_nodes.rx_node->hdl_node);
}


static void add_rx_tc_by_priority(struct ioss_device *idev, struct qos_rx_tc *rx_node)
{
	struct list_head *ptr;
	struct qos_rx_tc *entry;

	for (ptr = idev->ioss_qos_table.qos_rx_pending_table.next; ptr != &idev->ioss_qos_table.qos_rx_pending_table; ptr = ptr->next) {
		entry = to_qos_rx_tc(ptr);
		if (entry->tc_prio > rx_node->tc_prio) {
			list_add_tail(&rx_node->node, ptr);
			return;
		}
	}

	list_add_tail(&rx_node->node, &idev->ioss_qos_table.qos_rx_pending_table);
}

static void add_tx_tc_by_priority(struct ioss_device *idev, struct qos_routing_tx *tx_node)
{
	struct list_head *ptr;
	struct qos_routing_tx *entry;

	for (ptr = idev->ioss_qos_table.qos_tx_pending_table.next; ptr != &idev->ioss_qos_table.qos_tx_pending_table; ptr = ptr->next) {
		entry = to_qos_routing_tx(ptr);
		if (entry->tc_prio > tx_node->tc_prio) {
			list_add_tail(&tx_node->node, ptr);
			return;
		}
	}

	list_add_tail(&tx_node->node, &idev->ioss_qos_table.qos_tx_pending_table);
}

static int copy_rx_hdl_node(struct qos_routing_rx_hdl *src, struct qos_routing_rx_hdl *dest)
{
	size_t i = 0;
	size_t j = 0;

	dest->tc_prio = src->tc_prio;
	dest->hdl = src->hdl;

	dest->pcp.len = src->pcp.len;
	dest->pcp.arr = kzalloc(sizeof(u8) * dest->pcp.len, GFP_KERNEL);
	if (!dest->pcp.arr)
		goto pcp_alloc_err;

	for (i = 0; i < src->pcp.len; i++) {
		dest->pcp.arr[i] = src->pcp.arr[i];
	}

	dest->vlan_ids.len = src->vlan_ids.len;
	dest->vlan_ids.arr = kzalloc(sizeof(u16) * dest->vlan_ids.len, GFP_KERNEL);
	if (!dest->vlan_ids.arr)
		goto vlan_alloc_err;

	for (i = 0; i < src->vlan_ids.len; i++)
		dest->vlan_ids.arr[i] = src->vlan_ids.arr[i];

	dest->src.len = src->src.len;
	dest->src.arr = kzalloc(sizeof(struct qos_filters) * dest->src.len, GFP_KERNEL);
	if (!dest->src.arr)
		goto src_alloc_err;

	for (i = 0; i < src->src.len; i++) {
		memcpy(&dest->src.arr[i].address, &src->src.arr[i].address, sizeof(dest->src.arr[i].address));
		dest->src.arr[i].mask_length = src->src.arr[i].mask_length;
		dest->src.arr[i].port_num = src->src.arr[i].port_num;
		if (src->src.arr[i].proto != NULL)
			dest->src.arr[i].proto = kstrdup(src->src.arr[i].proto, GFP_KERNEL);
	}

	dest->dst.len = src->dst.len;
	dest->dst.arr = kzalloc(sizeof(struct qos_filters) * dest->dst.len, GFP_KERNEL);
	if (!dest->dst.arr)
		goto dest_alloc_err;

	for (i = 0; i < src->dst.len; i++) {
		memcpy(&dest->dst.arr[i].address, &src->dst.arr[i].address, sizeof(dest->dst.arr[i].address));
		dest->dst.arr[i].mask_length = src->dst.arr[i].mask_length;
		dest->dst.arr[i].port_num = src->dst.arr[i].port_num;
		if (src->dst.arr[i].proto != NULL)
			dest->dst.arr[i].proto = kstrdup(src->dst.arr[i].proto, GFP_KERNEL);
	}

	dest->smac.len = src->smac.len;
	dest->smac.arr = kzalloc(sizeof(u8[ETH_ALEN]) * dest->smac.len, GFP_KERNEL);
	if (!dest->smac.arr)
		goto smac_alloc_err;

	for (i = 0; i < src->smac.len; i++) {
		for (j = 0; j < ETH_ALEN; j++)
			dest->smac.arr[i][j] = src->smac.arr[i][j];
	}

	dest->dmac.len = src->dmac.len;
	dest->dmac.arr = kzalloc(sizeof(u8[ETH_ALEN]) * dest->dmac.len, GFP_KERNEL);
	if (!dest->dmac.arr)
		goto dmac_alloc_err;

	for (i = 0; i < src->dmac.len; i++) {
		for (j = 0; j < ETH_ALEN; j++)
			dest->dmac.arr[i][j] = src->dmac.arr[i][j];
	}

	return 0;

dmac_alloc_err:
	kfree(dest->smac.arr);
smac_alloc_err:
	kfree(dest->dst.arr);
dest_alloc_err:
	kfree(dest->src.arr);
src_alloc_err:
	kfree(dest->vlan_ids.arr);
vlan_alloc_err:
	kfree(dest->pcp.arr);
pcp_alloc_err:
	ioss_qos_dev_err(NULL, "Memory allocation failed for copy rx node\n");
	return -ENOMEM;
}


static int copy_rx_node(struct qos_rx_tc *src, struct qos_rx_tc *dest)
{
	struct qos_routing_rx_hdl *new_node, *tmp;
	struct list_head *ptr;

	dest->tc_prio = src->tc_prio;
	dest->committed = src->committed;
	dest->action = src->action;
	INIT_LIST_HEAD(&dest->hdl_node);

	list_for_each(ptr, &src->hdl_node) {
		new_node = kzalloc(sizeof(struct qos_routing_rx_hdl), GFP_KERNEL);
		if (!new_node)
			goto alloc_err;
		if (copy_rx_hdl_node(to_qos_routing_rx_hdl(ptr), new_node) < 0)
			goto alloc_err;
		list_add_tail(&new_node->node, &dest->hdl_node);
	}

	return 0;

alloc_err:
	list_for_each_entry_safe(new_node, tmp, &dest->hdl_node, node) {
		list_del(&new_node->node);
		kfree(new_node);
	}
	ioss_qos_dev_err(NULL, "Memory allocation failed for copy rx node\n");
	return -ENOMEM;
}

static int copy_rx_table(struct list_head *src, struct list_head *dest)
{
	struct list_head *ptr;
	struct qos_rx_tc *new_node, *tmp;

	list_for_each(ptr, src) {
		new_node = kzalloc(sizeof(struct qos_rx_tc), GFP_KERNEL);
		if (!new_node)
			goto alloc_err;
		if (copy_rx_node(to_qos_rx_tc(ptr), new_node) < 0)
			goto alloc_err;
		list_add_tail(&new_node->node, dest);
	}

	return 0;

alloc_err:
	list_for_each_entry_safe(new_node, tmp, dest, node) {
		list_del(&new_node->node);
		kfree(new_node);
	}
	ioss_qos_dev_err(NULL, "Memory allocation failed for copy rx table\n");
	return -ENOMEM;
}

static int copy_tx_node(struct qos_routing_tx *src, struct qos_routing_tx *dest)
{
	int i = 0;

	dest->tc_prio = src->tc_prio;
	dest->committed = src->committed;
	dest->disabled = src->disabled;
	dest->action = src->action;
	dest->cbs_bw.low_bw = src->cbs_bw.low_bw;
	dest->cbs_bw.high_bw = src->cbs_bw.high_bw;
	dest->handle = src->handle;
	dest->pcp.len = src->pcp.len;
	if (src->pcp.len) {
		dest->pcp.arr = kzalloc(sizeof(u8) * dest->pcp.len, GFP_KERNEL);
		if (!dest->pcp.arr)
			goto alloc_err;

		for (i = 0; i < src->pcp.len; i++)
			dest->pcp.arr[i] = src->pcp.arr[i];
	}
	return 0;

alloc_err:
	ioss_qos_dev_err(NULL,"Memory allocation failed for pcp arr\n");
	return -ENOMEM;
}

static int copy_tx_table(struct list_head *src, struct list_head *dest)
{
	struct list_head *ptr;
	struct qos_routing_tx *new_node,*tmp;

	list_for_each(ptr, src) {
		new_node = kzalloc(sizeof(struct qos_routing_tx), GFP_KERNEL);
		if (!new_node)
			goto alloc_err;

		copy_tx_node(to_qos_routing_tx(ptr), new_node);
		list_add_tail(&new_node->node, dest);
	}
	return 0;

alloc_err:
	list_for_each_entry_safe(new_node, tmp, dest, node) {
		list_del(&new_node->node);
		kfree(new_node);
	}
	ioss_qos_dev_err(NULL, "Memory allocation failed for copy tx table\n");
	return -ENOMEM;
}

static void clean_rx_hdl_node(struct qos_routing_rx_hdl *ptr)
{
	int i = 0;

	kfree(ptr->pcp.arr);
	kfree(ptr->vlan_ids.arr);

	for (i = 0; i < ptr->src.len; i++)
		kfree(ptr->src.arr[i].proto);
	kfree(ptr->src.arr);

	for (i = 0; i < ptr->dst.len; i++)
		kfree(ptr->dst.arr[i].proto);
	kfree(ptr->dst.arr);

	kfree(ptr->smac.arr);
	kfree(ptr->dmac.arr);
}

static void clean_rx_node(struct qos_rx_tc *node)
{
	struct list_head *ptr;
	struct list_head *temp;
	struct qos_routing_rx_hdl *del_node;

	list_for_each_safe(ptr, temp, &node->hdl_node) {
		list_del(ptr);
		del_node = to_qos_routing_rx_hdl(ptr);
		clean_rx_hdl_node(del_node);
		kfree(del_node);
	}

}

static void clean_rx_tc_node(struct qos_routing_rx *ptr)
{
	int i = 0;

	kfree(ptr->pcp.arr);
	kfree(ptr->vlan_ids.arr);

	for (i = 0; i < ptr->src.len; i++)
		kfree(ptr->src.arr[i].proto);
	kfree(ptr->src.arr);

	for (i = 0; i < ptr->dst.len; i++)
		kfree(ptr->dst.arr[i].proto);
	kfree(ptr->dst.arr);

	kfree(ptr->smac.arr);
	kfree(ptr->dmac.arr);
}

static void delete_rx_table(struct list_head *table)
{
	struct list_head *ptr;
	struct list_head *temp;
	struct qos_rx_tc *del_node;

	list_for_each_safe(ptr, temp, table) {
		list_del(ptr);
		del_node = to_qos_rx_tc(ptr);
		clean_rx_node(del_node);
		kfree(del_node);
	}
}

static void delete_rx_tc_table(struct list_head *table)
{
	struct list_head *ptr;
	struct list_head *temp;
	struct qos_routing_rx *del_node;

	list_for_each_safe(ptr, temp, table) {
		list_del(ptr);
		del_node = to_qos_routing_rx(ptr);
		clean_rx_tc_node(del_node);
		kfree(del_node);
	}
}

static void delete_tx_table(struct list_head *table)
{
	struct list_head *ptr;
	struct list_head *temp;
	struct qos_routing_tx *del_node;

	list_for_each_safe(ptr, temp, table) {
		list_del(ptr);
		del_node = to_qos_routing_tx(ptr);
		kfree(del_node->pcp.arr);
		kfree(del_node);
	}
}

static int get_used_bw(struct list_head *table)
{
	struct qos_routing_tx *tx_node;
	int bw_alloc = 0;

	list_for_each_entry(tx_node, table, node){
		if(tx_node->disabled)
			continue;
		bw_alloc += tx_node->bw_allocated;
	}
	return bw_alloc;
}

static u16 get_node_count(struct list_head *table)
{
	u16 count = 0;
	struct list_head* ptr;

	list_for_each(ptr, table)
		count++;

	return count;
}

static bool has_qos_table_changed(struct ioss_device *idev)
{
	struct list_head *ptr;
	struct qos_rx_tc *rx_node;
	struct qos_routing_tx *tx_node;

	if (get_node_count(&idev->ioss_qos_table.qos_rx_pending_table) != get_node_count(&idev->ioss_qos_table.qos_rx_committed_table))
		return true;
	if (get_node_count(&idev->ioss_qos_table.qos_tx_pending_table) != get_node_count(&idev->ioss_qos_table.qos_tx_committed_table))
		return true;

	list_for_each(ptr, &idev->ioss_qos_table.qos_rx_pending_table) {
		rx_node = to_qos_rx_tc(ptr);
		if (rx_node->committed == false)
			return true;
	}

	list_for_each(ptr, &idev->ioss_qos_table.qos_tx_pending_table) {
		tx_node = to_qos_routing_tx(ptr);
		if (tx_node->committed == false)
			return true;
	}

	return false;
}

void disable_qos_ipa_channels(struct ioss_device *idev);
int enable_qos_ipa_channels(struct ioss_device *idev, struct response resp);

void ioss_qos_remove_channels(struct ioss_interface *iface)
{
	struct ioss_channel *ch, *tmp_ch;
	struct ioss_device *idev = ioss_iface_dev(iface);

	/* Free QOS channels */
	list_for_each_entry_safe(ch, tmp_ch, &iface->invalid_channels, node) {
		if (ch->tc_mapping != 0) {
			ioss_dev_log(idev, "Moved %d to invalid channels", ch->channel_num);
			list_del(&ch->node);
			kfree_sensitive(ch->ioss_priv);
			kfree_sensitive(ch);
		}
	}

	idev->qos_rx_channels = 0;
	idev->qos_tx_channels = 0;

}

static int convert_flows_to_tc(struct ioss_device *idev, struct list_head *qos_rx)
{
	struct qos_routing_rx *qos_rx_tc_tbl, *tmp;
	struct qos_rx_tc *temp_rx;
	struct qos_routing_rx_hdl *temp_rx_hdl, *temp_rx_hdl2;
	struct list_head *qos_rx_hdl;
	int i = 0, j = 0;
	int dmac_clen = 0, smac_clen = 0, pcp_clen = 0, vlan_clen = 0, src_clen = 0, dst_clen = 0;

	/* Iterate over each flow nodes to find the size required for TC table */
	list_for_each_entry(temp_rx, qos_rx, node) {
		qos_rx_tc_tbl = kzalloc(sizeof(struct qos_routing_rx), GFP_KERNEL);
		if (!qos_rx_tc_tbl) {
			goto rx_tc_tbl_err;
		}

		qos_rx_tc_tbl->tc_prio = temp_rx->tc_prio;
		qos_rx_tc_tbl->action = temp_rx->action;
		qos_rx_tc_tbl->committed = temp_rx->committed;

		qos_rx_hdl = &temp_rx->hdl_node;
		list_for_each_entry(temp_rx_hdl2, qos_rx_hdl, node) {
			qos_rx_tc_tbl->dmac.len += temp_rx_hdl2->dmac.len;
			qos_rx_tc_tbl->smac.len += temp_rx_hdl2->smac.len;
			qos_rx_tc_tbl->pcp.len += temp_rx_hdl2->pcp.len;
			qos_rx_tc_tbl->vlan_ids.len += temp_rx_hdl2->vlan_ids.len;
			qos_rx_tc_tbl->src.len += temp_rx_hdl2->src.len;
			qos_rx_tc_tbl->dst.len += temp_rx_hdl2->dst.len;
		}

		qos_rx_tc_tbl->dmac.arr = kzalloc(sizeof(u8[ETH_ALEN]) * qos_rx_tc_tbl->dmac.len, GFP_KERNEL);
		if (!qos_rx_tc_tbl->dmac.arr)
			goto dmac_alloc_err;

		qos_rx_tc_tbl->smac.arr = kzalloc(sizeof(u8[ETH_ALEN]) * qos_rx_tc_tbl->smac.len, GFP_KERNEL);
		if (!qos_rx_tc_tbl->smac.arr)
			goto smac_alloc_err;

		qos_rx_tc_tbl->pcp.arr = kzalloc(sizeof(u8) * qos_rx_tc_tbl->pcp.len, GFP_KERNEL);
		if (!qos_rx_tc_tbl->pcp.arr)
			goto pcp_alloc_err;

		qos_rx_tc_tbl->vlan_ids.arr = kzalloc(sizeof(u16) * qos_rx_tc_tbl->vlan_ids.len, GFP_KERNEL);
		if (!qos_rx_tc_tbl->vlan_ids.arr)
			goto vlan_alloc_err;

		qos_rx_tc_tbl->src.arr = kzalloc(sizeof(struct qos_filters) * qos_rx_tc_tbl->src.len, GFP_KERNEL);
		if (!qos_rx_tc_tbl->src.arr)
			goto src_alloc_err;

		qos_rx_tc_tbl->dst.arr = kzalloc(sizeof(struct qos_filters) * qos_rx_tc_tbl->dst.len, GFP_KERNEL);
		if (!qos_rx_tc_tbl->dst.arr)
			goto dest_alloc_err;

		dmac_clen = 0;
		smac_clen = 0;
		pcp_clen = 0;
		vlan_clen = 0;
		src_clen = 0;
		dst_clen = 0;
		list_for_each_entry(temp_rx_hdl, qos_rx_hdl, node) {
			for (i = 0; i < temp_rx_hdl->dmac.len; i++) {
				for (j = 0; j < ETH_ALEN; j++)
					qos_rx_tc_tbl->dmac.arr[dmac_clen][j] = temp_rx_hdl->dmac.arr[i][j];
				dmac_clen++;
			}
			for (i = 0; i < temp_rx_hdl->smac.len; i++) {
				for (j = 0; j < ETH_ALEN; j++)
					qos_rx_tc_tbl->smac.arr[smac_clen][j] = temp_rx_hdl->smac.arr[i][j];
				smac_clen++;
			}

			for (i = 0; i < temp_rx_hdl->pcp.len; i++) {
				qos_rx_tc_tbl->pcp.arr[pcp_clen] = temp_rx_hdl->pcp.arr[i];
				pcp_clen++;
			}
			for (i = 0; i < temp_rx_hdl->vlan_ids.len; i++) {
				qos_rx_tc_tbl->vlan_ids.arr[vlan_clen] = temp_rx_hdl->vlan_ids.arr[i];
				vlan_clen++;
			}
			for (i = 0; i < temp_rx_hdl->src.len; i++) {
				memcpy(&qos_rx_tc_tbl->src.arr[src_clen].address, &temp_rx_hdl->src.arr[i].address, sizeof(temp_rx_hdl->src.arr[i].address));
				qos_rx_tc_tbl->src.arr[src_clen].mask_length = temp_rx_hdl->src.arr[i].mask_length;
				if (temp_rx_hdl->src.arr[i].port_num) {
					qos_rx_tc_tbl->src.arr[src_clen].port_num = temp_rx_hdl->src.arr[i].port_num;
					if (temp_rx_hdl->src.arr[i].proto != NULL)
						qos_rx_tc_tbl->src.arr[src_clen].proto = kstrdup(temp_rx_hdl->src.arr[i].proto, GFP_KERNEL);
				}
				src_clen++;
			}
			for (i = 0; i < temp_rx_hdl->dst.len; i++) {
				memcpy(&qos_rx_tc_tbl->dst.arr[dst_clen].address, &temp_rx_hdl->dst.arr[i].address, sizeof(temp_rx_hdl->dst.arr[i].address));
				qos_rx_tc_tbl->dst.arr[dst_clen].mask_length = temp_rx_hdl->dst.arr[i].mask_length;
				if (temp_rx_hdl->dst.arr[i].port_num) {
					qos_rx_tc_tbl->dst.arr[dst_clen].port_num = temp_rx_hdl->dst.arr[i].port_num;
					if (temp_rx_hdl->dst.arr[i].proto != NULL)
						qos_rx_tc_tbl->dst.arr[dst_clen].proto = kstrdup(temp_rx_hdl->dst.arr[i].proto, GFP_KERNEL);
				}
				dst_clen++;
			}
		}

		INIT_LIST_HEAD(&qos_rx_tc_tbl->node);
		list_add_tail(&qos_rx_tc_tbl->node, &idev->ioss_qos_table.qos_rx_tc_table);
	}

	return 0;

dest_alloc_err:
	kfree(qos_rx_tc_tbl->src.arr);
src_alloc_err:
	kfree(qos_rx_tc_tbl->vlan_ids.arr);
vlan_alloc_err:
	kfree(qos_rx_tc_tbl->pcp.arr);
pcp_alloc_err:
	kfree(qos_rx_tc_tbl->smac.arr);
smac_alloc_err:
	kfree(qos_rx_tc_tbl->dmac.arr);
dmac_alloc_err:
rx_tc_tbl_err:
	list_for_each_entry_safe(qos_rx_tc_tbl, tmp, &idev->ioss_qos_table.qos_rx_tc_table, node) {
		list_del(&qos_rx_tc_tbl->node);
		kfree(qos_rx_tc_tbl);
	}
	ioss_qos_dev_err(NULL, "Memory allocation failed for qos_rx_tc_tbl\n");
	return -ENOMEM;
}
/* Utils End */

static ssize_t show_add_tc(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_add_tc(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	/*
	* Four options
	* 1. rx <prio>
	* 2. tx <prio>
	* 3. rx done
	* 4. tx done
	*/

	char *token;
	u8 prio = 0;
	u16 len = 0;
	size_t i = 0;
	char *tmp = NULL;
	int ret = 0;
	struct ioss_device *idev = NULL;
	struct ioss_driver *idrv = NULL;
	bool is_dir_rx = false;
	bool add_to_list = false;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct kobject *kobj = real_kobj_from_dev(dev, 1);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	if (len != 2)
		return -EINVAL;

	tmp = buf;
	while ( (token = strsep(&buf, " ")) ) {
		if (0 == strlen(token))
			continue;

		if (i == 0) {
			if (!strncmp(token, "rx", 2))
				is_dir_rx = true;
			else if (!strncmp(token, "tx", 2))
				is_dir_rx = false;
			else
				goto add_err;
		}
		else {
			if (!strncmp(token, "done", 4))
				add_to_list = true;
			else if (kstrtou8(token, 10, &prio) < 0)
				goto add_err;
		}
		i++;
	}

	if (is_dir_rx) {
		if (add_to_list) {
			if (!idev->ioss_qos_new_nodes.rx_node)
				goto add_err;
			// Check if mandatory action param is provided
			if (idev->ioss_qos_new_nodes.rx_node->action == NOT_DEFINED) {
				ioss_qos_dev_err(NULL, "[ioss qos] action is a mandatory parameter\n");
				goto add_err;
			}
			add_rx_tc_by_priority(idev, idev->ioss_qos_new_nodes.rx_node);
			idev->ioss_qos_new_nodes.rx_node = NULL;
		}
		else {
			// Check if same prio already exists
			if (rx_tc_already_exists(idev, prio)) {
				ioss_qos_dev_err(NULL, "[ioss qos] : rx tc prio already exists");
				goto add_err;
			}
			idev->ioss_qos_new_nodes.rx_node = kzalloc(sizeof(struct qos_rx_tc), GFP_KERNEL);
			if (!idev->ioss_qos_new_nodes.rx_node) {
				kfree(tmp);
				ioss_qos_dev_err(NULL, "Memory allocation failed for rx node\n");
				return -ENOMEM;
			}
			idev->ioss_qos_new_nodes.rx_node->tc_prio = prio;
			INIT_LIST_HEAD(&idev->ioss_qos_new_nodes.rx_node->hdl_node);
		}
	}
	else {
		if (add_to_list) {
			if (!idev->ioss_qos_new_nodes.tx_node)
				goto add_err;
			// Check if mandatory action param is provided
			if (idev->ioss_qos_new_nodes.tx_node->action == NOT_DEFINED) {
				ioss_qos_dev_err(NULL, "[ioss qos] action is mandatory parameter\n");
				goto add_err;
			}
			ret = idrv->qos_ops->validate_tx_tc(idev, &idev->ioss_qos_table.qos_tx_pending_table,
							    idev->ioss_qos_new_nodes.tx_node);
			if (ret) {
				goto add_err;
			} else {
				add_tx_tc_by_priority(idev, idev->ioss_qos_new_nodes.tx_node);
				idev->ioss_qos_new_nodes.tx_node = NULL;
			}
		}
		else {
			// Check if same prio already exists
			if (tx_tc_already_exists(idev, prio)) {
				ioss_qos_dev_err(NULL, "[ioss qos] : tx tc prio already exists");
				goto add_err;
			}

			idev->ioss_qos_new_nodes.tx_node = kzalloc(sizeof(struct qos_routing_tx), GFP_KERNEL);
			if (!idev->ioss_qos_new_nodes.tx_node) {
				kfree(tmp);
				ioss_qos_dev_err(NULL, "Memory allocation failed for tx node\n");
				return -ENOMEM;
			}
			idev->ioss_qos_new_nodes.tx_node->tc_prio = prio;
		}
	}

	kfree(tmp);

	return size;

add_err:
	ioss_qos_dev_err(NULL, "[qos ioss] add tc failed\n");
	kfree(tmp);
	if (idev->ioss_qos_new_nodes.rx_node) {
		clean_rx_node(idev->ioss_qos_new_nodes.rx_node);
		idev->ioss_qos_new_nodes.rx_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add_tc failure\n");
	}
	else if (idev->ioss_qos_new_nodes.tx_node) {
		kfree(idev->ioss_qos_new_nodes.tx_node);
		idev->ioss_qos_new_nodes.tx_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned tx node due to add_tc failure\n");
	}
	return -EINVAL;
}

static ssize_t show_add_handle(struct device *dev,
	struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_add_handle(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{

	char *token;
	u32 handle = 0;
	u16 len = 0;
	size_t i = 0;
	char *tmp = NULL;
	bool add_to_list = false;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 1);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);

	tmp = buf;
	while ( (token = strsep(&buf, " ")) ) {
		if (0 == strlen(token))
			continue;

		if (!strncmp(token, "done", 4))
			add_to_list = true;
		else if (kstrtou32(token, 10, &handle) < 0)
			goto add_err;
		i++;
	}

	if (1) {
		if (add_to_list) {
			if (!idev->ioss_qos_new_nodes.rx_hdl_node)
				goto add_err;
			add_rx_handle(idev, idev->ioss_qos_new_nodes.rx_hdl_node);
			idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		}
		else {
			/* To-Do */
			// Check if same prio already exists
			/*
			if (rx_handle_already_exists(handle)) {
				ioss_qos_dev_err(NULL, "[ioss qos] : rx tc prio already exists");
				goto add_err;
			}
			*/
			idev->ioss_qos_new_nodes.rx_hdl_node = kzalloc(sizeof(struct qos_routing_rx_hdl), GFP_KERNEL);
			if (!idev->ioss_qos_new_nodes.rx_hdl_node) {
				kfree(tmp);
				ioss_qos_dev_err(NULL, "Memory allocation failed for rx_hdl_node\n");
				return -ENOMEM;
			}
			idev->ioss_qos_new_nodes.rx_hdl_node->hdl = handle;
		}
	}

	kfree(tmp);

	return size;

add_err:
	ioss_qos_dev_err(NULL, "[qos ioss] add tc failed\n");
	kfree(tmp);
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add_tc failure\n");
	}
	return -EINVAL;
}

static ssize_t show_tx_handle(struct device *dev,
	struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_tx_handle(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 1);

	u32 input = 0;

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	if (kstrtou32(user_buf, 0, &input)) {
		ioss_qos_dev_err(NULL, "Error in adding tx handle\n");
		return -EINVAL;
	}

	idev->ioss_qos_new_nodes.tx_node->handle = input;
	return size;
}

static ssize_t show_qos_table(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	struct ioss_device *idev = NULL;
	struct ioss_driver *idrv = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 1);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	return idrv->qos_ops->show_qos(idev, user_buf, &idev->ioss_qos_table.qos_rx_committed_table,
									&idev->ioss_qos_table.qos_tx_committed_table);
}

static ssize_t store_qos_table(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 1);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	if (sysfs_streq(user_buf, "clear-pending")) {
		delete_rx_table(&idev->ioss_qos_table.qos_rx_pending_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_pending_table);
		if (copy_rx_table(&idev->ioss_qos_table.qos_rx_committed_table, &idev->ioss_qos_table.qos_rx_pending_table) < 0)
			return -ENOMEM;

		delete_tx_table(&idev->ioss_qos_table.qos_tx_pending_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_pending_table);
		if (copy_tx_table(&idev->ioss_qos_table.qos_tx_committed_table, &idev->ioss_qos_table.qos_tx_pending_table) < 0)
			return -ENOMEM;

		ioss_qos_dev_log(NULL, "[ioss qos] : cleared pending nodes\n");

		idev->clear_qos_hw = false;
	}
	else if (sysfs_streq(user_buf, "clear")) {
		delete_rx_table(&idev->ioss_qos_table.qos_rx_pending_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_pending_table);
		if (copy_rx_table(&idev->ioss_qos_table.qos_rx_committed_table, &idev->ioss_qos_table.qos_rx_pending_table) < 0)
			return -ENOMEM;

		delete_tx_table(&idev->ioss_qos_table.qos_tx_pending_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_pending_table);
		if (copy_tx_table(&idev->ioss_qos_table.qos_tx_committed_table, &idev->ioss_qos_table.qos_tx_pending_table) < 0)
			return -ENOMEM;

		ioss_qos_dev_log(NULL, "[ioss qos] : cleared pending nodes and set clear HW flag\n");

		idev->clear_qos_hw = true;
	}
	else {
		ioss_qos_dev_err(NULL, "[ioss qos] : clear qos table : invalid argument\n");
		return -EINVAL;
	}

	return size;
}

static ssize_t show_del_tc(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_del_tc(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	/*
	* rx 2
	* tx 3
	*/
	char *token;
	u8 prio = 0;
	u16 len = 0;
	size_t i = 0;
	char *tmp = NULL;
	bool is_dir_rx = false;
	struct list_head *ptr;
	struct list_head *temp;
	struct qos_rx_tc *rx_node;
	struct qos_routing_tx *tx_node;
	struct ioss_device *idev = to_ioss_device(dev);
	struct kobject *kobj = real_kobj_from_dev(dev, 1);

	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);

	if (len != 2)
		return -EINVAL;

	tmp = buf;
	while ( (token = strsep(&buf, " ")) ) {
		if (0 == strlen(token))
			continue;

		if (i == 0) {
			if (!strncmp(token, "rx", 2))
				is_dir_rx = true;
			else if (!strncmp(token, "tx", 2))
				is_dir_rx = false;
			else
				goto del_err;
		}
		else {
			if (kstrtou8(token, 10, &prio) < 0)
				goto del_err;
		}
		i++;
	}

	if (is_dir_rx) {
		if (!rx_tc_already_exists(idev, prio)) {
			ioss_qos_dev_log(NULL, "[ioss qos] : entered tc priority not present in rx table\n");
			return -EINVAL;
		}

		list_for_each_safe(ptr, temp, &idev->ioss_qos_table.qos_rx_pending_table) {
			rx_node = to_qos_rx_tc(ptr);
			if (rx_node->tc_prio == prio) {
				list_del(ptr);
				clean_rx_node(rx_node);
				kfree(rx_node);
			}
		}
	}
	else {
		if (!tx_tc_already_exists(idev, prio)) {
			ioss_qos_dev_log(NULL, "[ioss qos] : entered tc priority not present in tx table\n");
			return -EINVAL;
		}

		list_for_each_safe(ptr, temp, &idev->ioss_qos_table.qos_tx_pending_table) {
			tx_node = to_qos_routing_tx(ptr);
			if (tx_node->tc_prio == prio) {
				list_del(ptr);
				kfree(tx_node);
			}
		}
	}

	kfree(tmp);

	return size;
del_err:
	ioss_qos_dev_err(NULL, "[qos ioss] del tc failed\n");
	kfree(tmp);
	return -EINVAL;
}

static ssize_t show_commit(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_commit(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	struct list_head *ptr;
	struct device *parent = NULL;
	struct ioss_device *idev = NULL;
	struct ioss_driver *idrv = NULL;
	struct net_device *net_dev = NULL;
	struct ioss_interface *iface = NULL;
	struct qos_rx_tc *rx_node = NULL;
	struct qos_routing_tx *tx_node = NULL;

	size_t i;
	u8 option;
	int ret = 0;
	struct response res;

	u32 inst_id;
	enum ipa_eth_client_type ct;
	struct ioss_iface_priv *ifp;
	struct ipa_eth_config *ipa_config;

	u8 rx_qos_channels = 0;
	u8 tx_qos_channels = 0;

	if (kstrtou8(user_buf, 10, &option) < 0)
		return -EINVAL;
	if (option != 1)
		return -EINVAL;

	parent = kobj_to_dev(dev->kobj.parent);
	if (!parent)
		return -EINVAL;

	net_dev = to_net_dev(parent);
	if (!net_dev)
		return -EINVAL;

	iface = ioss_netdev_to_iface(net_dev);
	if (!iface)
		return -EINVAL;

	idev = ioss_iface_dev(iface);
	if(!idev)
		return -EINVAL;

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	inst_id = iface->instance_id;
	ct = ioss_ipa_hal_get_ctype(idev);
	ifp = iface->ioss_priv;
	ipa_config = &ifp->ipa_config;

	if (has_qos_table_changed(idev) == false) {
		if (idev->clear_qos_hw == true) {
			delete_rx_table(&idev->ioss_qos_table.qos_rx_pending_table);
			INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_pending_table);
			delete_rx_table(&idev->ioss_qos_table.qos_rx_committed_table);
			INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_committed_table);

			delete_tx_table(&idev->ioss_qos_table.qos_tx_pending_table);
			INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_pending_table);
			delete_tx_table(&idev->ioss_qos_table.qos_tx_committed_table);
			INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_committed_table);

			idev->clear_qos_hw = false;
			idev->qos_enabled = false;
			disable_qos_ipa_channels(idev);
			memset(&idev->curr_qos_config, 0, sizeof(idev->curr_qos_config));
			ioss_qos_dev_log(idev, "[ioss qos] : qos disabled\n");
			return size;
		}
		else {
			ioss_qos_dev_err(idev, "[ioss qos] : commit fail : trying to perform empty commit\n");
			return -EINVAL;
		}
	}

  	if (!idev->qos_enabled) {
		/* Non BE Pipes are SW by default */
		for (i = 1; i < ARRAY_SIZE(idev->curr_qos_config.is_rx_tc_sw); i++)
			idev->curr_qos_config.is_rx_tc_sw[i] = 1;
		for (i = 1; i < ARRAY_SIZE(idev->curr_qos_config.is_tx_tc_sw); i++)
			idev->curr_qos_config.is_tx_tc_sw[i] = 1;
	}
	// Get IPA Config
	iface->ipa_config = NULL;

	memset(ipa_config, 0, sizeof(*ipa_config));
#if IPA_ETH_API_VER >= 4
	ret = ipa_eth_get_config_type(ct, inst_id, ipa_config);
	if (ret) {
		ioss_qos_dev_err(idev, "Failed to get IPA config for %u.%u", ct, inst_id);
		return ret;
	}
#endif
	iface->ipa_config = ipa_config->config;
	ioss_qos_dev_log(idev, "[ioss qos] : IPA config = %s", iface->ipa_config);

	if (strnstr(iface->ipa_config, "qos", IPA_ETH_CONFIG_LEN)) {
		ioss_qos_dev_log(idev, "[ioss qos] : Setting IOSS-IPA config to QOS");
	}
	else {
		ioss_qos_dev_err(idev, "[ioss qos] : Received default/invalid IPA config. Only connect BE pipes");
		// return -EINVAL;
	}

	for (i = 0; i < ipa_config->num_dma_channel; i++) {
		if (ipa_config->dma_config[i].traffic_type != IPA_ETH_PIPE_TRAFFIC_TYPE_QOS)
			continue;
		if (ipa_config->dma_config[i].dir == IPA_ETH_PIPE_DIR_RX)
			rx_qos_channels++;
		else if (ipa_config->dma_config[i].dir == IPA_ETH_PIPE_DIR_TX)
			tx_qos_channels++;
	}

	/* Remove BE channels from the count */
	if (rx_qos_channels)
		rx_qos_channels--;
	if (tx_qos_channels)
		tx_qos_channels--;

	idev->qos_rx_channels = rx_qos_channels;
	idev->qos_tx_channels = tx_qos_channels;
	ioss_qos_dev_log(idev, "[ioss qos] : set idev qos_rx_channels=%u and qos_tx_channels=%u",
			 idev->qos_rx_channels, idev->qos_tx_channels);

	// idev ops
	if (convert_flows_to_tc(idev, &idev->ioss_qos_table.qos_rx_pending_table) < 0)
		return -ENOMEM;

	if (idrv->qos_ops->prepare_qos) {
		res = idrv->qos_ops->prepare_qos(idev, &idev->ioss_qos_table.qos_rx_tc_table, &idev->ioss_qos_table.qos_tx_pending_table);
		ioss_dev_log(idev, "[ioss qos]: glue returned response with err: %d, num_tx_pipes: %u, num_rx_pipes: %u",
					res.qos_response_status, res.num_tx_pipes, res.num_rx_pipes);
	}

	if (!list_empty(&idev->ioss_qos_table.qos_rx_tc_table)) {
		delete_rx_tc_table(&idev->ioss_qos_table.qos_rx_tc_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_tc_table);
	}

	if (res.qos_response_status == QOS_COMMIT_FAIL) {
		ioss_qos_dev_err(idev, "[ioss qos] : prepare_qos returned error, commit failed");
		return -EINVAL;
	}
	else if (res.qos_response_status == QOS_COMMIT_LINK_DOWN) {
		ioss_qos_dev_err(idev, "[ioss qos] : commit  : Ethernet Link down \n");
	}
	else if (res.qos_response_status == QOS_COMMIT_BW_EXHAUST) {
		ioss_qos_dev_err(idev, "[ioss qos] : commit fail : BW EXHAUSTED \n");
	}
	else if (res.qos_response_status == QOS_COMMIT_EMPTY) {
		ioss_qos_dev_err(idev, "[ioss qos] : commit ignored : trying to perform empty commit\n");
	}
	else if (res.qos_response_status == QOS_COMMIT_SUCCESS) {
		idev->qos_commit_in_progress = true;
		idev->qos_response = res;
	}

	list_for_each(ptr, &idev->ioss_qos_table.qos_rx_pending_table) {
		rx_node = to_qos_rx_tc(ptr);
		rx_node->committed = true;
	}
	list_for_each(ptr, &idev->ioss_qos_table.qos_tx_pending_table) {
		tx_node = to_qos_routing_tx(ptr);
		tx_node->committed = true;
	}

	if (!list_empty(&idev->ioss_qos_table.qos_rx_committed_table)) {
		delete_rx_table(&idev->ioss_qos_table.qos_rx_committed_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_committed_table);
	}
	if (copy_rx_table(&idev->ioss_qos_table.qos_rx_pending_table, &idev->ioss_qos_table.qos_rx_committed_table) < 0)
		return -ENOMEM;

	if (!list_empty(&idev->ioss_qos_table.qos_tx_committed_table)) {
		delete_tx_table(&idev->ioss_qos_table.qos_tx_committed_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_committed_table);
	}
	if (copy_tx_table(&idev->ioss_qos_table.qos_tx_pending_table, &idev->ioss_qos_table.qos_tx_committed_table) < 0)
		return -ENOMEM;

	if (res.qos_response_status == QOS_COMMIT_SUCCESS)
		ret = enable_qos_ipa_channels(idev, res);

	idev->qos_enabled = true;

	for (i = 0; i < ARRAY_SIZE(idev->curr_qos_config.is_rx_tc_sw); i++) {
		idev->curr_qos_config.is_rx_tc_sw[i] = res.qos_pipe_mapping.is_rx_tc_sw[i];
		idev->curr_qos_config.pipe_to_tc_mapping_rx[i] = res.qos_pipe_mapping.pipe_to_tc_mapping_rx[i];
	}

	for (i = 0; i < ARRAY_SIZE(idev->curr_qos_config.is_tx_tc_sw); i++) {
		idev->curr_qos_config.is_tx_tc_sw[i] = res.qos_pipe_mapping.is_tx_tc_sw[i];
		idev->curr_qos_config.pipe_to_tc_mapping_tx[i] = res.qos_pipe_mapping.pipe_to_tc_mapping_tx[i];
	}
	ioss_qos_dev_log(idev, "[ioss qos] : set idev->qos_enabled to true\n");
	return size;
}

static ssize_t show_qos_info(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	struct device *parent = NULL;
	struct net_device *net_dev = NULL;
	struct ioss_interface *iface = NULL;
	struct ioss_device *idev = NULL;
	struct ioss_driver *idrv = NULL;
	const ssize_t BUF_SIZE = PAGE_SIZE;
	int bytes_written = 0;

	parent = kobj_to_dev(dev->kobj.parent);
	if (!parent)
		return -EINVAL;

	net_dev = to_net_dev(parent);
	if (!net_dev)
		return -EINVAL;

	iface = ioss_netdev_to_iface(net_dev);
	if (!iface)
		return -EINVAL;

	idev = ioss_iface_dev(iface);
	if(!idev)
		return -EINVAL;

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	bytes_written += idrv->qos_ops->get_qos_info(idev, user_buf, BUF_SIZE);

	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "used_bw: %d\n", get_used_bw(&idev->ioss_qos_table.qos_tx_pending_table));
	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "ioss_ipa_config: %s\n", iface->ipa_config);
	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "ioss_ipa_rx_pipes: %d\n", idev->qos_rx_channels + 1);
	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "ioss_ipa_tx_pipes: %d\n", idev->qos_tx_channels + 1);
	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "committed: %s\n", idev->qos_enabled ? "yes" :"no");
	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "pending: %s\n", has_qos_table_changed(idev) ? "yes": "no");
	bytes_written += snprintf(user_buf + bytes_written, BUF_SIZE - bytes_written,
				  "max_supported_speed: %d\n", idev->qos_hw_cap.max_link_speed);
	return bytes_written;
}

static ssize_t store_info(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	return 0;
}

static ssize_t show_action(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_action(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *token;
	u16 len = 0;
	size_t i = 0;
	char *tmp = NULL;
	bool is_dir_rx = false;
	bool is_action_sw = false;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);

	if (len != 2)
		return -EINVAL;

	tmp = buf;
	while ( (token = strsep(&buf, " ")) ) {
		if (0 == strlen(token))
			continue;

		if (i == 0) {
			if (!strncmp(token, "rx", 2))
				is_dir_rx = true;
			else if (!strncmp(token, "tx", 2))
				is_dir_rx = false;
			else
				goto action_err;
		}
		else {
			if (!strncmp(token, "sw", 2))
				is_action_sw = true;
			else if (!strncmp(token, "hw", 2))
				is_action_sw = false;
			else
				goto action_err;
		}
		i++;
	}

	if (is_dir_rx && idev->ioss_qos_new_nodes.rx_node) {
		if (is_action_sw)
			idev->ioss_qos_new_nodes.rx_node->action = IOSS_QOS_SW_PATH;
		else
			idev->ioss_qos_new_nodes.rx_node->action = IOSS_QOS_HW_PATH;
	}
	else if (!is_dir_rx && idev->ioss_qos_new_nodes.tx_node) {
		if (is_action_sw)
			idev->ioss_qos_new_nodes.tx_node->action = IOSS_QOS_SW_PATH;
		else
			idev->ioss_qos_new_nodes.tx_node->action = IOSS_QOS_HW_PATH;
	}
	else {
		goto action_err;
	}

	kfree(tmp);

	return size;

action_err:
	ioss_qos_dev_err(NULL, "[ioss qos] : invalid direction/action pair entered\n");
	kfree(tmp);
	if (idev->ioss_qos_new_nodes.rx_node) {
		clean_rx_node(idev->ioss_qos_new_nodes.rx_node);
		idev->ioss_qos_new_nodes.rx_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add action failure\n");
	}
	else if (idev->ioss_qos_new_nodes.tx_node) {
		kfree(idev->ioss_qos_new_nodes.tx_node);
		idev->ioss_qos_new_nodes.tx_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned tx node due to add action failure\n");
	}
	return -EINVAL;
}

static ssize_t show_bw(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_bw(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *bw;
	int i = 0;
	u16 bw_val;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	int max_usable_bw;
	int bw_total_min = 0;
	struct ioss_device *idev = NULL;
	struct qos_routing_tx *temp_tx;

	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " :");
	kfree(tmp);

	if (!idev->ioss_qos_new_nodes.tx_node)
		return -EINVAL;

	if (len != 2)
		return -EINVAL;

	tmp = buf;
	while ( (bw = strsep(&buf, " :")) ) {
		if (0 == strlen(bw))
			continue;
		if (kstrtou16(bw, 10, &bw_val) < 0)
			return -EINVAL;
		if (i == 0)
			idev->ioss_qos_new_nodes.tx_node->cbs_bw.low_bw = bw_val;
		else
			idev->ioss_qos_new_nodes.tx_node->cbs_bw.high_bw = bw_val;
		i++;
	}

	kfree(tmp);

	max_usable_bw = idev->qos_hw_cap.max_usable_bw;

	if (!is_valid_bw(idev->ioss_qos_new_nodes.tx_node->cbs_bw.low_bw)) {
		ioss_qos_dev_err(NULL, "[ioss qos] Low BW must be in the range [%d, %d]\n", BW_LOWER_LIMIT, BW_UPPER_LIMIT);
		goto bw_err;
	}

	bw_total_min = idev->ioss_qos_new_nodes.tx_node->cbs_bw.low_bw;
	list_for_each_entry(temp_tx, &idev->ioss_qos_table.qos_tx_pending_table, node) {
		bw_total_min += temp_tx->cbs_bw.low_bw;
	}

	if (bw_total_min > max_usable_bw) {
		ioss_qos_dev_err(NULL, "[ioss qos] Exceeding MAX allowed BW [%d, %d]\n", BW_LOWER_LIMIT, max_usable_bw);
		goto bw_err;
	}

	if (idev->ioss_qos_new_nodes.tx_node->cbs_bw.low_bw > idev->ioss_qos_new_nodes.tx_node->cbs_bw.high_bw) {
		ioss_qos_dev_err(NULL, "[ioss qos] Low BW must be <= High BW\n");
		goto bw_err;
	}

	return size;

bw_err:
	ioss_qos_dev_err(NULL, "[ioss qos] : invalid direction/action pair entered\n");
	if (idev->ioss_qos_new_nodes.tx_node) {
		kfree(idev->ioss_qos_new_nodes.tx_node);
		idev->ioss_qos_new_nodes.tx_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned tx node due to add bw failure\n");
	}
	return -EINVAL;
}

static ssize_t show_tx_pcp(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_tx_pcp(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *pcp;
	int i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.tx_node)
		goto tx_pcp_err;

	if (idev->ioss_qos_new_nodes.tx_node->pcp.arr)
		kfree(idev->ioss_qos_new_nodes.tx_node->pcp.arr);

	idev->ioss_qos_new_nodes.tx_node->pcp.arr = kzalloc(sizeof(u8) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.tx_node->pcp.len = len;

	while ( (pcp = strsep(&buf, " ")) ) {
		if (0 == strlen(pcp))
			continue;
		if (kstrtou8(pcp, 10, &idev->ioss_qos_new_nodes.tx_node->pcp.arr[i]) < 0)
			goto tx_pcp_err;
		if (!is_valid_pcp(idev->ioss_qos_new_nodes.tx_node->pcp.arr[i]))
			goto tx_pcp_err;
		i++;
	}

	kfree(tmp);

	return size;

tx_pcp_err:
	ioss_qos_dev_err(NULL, "[qos ioss] : invalid pcp value entered\n");
	if (idev->ioss_qos_new_nodes.tx_node) {
		kfree(idev->ioss_qos_new_nodes.tx_node->pcp.arr);
		kfree(idev->ioss_qos_new_nodes.tx_node);
		idev->ioss_qos_new_nodes.tx_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned tx node due to add pcp failure\n");
	}
	kfree(tmp);
	return -EINVAL;
}

static ssize_t show_pcp(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_pcp(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *pcp;
	int i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.rx_hdl_node)
		goto pcp_err;

	if (idev->ioss_qos_new_nodes.rx_hdl_node->pcp.arr)
		kfree(idev->ioss_qos_new_nodes.rx_hdl_node->pcp.arr);

	idev->ioss_qos_new_nodes.rx_hdl_node->pcp.arr = kzalloc(sizeof(u8) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.rx_hdl_node->pcp.len = len;

	while ( (pcp = strsep(&buf, " ")) ) {
		if (0 == strlen(pcp))
			continue;
		if (kstrtou8(pcp, 10, &idev->ioss_qos_new_nodes.rx_hdl_node->pcp.arr[i]) < 0)
			goto pcp_err;
		if (!is_valid_pcp(idev->ioss_qos_new_nodes.rx_hdl_node->pcp.arr[i]))
			goto pcp_err;
		i++;
	}

	kfree(tmp);

	return size;

pcp_err:
	ioss_qos_dev_err(NULL, "[qos ioss] : invalid pcp value entered\n");
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add pcp failure\n");
	}
	kfree(tmp);
	return -EINVAL;
}

static ssize_t show_vlan_id(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_vlan_id(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *vid;
	int i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.rx_hdl_node)
		goto vlan_err;

	if (idev->ioss_qos_new_nodes.rx_hdl_node->vlan_ids.arr)
		kfree(idev->ioss_qos_new_nodes.rx_hdl_node->vlan_ids.arr);

	idev->ioss_qos_new_nodes.rx_hdl_node->vlan_ids.arr = kzalloc(sizeof(u16) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.rx_hdl_node->vlan_ids.len = len;

	while ( (vid = strsep(&buf, " ")) ) {
		if (0 == strlen(vid))
			continue;
		if (kstrtou16(vid, 10, &idev->ioss_qos_new_nodes.rx_hdl_node->vlan_ids.arr[i]) < 0)
			goto vlan_err;
		if (!is_valid_vlan_id(idev->ioss_qos_new_nodes.rx_hdl_node->vlan_ids.arr[i]))
			goto vlan_err;
		i++;
	}

	kfree(tmp);

	return size;

vlan_err:
	ioss_qos_dev_err(NULL, "[qos ioss] : invalid vlan id entered \n");
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add vlan id failure\n");
	}
	kfree(tmp);
	return -EINVAL;
}


static ssize_t show_src(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_src(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *src;
	int i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	buf = strim(buf);

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.rx_hdl_node)
		goto src_err;

	if (idev->ioss_qos_new_nodes.rx_hdl_node->src.arr)
		kfree(idev->ioss_qos_new_nodes.rx_hdl_node->src.arr);

	idev->ioss_qos_new_nodes.rx_hdl_node->src.arr = kzalloc(sizeof(struct qos_filters) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.rx_hdl_node->src.len = len;

	while ( (src = strsep(&buf, " ")) ) {
		if (0 == strlen(src))
			continue;
		if (extract_qos_filters(src, &(idev->ioss_qos_new_nodes.rx_hdl_node->src.arr[i])))
			goto src_err;
		i++;
	}

	kfree(tmp);

	return size;
src_err:
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add src failure\n");
	}
	kfree(tmp);
	return -EINVAL;
}

static ssize_t show_dst(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_dst(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *src;
	int i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	buf = strim(buf);

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.rx_hdl_node)
		goto dst_err;

	if (idev->ioss_qos_new_nodes.rx_hdl_node->dst.arr)
		kfree(idev->ioss_qos_new_nodes.rx_hdl_node->dst.arr);

	idev->ioss_qos_new_nodes.rx_hdl_node->dst.arr = kzalloc(sizeof(struct qos_filters) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.rx_hdl_node->dst.len = len;

	while ( (src = strsep(&buf, " ")) ) {
		if (0 == strlen(src))
			continue;
		if (extract_qos_filters(src, &(idev->ioss_qos_new_nodes.rx_hdl_node->dst.arr[i])))
			goto dst_err;
		i++;
	}

	kfree(tmp);

	return size;
dst_err:
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add dst failure\n");
	}
	kfree(tmp);
	return -EINVAL;
}

static ssize_t show_smac(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_smac(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *mac;
	u16 i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.rx_hdl_node)
		goto smac_err;

	if (idev->ioss_qos_new_nodes.rx_hdl_node->smac.arr)
		kfree(idev->ioss_qos_new_nodes.rx_hdl_node->smac.arr);

	idev->ioss_qos_new_nodes.rx_hdl_node->smac.arr = kzalloc(sizeof(u8[ETH_ALEN]) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.rx_hdl_node->smac.len = len;

	while ( (mac = strsep(&buf, " ")) ) {
		if (0 == strlen(mac))
			continue;
		if (!mac_pton(mac, idev->ioss_qos_new_nodes.rx_hdl_node->smac.arr[i])) {
			ioss_qos_dev_err(NULL, "[ioss qos] : invalid smac address entered\n");
			goto smac_err;
		}
		i++;
	}

	kfree(tmp);

	return size;

smac_err:
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add smac failure\n");
	}
	return -EINVAL;
}

static ssize_t show_dmac(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	return 0;
}

static ssize_t store_dmac(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	char *mac;
	u16 i = 0;
	u16 len = 0;
	char *tmp = NULL;
	char *dup = kstrdup(user_buf, GFP_KERNEL);
	char *buf = kstrdup(user_buf, GFP_KERNEL);
	struct ioss_device *idev = NULL;
	struct kobject *kobj = real_kobj_from_dev(dev, 2);

	idev = ioss_dev_from_kobj(kobj);
	if(!idev)
		return -EINVAL;

	tmp = dup;
	len = get_num_arguments(&dup, " ");
	kfree(tmp);
	tmp = buf;

	if (!idev->ioss_qos_new_nodes.rx_hdl_node)
		goto dmac_err;

	if (idev->ioss_qos_new_nodes.rx_hdl_node->dmac.arr)
		kfree(idev->ioss_qos_new_nodes.rx_hdl_node->dmac.arr);

	idev->ioss_qos_new_nodes.rx_hdl_node->dmac.arr = kzalloc(sizeof(u8[ETH_ALEN]) * len, GFP_KERNEL);
	idev->ioss_qos_new_nodes.rx_hdl_node->dmac.len = len;

	while ( (mac = strsep(&buf, " ")) ) {
		if (0 == strlen(mac))
			continue;
		if (!mac_pton(mac, idev->ioss_qos_new_nodes.rx_hdl_node->dmac.arr[i])) {
			ioss_qos_dev_err(NULL, "[ioss qos] : invalid dmac address entered\n");
			goto dmac_err;
		}
		i++;
	}

	kfree(tmp);

	return size;

dmac_err:
	if (idev->ioss_qos_new_nodes.rx_hdl_node) {
		clean_rx_hdl_node(idev->ioss_qos_new_nodes.rx_hdl_node);
		idev->ioss_qos_new_nodes.rx_hdl_node = NULL;
		ioss_qos_dev_log(NULL, "[ioss qos] cleaned rx node due to add dmac failure\n");
	}
	return -EINVAL;
}

static int qos_id = 0;
module_param(qos_id, int, 0);
MODULE_PARM_DESC(qos_id, "The uid/gid to assign to the qos sysfs nodes\n");

static DEVICE_ATTR(add_tc, S_IRWXU | S_IRUGO | S_IRWXG,
		show_add_tc, store_add_tc);
static DEVICE_ATTR(add_handle, S_IRWXU | S_IRUGO | S_IRWXG,
		show_add_handle, store_add_handle);
static DEVICE_ATTR(tx_handle, S_IRWXU | S_IRUGO | S_IRWXG,
		show_tx_handle, store_tx_handle);

static DEVICE_ATTR(qos_table, S_IRWXU | S_IRUGO | S_IRWXG,
		show_qos_table, store_qos_table);
static DEVICE_ATTR(del_tc, S_IRWXU | S_IRUGO | S_IRWXG,
		show_del_tc, store_del_tc);
static DEVICE_ATTR(commit, S_IRWXU | S_IRUGO | S_IRWXG,
		show_commit, store_commit);
static DEVICE_ATTR(info, S_IRWXU | S_IRUGO | S_IRWXG,
		show_qos_info, store_info);

static DEVICE_ATTR(vlan_id, S_IRWXU | S_IRUGO | S_IRWXG,
		show_vlan_id, store_vlan_id);
static DEVICE_ATTR(pcp, S_IRWXU | S_IRUGO | S_IRWXG,
		show_pcp, store_pcp);
static DEVICE_ATTR(src, S_IRWXU | S_IRUGO | S_IRWXG,
		show_src, store_src);
static DEVICE_ATTR(dst, S_IRWXU | S_IRUGO | S_IRWXG,
		show_dst, store_dst);
static DEVICE_ATTR(bw, S_IRWXU | S_IRUGO | S_IRWXG,
		show_bw, store_bw);
static DEVICE_ATTR(action, S_IRWXU | S_IRUGO | S_IRWXG,
		show_action, store_action);
static DEVICE_ATTR(smac, S_IRWXU | S_IRUGO | S_IRWXG,
		show_smac, store_smac);
static DEVICE_ATTR(dmac, S_IRWXU | S_IRUGO | S_IRWXG,
		show_dmac, store_dmac);
static DEVICE_ATTR(tx_pcp, S_IRWXU | S_IRUGO | S_IRWXG,
		show_tx_pcp, store_tx_pcp);


int ioss_qos_create_sysfs(struct device *dev)
{
	struct ioss_device *idev = to_ioss_device(dev);
	struct kobject *qos_kobj = NULL;
	struct kobject *tc_param_kobj = NULL;

	qos_kobj = kobject_create_and_add("qos", &idev->net_dev->dev.kobj);
	if (!qos_kobj) {
		ioss_qos_dev_err(idev, "Unable to create qos kobject");
		goto err_qos_kobj;
	}

	__create_sysfs(idev, qos_kobj, add_tc, 0, qos_id, err_qos_sysfs);
	__create_sysfs(idev, qos_kobj, qos_table, 0, qos_id, err_qos_sysfs);
	__create_sysfs(idev, qos_kobj, del_tc, 0, qos_id, err_qos_sysfs);
	__create_sysfs(idev, qos_kobj, commit, 0, qos_id, err_qos_sysfs);
	__create_sysfs(idev, qos_kobj, info, 0, qos_id, err_qos_sysfs);
	__create_sysfs(idev, qos_kobj, add_handle, 0, qos_id, err_qos_sysfs);
	__create_sysfs(idev, qos_kobj, tx_handle, 0, qos_id, err_qos_sysfs);

	tc_param_kobj = kobject_create_and_add("add_tc_params", qos_kobj);
	if (!tc_param_kobj) {
		ioss_qos_dev_err(idev, "Unable to create qos/add_tc_params kobject");
		goto err_tc_param_kobj;
	}

	__create_sysfs(idev, tc_param_kobj, vlan_id, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, pcp, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, src, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, dst, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, bw, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, action, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, smac, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, dmac, 0, qos_id, err_tc_param_sysfs);
	__create_sysfs(idev, tc_param_kobj, tx_pcp, 0, qos_id, err_tc_param_sysfs);

	idev->qos_kobj = qos_kobj;
	idev->qos_tc_params_kobj = tc_param_kobj;

	return 0;

err_tc_param_sysfs:
	kobject_del(tc_param_kobj);
err_tc_param_kobj:
	kobject_put(tc_param_kobj);
err_qos_sysfs:
	kobject_del(qos_kobj);
err_qos_kobj:
	kobject_put(qos_kobj);
	return -EINVAL;
}

void ioss_qos_init(struct ioss_device *idev)
{
	INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_pending_table);
	INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_committed_table);
	INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_pending_table);
	INIT_LIST_HEAD(&idev->ioss_qos_table.qos_tx_committed_table);
	INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_tc_table);
}


void ioss_qos_remove_sysfs(struct device *dev)
{
        struct ioss_device *idev = to_ioss_device(dev);

	kobject_del(idev->qos_tc_params_kobj);
	kobject_put(idev->qos_tc_params_kobj);
	kobject_del(idev->qos_kobj);
	kobject_put(idev->qos_kobj);
}

int ioss_qos_add_idev(struct ioss_device *idev)
{
	struct ioss_driver *idrv = to_ioss_driver(idev->dev.driver);

	if (!idrv->qos_ops)
		return 0;

	// Check if any of the required QoS operations are null
	if (!idrv->qos_ops->prepare_qos ||
	    !idrv->qos_ops->request_qos ||
	    !idrv->qos_ops->enable_qos ||
	    !idrv->qos_ops->clear_qos ||
	    !idrv->qos_ops->show_qos ||
	    !idrv->qos_ops->clear_qos_cache ||
	    !idrv->qos_ops->get_qos_info ||
	    !idrv->qos_ops->validate_tx_tc ||
	    !idrv->qos_ops->get_hw_caps){
		ioss_qos_dev_err(idev, "Missing Required QoS Operations");
		return -EINVAL;
	}

	idrv->qos_ops->get_hw_caps(idev, &idev->qos_hw_cap);

	return ioss_qos_create_sysfs(&idev->dev);
}

void ioss_qos_remove_idev(struct ioss_device *idev)
{
	struct ioss_driver *idrv = to_ioss_driver(idev->dev.driver);

	if (!idrv->qos_ops)
		return;

	if (idev->qos_enabled) {
		idrv->qos_ops->clear_qos(idev);
		idev->qos_enabled = false;
	}

	ioss_qos_remove_sysfs(&idev->dev);
}

static int ioss_parse_qos_channel(struct ioss_device *idev,
		struct device_node *np, u32 tc_mapping, int ch_num)
{
	const char *key = NULL;
	struct ioss_channel *ch = NULL;
	struct ioss_interface *iface = &idev->interface;

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return -ENOMEM;

	ch->ioss_priv = kzalloc(sizeof(struct ioss_ch_priv), GFP_KERNEL);
	if (!ch->ioss_priv) {
		kfree_sensitive(ch);
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

	key = kstrdup("qcom,ring-size", GFP_KERNEL);
	if (of_property_read_u32(np, key, &ch->default_config.ring_size)) {
		ioss_qos_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}
	kfree(key);

	key = kstrdup("qcom,buff-size", GFP_KERNEL);
	if (of_property_read_u32(np, key, &ch->default_config.buff_size)) {
		ioss_qos_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}
	kfree(key);

	key = kstrdup("qcom,mod-count-min", GFP_KERNEL);
	if (of_property_read_u32(np, key, &ch->event.mod_count_min)) {
		ioss_qos_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}
	kfree(key);

	key = kstrdup("qcom,mod-count-max", GFP_KERNEL);
	if (of_property_read_u32(np, key, &ch->event.mod_count_max)) {
		ioss_qos_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}
	kfree(key);

	key = kstrdup("qcom,mod-usecs-min", GFP_KERNEL);
	if (of_property_read_u32(np, key, &ch->event.mod_usecs_min)) {
		ioss_qos_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}
	kfree(key);

	key = kstrdup("qcom,mod-usecs-max", GFP_KERNEL);
	if (of_property_read_u32(np, key, &ch->event.mod_usecs_max)) {
		ioss_qos_dev_err(idev, "Failed to parse key %s", key);
		goto err;
	}
	kfree(key);

	if (!!of_find_property(np, "qcom,rx-filter-be", NULL))
		ch->filter_types |= IOSS_RXF_F_BE;

	if (!!of_find_property(np, "qcom,rx-filter-ip", NULL))
		ch->filter_types |= IOSS_RXF_F_IP;

	ch->default_config.desc_alctr = &ioss_default_alctr;
	ch->default_config.buff_alctr = &ioss_default_alctr;

	ch->tc_mapping = tc_mapping;
	ch->channel_num = ch_num;
	ch->traffic_type = IOSS_TRAFFIC_QOS;

	list_add_tail(&ch->node, &iface->valid_channels);

	ioss_qos_dev_log(NULL, "Alloc new channel with direction = %d bitmask = %x ch_num=%d",
				ch->direction, ch->tc_mapping, ch->channel_num);

	return 0;

err:
	kfree(key);
	kfree_sensitive(ch->ioss_priv);
	kfree_sensitive(ch);

	return -EINVAL;
}

int ioss_alloc_qos_ch(struct ioss_device *idev, struct response resp)
{
	int i;
	struct device_node *np = dev_of_node(idev->dev.parent);
	struct device_node *n = of_parse_phandle(np, "qcom,ioss_qos_channels", 1);

	/* Allocate QOS channels */
	for (i = resp.num_tx_pipes - 1; i >= 0; i--) {
		ioss_qos_dev_log(NULL, "[ioss] tx, bmap=%X, ch_num=%d\n", resp.qos_pipe_mapping.pipe_to_tc_mapping_tx[i], i);
		if (resp.qos_pipe_mapping.is_tx_tc_sw[i] || resp.qos_pipe_mapping.pipe_to_tc_mapping_tx[i] == 0)
			continue;
		if (ioss_parse_qos_channel(idev, n, resp.qos_pipe_mapping.pipe_to_tc_mapping_tx[i], i)) {
			ioss_qos_dev_err(idev, "Failed to parse qos tx channel[%d]", i);
			goto err;
		}
	}

	n = of_parse_phandle(np,"qcom,ioss_qos_channels", 0);

	/* Allocate QOS channels */
	for (i = resp.num_rx_pipes - 1; i >= 0; i--) {
		ioss_qos_dev_log(NULL, "[ioss qos] rx, bmap=%X, ch_num=%d\n", resp.qos_pipe_mapping.pipe_to_tc_mapping_rx[i], i);
		if (resp.qos_pipe_mapping.is_rx_tc_sw[i] || resp.qos_pipe_mapping.pipe_to_tc_mapping_rx[i] == 0)
			continue;
		if (ioss_parse_qos_channel(idev, n, resp.qos_pipe_mapping.pipe_to_tc_mapping_rx[i], i)) {
			ioss_qos_dev_err(idev, "Failed to parse qos rx channel[%d]", i);
			goto err;
		}
	}

	return 0;

err:
	return -EINVAL;
}

static bool ioss_ipa_reconnect_required(struct ioss_device *idev, struct response *resp)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(resp->qos_pipe_mapping.is_rx_tc_sw); i++) {
		ioss_qos_dev_log(idev, "ch_rx = %d, is_sw = %d -> %d, bmap = %u -> %u \n",
			i, idev->curr_qos_config.is_rx_tc_sw[i], resp->qos_pipe_mapping.is_rx_tc_sw[i],
			idev->curr_qos_config.pipe_to_tc_mapping_rx[i], resp->qos_pipe_mapping.pipe_to_tc_mapping_rx[i]);

		/* Special Case: HW channel becomes a SW channel and vice-versa */
		if (resp->qos_pipe_mapping.is_rx_tc_sw[i] != idev->curr_qos_config.is_rx_tc_sw[i])
			return true;
		if (resp->qos_pipe_mapping.is_rx_tc_sw[i])
			continue;
		if (resp->qos_pipe_mapping.pipe_to_tc_mapping_rx[i] != idev->curr_qos_config.pipe_to_tc_mapping_rx[i])
			return true;
	}

	for (i = 0; i < ARRAY_SIZE(resp->qos_pipe_mapping.is_tx_tc_sw); i++) {
		ioss_qos_dev_log(idev, "ch_tx = %d, is_sw = %d -> %d, bmap = %u -> %u \n",
			i, idev->curr_qos_config.is_tx_tc_sw[i], resp->qos_pipe_mapping.is_tx_tc_sw[i],
			idev->curr_qos_config.pipe_to_tc_mapping_tx[i], resp->qos_pipe_mapping.pipe_to_tc_mapping_tx[i]);

		/* Special Case: HW channel becomes a SW channel and vice-versa */
		if (resp->qos_pipe_mapping.is_tx_tc_sw[i] != idev->curr_qos_config.is_tx_tc_sw[i])
			return true;
		if (resp->qos_pipe_mapping.is_tx_tc_sw[i])
			continue;
		if (resp->qos_pipe_mapping.pipe_to_tc_mapping_tx[i] != idev->curr_qos_config.pipe_to_tc_mapping_tx[i])
			return true;
	}

	return false;
}

#define QOS_UEVENT_TS_STATUS "QOS_ACTION=ts-status"
#define QOS_UEVENT_TS_OK "QOS_TS_OK="
#define QOS_UEVENT_TS_FAIL "QOS_TS_FAIL="

static void ioss_qos_send_ts_status(struct ioss_device *idev)
{
	int ret;
	struct qos_routing_tx *n;
	char action[] = QOS_UEVENT_TS_STATUS;
	char ok[120] = QOS_UEVENT_TS_OK;
	size_t ok_count = sizeof(QOS_UEVENT_TS_OK) - 1;
	char fail[120] = QOS_UEVENT_TS_FAIL;
	size_t fail_count = sizeof(QOS_UEVENT_TS_FAIL) - 1;
	char *envp[] = {action, ok, fail, NULL};

	list_for_each_entry(n, &idev->ioss_qos_table.qos_tx_committed_table, node) {
		if (n->disabled)
			fail_count += snprintf(fail + fail_count, sizeof(fail) - fail_count,
					       "%u,", n->tc_prio);
		else
			ok_count += snprintf(ok + ok_count, sizeof(ok) - ok_count, "%u,",
					     n->tc_prio);
	}

	if(ok[ok_count - 1] == ',')
		ok[ok_count - 1] = '\0';

	if(fail[fail_count - 1] == ',')
		fail[fail_count - 1] = '\0';

	ret = kobject_uevent_env(&idev->dev.kobj, KOBJ_CHANGE, envp);
	if (ret)
		ioss_qos_dev_err(idev, "Failed to send uevent: %d", ret);
}

static int __ioss_qos_enable(struct ioss_device *idev)
{
	int ret;
	struct ioss_driver *idrv;

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	if (!idrv->qos_ops || !idrv->qos_ops->enable_qos)
		return -EINVAL;

	ret = idrv->qos_ops->enable_qos(idev);

	if (!ret)
		ioss_qos_send_ts_status(idev);

	return ret;
}

int enable_qos_ipa_channels(struct ioss_device *idev, struct response resp)
{
	int ret = 0;
	struct ioss_driver *idrv = NULL;
	struct ioss_interface *iface = &idev->interface;
	bool do_ipa_reconnect = ioss_ipa_reconnect_required(idev, &resp);

	if (!iface)
		return -EINVAL;

	if (do_ipa_reconnect) {
		idev->dev.offline = 1;
		ioss_iface_queue_refresh(iface, true);
		ioss_qos_dev_log(idev, "Device Offline set to %d", idev->dev.offline);
	}

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;


	if (do_ipa_reconnect) {
		idev->dev.offline = 0;
		ioss_iface_queue_refresh(iface, true);
		ioss_qos_dev_log(idev, "Device Offline set to %d", idev->dev.offline);
	}
	else {
		ret = idrv->qos_ops->request_qos(idev);
		ret = __ioss_qos_enable(idev);
		idev->qos_commit_in_progress = false;
	}

	return ret;
}

void disable_qos_ipa_channels(struct ioss_device *idev)
{
	struct ioss_interface *iface = &idev->interface;
	struct ioss_driver *idrv = to_ioss_driver(idev->dev.driver);
	struct ioss_channel *ch, *tmp_ch;
	int count = 0;
	int ret = 0;

		/* Count QOS channels */
		list_for_each_entry_safe(ch, tmp_ch, &iface->valid_channels, node) {
			if (ch->tc_mapping != 0) {
				count++;
			}
		}

		list_for_each_entry_safe(ch, tmp_ch, &iface->invalid_channels, node) {
			if (ch->tc_mapping != 0) {
				count++;
			}
		}

	if(!count) {
		ret = idrv->qos_ops->clear_qos(idev);
		return;
	}

	idev->dev.offline = 1;
	ioss_iface_queue_refresh(iface, true);
	ioss_qos_dev_log(idev, "Device Offline set to %d", idev->dev.offline);

	ioss_qos_dev_log(idev, "[ioss qos] : deleting qos tables and clearing qos filters from HW\n");
	ret = idrv->qos_ops->clear_qos(idev);
	ioss_qos_dev_log(idev, "[ioss qos] : clear_qos returned %d\n", ret);

	idev->dev.offline = 0;
	ioss_iface_queue_refresh(iface, true);
	ioss_qos_dev_log(idev, "Device Offline set to %d", idev->dev.offline);
}

int ioss_qos_reconfigure(struct ioss_device *idev)
{
	int ret = 0;
	struct ioss_driver *idrv = NULL;

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	ret = idrv->qos_ops->clear_qos_cache(idev);
	ret = idrv->qos_ops->request_qos(idev);

	return ret;
}

int ioss_qos_enable(struct ioss_device *idev)
{
	int ret = 0;
	struct ioss_driver *idrv = NULL;

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return -EINVAL;

	if (!idrv->qos_ops || !idrv->qos_ops->enable_qos)
		return -EINVAL;

	if (idev->qos_enabled)
		ret = __ioss_qos_enable(idev);

	return ret;
}

void ioss_qos_clear_cache(struct ioss_device *idev)
{
	struct ioss_driver *idrv = to_ioss_driver(idev->dev.driver);

	if (!idrv->qos_ops || !idrv->qos_ops->clear_qos_cache)
		return;

	idrv->qos_ops->clear_qos_cache(idev);
}

void ioss_qos_refresh(struct ioss_device *idev)
{
	struct ioss_driver *idrv = NULL;
	struct ioss_interface *iface = &idev->interface;

	size_t i;
	int ret = 0;
	struct response res;

	u32 inst_id;
	enum ipa_eth_client_type ct;
	struct ioss_iface_priv *ifp;
	struct ipa_eth_config *ipa_config;

	u8 rx_qos_channels = 0;
	u8 tx_qos_channels = 0;

	if (list_empty(&idev->ioss_qos_table.qos_rx_committed_table)
	    && list_empty(&idev->ioss_qos_table.qos_tx_committed_table)) {
		ioss_qos_dev_log(idev, "Nothing to commit - skipping");
		return;
	}

	idrv = to_ioss_driver(idev->dev.driver);
	if (!idrv)
		return;

	inst_id = iface->instance_id;
	ct = ioss_ipa_hal_get_ctype(idev);
	ifp = iface->ioss_priv;
	ipa_config = &ifp->ipa_config;

	if (!idev->qos_enabled) {
		/* Non BE Pipes are SW by default */
		for (i = 1; i < ARRAY_SIZE(idev->curr_qos_config.is_rx_tc_sw); i++)
			idev->curr_qos_config.is_rx_tc_sw[i] = 1;
		for (i = 1; i < ARRAY_SIZE(idev->curr_qos_config.is_tx_tc_sw); i++)
			idev->curr_qos_config.is_tx_tc_sw[i] = 1;
	}
	// Get IPA Config
	iface->ipa_config = NULL;

	memset(ipa_config, 0, sizeof(*ipa_config));
#if IPA_ETH_API_VER >= 4
	ret = ipa_eth_get_config_type(ct, inst_id, ipa_config);
	if (ret) {
		ioss_qos_dev_err(idev, "Failed to get IPA config for %u.%u", ct, inst_id);
		return;
	}
#endif
	iface->ipa_config = ipa_config->config;
	ioss_qos_dev_log(idev, "[ioss qos] : IPA config = %s", iface->ipa_config);

	if (strnstr(iface->ipa_config, "qos", IPA_ETH_CONFIG_LEN)) {
		ioss_qos_dev_log(idev, "[ioss qos] : Setting IOSS-IPA config to QOS");
	}
	else {
		ioss_qos_dev_log(idev,
				  "[ioss qos] : Received default/invalid IPA config. Only connect BE pipes");
	}

	for (i = 0; i < ipa_config->num_dma_channel; i++) {
		if (ipa_config->dma_config[i].traffic_type != IPA_ETH_PIPE_TRAFFIC_TYPE_QOS)
			continue;
		if (ipa_config->dma_config[i].dir == IPA_ETH_PIPE_DIR_RX)
			rx_qos_channels++;
		else if (ipa_config->dma_config[i].dir == IPA_ETH_PIPE_DIR_TX)
			tx_qos_channels++;
	}

	/* Remove BE channels from the count */
	if (rx_qos_channels)
		rx_qos_channels--;
	if (tx_qos_channels)
		tx_qos_channels--;

	idev->qos_rx_channels = rx_qos_channels;
	idev->qos_tx_channels = tx_qos_channels;
	ioss_qos_dev_log(idev, "[ioss qos] : set idev qos_rx_channels=%u and qos_tx_channels=%u",
			 idev->qos_rx_channels, idev->qos_tx_channels);

	convert_flows_to_tc(idev, &idev->ioss_qos_table.qos_rx_committed_table);

	if (!idrv->qos_ops || !idrv->qos_ops->prepare_qos)
		return;

	if (!idev->qos_commit_in_progress) {
		res = idrv->qos_ops->prepare_qos(idev, &idev->ioss_qos_table.qos_rx_tc_table,
					 	 &idev->ioss_qos_table.qos_tx_committed_table);
		ioss_dev_log(idev,
			     "[ioss qos]: glue returned response with err: %d, num_tx_pipes: %u, num_rx_pipes: %u",
			     res.qos_response_status, res.num_tx_pipes, res.num_rx_pipes);
	}
	else {
		res = idev->qos_response;
	}
	idev->qos_commit_in_progress = false;

	if (!list_empty(&idev->ioss_qos_table.qos_rx_tc_table)) {
		delete_rx_tc_table(&idev->ioss_qos_table.qos_rx_tc_table);
		INIT_LIST_HEAD(&idev->ioss_qos_table.qos_rx_tc_table);
	}

	if (res.qos_response_status == QOS_COMMIT_FAIL) {
		ioss_qos_dev_err(idev, "[ioss qos] : prepare_qos returned error, commit failed");
		return;
	}
	else if (res.qos_response_status == QOS_COMMIT_EMPTY) {
		ioss_qos_dev_err(idev, "Detected empty commit. Proceeding...");
	}
	else if (res.qos_response_status == QOS_COMMIT_LINK_DOWN) {
		ioss_qos_dev_err(idev, "[ioss qos] : commit  : Ethernet Link down \n");
	}

	ioss_alloc_qos_ch(idev, res);

	if (!idrv->qos_ops->request_qos)
		return;
	idrv->qos_ops->request_qos(idev);

	idev->qos_enabled = true;

	for (i = 0; i < ARRAY_SIZE(idev->curr_qos_config.is_rx_tc_sw); i++) {
		idev->curr_qos_config.is_rx_tc_sw[i] = res.qos_pipe_mapping.is_rx_tc_sw[i];
		idev->curr_qos_config.pipe_to_tc_mapping_rx[i] = res.qos_pipe_mapping.pipe_to_tc_mapping_rx[i];
	}

	for (i = 0; i < ARRAY_SIZE(idev->curr_qos_config.is_tx_tc_sw); i++) {
		idev->curr_qos_config.is_tx_tc_sw[i] = res.qos_pipe_mapping.is_tx_tc_sw[i];
		idev->curr_qos_config.pipe_to_tc_mapping_tx[i] = res.qos_pipe_mapping.pipe_to_tc_mapping_tx[i];
	}
	ioss_qos_dev_log(idev, "[ioss qos] : set idev->qos_enabled to true\n");
}
