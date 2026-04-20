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

#include <linux/netdevice.h>
#include "ioss_i.h"
#include "ioss_version.h"

#define MAX_PARSABLE_FRP_ENTRIES 72
#define CONFIG_LEN 40

#define EMAC0_EMAC_INTERRUPT_ENABLE 0x7C

/* EMAC0_EMAC_INTERRUPT_ENABLE */
#define MASK_ALL_IPA_CH	0x0000FFFF
#define EMAC0_IPA_RX_INTR_EN	BIT(8)
#define EMAC0_IPA_TX_INTR_EN	BIT(0)
#define EMAC0_IPA_RX2_INTR_EN	BIT(10)
#define EMAC0_IPA_TX2_INTR_EN	BIT(2)
#define EMAC0_IPA_RX3_INTR_EN	BIT(11)
#define EMAC0_IPA_TX3_INTR_EN	BIT(3)

#define XGMAC_AE_SHIFT	31
#define XGMAC_MBC_SHIFT	24
#define GMAC_AE_SHIFT	31
#define GMAC_MBC_SHIFT	24

#define EMAC_HW_v3_1_0 0x30010000
#define EMAC_HW_v4_0_0 0x40000000

/* Supporting MACROs for SCM API's */
#define EMAC_SELECT_ALLCH		0xFFFF
#define EMAC_CHANNEL_INTR_EN	(0x1 << 31)

/* Channel configs for various features */

/* Default Config */
#define DEFAULT_QUEUE_RX_TX	IPA_MUL_QUEUE_BE0

#define DEFAULT_CHANNEL_RX_TX	IPA_MUL_CHANNEL_BE0

/* Easymesh Config */
#define EASYMESH_QUEUE_RX_TX_BE_TAGGED	IPA_MUL_QUEUE_BE0
#define EASYMESH_QUEUE_RX_TX_BE	IPA_MUL_QUEUE_BE0

#define EASYMESH_CHANNEL_RX_TX_BE_TAGGED	IPA_MUL_CHANNEL_BE2
#define EASYMESH_CHANNEL_RX_TX_BE	IPA_MUL_CHANNEL_BE0

#define	EZMESH_VID_FILTER	0

/* TSN Config */
#define TSN_QUEUE_TX_LL	IPA_MUL_QUEUE_BE3

#define TSN_CHANNEL_RX_DEFAULT	IPA_MUL_CHANNEL_BE0
#define TSN_CHANNEL_TX_DEFAULT	IPA_MUL_CHANNEL_BE0
#define TSN_CHANNEL_TX_LL	IPA_MUL_CHANNEL_BE3

/* QOS Config */
#define QOS_RX_PCP0_QUEUE	0

enum channel_dir {
	CH_DIR_RX,
	CH_DIR_TX,
};

struct stmmac_ipa_version {
	unsigned char major;  /* IPA I/F major version */
	unsigned char minor;  /* IPA I/F minor version */
};

/*!
 * \remarks :
 * desc_virt_addrs_base : If mem_ops is valid, the base pointer will be updated mem_ops callbacks
 * desc_dma_addrs_base : If mem_ops is valid, the base address will be updated mem_ops callbacks
 */

struct ipa_desc_addr {
	struct dma_desc *desc_virt_addrs_base;	/* Base address to descriptor vitrual addr */
	dma_addr_t desc_dma_addrs_base;		/* Base address to descriptor dma addr */
};

struct ipa_buff_pool_addr {
	void **buff_pool_va_addrs_base;		/* array to hold each buffer VA */
	dma_addr_t *buff_pool_dma_addrs_base;	/* array to hold each buffer DMA addr */
};

struct rxp_filter_entry {
	u32 match_data;
	u32 match_en;
	u8 af:1;
	u8 rf:1;
	u8 im:1;
	u8 nc:1;
	u8 res1:4;
	u8 frame_offset;
	u8 ok_index;
	u8 dma_ch_no;
	u32 res2;
};

enum stmmac_ch_flags {
	STMMAC_CONTIG_BUFS = BIT(0), /* Alloc entire ring buffer memory as a contiguous block */
	/*...*/
};

/* Represents Tx/Rx channel allocated for IPA offload data path */
struct channel_info {
	unsigned int channel_num;	/* Channel number */
	enum channel_dir direction;	/* Tx/Rx */

	unsigned int buf_size;		/* Buffer size */
	unsigned int desc_cnt;		/* Descriptor Count */
	size_t desc_size;		/* Descriptor size in bytes */

