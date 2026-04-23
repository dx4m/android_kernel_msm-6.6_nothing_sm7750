#ifndef _QCOM_DMA_RESERVE_POOL_H
#define _QCOM_DMA_RESERVE_POOL_H

#include <linux/kthread.h>
#include <linux/types.h>
#include "qcom_dynamic_page_pool.h"

#define MAX_inpool_pids 4
#define MAX_platform_cpus 8

struct dynamic_reserve_pool {
	int inpool_pages;
	int low_wm;
	int high_wm;
	int ori_wm;
	bool prealloc_flag;
	bool stop_flag;
	bool wait_flag;
	bool prealloc_wait_flag;
	pid_t inpool_pids[MAX_inpool_pids];
	wait_queue_head_t wq;
	wait_queue_head_t prealloc_wq;
	struct task_struct *tsk;
	struct task_struct *prealloc_tsk;
	struct list_head list;
	struct mutex prealloc_mtx;
	struct dynamic_page_pool **pools;
};

int dynamic_reserve_pool_free(struct dynamic_reserve_pool *pool, struct page *page,
			    int index);
void dynamic_reserve_pool_alloc(struct dynamic_reserve_pool *reserve_pool, unsigned long *size_remaining_p,
				   unsigned int *max_order_p, struct list_head *pages_p, int *i_p);
struct dynamic_reserve_pool *dynamic_reserve_pool_create_start(void);

#endif /* _QCOM_DMA_RESERVE_POOL_H */