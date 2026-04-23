#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/rcupdate.h>
#include <linux/sizes.h>

#define PROC_DIR_NAME	"cpu_monitor"
#define PROC_ENABLE_NAME "enable_monitor"
#define PROC_PID_NAME	"target_pid"
#define PROC_PERIOD_NAME "period_ms"
#define PROC_THRESH_NAME "thresh_pct"
#define PROC_COUNT_NAME	   "exceed_count"

static unsigned int enable_monitor = 1;
static pid_t   target_pid = 0;
static unsigned int period_ms  = 1000;
static unsigned int thresh_pct = 90;
static unsigned int exceed_threshold = 5;
static unsigned int exceed_count = 0;
static bool first_sample = true;

static u64 last_runtime_ns = 0;

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_enable_entry;
static struct proc_dir_entry *proc_pid_entry;
static struct proc_dir_entry *proc_period_entry;
static struct proc_dir_entry *proc_thresh_entry;
static struct proc_dir_entry *proc_count_entry;


static struct task_struct *monitor_thread;



static int format_val(char *buf, size_t len, unsigned long val)
{
	return scnprintf(buf, len, "%lu\n", val);
}

static ssize_t read_enable(struct file *file, char __user *ubuf,
						   size_t count, loff_t *ppos)
{
	char buf[SZ_32];
	int len;
	if (*ppos > 0)
		return 0;
	len = scnprintf(buf, sizeof(buf), "%u\n", enable_monitor);
	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;
	*ppos = len;
	return len;
}

static ssize_t write_enable(struct file *file, const char __user *ubuf,
							size_t count, loff_t *ppos)
{
	char buf[SZ_32];
	unsigned long v;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;

	buf[count] = '\0';
	ret = kstrtoul(buf, 10, &v);

	if (ret)
		return ret;

	enable_monitor = (v != 0);
	pr_info("cpu_monitor: enable_monitor set to %u\n", enable_monitor);
	first_sample = true;

	return count;
}

static const struct proc_ops enable_ops = {
	.proc_read = read_enable,
	.proc_write = write_enable,
};

static ssize_t read_exceed_count(struct file *file,
									char __user *ubuf,
									size_t count,
									loff_t *ppos)
{
	char buf[SZ_32];
	int len;
	if (*ppos > 0)
		return 0;
	len = scnprintf(buf, sizeof(buf), "%u\n", exceed_threshold);
	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;
	*ppos = len;
	return len;
}

static ssize_t write_exceed_count(struct file *file,
								const char __user *ubuf,
								size_t count,
								loff_t *ppos)
{
	char buf[SZ_32];
	unsigned long v;
	int ret;
	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	ret = kstrtoul(buf, 10, &v);
	if (ret)
		return ret;
	exceed_threshold = v;
	first_sample = true;
	pr_info("cpu_monitor: exceed_threshold set to %u\n", exceed_threshold);
	return count;
}

static const struct proc_ops count_ops = {
	.proc_read = read_exceed_count,
	.proc_write = write_exceed_count,
};

static ssize_t read_target_pid(struct file *file,
							   char __user *ubuf,
							   size_t count,
							   loff_t *ppos)
{
	char buf[SZ_32];
	int len;

	if (*ppos > 0)
		return 0;

	len = format_val(buf, sizeof(buf), target_pid);
	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;
	*ppos = len;
	return len;
}

static ssize_t write_target_pid(struct file *file,
								const char __user *ubuf,
								size_t count,
								loff_t *ppos)
{
	char buf[SZ_32];
	unsigned long val;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	target_pid = val;
	last_runtime_ns = 0;
	first_sample = true;
	pr_info("cpu_monitor: target_pid set to %lu\n", val);
	return count;
}

static const struct proc_ops pid_ops = {
	.proc_read = read_target_pid,
	.proc_write = write_target_pid,
};

static ssize_t read_period_ms(struct file *file,
							  char __user *ubuf,
							  size_t count,
							  loff_t *ppos)
{
	char buf[SZ_32];
	int len;

	if (*ppos > 0)
		return 0;

	len = format_val(buf, sizeof(buf), period_ms);
	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;
	*ppos = len;
	return len;
}

static ssize_t write_period_ms(struct file *file,
							   const char __user *ubuf,
							   size_t count,
							   loff_t *ppos)
{
	char buf[SZ_32];
	unsigned long val;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	period_ms = val;
	first_sample = true;
	pr_info("cpu_monitor: period_ms set to %lu ms\n", val);
	return count;
}

static const struct proc_ops period_ops = {
	.proc_read = read_period_ms,
	.proc_write = write_period_ms,
};