	struct ipa_desc_addr desc_addr; /* Structure containing the address of descriptors */
	struct ipa_buff_pool_addr buff_pool_addr; /* Structure containing the address of Tx/RX buffers */

	struct mem_ops *mem_ops;	/* store mem ops to use for allocate/freeing */
	void *client_ch_priv;		/* channel specific private data */
	unsigned int ch_flags;

	struct pci_dev *dma_pdev;	/* pdev that should be used for dma allocation */
	dma_addr_t dma_map_dbaddr;	/* dma mapped address for ntn3 fw to access the db*/

	bool ezmesh_enabled;		/* stores ezmesh enabled infomration */
	bool tsn_enabled;		/* flag to know if tsn feature is enabled or not*/
	bool qos_enabled;		/* flag to check if qos is enabled*/
	enum ioss_traffic_type traffic_type_info; /* Stores traffic type */

};

struct request_channel_input {
	unsigned int desc_cnt;   /* No. of required descriptors for the Tx/Rx Channel */
	enum channel_dir ch_dir; /* CH_DIR_RX for Rx Channel, CH_DIR_TX for Tx Channel */

	unsigned int buf_size;   /* Data buffer size */
	unsigned long flags;	 /* flags for memory allocation Same as gfp_t? */
	struct net_device *ndev; /* stmmac netdev data structure */

	struct mem_ops *mem_ops; /* If NULL, use default flags for memory allocation.
				  * otherwise use the function pointers provided for
				  * descriptor and buffer allocation
				 */
	void *client_ch_priv;    /* To store in channel_info and pass it to mem_ops */
	unsigned int ch_flags;

	char ipa_config_info[CONFIG_LEN]; /* Stores config information */
	enum ioss_traffic_type traffic_type_info; /* Stores traffic type */
	phys_addr_t tail_ptr_addr;
	int channel_num;
};

struct mem_ops {
	void *(*alloc_descs)(struct net_device *ndev, size_t size, dma_addr_t *daddr,
			     gfp_t gfp, struct mem_ops *ops, struct channel_info *channel);
	void *(*alloc_buf)(struct net_device *ndev, size_t size, dma_addr_t *daddr,
			   gfp_t gfp, struct mem_ops *ops, struct channel_info *channel);

	void (*free_descs)(struct net_device *ndev, void *buf, size_t size,
			   dma_addr_t *daddr, struct mem_ops *ops, struct channel_info  *channel);
	void (*free_buf)(struct net_device *ndev, void *buf, size_t size,
			 dma_addr_t *daddr, struct mem_ops *ops, struct channel_info *channel);
};

struct mac_addr_list {
	u8 addr[6];	/* 0th to 3rd bytes will be programmed in MAC_Address_Low.ADDLO
			 * 4th - 5th bytes will be programmed in MAC_Address_High.ADDRHI
			 */
	u8 ae;		/* Address Enable */
	u8 mbc;		/* Mask Byte Control */
	u16 dcs;	/* DMA Channel Select */
};

/*qos*/

enum qos_filter_type {
	PCP,
	SRC_MAC,
	DEST_MAC,
	VLAN_ID,
	SRC_IP,
	DEST_IP,
	SRC_PORT,
	DEST_PORT,
	INVALID_FILTER,
};

struct filter_map_info {
	u8 queue;
	u8 channel;
	enum qos_filter_type filter;
};

struct tx_route_info {
	u8 channel;
	u16 acc_bw;
	u8 mode_to_use;
	unsigned int idle_slope;
	unsigned int send_slope;
	unsigned int hi_credit;
	unsigned int low_credit;
};

struct qos_struct {
	struct list_head pcp_route_table;
	struct list_head dma_filter_table;
	struct list_head flt_to_app;
	struct tx_route_info tx_routing_info[MTL_MAX_TX_QUEUES];
	struct qos_pipe_mapping pipe_map;
	enum action rx_channel_info[MTL_MAX_RX_QUEUES];
	enum action tx_channel_info[MTL_MAX_TX_QUEUES];
	u8 backup_pcp_map[MTL_MAX_RX_QUEUES];
	u16 backup_rx_fifo_size[MTL_MAX_RX_QUEUES];
	u16 bw_allocated[MTL_MAX_TX_QUEUES];
	u8 queue_to_pcp_map[MTL_MAX_RX_QUEUES];
	u8 queue_to_ch_map[MTL_MAX_RX_QUEUES];
	u8 tc_to_queue_map[8];
	u8 filter_cnt;
	u8 queue_cnt;
	int asgn_sw_queue;
	int asgn_hw_queue;
	u8 ipa_qos_rx_ch;
	u8 ipa_qos_tx_ch;
};

