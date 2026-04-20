/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#ifndef _EMAC_CTRL_SVM_MSGQ_H_
#define _EMAC_CTRL_SVM_MSGQ_H_

int qcom_ethmsgq_init(struct device *dev);
int qcom_ethmsgq_deinit(struct device *dev);

enum {
	NOTIFICATION = 0,
	REQUEST = 1
} msg_type;

#endif /* _EMAC_CTRL_SVM_MSGQ_H_ */
