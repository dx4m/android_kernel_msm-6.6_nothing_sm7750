#define RESERVE_POOL_TAG "reserve_pool"
#define pr_fmt(fmt) RESERVE_POOL_TAG ": " fmt

#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/vmstat.h>
#include <linux/oom.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/sizes.h>
#include <trace/hooks/mm.h>
#include <uapi/linux/sched/types.h>

#include "qcom_dma_reserve_pool.h"


#define RESERVE_POOL_SIZE SZ_128M
#define BYTES_TO_PAGES(bytes) (bytes >> PAGE_SHIFT)
#define PAGES_TO_KB(n_pages)	((n_pages) << (PAGE_SHIFT - 10))
#define PAGES_TO_MB(n_pages)	(PAGES_TO_KB(n_pages) >> 10)
#define MAX_HIGH_WM BYTES_TO_PAGES(SZ_2G)
#define MAX_TOTAL_RAM_SIZE BYTES_TO_PAGES(SZ_4G)
#define MAX_RESERVE_POOL_SIZE_TO_PAGES BYTES_TO_PAGES(RESERVE_POOL_SIZE)

#define ORDER9_PAGE_BYTES   (1UL << (PAGE_SHIFT + 9))
#define MAX_ORDER9_PAGES    ((RESERVE_POOL_SIZE / 4) / ORDER9_PAGE_BYTES)

atomic64_t reserve_pool_pages = ATOMIC64_INIT(0);

static LIST_HEAD(reserve_pool_list);
static DEFINE_MUTEX(reserve_pool_list_lock);
struct pass_proc_data *pdata[MAX_inpool_pids];

struct dynamic_reserve_pool *global_reserve_pool;

#define DEFINE_PROC_ATTRIBUTE_EX(__name, _write, container_type, data_expr) \
static int __name##_open(struct inode *inode, struct file *file) {          \
    container_type *container = pde_data(inode);                            \
    struct dynamic_reserve_pool *data = data_expr;                          \
    return single_open(file, __name##_show, data);                          \
}                                                                           \
static const struct proc_ops __name##_proc_ops = {                          \
    .proc_open    = __name##_open,                                          \
    .proc_read    = seq_read,                                               \
    .proc_write   = _write,                                                 \
    .proc_lseek   = seq_lseek,                                              \
    .proc_release = single_release,                                         \
}

#define DEFINE_RESERVE_POOL_PROC_ATTRIBUTE(__name, _write) \
    DEFINE_PROC_ATTRIBUTE_EX(__name, _write, struct dynamic_reserve_pool, container)

