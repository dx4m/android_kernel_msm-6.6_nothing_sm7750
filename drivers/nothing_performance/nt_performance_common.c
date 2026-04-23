#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <trace/hooks/vendor_hooks.h>
#include <trace/hooks/sys.h>
#include <trace/hooks/sched.h>

#include "nt_performance_common.h"
#include "nt_performance_print.h"

#if IS_ENABLED(CONFIG_NOTHING_NAMED_THREAD_AFFINITY)
#include "nothing_named_thread_affinity.h"
#endif /* CONFIG_NOTHING_NAMED_THREAD_AFFINITY */

#define REGISTER_ANDROID_VENDOR_HOOK(name) \
	{ \
		ret = register_trace_##name( \
				probe_##name, NULL); \
		if (ret) { \
			pr_err("Register vendor hook %s failed: %d", #name, ret); \
		} else { \
			hook_trace_##name = true; \
		} \
	}


bool hook_trace_android_vh_syscall_prctl_finished = false;
bool hook_trace_android_vh_sched_setaffinity_early = false;

static void probe_android_vh_sched_setaffinity_early(void *ignore, struct task_struct *p, const struct cpumask *in_mask, bool *skip)
{
#if IS_ENABLED(CONFIG_NOTHING_NAMED_THREAD_AFFINITY)
	check_setaffinity_skip_by_named_thread_affinity(ignore, p, in_mask, skip);
#endif /* CONFIG_NOTHING_NAMED_THREAD_AFFINITY */
}

static void probe_android_vh_syscall_prctl_finished(void *ignore, int option, struct task_struct *p)
{
#if IS_ENABLED(CONFIG_NOTHING_NAMED_THREAD_AFFINITY)
	set_thread_affinity_on_set_name(ignore, option, p);
#endif /* CONFIG_NOTHING_NAMED_THREAD_AFFINITY */
}

void register_nt_performance_vendor_hooks(void)
{
	int ret;

	REGISTER_ANDROID_VENDOR_HOOK(android_vh_syscall_prctl_finished)
	REGISTER_ANDROID_VENDOR_HOOK(android_vh_sched_setaffinity_early)
}

static int __init nt_performance_init(void)
{
	int ret = 0;

	pr_info("Module init");

	register_nt_performance_vendor_hooks();

#if IS_ENABLED(CONFIG_NOTHING_NAMED_THREAD_AFFINITY)
	nt_named_thread_affinity_init();
#endif /* CONFIG_NOTHING_NAMED_THREAD_AFFINITY */

	return ret;
}

static void __exit nt_performance_exit(void)
{
	/*
	 * vendor hook cannot unregister
	 */
}

module_init(nt_performance_init);
module_exit(nt_performance_exit);
MODULE_LICENSE("GPL v2");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("<BSP_CORE@nothing.tech>");
MODULE_DESCRIPTION("NOTHING PERFORMANCE COMMON");