enum data_path {
	SW_PATH,
	HW_PATH,
	SW_HW_PATH,
};

struct src_ip {
       unsigned char ipv6_src_addr[16];
       u32 ipv4_src_addr;
       u8 src_mask_length;
       bool ipv6_src;
};

struct dest_ip {
       unsigned char ipv6_dst_addr[16];
       u32 ipv4_dst_addr;
       u8 dst_mask_length;
       bool ipv6_dst;
};

struct pcp_routing {
	u8 tc_prio;
	u8 pcp;
	u8 queue;
	u8 dma_ch;
	enum action path;
	struct list_head node;
};
#define to_pcp_routing(ptr) list_entry(ptr, struct pcp_routing, node)

struct dma_filter_table {
	u8 tc_prio;
	u8 dma_ch;
	bool applied;
	union  {
		u16 vlan_id;
		struct src_ip ip_src;
		struct dest_ip ip_dest;

		struct port src_port;
		struct port dst_port;
	};
    struct list_head node;
};
#define to_dma_filter_table(ptr) list_entry(ptr, struct dma_filter_table, node)

#define PCP_MAX_VALUE	7

/*CBS params*/
#define SGMII_INTERFACE_BIT	8
#define SGMII_2500X_BIT	32
#define MAX_FRAME_SIZE	1518
#define MAX_INTERFERENCE_SIZE	1518

/*!
 * \brief API to allocate a channel for IPA  Tx/Rx datapath,
 *	  allocate memory and buffers for the DMA channel, setup the
 *	  descriptors and configure the require registers and
 *	  mark the channel as used by IPA in the stmmac driver
 *
 *	  The API will check for NULL pointers and Invalid arguments such as,
 *	  out of bounds buf size > 9K bytes, descriptor count > 512
 *
 * \param[in] channel_input : data structure specifying all input needed to request a channel
 *
 * \return channel_info : Allocate memory for channel_info structure and initialize the
 *			  structure members NULL on fail.
 * \remarks :In case of Tx, only TDES0 and TDES1 will be updated with buffer addresses.
 *	     TDES2 and TDES3 must be updated by the offloading driver.
 */
struct channel_info *request_channel(struct request_channel_input *channel_input);

/*!
 * \brief Release the resources associated with the channel
 *	  and mark the channel as free in the stmmac driver,
 *	  reset the descriptors and registers
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer or memory buffers in channel pointer are NULL
 *
 * \remarks : DMA Channel has to be stopped prior to invoking this API
 */
int release_channel(struct net_device *ndev, struct channel_info *channel);

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
int request_event(struct net_device *ndev, struct channel_info *channel, dma_addr_t addr, u64 data);

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
int release_event(struct net_device *ndev, struct channel_info *channel);

/*!
 * \brief Enable interrupt generation for given channel
 *
 * The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL
 */
int enable_event(struct net_device *ndev, struct channel_info *channel);

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
int disable_event(struct net_device *ndev, struct channel_info *channel);

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
int set_event_mod(struct net_device *ndev, struct channel_info *channel, unsigned int wdt);

/*!
 * \brief This API will configure the FRP table with the parameters passed through rx_filter_info.
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel,
 *	  number of filter entries > 72
 *
 * \param[in] ndev : stmmac netdev data structure
 * \param[in] filter_params: filter_params containig the parameters based on which
 *			     packet will pass or drop
 * \return : Return 0 on success, -ve value on error
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL filter_params, if number of entries > 72
 *
 * \remarks : The entries should be prepared considering the filtering and routing to CortexA also
 *	      MAC Rx will be stopped while updating FRP table dynamically.
 */

int set_rx_filter(struct net_device *ndev, struct rxp_filter_entry *filter_entries);

/*!
 * \brief This API will clear the FRP filters and route all packets to RxCh0
 *
 *	 The API will check for NULL pointers
 *
 * \param[in] ndev : stmmac netdev data structure
 * \return : Return 0 on success, -ve value on error
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *
 * \remarks : MAC Rx will be stopped while updating FRP table dynamically.

 */
int clear_rx_filter(struct net_device *ndev);

/*!
 * \brief Start the DMA channel. channel_dir member variable
 *	  will be used to start the Tx/Rx channel
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer NULL

 */
int start_channel(struct net_device *ndev, struct channel_info *channel);