#define DEFINE_RESERVE_POOL_PROC_RW_ATTRIBUTE(__name)  \
    DEFINE_RESERVE_POOL_PROC_ATTRIBUTE(__name, __name##_write)

#define DEFINE_RESERVE_POOL_PROC_RO_ATTRIBUTE(__name)  \
    DEFINE_RESERVE_POOL_PROC_ATTRIBUTE(__name, NULL)

#define DEFINE_INPOOL_PID_PROC_RW_ATTRIBUTE(__name) \
    DEFINE_PROC_ATTRIBUTE_EX(__name, __name##_write, struct pass_proc_data, container->reserve_pool)

static bool reserve_pool_enable = true;

static bool is_inpool_process(struct dynamic_reserve_pool *reserve_pool, pid_t tgid)
{
	int i;
	for (i = 0; i < MAX_inpool_pids; i++) {
		if (reserve_pool->inpool_pids[i] == tgid)
			return true;
	}
	return false;
}

void reserve_page_pool_add(struct dynamic_page_pool *pool, struct page *page)
{
	unsigned long flags;
	spin_lock_irqsave(&pool->lock, flags);
	if (PageHighMem(page)) {
		list_add_tail(&page->lru, &pool->high_items);
		pool->high_count++;
	} else {
		list_add_tail(&page->lru, &pool->low_items);
		pool->low_count++;
	}

	spin_unlock_irqrestore(&pool->lock, flags);
	atomic_inc(&pool->count);
	atomic64_add(1 << pool->order, &reserve_pool_pages);
}

struct page *reserve_page_pool_remove(struct dynamic_page_pool *pool, bool high)
{
	struct page *page;

	if (high) {
		BUG_ON(!pool->high_count);
		page = list_first_entry(&pool->high_items, struct page, lru);
		pool->high_count--;
	} else {
		BUG_ON(!pool->low_count);
		page = list_first_entry(&pool->low_items, struct page, lru);
		pool->low_count--;
	}

	atomic_dec(&pool->count);
	list_del(&page->lru);
	atomic64_sub(1 << pool->order, &reserve_pool_pages);
	return page;
}

static void reserve_page_pool_free(struct dynamic_page_pool *pool, struct page *page)
{
	BUG_ON(pool->order != compound_order(page));

	reserve_page_pool_add(pool, page);
}

static struct dynamic_page_pool *per_order_page_pool_create(gfp_t gfp_mask, unsigned int order)
{
	struct dynamic_page_pool *pool = kmalloc(sizeof(*pool), GFP_KERNEL);

	if (!pool)
		return NULL;
	pool->high_count = 0;
	pool->low_count = 0;
	INIT_LIST_HEAD(&pool->low_items);
	INIT_LIST_HEAD(&pool->high_items);
	pool->gfp_mask = gfp_mask | __GFP_COMP;
	pool->order = order;
	spin_lock_init(&pool->lock);

	return pool;
}

static void dynamic_page_pool_destroy_perorder(struct dynamic_page_pool *pool)
{
	struct page *page, *tmp;
	LIST_HEAD(pages);
	int num_pages = 0;
	int ret = DYNAMIC_POOL_SUCCESS;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	while (true) {
		if (pool->low_count)
			page = reserve_page_pool_remove(pool, false);
		else if (pool->high_count)
			page = reserve_page_pool_remove(pool, true);
		else
			break;

		list_add(&page->lru, &pages);
		num_pages++;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	if (num_pages && pool->prerelease_callback)
		ret = pool->prerelease_callback(pool, &pages, num_pages);

	if (ret != DYNAMIC_POOL_SUCCESS) {
		pr_err("%s: Page reclamation failed during pool destruction.\n", __func__);
		return;
	}

	list_for_each_entry_safe(page, tmp, &pages, lru) {
		list_del(&page->lru);
		__free_pages(page, pool->order);
	}

	kfree(pool);
}

static struct dynamic_page_pool **dynamic_reserve_pool_create_pools(int vmid,
							  prerelease_callback callback)
{
	struct dynamic_page_pool **pool_list;
	int i;
	int ret;

	pool_list = kmalloc_array(NUM_ORDERS, sizeof(*pool_list), GFP_KERNEL);
	if (!pool_list)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < NUM_ORDERS; i++) {
		pool_list[i] = per_order_page_pool_create(order_flags[i],
							orders[i]);
		pool_list[i]->vmid = vmid;
		pool_list[i]->prerelease_callback = callback;
		atomic_set(&pool_list[i]->count, 0);
		pool_list[i]->last_low_watermark_ktime = 0;

		if (IS_ERR_OR_NULL(pool_list[i])) {
			int j;

			pr_err("%s: failed to allocate page pool (order %u)\n", __func__, orders[i]);
			for (j = 0; j < i; j++) {
				dynamic_page_pool_destroy_perorder(pool_list[j]);
			}
			ret = -ENOMEM;
			goto free_pool_arr;
		}
	}

	return pool_list;

free_pool_arr:
	kfree(pool_list);

	return ERR_PTR(ret);
}

static void dynamic_reserve_pool_release_pools(struct dynamic_page_pool **pool_list)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++)
		dynamic_page_pool_destroy_perorder(pool_list[i]);

	kfree(pool_list);
}

static inline unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

static int dynamic_reserve_pool_nr_pages(struct dynamic_reserve_pool *pool)
{
	int i;
	int count = 0;

	if (unlikely(!pool)) {
		pr_err("%s: reserve pool is NULL!\n", __func__);
		return 0;
	}

	for (i = 0; i < NUM_ORDERS; i++)
		count += dynamic_page_pool_total(pool->pools[i], 1);

	return count;
}

static int dynamic_reserve_page_pool_refill(struct dynamic_page_pool *pool)
{
	struct page *page;
	gfp_t gfp_refill = pool->gfp_mask;

	if (!pool) {
		pr_err("%s: reserve pool is NULL!\n", __func__);
		return -ENOENT;
	}

	page = alloc_pages(gfp_refill, pool->order);
	if (!page)
		return -ENOMEM;

	reserve_page_pool_free(pool, page);
	return 0;
}

static int dynamic_reserve_pool_kthread(void *p)
{
	int i;
	struct dynamic_reserve_pool *reserve_pool;
	int ret;

	if (!p) {
		pr_err("%s: reserve pool is NULL!\n", __func__);
		return 0;
	}

	reserve_pool = (struct dynamic_reserve_pool *)p;

	while (true) {
		ret = wait_event_interruptible(reserve_pool->wq,
					       (reserve_pool->wait_flag == 1));
		if (ret < 0)
			continue;

		reserve_pool->wait_flag = 0;

		for (i = 0; i < NUM_ORDERS; i++) {
			while (!reserve_pool->stop_flag && dynamic_reserve_pool_nr_pages(reserve_pool) < reserve_pool->low_wm) {
				if (dynamic_reserve_page_pool_refill(reserve_pool->pools[i]) < 0)
					break;
			}
		}
	}

	return 0;
}

