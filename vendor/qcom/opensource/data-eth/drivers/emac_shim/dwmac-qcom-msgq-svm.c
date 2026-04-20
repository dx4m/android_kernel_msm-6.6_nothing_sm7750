/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <linux/compiler.h>/* for __maybe_unused */
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <soc/qcom/boot_stats.h>
#include <linux/gunyah/gh_msgq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/pm.h>
#include <linux/suspend.h>
#include "../include/linux/emac_ctrl_fe_api.h"

enum msg_type {
	MSGQ_HANDSHAKE_REQ,
	MSGQ_HANDSHAKE_RESP,
	NOTIFICATION,
	REG_EVENTS,
	UNREG_EVENTS,
	DMA_STOP_ACK,
	UNICAST_ADD,
	MULTICAST_ADD,
	VLAN_ADD,
	UNICAST_DEL,
	MULTICAST_DEL,
	VLAN_DEL,
	PRIO_ADD,
	PRIO_DEL,
};

enum emac_msgq_state{
	EMAC_DMA_DRV_UNREG,
	EMAC_DMA_DRV_REG,
};

#define EMAC_MAX_VLAN_ID          4094
#define EMAC_MIN_VLAN_ID             1
#define IS_VLAN_ID_VALID(vlan_id)    (vlan_id < EMAC_MAX_VLAN_ID && vlan_id >= EMAC_MIN_VLAN_ID)

struct msg_format {
	uint8_t  type;   	/* Message type */
	uint8_t  event;     /* Event Id*/
	uint8_t  data[12];  /* Data field length - 2 mac address can be stored */
} __packed;

struct emac_msgq_priv {
	struct device 			*dev;
	void 				*msgq_hdl;
	bool 				is_pvm;
	bool 				init_done;
	bool notify_hw_events;
	struct msg_format		tx_msg;
	struct msg_format		rx_msg;
	enum emac_msgq_state   	emac_dma_drv_state;
	enum emac_ctrl_fe_gvm_event 	emac_state;
	struct mutex                    emac_msgq_lock;
	struct task_struct 		*recv_thread;
};

ssize_t test_msgq_kick_eth_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count);

static struct emac_msgq_priv *msgq_priv;
static ATOMIC_NOTIFIER_HEAD(emac_msgq_rx_notifier_chain);
static struct device_attribute test_msgq_attr = __ATTR_WO(test_msgq_kick_eth);

/* Returns 0 on success, other values for appropriate failure*/
int emac_msgq_xmit(struct msg_format *msg) {
	int ret = 0;
	if(!msgq_priv->init_done && msg->type != MSGQ_HANDSHAKE_REQ) {
		dev_err(msgq_priv->dev, "msgq not initialized\n");
		return -EINVAL;
	}

	ret = gh_msgq_send(msgq_priv->msgq_hdl, msg, sizeof(*msg), 0);
	if (ret)
		dev_err(msgq_priv->dev, "send msgq failed %d\n", ret);
	return ret;
}

static int recv_thread(void *data)
{
	int cpu = smp_processor_id();
	size_t recv_size;
	int ret;

	dev_err(msgq_priv->dev, "%d %s\n", __LINE__, __func__);

	while (!kthread_should_stop()) {
		gh_msgq_recv(msgq_priv->msgq_hdl, &msgq_priv->rx_msg, sizeof(msgq_priv->rx_msg),
				 &recv_size, 0);

		dev_info(msgq_priv->dev, "EMAC recd msg on CPU %d: Msg type 0x%x Event = 0x%x\n",
			cpu, msgq_priv->rx_msg.type, msgq_priv->rx_msg.event);

		switch (msgq_priv->rx_msg.type) {
		case MSGQ_HANDSHAKE_RESP:
			msgq_priv->init_done = true;
			break;

		case NOTIFICATION:
			if (msgq_priv->emac_dma_drv_state == EMAC_DMA_DRV_REG) {
				switch (msgq_priv->rx_msg.event) {
				case EMAC_HW_DOWN:
				case EMAC_HW_UP:
				case EMAC_LINK_DOWN:
				case EMAC_LINK_UP:
					atomic_notifier_call_chain(&emac_msgq_rx_notifier_chain,
								   msgq_priv->rx_msg.event, NULL);
					msgq_priv->emac_state = msgq_priv->rx_msg.event;
					break;

				default:
					break;
				}
			}
			else
				dev_err(msgq_priv->dev, "Recd msg in unregestered State\n",
					msgq_priv->rx_msg.event);
			break;

		default:
			break;
		}
	}
	return 0;
}

