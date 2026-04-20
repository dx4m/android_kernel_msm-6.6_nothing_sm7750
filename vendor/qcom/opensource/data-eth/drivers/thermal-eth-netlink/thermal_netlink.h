//SPDX-License-Identifier: GPL-2.0-only
//Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.

#include <stdbool.h>

typedef void (*nl_trip_cb)(int tz_id, int trip_id, void *data);

#ifdef ENABLE_THERMAL_NETLINK
int thermal_nl_init(void);
void thermal_nl_stop(void);
int thermal_nl_register_trip(nl_trip_cb cb, void *data);
#else
static inline int thermal_nl_init(void)
{
	return -1;
}

static inline void thermal_nl_stop(void) { }

static inline int thermal_nl_register_trip(nl_trip_cb cb, void *data)
{
	return -1;
}

#endif
int initCdev(void);