static int dynamic_reserve_pool_prealloc_kthread(void *p)
{
	int i;
	struct dynamic_reserve_pool *pool;
	u64 timeout_jiffies;
	int ret;
	unsigned long begin;

	if (!p) {
		pr_err("%s: reserve pool is NULL!\n", __func__);
		return 0;
	}

	pool = (struct dynamic_reserve_pool *)p;
	while (true) {
		ret = wait_event_interruptible(pool->prealloc_wq,
					       (pool->prealloc_wait_flag == 1));
		if (ret < 0)
			continue;

		pool->prealloc_wait_flag = 0;

		mutex_lock(&pool->prealloc_mtx);
		timeout_jiffies = get_jiffies_64() + 2 * HZ;
		begin = jiffies;

		pr_info("%s: prealloc NR %dMB high %dMB\n", current->comm,
		PAGES_TO_MB(dynamic_reserve_pool_nr_pages(pool)), PAGES_TO_MB(pool->high_wm));

		for (i = 0; i < NUM_ORDERS; i++) {
			while (!pool->stop_flag && dynamic_reserve_pool_nr_pages(pool) < pool->high_wm) {
				if (time_after64(get_jiffies_64(), timeout_jiffies)) {
					pr_warn("prealloc timeout!\n");
					break;
				}

				if (dynamic_reserve_page_pool_refill(pool->pools[i]) < 0)
					break;
			}
		}
		pr_info("%s: prealloc done – NR %dMB high %dMB time %dms\n", current->comm,
			PAGES_TO_MB(dynamic_reserve_pool_nr_pages(pool)),
			PAGES_TO_MB(pool->high_wm),
			jiffies_to_msecs(jiffies - begin));

		pool->high_wm = max(dynamic_reserve_pool_nr_pages(pool), pool->low_wm);
		pool->prealloc_flag = false;
		mutex_unlock(&pool->prealloc_mtx);
	}

	return 0;
}

static struct page *do_dynamic_reserve_pool_alloc(struct dynamic_reserve_pool *pool,
				      unsigned long size,
				      unsigned int max_order)
{
	int i;
	unsigned long flags;
	struct page *page = NULL;

	if (!pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return NULL;
	}

	for (i = 0; i < NUM_ORDERS; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;

		spin_lock_irqsave(&pool->pools[i]->lock, flags);
		if (pool->pools[i]->high_count)
			page = reserve_page_pool_remove(pool->pools[i], true);
		else if (pool->pools[i]->low_count)
			page = reserve_page_pool_remove(pool->pools[i], false);
		spin_unlock_irqrestore(&pool->pools[i]->lock, flags);

		if (!page)
			continue;
		return page;
	}
	return NULL;
}

static void dynamic_reserve_pool_dec_high(struct dynamic_reserve_pool *pool, int nr_pages)
{
	if (pool->prealloc_flag)
		return;

	if (unlikely(nr_pages < 0))
		return;

	pool->high_wm = max(pool->low_wm, pool->high_wm - nr_pages);

	return;
}

void dynamic_reserve_pool_alloc(struct dynamic_reserve_pool *reserve_pool, unsigned long *size_remaining_p,
					unsigned int *max_order_p, struct list_head *pages_p, int *i_p)
{
	struct page *page;
	unsigned long alloc_sz = 0;

	if (reserve_pool == NULL)
		return;


	if (reserve_pool->inpool_pids[0] || reserve_pool->inpool_pids[1] || reserve_pool->inpool_pids[2]|| reserve_pool->inpool_pids[3]) {
		if (!is_inpool_process(reserve_pool, current->tgid)) {
			if (dynamic_reserve_pool_nr_pages(reserve_pool) < reserve_pool->inpool_pages) {
				return;
			}
		}
	}

	while (*size_remaining_p > 0) {
		if (fatal_signal_pending(current))
			return;

		page = do_dynamic_reserve_pool_alloc(reserve_pool,
						*size_remaining_p, *max_order_p);
		if (!page)
			break;

		list_add_tail(&page->lru, pages_p);
		*size_remaining_p -= page_size(page);
		alloc_sz += page_size(page);
		*max_order_p = compound_order(page);
		(*i_p)++;
	}


	dynamic_reserve_pool_dec_high(reserve_pool, alloc_sz >> PAGE_SHIFT);
	*max_order_p = orders[0];
}
EXPORT_SYMBOL_GPL(dynamic_reserve_pool_alloc);

