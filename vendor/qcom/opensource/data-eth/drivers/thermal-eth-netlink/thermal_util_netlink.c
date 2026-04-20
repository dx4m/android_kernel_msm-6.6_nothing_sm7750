//SPDX-License-Identifier: GPL-2.0-only
//Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <linux/thermal.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/mngt.h>
#include <netlink/genl/ctrl.h>
#include <netlink/netlink.h>
#include <dirent.h>
#include "thermal_netlink.h"
#include "ds_util.h"

#define MAX_LENGTH		256
#define MAX_PATH		256
#define THERMAL_SYSFS		"/sys/class/thermal/"
#define TZ_TYPE			"type"
#define CDEV_DIR_NAME		"cooling_device"
#define CDEV_DIR_FMT		"cooling_device%d"
#define CDEV_TYPE_NAME		"ethernet-usxgmii"

struct cdevData {
//	Mitigation data;
	int cdevn;
};

enum thermal_nl_cb_type {
	THERMAL_NL_TRIP,
};

union thermal_nl_cb {
	nl_trip_cb trip_cb;
};

struct thermal_nl_cb_data {
	union thermal_nl_cb cb;
	enum thermal_nl_cb_type type;
	void *data;
	struct thermal_nl_cb_data *next;
};

struct thermal_nl_socket {
	struct nl_sock *soc;
	int grp_id;
	struct thermal_nl_cb_data *head_ptr;
	pthread_mutex_t cb_mutex;
};

static struct thermal_nl_socket nl_socket = {
	.soc = NULL,
	.grp_id = -1,
	.head_ptr = NULL,
};

