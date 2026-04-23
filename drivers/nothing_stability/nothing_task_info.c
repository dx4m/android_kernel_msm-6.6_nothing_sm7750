#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#define PID_BUF_SZ SZ_128
static pid_t target_pid_t = 1;
static int show_all_pid_tasks(struct seq_file *m, void *p)
{
	struct task_struct *process, *thread;
	seq_printf(m, "pid, uid, tgid, name\n");
	rcu_read_lock();
	for_each_process(process) {
		if (!process) {
			continue;
		}
		if (process->pid == target_pid_t) {
			for_each_thread(process, thread) {
				if (!thread) {
					continue;
				}
				seq_printf(m, "%5d, %5d, %5d, %s\n",
					thread->pid,
					from_kuid(&init_user_ns, task_uid(thread)),
					thread->tgid,
					thread->comm
					);
			}
		}
	}
	rcu_read_unlock();
	return 0;
}
static ssize_t show_all_pid_tasks_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos) {
	char buf[PID_BUF_SZ];
	struct task_struct *process;
	pid_t pid;
	int pid_val;
	if (count > PID_BUF_SZ - 1) {
		return -EINVAL;
	}
	if (copy_from_user(buf, ubuf, count)) {
		return -EFAULT;
	}
	buf[count] = '\0';
	if (kstrtoint(buf, 10, &pid_val)) {
		return -EINVAL;
	}
	pid = pid_val;
	rcu_read_lock();
	for_each_process(process) {
		if (!process)
			continue;
		if (process->pid == pid) {
			target_pid_t = pid;
			rcu_read_unlock();
			return count;
		}
	}
	rcu_read_unlock();
	return -ESRCH;
}
static int show_all_pid_tasks_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_all_pid_tasks, NULL);
}
static const struct proc_ops show_all_pid_tasks_fops = {
	.proc_open       = show_all_pid_tasks_open,
	.proc_write      = show_all_pid_tasks_write,
	.proc_read       = seq_read,
	.proc_lseek      = seq_lseek,
	.proc_release    = single_release,
};
static int show_all_process_thread_id(struct seq_file *m, void *p)
{
	struct task_struct *process;
	seq_printf(m, "pid uid tgid name\n");
	rcu_read_lock();
	for_each_process(process) {
		if (!process) {
			continue;
		}
		seq_printf(m, "%5d %5d %5d %s\n",
			process->pid,
			from_kuid(&init_user_ns, task_uid(process)),
			process->tgid,
			process->comm
			);
	}
	rcu_read_unlock();
	return 0;
}
static int show_all_tasks_id_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_all_process_thread_id, NULL);
}
static const struct proc_ops show_all_tasks_id_fops = {
	.proc_open       = show_all_tasks_id_open,
	.proc_read       = seq_read,
	.proc_lseek      = seq_lseek,
	.proc_release    = single_release,
};
static int __init nt_taskinfo_init(void)
{
	struct proc_dir_entry *root, *show_all_tasks_id_pentry, *show_all_pid_tasks_pentry;
	root = proc_mkdir("nt_taskinfo", NULL);
	if(!root){
		pr_err("mkdir nt_taskinfo failed!\n");
		return -1;
	}
	show_all_tasks_id_pentry = proc_create("show_all_tasks_id", S_IRUGO, root, &show_all_tasks_id_fops);
	if(!show_all_tasks_id_pentry) {
		pr_err("create node show_all_tasks node failed!\n");
		return -1;
	}
	show_all_pid_tasks_pentry= proc_create("show_all_pid_tasks", S_IRUGO | S_IWUGO, root, &show_all_pid_tasks_fops);
	if(!show_all_pid_tasks_pentry) {
		pr_err("create node show_all_pid_tasks node failed!\n");
		return -1;
	}
	return 0;
}
device_initcall(nt_taskinfo_init);
MODULE_LICENSE("GPL v2");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("<BSP_CORE@nothing.tech>");
MODULE_DESCRIPTION("NOTHING task information");
MODULE_IMPORT_NS(MINIDUMP);