static void dynamic_reserve_pool_wakeup_process(struct dynamic_reserve_pool *pool)
{
	if (!reserve_pool_enable)
		return;

	if (!pool) {
		pr_err("%s: reserve_pool is NULL!\n", __func__);
		return;
	}

	if (!pool->stop_flag && !pool->prealloc_flag) {
		pool->wait_flag = 1;
		wake_up_interruptible(&pool->wq);
	}
}

static int dynamic_page_pool_do_shrink(struct dynamic_page_pool *pool, gfp_t gfp_mask,
				       int nr_to_scan)
{
	int freed = 0;
	bool high;
	struct page *page, *tmp;
	LIST_HEAD(pages);
	int ret = DYNAMIC_POOL_SUCCESS;
	unsigned long flags;

	if (current_is_kswapd())
		high = true;
	else
		high = !!(gfp_mask & __GFP_HIGHMEM);

	if (nr_to_scan == 0)
		return dynamic_page_pool_total(pool, high);

	while (freed < nr_to_scan) {
		spin_lock_irqsave(&pool->lock, flags);
		if (pool->low_count) {
			page = reserve_page_pool_remove(pool, false);
		} else if (high && pool->high_count) {
			page = reserve_page_pool_remove(pool, true);
		} else {
			spin_unlock_irqrestore(&pool->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&pool->lock, flags);
		list_add(&page->lru, &pages);
		freed += (1 << pool->order);
	}

	if (freed && pool->prerelease_callback)
		ret = pool->prerelease_callback(pool, &pages, freed >> pool->order);

	if (ret != DYNAMIC_POOL_SUCCESS) {
		pr_err("Failed to reclaim pages from secure page pool, ret=%d\n", ret);
		return 0;
	}

	list_for_each_entry_safe(page, tmp, &pages, lru) {
		list_del(&page->lru);
		__free_pages(page, pool->order);
	}

	return freed;
}

static void dynamic_reserve_pool_all_free(struct dynamic_reserve_pool *pool, gfp_t gfp_mask,
				int nr_to_scan)
{
	int i;

	if (!pool) {
		pr_err("%s: reserve_pool is NULL!\n", __func__);
		return;
	}

	for (i = 0; i < NUM_ORDERS; i++)
		dynamic_page_pool_do_shrink(pool->pools[i], gfp_mask, nr_to_scan);
}

int dynamic_reserve_pool_free(struct dynamic_reserve_pool *pool, struct page *page,
		    int index)
{
	if (!reserve_pool_enable) {
		dynamic_reserve_pool_all_free(pool, __GFP_HIGHMEM, MAX_HIGH_WM);
		return -1;
	}

	if ((!pool) || (!page)) {
		pr_err("%s: reserve_pool or page is NULL!\n", __func__);
		return -1;
	}

	if (dynamic_reserve_pool_nr_pages(pool) > pool->low_wm) //pool full
		return -1;

	reserve_page_pool_free(pool->pools[index], page);
	return 0;
}

static int dynamic_reserve_pool_do_shrink(struct dynamic_reserve_pool *reserve_pool,
				gfp_t gfp_mask, int nr_to_scan)
{
	int nr_max_free;
	int nr_to_free;
	int nr_freed;
	int nr_total = 0;
	int only_scan = 0;
	int i;

	if (!reserve_pool) {
		pr_err("%s: reserve pool is NULL!\n", __func__);
		return 0;
	}

	if (reserve_pool->tsk->pid == current->pid ||
	    reserve_pool->prealloc_tsk->pid == current->pid)
		return 0;

	if (!nr_to_scan) {
		only_scan = 1;
	} else {
		nr_max_free = dynamic_reserve_pool_nr_pages(reserve_pool) -
			(reserve_pool->high_wm + SZ_256);
		nr_to_free = min(nr_max_free, nr_to_scan);
		if (nr_to_free <= 0)
			return 0;
	}

	for (i = 0; i < NUM_ORDERS; i++) {
		if (only_scan) {
			nr_total += dynamic_page_pool_do_shrink(reserve_pool->pools[i],
								gfp_mask, nr_to_scan);
		} else {
			nr_freed = dynamic_page_pool_do_shrink(reserve_pool->pools[i],
							       gfp_mask, nr_to_free);
			nr_to_free -= nr_freed;
			nr_total += nr_freed;
			if (nr_to_free <= 0)
				break;
		}
	}

	return nr_total;
}

static int dynamic_reserve_pool_shrink(gfp_t gfp_mask, int nr_to_scan)
{
	struct dynamic_reserve_pool *reserve_pool;
	int nr_total = 0;
	int nr_freed;
	int only_scan = 0;

	if (!mutex_trylock(&reserve_pool_list_lock))
		return 0;

	if (!nr_to_scan)
		only_scan = 1;

	list_for_each_entry(reserve_pool, &reserve_pool_list, list) {
		if (only_scan) {
			nr_total += dynamic_reserve_pool_do_shrink(reserve_pool,
								 gfp_mask,
								 nr_to_scan);
		} else {
			nr_freed = dynamic_reserve_pool_do_shrink(reserve_pool,
								gfp_mask,
								nr_to_scan);
			nr_to_scan -= nr_freed;
			nr_total += nr_freed;
			if (nr_to_scan <= 0)
				break;
		}
	}
	mutex_unlock(&reserve_pool_list_lock);

	return nr_total;
}

static unsigned long dynamic_reserve_pool_shrink_count(struct shrinker *shrinker,
						    struct shrink_control *sc)
{
	int nr_total;

	nr_total = dynamic_reserve_pool_shrink(sc->gfp_mask, 0);

	return nr_total;
}

static unsigned long dynamic_reserve_pool_shrink_scan(struct shrinker *shrinker,
						   struct shrink_control *sc)
{
	int to_scan = sc->nr_to_scan;
	int nr_total;

	if (to_scan == 0)
		return 0;

	nr_total = dynamic_reserve_pool_shrink(sc->gfp_mask, to_scan);
	return nr_total;
}

static void dynamic_reserve_pool_destroy(struct dynamic_reserve_pool *pool)
{
	mutex_lock(&reserve_pool_list_lock);
	list_del(&pool->list);
	mutex_unlock(&reserve_pool_list_lock);

	dynamic_reserve_pool_release_pools(pool->pools);
	return;
}

static inline void print_pool_info(struct seq_file *s, struct dynamic_page_pool *pool)
{
	unsigned long high_total = (PAGE_SIZE << pool->order) * pool->high_count;
	unsigned long low_total  = (PAGE_SIZE << pool->order) * pool->low_count;
	seq_printf(s, "order %u: highmem pages = %d (%lu total), lowmem pages = %d (%lu total)\n",
			pool->order, pool->high_count, high_total,
			pool->low_count, low_total);
}

static int dynamic_reserve_pool_show(struct seq_file *s, void *v)
{
	struct dynamic_reserve_pool *reserve_pool = s->private;
	int i;

	seq_printf(s, "free %dMB, in prealloc: %d\nori_wm: %dMB low_wm: %dMB high_wm: %dMB\n",
			PAGES_TO_MB(dynamic_reserve_pool_nr_pages(reserve_pool)),
			reserve_pool->prealloc_flag,
			PAGES_TO_MB(reserve_pool->ori_wm),
			PAGES_TO_MB(reserve_pool->low_wm),
			PAGES_TO_MB(reserve_pool->high_wm));

	for (i = 0; i < NUM_ORDERS; i++) {
		print_pool_info(s, reserve_pool->pools[i]);
	}
	return 0;
}

static int parse_int_from_user(const char __user *user_buf, size_t count, int *value)
{
	char buf[SZ_16] = {0};

	if (count > sizeof(buf) - 1)
		count = sizeof(buf) - 1;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	return kstrtoint(strstrip(buf), 0, value);
}

static ssize_t dynamic_reserve_pool_write(struct file *file,
				     const char __user *user_buf,
				     size_t count, loff_t *ppos)
{
	int err, nr_pages;
	struct dynamic_reserve_pool *reserve_pool = pde_data(file_inode(file));

	if (IS_ERR_OR_NULL(reserve_pool)) {
		pr_err("%s: reserve pool is NULL.\n", current->comm);
		return -EFAULT;
	}

	err = parse_int_from_user(user_buf, count, &nr_pages);
	if(err)
		return err;

	pr_info("%s %s: nr %d low_wm %d.\n", current->comm, __func__, nr_pages, reserve_pool->low_wm);
	if (nr_pages == 0) {
		pr_info("%s: set reset flag.\n", current->comm);
		reserve_pool->high_wm = reserve_pool->low_wm = reserve_pool->ori_wm;
		reserve_pool->stop_flag = false;
		return count;
	}

	if (nr_pages == -1) {
		pr_info("%s: set force flag.\n", current->comm);
		reserve_pool->stop_flag = true;
		return count;
	}

	if (nr_pages < 0 || nr_pages >= MAX_HIGH_WM ||
	    nr_pages <= reserve_pool->low_wm)
		return -EINVAL;

	if (mutex_trylock(&reserve_pool->prealloc_mtx)) {
		long mem_avail = si_mem_available();

		pr_info("%s: high %dMB, avail %ldMB\n",
			current->comm, PAGES_TO_MB(nr_pages), PAGES_TO_MB(mem_avail));

		reserve_pool->prealloc_flag = true;
		reserve_pool->stop_flag = false;
		reserve_pool->high_wm = nr_pages;

		reserve_pool->prealloc_wait_flag = 1;
		wake_up_interruptible(&reserve_pool->prealloc_wq);
		mutex_unlock(&reserve_pool->prealloc_mtx);
	} else {
		pr_err("%s: prealloc is running\n", current->comm);
		return -EBUSY;
	}

	return count;
}

DEFINE_RESERVE_POOL_PROC_RW_ATTRIBUTE(dynamic_reserve_pool);


static int dynamic_reserve_pool_pages_show(struct seq_file *s, void *v)
{
	struct dynamic_reserve_pool *reserve_pool = s->private;

	seq_printf(s, "low %dMB\n", PAGES_TO_MB(reserve_pool->inpool_pages));

	return 0;
}

static ssize_t dynamic_reserve_pool_pages_write(struct file *file,
						 const char __user *user_buf,
						 size_t count, loff_t *ppos)
{

	int err, nr_pages;
	struct dynamic_reserve_pool *reserve_pool = pde_data(file_inode(file));

	if (IS_ERR_OR_NULL(reserve_pool)) {
		pr_err("%s: reserve pool is NULL.\n", current->comm);
		return -EFAULT;
	}

	err = parse_int_from_user(user_buf, count, &nr_pages);
	if(err)
		return err;

	pr_info("%s: %s %d\n", current->comm, __func__ , nr_pages);

	if (nr_pages <= 0 || nr_pages >= MAX_HIGH_WM)
		return -EINVAL;

	reserve_pool->inpool_pages = nr_pages;
	nr_pages = reserve_pool->inpool_pages;
	reserve_pool->ori_wm = nr_pages;
	reserve_pool->high_wm = nr_pages;
	reserve_pool->low_wm = nr_pages;
	dynamic_reserve_pool_wakeup_process(reserve_pool);
	return count;
}

DEFINE_RESERVE_POOL_PROC_RW_ATTRIBUTE(dynamic_reserve_pool_pages);

static int dynamic_reserve_pool_info_show(struct seq_file *s, void *v)
{
	struct dynamic_reserve_pool *reserve_pool = s->private;

	seq_printf(s, "inpool %dMB, avail %ldMB, in prealloc: %d\n",
		   PAGES_TO_MB(dynamic_reserve_pool_nr_pages(reserve_pool)),
		   PAGES_TO_MB(si_mem_available()),
		   reserve_pool->prealloc_flag);

	return 0;
}

DEFINE_RESERVE_POOL_PROC_RO_ATTRIBUTE(dynamic_reserve_pool_info);


static struct shrinker reserve_pool_shrinker = {
	.count_objects = dynamic_reserve_pool_shrink_count,
	.scan_objects = dynamic_reserve_pool_shrink_scan,
	.seeks = DEFAULT_SEEKS,
	.batch = 0,
};

static int dynamic_reserve_pool_init_shrinker(void)
{
	int ret;
	static bool registered;

	if (registered)
		return 0;

	ret = register_shrinker(&reserve_pool_shrinker, "reserve_pool_shrinker");
	if (ret)
		return ret;

	registered = true;
	return 0;
}

static inline void set_cpumask(int end_cpu, struct cpumask *mask)
{
	int i;

	cpumask_clear(mask);
	for (i = 0; i <= end_cpu; i++)
		cpumask_set_cpu(i, mask);
}

static ssize_t reserve_pool_cpu_set_write(struct file *file, const char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	int err, cpu, i;
	struct cpumask cpu_mask = { CPU_BITS_NONE };
	struct dynamic_reserve_pool *reserve_pool = pde_data(file_inode(file));

	if (reserve_pool == NULL)
		return -EFAULT;

	err = parse_int_from_user(user_buf, count, &cpu);
	if (err)
		return err;

	if (cpu < 0 || cpu >= MAX_platform_cpus)
		return -EINVAL;

	for (i = 0; i <= cpu; i++)
		cpumask_set_cpu(i, &cpu_mask);

	set_cpus_allowed_ptr(reserve_pool->prealloc_tsk, &cpu_mask);

	pr_info("%s:%d set %s set_cpu 0-%d\n",
		current->comm, current->tgid,
		reserve_pool->prealloc_tsk->comm, cpu);
	return count;
}

static int reserve_pool_cpu_set_show(struct seq_file *s, void *unused)
{
	struct dynamic_reserve_pool *reserve_pool = s->private;

	seq_printf(s, "affinity:[%*pbl]\n",
			cpumask_pr_args(reserve_pool->prealloc_tsk->cpus_ptr));
	return 0;
}
DEFINE_RESERVE_POOL_PROC_RW_ATTRIBUTE(reserve_pool_cpu_set);

struct pass_proc_data {
	struct dynamic_reserve_pool *reserve_pool;
	int index;
};

static ssize_t inpool_pid_common_write(struct file *file, const char __user *user_buf,
				size_t count, loff_t *ppos)
{
	struct task_struct *tsk;
    int err, pid, index;
	struct pass_proc_data *pass_data = pde_data(file_inode(file));
	struct dynamic_reserve_pool *reserve_pool = pass_data->reserve_pool;
	index = pass_data->index;

	pr_info("inpool_pid_%d_write\n", index);
	if (reserve_pool == NULL) {
		pr_info("inpool_pid_%d_write NULL\n", index);
		return -EFAULT;
	}

	err = parse_int_from_user(user_buf, count, &pid);
	if (err)
		return err;

	if (pid == 0) {
		reserve_pool->inpool_pids[index] = 0;
		pr_info("remove inpool_pids[%d]\n", index);
		return count;
	}

	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (tsk != NULL) {
		pr_info("%s:%d set inpool_pid %s:%d\n",
			current->comm, current->tgid,
			tsk->comm, tsk->tgid);
		reserve_pool->inpool_pids[index] = tsk->tgid;
	}
	rcu_read_unlock();

	if (!tsk)
		return -EINVAL;

	return count;
}

static int inpool_pid_common_show(struct seq_file *s, void *unused)
{
	struct dynamic_reserve_pool *reserve_pool = s->private;

	seq_printf(s, "inpool pids:[%d %d %d %d]\n", reserve_pool->inpool_pids[0], reserve_pool->inpool_pids[1], reserve_pool->inpool_pids[2], reserve_pool->inpool_pids[3]);
	return 0;
}
DEFINE_INPOOL_PID_PROC_RW_ATTRIBUTE(inpool_pid_common);

static void meminfo_adjust(void *data, unsigned long *totalram, unsigned long *freeram)
{
	unsigned long pool_pages = 0;
	int i;

	if (unlikely(!reserve_pool_enable))
		return;


	if ((unsigned long)totalram > PAGE_SIZE) {
		for (i = 0; i < NUM_ORDERS; i++) {
			struct dynamic_page_pool *pool = global_reserve_pool->pools[i];
			pool_pages += pool->high_count << pool->order;
			pool_pages += pool->low_count << pool->order;
		}
		*freeram += pool_pages;
	}
}

static struct dynamic_reserve_pool *dynamic_reserve_pool_create(int inpool_pages,
							    struct proc_dir_entry *root_dir)
{
	struct task_struct *tsk;
	struct task_struct *pre_tsk;
	struct dynamic_reserve_pool *reserve_pool;
	char buf[SZ_128];
	struct cpumask mask;
	int end_cpu = 3;
	int ret = 0;
	int nr_pages;
	int i,j;

	struct proc_dir_entry *proc_info, *proc_inpool_pages, *proc_stat, *proc_pid[MAX_inpool_pids], *proc_cpu;

	if (!root_dir) {
		pr_err("%s: reserve_pool dir does not exist.\n", __func__);
		return NULL;
	}

	ret = dynamic_reserve_pool_init_shrinker();
	if (ret) {
		pr_err("%s: reserve_pool shrinker init failed.\n", __func__);
		return NULL;
	}

	reserve_pool = kzalloc(sizeof(struct dynamic_reserve_pool) +
			     sizeof(struct dynamic_page_pool *) * NUM_ORDERS,
			     GFP_KERNEL);

	if (!reserve_pool) {
		pr_err("%s: reserve_pool is NULL!\n", __func__);
		return NULL;
	}

	reserve_pool->pools = dynamic_reserve_pool_create_pools(0, NULL);

	reserve_pool->inpool_pages = inpool_pages;
	nr_pages = inpool_pages;
	reserve_pool->ori_wm = nr_pages;
	reserve_pool->high_wm = nr_pages;
	reserve_pool->low_wm = nr_pages;
	global_reserve_pool = reserve_pool;

	proc_info = proc_create_data("reserve_pool", 0770,
					root_dir,
					&dynamic_reserve_pool_proc_ops,
					reserve_pool);
	if (IS_ERR_OR_NULL(proc_info)) {
		pr_err("Failed to initialize proc entry reserve_pool\n");
		goto destroy_pools;
	}

	proc_inpool_pages = proc_create_data("reserve_pool_pages", 0770, root_dir,
						&dynamic_reserve_pool_pages_proc_ops,
						reserve_pool);
	if (IS_ERR_OR_NULL(proc_inpool_pages)) {
		pr_err("Failed to initialize proc entry reserve_pool_pages\n");
		goto destroy_proc_info;
	}

	proc_stat = proc_create_data("reserve_pool_info", 0550,
						 root_dir,
						 &dynamic_reserve_pool_info_proc_ops,
						 reserve_pool);
	if (IS_ERR_OR_NULL(proc_stat)) {
		pr_err("Failed to initialize proc entry reserve_pool_info\n");
		goto destroy_proc_inpool_pages;
	}

	proc_cpu = proc_create_data("reserve_pool_cpu_set", 0770, root_dir, &reserve_pool_cpu_set_proc_ops,
				    reserve_pool);
	if (!proc_cpu) {
		pr_err("Failed to initialize proc entry reserve_pool_cpu\n");
		goto destroy_proc_stat;
	}

	for (i = 0; i < MAX_inpool_pids; i++) {
		pr_err("Created proc entry inpool_pid_%d\n", i);
		pdata[i] = vzalloc(sizeof(struct pass_proc_data));
		if (!pdata[i])
			goto destroy_proc_pid;

		pdata[i]->reserve_pool = reserve_pool;
		pdata[i]->index = i;

		memset(buf, 0, SZ_128);
		snprintf(buf, SZ_128, "inpool_pid_%d", i);
		proc_pid[i] = proc_create_data(buf, 0770, root_dir,
					&inpool_pid_common_proc_ops, pdata[i]);
		if (!proc_pid[i]) {
			pr_err("Failed to create proc entry inpool_pid_%d\n", i);
			goto destroy_proc_pid;
		}
	}

	init_waitqueue_head(&reserve_pool->wq);
	tsk = kthread_run(dynamic_reserve_pool_kthread, reserve_pool,
			  "reserve_pool_thread");

	if (IS_ERR_OR_NULL(tsk)) {
		pr_err("%s: Failed to create [reserve_pool_thread] kthread\n", __func__);
		goto destroy_proc_pid;
	}
	reserve_pool->tsk = tsk;
	set_cpumask(end_cpu, &mask);
	set_cpus_allowed_ptr(tsk, &mask);

	mutex_init(&reserve_pool->prealloc_mtx);
	init_waitqueue_head(&reserve_pool->prealloc_wq);
	pre_tsk = kthread_run(dynamic_reserve_pool_prealloc_kthread, reserve_pool,
			  "reserve_pool_prealloc_thread");
	if (IS_ERR_OR_NULL(pre_tsk)) {
		pr_err("%s: Failed to create [reserve_pool_prealloc_thread] kthread\n", __func__);
		goto destroy_proc_stat;
	}
	reserve_pool->prealloc_tsk = pre_tsk;
	set_cpus_allowed_ptr(pre_tsk, &mask);

	dynamic_reserve_pool_wakeup_process(reserve_pool);

	mutex_lock(&reserve_pool_list_lock);
	list_add(&reserve_pool->list, &reserve_pool_list);
	mutex_unlock(&reserve_pool_list_lock);
	return reserve_pool;

destroy_proc_pid:
	for (j = 0; j < MAX_inpool_pids; j++) {
		if (proc_pid[j])
			proc_remove(proc_pid[j]);
		if (pdata[j])
			vfree(pdata[j]);
	}
	proc_remove(proc_cpu);
destroy_proc_stat:
	proc_remove(proc_stat);
destroy_proc_inpool_pages:
	proc_remove(proc_inpool_pages);
destroy_proc_info:
	proc_remove(proc_info);
destroy_pools:
	dynamic_reserve_pool_destroy(reserve_pool);

	kfree(reserve_pool);
	reserve_pool = NULL;
	return NULL;
}

struct dynamic_reserve_pool *dynamic_reserve_pool_create_start(void)
{
	struct dynamic_reserve_pool *reserve_pool = NULL;
	struct proc_dir_entry *reserve_root_dir;
	int ret = 0;

	pr_err("%s: start\n", __func__);

	reserve_root_dir = proc_mkdir(RESERVE_POOL_TAG, NULL);
	if (!IS_ERR_OR_NULL(reserve_root_dir)) {
		int inpool_pages = 0;

		if (totalram_pages() > MAX_TOTAL_RAM_SIZE) {
			inpool_pages = MAX_RESERVE_POOL_SIZE_TO_PAGES;
		}

		reserve_pool = dynamic_reserve_pool_create(inpool_pages,
						reserve_root_dir);
		if (!reserve_pool)
			pr_err("%s: Failed to create reserve pool\n", __func__);
	} else {
		pr_err("%s: Failed to create reserve_pool root dir.\n", __func__);
		return reserve_pool;
	}

	ret = register_trace_android_vh_si_meminfo_adjust(meminfo_adjust, NULL);
	if (ret != 0) {
		pr_err("register_trace_android_vh_si_meminfo_adjust failed! ret=%d\n", ret);
	}

	return reserve_pool;
}
EXPORT_SYMBOL_GPL(dynamic_reserve_pool_create_start);