ssize_t test_msgq_kick_eth_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	char send_buf[64] = "test msg";
	char recv_buf[64];
	int ret;
	bool flag;
	size_t recv_size;

	dev_err(dev, "%d %s\n", __LINE__, __func__);

	ret = strtobool(buf, &flag);
	if (ret) {
		dev_err(dev, "invalid user input\n");
		return -1;
	}
	if (flag) {
		ret = gh_msgq_send(msgq_priv->msgq_hdl, send_buf, sizeof(send_buf), 0);
		if (ret)
			dev_err(dev, "send msgq failed %d\n", ret);
		else
			dev_info(dev, "send msgq success\n");
	}
	return ret ? ret : count;
}

int emac_ctrl_fe_register_notifier(struct notifier_block *nb)
{
	int ret = 0;
	if (msgq_priv && msgq_priv->init_done) {
		/*DMA Driver is now registered*/
		dev_info(msgq_priv->dev, "Register for Event notification \n");

		ret = atomic_notifier_chain_register(&emac_msgq_rx_notifier_chain, nb);
		if(ret)
			goto reg_notifier_fail;

		msgq_priv->emac_dma_drv_state = EMAC_DMA_DRV_REG;

		/*process notification sequence so far*/
		mutex_lock(&msgq_priv->emac_msgq_lock);
		msgq_priv->tx_msg.type = REG_EVENTS;
		ret = emac_msgq_xmit(&msgq_priv->tx_msg);
		mutex_unlock(&msgq_priv->emac_msgq_lock);

		if(ret)
			goto reg_fail;
	}

	return ret;

reg_fail:
	msgq_priv->emac_dma_drv_state = EMAC_DMA_DRV_UNREG;
	atomic_notifier_chain_unregister(&emac_msgq_rx_notifier_chain, nb);
reg_notifier_fail:
	dev_err(msgq_priv->dev, "Register for Event notification failed: ret = %d\n", ret);
	return ret;
}
EXPORT_SYMBOL_GPL(emac_ctrl_fe_register_notifier);

int emac_ctrl_fe_unregister_notifier(struct notifier_block *nb)
{
	int ret = 0;
	if (msgq_priv && msgq_priv->init_done) {
		msgq_priv->emac_dma_drv_state = EMAC_DMA_DRV_UNREG;

		mutex_lock(&msgq_priv->emac_msgq_lock);
		msgq_priv->tx_msg.type = UNREG_EVENTS;
		ret = emac_msgq_xmit(&msgq_priv->tx_msg);
		mutex_unlock(&msgq_priv->emac_msgq_lock);

		if(ret)
			return ret;

		dev_info(msgq_priv->dev, "Sent UnRegister Event Cmd \n");
	}

	return atomic_notifier_chain_unregister(&emac_msgq_rx_notifier_chain, nb);

}
EXPORT_SYMBOL_GPL(emac_ctrl_fe_unregister_notifier);

int emac_ctrl_fe_register_ready_cb(void (*emac_ctrl_fe_ready_cb)(void *user_data),
	void *user_data)
{
	if (msgq_priv && msgq_priv->init_done) {
		/*call the callback*/
		if (emac_ctrl_fe_ready_cb)
			emac_ctrl_fe_ready_cb(user_data);
		return 0;
	}
	else {
		dev_err(msgq_priv->dev, "Msgq not ready yet\n");
		return -EINVAL;
	}
}
EXPORT_SYMBOL(emac_ctrl_fe_register_ready_cb);

void __maybe_unused emac_ctrl_fe_gvm_dma_stopped(void){
	dev_info(msgq_priv->dev, "Send DMA STOP to Host");
	mutex_lock(&msgq_priv->emac_msgq_lock);
	msgq_priv->tx_msg.type = DMA_STOP_ACK;
	emac_msgq_xmit(&msgq_priv->tx_msg);
	mutex_unlock(&msgq_priv->emac_msgq_lock);
}
EXPORT_SYMBOL(emac_ctrl_fe_gvm_dma_stopped);