static bool stop;
static pthread_t thermal_sensor_event_thread;
static int old_state;
static int get_cid;
static int thermal_nl_parse_family(struct nl_msg *msg, void *data)
{
	struct nlattr *tb[CTRL_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *mc_group;
	int rem_mcgrp;

	nla_parse(tb, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (!tb[CTRL_ATTR_MCAST_GROUPS]) {
		printf("Multicast group not available\n");
		return -1;
	}

	nla_for_each_nested(mc_group, tb[CTRL_ATTR_MCAST_GROUPS], rem_mcgrp) {

		struct nlattr *nl_group[CTRL_ATTR_MCAST_GRP_MAX + 1];

		nla_parse(nl_group, CTRL_ATTR_MCAST_GRP_MAX,
			  nla_data(mc_group), nla_len(mc_group), NULL);

		if (!nl_group[CTRL_ATTR_MCAST_GRP_NAME] ||
				!nl_group[CTRL_ATTR_MCAST_GRP_ID])
			continue;

		if (!strncmp(nla_data(nl_group[CTRL_ATTR_MCAST_GRP_NAME]),
				      THERMAL_GENL_EVENT_GROUP_NAME,
				      nla_len(nl_group[CTRL_ATTR_MCAST_GRP_NAME])))
			nl_socket.grp_id = nla_get_u32(
						       nl_group[CTRL_ATTR_MCAST_GRP_ID]);

	}

	return 0;
}

static int thermal_nl_send_msg(struct nl_msg *msg)
{
	int ret = 0;

	ret = nl_send_auto(nl_socket.soc, msg);
	if (ret < 0)
		return ret;
	nl_socket_disable_seq_check(nl_socket.soc);
	nl_socket_modify_cb(nl_socket.soc, NL_CB_VALID, NL_CB_CUSTOM,
			    thermal_nl_parse_family, NULL);
	ret = nl_recvmsgs_default(nl_socket.soc);
	return ret;
}

static int thermal_nl_fetch_id(void)
{
	struct nl_msg *msg;
	int ctrlid, ret = 0;

	msg = nlmsg_alloc();
	if (!msg)
		return -ENOMEM;

	ctrlid = genl_ctrl_resolve(nl_socket.soc, "nlctrl");
	genlmsg_put(msg, 0, 0, ctrlid, 0, 0, CTRL_CMD_GETFAMILY, 0);

	nla_put_string(msg, CTRL_ATTR_FAMILY_NAME,
		       THERMAL_GENL_FAMILY_NAME);
	thermal_nl_send_msg(msg);

	nlmsg_free(msg);

	if (nl_socket.grp_id != -1) {
		ret = nl_socket_add_membership(nl_socket.soc,
					       nl_socket.grp_id);
		if (ret)
			return ret;
	}
	return 0;
}

static void notify_event_cb(struct thermal_nl_socket *soc_data,
		int cdev_id, int cur_state)
{
	struct thermal_nl_cb_data *ptr = soc_data->head_ptr;

	if (cdev_id == -1)
		return;
	pthread_mutex_lock(&soc_data->cb_mutex);
	ptr = soc_data->head_ptr;
	while (ptr) {
		(ptr->cb.trip_cb)(cdev_id, cur_state, ptr->data);
		ptr = ptr->next;
	}
	pthread_mutex_unlock(&soc_data->cb_mutex);
}

static int thermal_nl_event_cb(struct nl_msg *n, void *data)
{
	struct thermal_nl_socket *soc_data =
		(struct thermal_nl_socket *)data;
	struct nlmsghdr *nl_hdr = nlmsg_hdr(n);
	struct genlmsghdr *genlhdr = genlmsg_hdr(nl_hdr);
	struct nlattr *attrs[THERMAL_GENL_ATTR_MAX + 1];
	int cdev_id = -1, cur_state = -1;

	genlmsg_parse(nl_hdr, 0, attrs, THERMAL_GENL_ATTR_MAX, NULL);

	switch (genlhdr->cmd) {
	case THERMAL_GENL_EVENT_CDEV_STATE_UPDATE:
		if (attrs[THERMAL_GENL_ATTR_CDEV_ID]) {
			cdev_id = nla_get_u32(attrs[THERMAL_GENL_ATTR_CDEV_ID]);
			if (cdev_id !=  get_cid) {
				notify_event_cb(soc_data, cdev_id, cur_state);
				return 0;
			}
		}
	
		if (attrs[THERMAL_GENL_ATTR_CDEV_CUR_STATE])
			cur_state = nla_get_u32(
						attrs[THERMAL_GENL_ATTR_CDEV_CUR_STATE]);

		if ((old_state == 0) && (cur_state == 1)) {
			if (system("echo 1 > /sys/class/net/eth1/suspend_ipa_offload") == -1)
				return -1;
			if (system("echo 1 > /sys/class/net/eth1/thermal_netlink") == -1)
				return -1;
			if (system("echo 1 > /sys/class/net/eth1/change_if") == -1)
				return -1;
			if (system("echo 0 > /sys/class/net/eth1/thermal_netlink") == -1)
				return -1;
			if (system("echo 0 > /sys/class/net/eth1/suspend_ipa_offload") == -1)
				return -1;

		} else if ((old_state == 1) && (cur_state == 0)) {
			if (system("echo 1 > /sys/class/net/eth1/suspend_ipa_offload") == -1)
				return -1;
			if (system("echo 1 > /sys/class/net/eth1/thermal_netlink") == -1)
				return -1;
			if (system("echo 2 > /sys/class/net/eth1/change_if") == -1)
				return -1;
			if (system("echo 0 > /sys/class/net/eth1/thermal_netlink") == -1)
				return -1;
			if (system("echo 0 > /sys/class/net/eth1/suspend_ipa_offload") == -1)
				return -1;
		}
		old_state = cur_state;
		notify_event_cb(soc_data, cdev_id, cur_state);
		break;
	}

	return 0;
}

static void *thermal_sensor_netlink(void *data)
{
	if (nl_socket.grp_id == -1)
		return NULL;

	nl_socket_disable_seq_check(nl_socket.soc);
	nl_socket_modify_cb(nl_socket.soc, NL_CB_VALID, NL_CB_CUSTOM,
			    thermal_nl_event_cb, &nl_socket);
	while (!stop)
		nl_recvmsgs_default(nl_socket.soc);

	return NULL;
}

void thermal_nl_stop(void)
{
	struct thermal_nl_cb_data *ptr = NULL, *n = NULL;

	stop = true;
	pthread_join(thermal_sensor_event_thread, NULL);
	nl_socket_free(nl_socket.soc);
	nl_socket.soc = NULL;
	pthread_mutex_lock(&nl_socket.cb_mutex);
	ptr = nl_socket.head_ptr;
	while (ptr) {
		n = ptr->next;
		free(ptr);
		ptr = n;
	}
	nl_socket.head_ptr = NULL;
	pthread_mutex_unlock(&nl_socket.cb_mutex);
}

int thermal_nl_init(void)
{
	int ret = -1;

	if (nl_socket.soc != NULL)
		return 0;

	if (!nl_socket.soc) {
		nl_socket.soc = nl_socket_alloc();
		if (!nl_socket.soc)
			return -1;

		if (genl_connect(nl_socket.soc)) {
			nl_socket_free(nl_socket.soc);
			nl_socket.soc = NULL;
			return -1;
		}
		stop = false;
	}

	ret = thermal_nl_fetch_id();
	if (ret) {
		nl_socket_free(nl_socket.soc);
		nl_socket.soc = NULL;
		return ret;
	}

	initCdev();
	pthread_mutex_init(&nl_socket.cb_mutex, NULL);
	pthread_create(&thermal_sensor_event_thread, NULL,
		       thermal_sensor_netlink, NULL);
	return 0;
}

static int thermal_nl_add_cb(struct thermal_nl_socket *soc,
			     union thermal_nl_cb cb, void *data,
			     enum thermal_nl_cb_type type)
{
	struct thermal_nl_cb_data *ptr = NULL;

	ptr = malloc(sizeof(*ptr));
	if (!ptr)
		return -1;
	ptr->cb = cb;
	ptr->data = data;
	ptr->type = type;
	pthread_mutex_lock(&soc->cb_mutex);
	ptr->next = soc->head_ptr;
	soc->head_ptr = ptr;
	pthread_mutex_unlock(&soc->cb_mutex);

	return 0;
}

int thermal_nl_register_trip(nl_trip_cb cb, void *data)
{
	union thermal_nl_cb local_cb;

	if (!cb)
		return -1;
	local_cb.trip_cb = cb;

	return thermal_nl_add_cb(&nl_socket, local_cb, data, THERMAL_NL_TRIP);
}

int readLineFromFile(char *path, char out[MAX_PATH][MAX_PATH], int l_var)
{
	char *fgets_ret;
	FILE *fd;
	int cid = -1;
	char buf[MAX_LENGTH];

	fd = fopen(path, "r");
	if (fd == NULL)
		return errno;

	fgets_ret = fgets(buf, MAX_LENGTH, fd);
	if (fgets_ret != NULL) {
		strlcpy(out[l_var], buf, strlen(out[l_var]));
		if (strncmp(buf, CDEV_TYPE_NAME,
			strlen(CDEV_TYPE_NAME)) == 0) {
			sscanf(path, "%*[^0-9]%d", &cid);
		}
	} else {
		(void)ferror(fd);
	}
	(void)fclose(fd);
	return cid;
}

int initCdev(void)
{
	DIR *tdir = NULL;
	struct dirent *tdirent = NULL;
	char name[MAX_PATH] = {0};
	char cwd[MAX_PATH] = {0};
	int l_var = -1;

	if (!getcwd(cwd, sizeof(cwd)))
		return 0;

	tdir = opendir(THERMAL_SYSFS);
	if (!tdir)
		return 0;

	while ((tdirent = readdir(tdir))) {
		char buf[MAX_LENGTH][MAX_LENGTH];
		struct dirent *tzdirent;
		struct cdevData cdevInst;

		if (strncmp(tdirent->d_name, CDEV_DIR_NAME,
		    strlen(CDEV_DIR_NAME)) != 0)
			continue;

		snprintf(name, MAX_PATH, "%s%s/%s", THERMAL_SYSFS,
			 tdirent->d_name, TZ_TYPE);
		++l_var;
		get_cid = readLineFromFile(name, buf, l_var);

		if (get_cid >= 0)
			break;
	}
	printf("cid = %d\n", get_cid);
	closedir(tdir);
	/* Restore current working dir */
	chdir(cwd);

	return 0;
}