static ssize_t read_thresh_pct(struct file *file,
							   char __user *ubuf,
							   size_t count,
							   loff_t *ppos)
{
	char buf[SZ_32];
	int len;

	if (*ppos > 0)
		return 0;

	len = format_val(buf, sizeof(buf), thresh_pct);
	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;
	*ppos = len;
	return len;
}

static ssize_t write_thresh_pct(struct file *file,
								const char __user *ubuf,
								size_t count,
								loff_t *ppos)
{
	char buf[SZ_32];
	unsigned long val;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	thresh_pct = val;
	first_sample = true;
	pr_info("cpu_monitor: thresh_pct set to %lu%%\n", val);
	return count;
}

static const struct proc_ops thresh_ops = {
	.proc_read = read_thresh_pct,
	.proc_write = write_thresh_pct,
};

static u64 get_group_runtime_ns(pid_t pid)
{
	u64 total = 0;
	struct pid *ps;
	struct task_struct *leader, *t;

	ps = find_get_pid(pid);
	leader = ps ? pid_task(ps, PIDTYPE_PID) : NULL;
	if (ps)
		put_pid(ps);
	if (!leader)
		return 0;

	rcu_read_lock();
	for_each_thread(leader, t) {
		total += t->se.sum_exec_runtime;
	}
	rcu_read_unlock();
	return total;
}

static int monitor_fn(void *data)
{
	while (!kthread_should_stop()) {
		u64 total_ns, delta_ns;
		u64 period_ns = period_ms * 1000ULL * 1000ULL;
		unsigned long usage;
		pid_t pid = target_pid;
		struct pid *ps;
		struct task_struct *p;

		if (!enable_monitor) {
			first_sample = true;
			msleep_interruptible(period_ms);
			continue;
		}

		if (pid == 0) {
			first_sample = true;
			msleep_interruptible(period_ms);
			continue;
		}

		total_ns = get_group_runtime_ns(pid);
		if (first_sample) {
			last_runtime_ns = total_ns;
			first_sample = false;
			msleep_interruptible(period_ms);
			continue;
		}

		delta_ns = total_ns - last_runtime_ns;
		last_runtime_ns = total_ns;
		usage = div_u64(delta_ns * 100, period_ns);

		if (usage > thresh_pct) {
			exceed_count++;
			if (exceed_count >= exceed_threshold) {
				ps = find_get_pid(pid);
				p = ps ? pid_task(ps, PIDTYPE_PID) : NULL;
				if (ps)
					put_pid(ps);
				if (p) {
					pr_info("cpu_monitor: pid=%d usage=%lu%% target%u%% x=%u, sending SIGKILL\n",
							pid, usage, thresh_pct, exceed_count);
					send_sig(SIGKILL, p, 1);
					target_pid = 0;
					first_sample = true;
					exceed_count = 0;
					pr_info("cpu_monitor: target_pid reset after kill\n");
				}
			}
		} else {
			if (usage > 0 && usage >= (thresh_pct / 2)) {
				pr_info("cpu_monitor: pid=%d cpu usage=%lu%% (%llu/%llu ns) x%u\n",
					pid, usage, delta_ns, period_ns, exceed_count);
			}
			exceed_count = 0;
		}

		msleep_interruptible(period_ms);
	}
	return 0;
}

static int __init cpu_monitor_init(void)
{
	proc_dir = proc_mkdir(PROC_DIR_NAME, NULL);
	if (!proc_dir)
		return -ENOMEM;

	proc_pid_entry = proc_create(PROC_PID_NAME, 0660, proc_dir, &pid_ops);
	proc_enable_entry = proc_create(PROC_ENABLE_NAME, 0660, proc_dir, &enable_ops);
	proc_period_entry = proc_create(PROC_PERIOD_NAME, 0660, proc_dir, &period_ops);
	proc_thresh_entry = proc_create(PROC_THRESH_NAME, 0660, proc_dir, &thresh_ops);
	proc_count_entry  = proc_create(PROC_COUNT_NAME,  0660, proc_dir, &count_ops);

	monitor_thread = kthread_run(monitor_fn, NULL, "nt_task_km");
	if (IS_ERR(monitor_thread)) {
		pr_err("cpu_monitor: failed to start thread\n");
		monitor_thread = NULL;
	}

	pr_info("cpu_monitor: module loaded, configure under /proc/%s/\n", PROC_DIR_NAME);
	return 0;
}

static void __exit cpu_monitor_exit(void)
{
	if (monitor_thread)
		kthread_stop(monitor_thread);

	proc_remove(proc_pid_entry);
	proc_remove(proc_enable_entry);
	proc_remove(proc_period_entry);
	proc_remove(proc_thresh_entry);
	proc_remove(proc_count_entry);
	proc_remove(proc_dir);

	pr_info("cpu_monitor: module unloaded\n");
}

module_init(cpu_monitor_init);
module_exit(cpu_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Independent procfs nodes with group CPU usage monitor");