int __maybe_unused emac_ctrl_fe_mac_addr_chg(struct unicast_mac_addr *new_mac_addr) {
	if (!is_valid_ether_addr(new_mac_addr->enm_addr)) {
		dev_err(msgq_priv->dev, "invalid ethernet address:\n");
		//return -EINVAL;
	}

	dev_info(msgq_priv->dev, "Requesting Filter for unicast MacAddr: %pM\n", new_mac_addr->enm_addr);

	mutex_lock(&msgq_priv->emac_msgq_lock);
	msgq_priv->tx_msg.type = UNICAST_ADD;
	memcpy(&(msgq_priv->tx_msg.data[0]), new_mac_addr->enm_addr, ETH_ALEN);
	emac_msgq_xmit(&msgq_priv->tx_msg);
	mutex_unlock(&msgq_priv->emac_msgq_lock);

	return 0;
}
EXPORT_SYMBOL(emac_ctrl_fe_mac_addr_chg);


/* request filter addition at EMAC HW*/
int __maybe_unused emac_ctrl_fe_filter_add_request(enum emac_ctrl_fe_filter_types filter_type,
	union emac_ctrl_fe_filter *filter) {

	if (!msgq_priv)
		return -EINVAL;

	dev_info(msgq_priv->dev, "EMAC Filter Add Req %d", filter_type);
	mutex_lock(&msgq_priv->emac_msgq_lock);

	switch (filter_type) {
	case UNICAST_FILTER:
		if (!is_valid_ether_addr(filter->unicast_mac.enm_addr)) {
			dev_err(msgq_priv->dev, "invalid ethernet address:\n");
			//return -EINVAL;
		}

		dev_info(msgq_priv->dev, "Requesting Filter for unicast MacAddr: %pM\n", filter->unicast_mac.enm_addr);
		msgq_priv->tx_msg.type = UNICAST_ADD;
		memcpy(&(msgq_priv->tx_msg.data[0]), &filter->unicast_mac, ETH_ALEN);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	case MULTICAST_FILTER:
		if (!is_multicast_ether_addr(filter->multi_mac.enm_addr)) {
			dev_err(msgq_priv->dev, "invalid multicast address:\n");
			//return -EINVAL;
		}

		dev_info(msgq_priv->dev, "Requesting Filter for multicast MacAddr: %pM\n", filter->multi_mac.enm_addr);
		msgq_priv->tx_msg.type = MULTICAST_ADD;
		memcpy(&msgq_priv->tx_msg.data[0], filter->multi_mac.enm_addr, ETH_ALEN);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	case VLAN_FILTER:
		if (!IS_VLAN_ID_VALID(filter->vlan_id) ) {
			dev_err(msgq_priv->dev, "invalid vlan_id %d:\n", filter->vlan_id);
			//return -EINVAL;
		}

		dev_info(msgq_priv->dev, "Requesting Vlan filter Add: %d\n", filter->vlan_id);
		msgq_priv->tx_msg.type = VLAN_ADD;
		memcpy(&msgq_priv->tx_msg.data[0], filter->multi_mac.enm_addr, 2);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	case PRIORITY_FILTER:
		dev_info(msgq_priv->dev, "Requesting Vlan priority Add: %d\n", filter->vlan_prio);
		msgq_priv->tx_msg.type = PRIO_ADD;
		memcpy(&msgq_priv->tx_msg.data[0], filter->multi_mac.enm_addr, 1);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	default:
		break;
	}

	mutex_unlock(&msgq_priv->emac_msgq_lock);
	return 0;
}
EXPORT_SYMBOL(emac_ctrl_fe_filter_add_request);