/*!
 * \brief Stop the DMA channel. channel_dir member variable will be
 *	  used to stop the Tx/Rx channel. In case of Rx, clear the
 *	  MTL queue associated with the channel and this will result in packet drops
 *
 *	  The API will check for NULL pointers and Invalid arguments such as non IPA channel
 *
 * \param[in] ndev : stmmac netdev data structure
 * \param[in] channel : Pointer to structure containing channel_info that needs to be released
 *
 * \return : Return 0 on success, -ve value on error
 *	     -EPERM if non IPA channels are accessed
 *	     -ENODEV if ndev is NULL, stmmac_priv extracted from ndev is NULL
 *	     -EINVAL if channel pointer  NULL
 */
int stop_channel(struct net_device *ndev, struct channel_info *channel);

/*!
 * \brief Configure MAC registers at a particular index in the MAC Address list
 *
 * \param[in] ndev : stmmac netdev data structure
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
 *	      when " stmmacmac_set_rx_mode" is invoked via "ndo_set_rx_mode" callback.
 */
int set_mac_addr(struct net_device *ndev, struct mac_addr_list *mac_addr, u8 index);

/* Enable PFC and queue routing for QOS
 *param[in] priv : stmmac priv data structure
 * \param[in] qos_table_info : qos_struct which contains required qos_info post
 * qos_table processing
 */
void stmmac_enable_qos_queue_cfg(struct stmmac_priv *priv, struct qos_struct *qos_table_info);

/* Enable filters to route QOS traffic
 * param[in] priv : stmmac priv data structure
 * param[in] qos_table_info : qos_struct which contains required qos_info post
 * qos_table processing
 */
void stmmac_enable_qos_filtering(struct net_device *ndev, struct qos_struct *qos_table_info);

/* Configure CBS for qos queues
 * param[in] ndev : stmmac netdev data structure
 * param[in] qos_table_info : qos_struct which contains required qos_info post
 * qos_table processing
 */
void stmmac_config_qos_cbs(struct stmmac_priv *priv, struct qos_struct *qos_table_info);

/* Restore qos queue routing after disabling QOS
 *param[in] priv : stmmac priv data structure
 * \param[in] qos_table_info : qos_struct which contains required qos_info post
 * qos_table processing
 */
void stmmac_restore_qos_queue_cfg(struct stmmac_priv *priv, struct qos_struct *qos_table_info);

/* Remove filters to route QOS traffic
 * param[in] priv : stmmac priv data structure
 * param[in] filter_type : filter to be cleared
 * param[in] action : filter action
 * qos_table processing
 */
void stmmac_remove_qos_filtering(struct net_device *ndev, int filter_type, enum idx_action action);

/* Restore DMA configuration after disabling QOS
 * param[in] ndev : stmmac netdev data structure
 * \param[in] qos_table_info : qos_struct which contains required qos_info post
 * qos_table processing
 */
void stmmac_restore_dma_config(struct net_device *ndev, struct qos_struct *qos_table_info);

/* Backup pcp routing before applying qos config
 * \param[in] qos_table_info : qos_struct which contains required qos_info post
 * qos_table processing
 */
void stmmac_backup_pcp(struct stmmac_priv *priv, struct qos_struct *qos_table_info);

/* Configure tx queue
 * param[in] priv : stmmac priv data structure
 * param[in] queue number
 * param[in] txmode is DCB/AVB
 */
void stmmac_configure_tx_queue(struct stmmac_priv *priv, u8 queue, u8 txmode);

/* stmmac qos tx routing strategy
 * param[in] ndev : stmmac netdev data structure
 */
bool stmmac_is_skprio_routing (struct net_device *ndev);

/* Configure RX queue path in SW/HW
 * param[in] ndev : stmmac netdev data structure
 * param[in] queue number
 * skip_sw : Queue is for SW/HW
 */
int config_rx_queue_path(struct net_device *ndev, u32 queue, bool skip_sw);

/* Configure TX queue path is SW/HW
 * param[in] ndev : stmmac netdev data structure
 * param[in] queue number
 * skip_sw : Queue is for SW/HW
 */
int config_tx_queue_path(struct net_device *ndev, u32 queue, bool skip_sw);

/* MAC-PCS Interface width
 * param[in] priv : stmmac priv data structure
 */
#if IS_ENABLED(CONFIG_ETHQOS_QCOM_VER4)
ssize_t stmmac_intf_width(struct stmmac_priv *priv);
#endif