/* request filter deletion at EMAC HW*/
int __maybe_unused emac_ctrl_fe_filter_del_request(enum emac_ctrl_fe_filter_types filter_type,
	union emac_ctrl_fe_filter filter) {

	if (!msgq_priv)
		return -EINVAL;
	mutex_lock(&msgq_priv->emac_msgq_lock);

	dev_info(msgq_priv->dev, "EMAC Filter Del Req %d", filter_type);
	switch (filter_type) {
	case UNICAST_FILTER:
		if (!is_valid_ether_addr(filter.unicast_mac.enm_addr)) {
			dev_err(msgq_priv->dev, "invalid ethernet address:\n");
			//return -EINVAL;
		}

		dev_info(msgq_priv->dev, "Deleting Filter for unicast MacAddr: %pM\n", filter.unicast_mac.enm_addr);
		msgq_priv->tx_msg.type = UNICAST_DEL;
		memcpy(&msgq_priv->tx_msg.data[0], &filter.unicast_mac, ETH_ALEN);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	case MULTICAST_FILTER:
		if (!is_multicast_ether_addr(filter.multi_mac.enm_addr)) {
			dev_err(msgq_priv->dev, "invalid multicast address:\n");
			//return -EINVAL;
		}

		dev_info(msgq_priv->dev, "Deleting Filter for multicast MacAddr: %pM\n", filter.multi_mac.enm_addr);
		msgq_priv->tx_msg.type = MULTICAST_DEL;
		memcpy(&msgq_priv->tx_msg.data[0], filter.multi_mac.enm_addr, ETH_ALEN);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	case VLAN_FILTER:
		if (!IS_VLAN_ID_VALID(filter.vlan_id) ) {
			dev_err(msgq_priv->dev, "invalid vlan_id: %d\n", filter.vlan_id);
			//return -EINVAL;
		}

		dev_info(msgq_priv->dev, "Deleting Vlan filter: %d\n", filter.vlan_id);
		msgq_priv->tx_msg.type = VLAN_DEL;
		memcpy(&msgq_priv->tx_msg.data[0], filter.multi_mac.enm_addr, 2);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	case PRIORITY_FILTER:
		dev_info(msgq_priv->dev, "Deleting Vlan priority: %d\n", filter.vlan_prio);
		msgq_priv->tx_msg.type = PRIO_DEL;
		memcpy(&msgq_priv->tx_msg.data[0], filter.multi_mac.enm_addr, 1);
		emac_msgq_xmit(&msgq_priv->tx_msg);
		break;

	default:
		break;
	}

	mutex_unlock(&msgq_priv->emac_msgq_lock);
	return 0;
}
EXPORT_SYMBOL(emac_ctrl_fe_filter_del_request);


int qcom_ethmsgq_init(struct device *dev)
{
	struct device_node *node = dev->of_node;
	int ret;

	dev_err(dev, "%d %s\n", __LINE__, __func__);

	msgq_priv = devm_kzalloc(dev, sizeof(*msgq_priv), GFP_KERNEL);
	if (!msgq_priv)
		return -ENOMEM;

	msgq_priv->dev = dev;

	/* we are in SVM*/
	msgq_priv->is_pvm = false;
	msgq_priv->msgq_hdl = gh_msgq_register(GH_MSGQ_LABEL_ETH);
	if (IS_ERR_OR_NULL(msgq_priv->msgq_hdl)) {
		ret = PTR_ERR(msgq_priv->msgq_hdl);
		dev_err(dev, "failed to get gunyah msgq %d\n", ret);
		return ret;
	}

	/* Init mutex */
	mutex_init(&msgq_priv->emac_msgq_lock);

	msgq_priv->recv_thread = kthread_run(recv_thread, msgq_priv, dev_name(dev));

	/* sysfs node for validation */
	ret = sysfs_create_file(&dev->kobj, &test_msgq_attr.attr);
	if (ret)
		dev_err(dev, "Create sysfs node test kick failed\n");

	/* Send Init to PVM and wait for Init response to confirm MSGQ is functional */
	mutex_lock(&msgq_priv->emac_msgq_lock);
	msgq_priv->tx_msg.type = MSGQ_HANDSHAKE_REQ;
	ret = emac_msgq_xmit(&msgq_priv->tx_msg);
	mutex_unlock(&msgq_priv->emac_msgq_lock);

	if (ret)
		dev_err(dev, "Msgq not ready\n");

	return ret;
}

int qcom_ethmsgq_deinit(struct device *dev)
{
	if (msgq_priv->msgq_hdl)
		gh_msgq_unregister(msgq_priv->msgq_hdl);
	return 0;
}

