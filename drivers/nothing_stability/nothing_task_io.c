#include <linux/atomic.h>
#include <linux/fcntl.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched/debug.h>
#include <linux/sizes.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/profile.h>
#include <linux/kfifo.h>

#define NT_task_info(fmt, arg...) \
    pr_info("[nt_task] " fmt, ##arg)
#define NT_task_err(fmt, arg...) \
    pr_err("[nt_task_] " fmt, ##arg)

#define NT_MAX_RECORDS                    (3)
#define NT_MAX_DIFF_RECORD                (NT_MAX_RECORDS - 1)
#define NT_INTERVAL                       (300)
#define NT_RESCHEDULE                     (10)
#define NT_DELAY_INTERVAL                 (1)
#define NT_HASH_BITS                      (10)
#define NT_HASH_SIZE                      (1 << NT_HASH_BITS)
#define NT_TASK_IO_THRESHOLD              (10)
#define MB_to_Bytes                       (1024 * 1024)
#define NT_TIMESTAMP_MAX_LEN              (50)
#define NT_TASK_NAME_MAX_LEN              (100)
#define NT_SNAPSHOT_KFIFO_MAX_LEN         (100)
#define NT_TASK_IO_LOWER_THRESHOLD        (10)
#define SHOW_OTHER_RECORD_LOG             (0)
static int record_threshold = NT_TASK_IO_THRESHOLD;
static int debug_log_enable = 0;
static int trigger_scan = 0;

#define COPY_IO_INFO(dest, src)  \
	do { \
		(dest)->pid = (src)->pid; \
		(dest)->tgid = (src)->tgid; \
		strscpy_pad((dest)->comm, (src)->comm, TASK_COMM_LEN); \
		strscpy_pad((dest)->name, (src)->name, NT_TASK_NAME_MAX_LEN); \
		(dest)->read_bytes = (src)->read_bytes; \
		(dest)->write_bytes = (src)->write_bytes; \
		(dest)->cancelled_write_bytes = (src)->cancelled_write_bytes; \
		(dest)->comm_res = (src)->comm_res; \
		(dest)->task_exit = (src)->task_exit; \
		(dest)->task_exit_record = (src)->task_exit_record; \
	} while (0)

#define COPY_PRE_IO_INFO(dest, src)  \
	do { \
		(dest)->pre_read_bytes = (src)->read_bytes; \
		(dest)->pre_write_bytes = (src)->write_bytes; \
		(dest)->pre_cancelled_write_bytes = (src)->cancelled_write_bytes; \
	} while (0)

#define SAME_PID_AND_NO_EXIT_EVENT(entry, next_entry) \
	((entry->io_info.pid == next_entry->io_info.pid) && \
	(!(entry->io_info.task_exit)) && \
	(!(next_entry->io_info.task_exit)))

#define READ_DIFF(entry, next_entry) \
	(((entry->io_info.read_bytes) > (next_entry->io_info.read_bytes)) ? 0 : \
	(((next_entry->io_info.read_bytes) - (entry->io_info.read_bytes)) / MB_to_Bytes))

#define CANCEL_WRITE_BYTES_THRESHOLD(entry, next_entry) \
	((((entry->io_info.cancelled_write_bytes) / MB_to_Bytes) > record_threshold) || \
	(((next_entry->io_info.cancelled_write_bytes / MB_to_Bytes)) > record_threshold))

#define ENTRY_WRITE_DIFF(entry, diff) \
	do { \
		if ((entry->io_info.write_bytes) > (entry->io_info.cancelled_write_bytes)) { \
			(diff) = (entry->io_info.write_bytes) - \
				(entry->io_info.cancelled_write_bytes); \
		} else { \
			(diff) = 0; \
		} \
	} while(0)

#define WRITE_DIFF(entry, next_entry, write_diff) \
	do { \
		unsigned long entry_diff, next_entry_diff; \
		ENTRY_WRITE_DIFF(entry, entry_diff); \
		ENTRY_WRITE_DIFF(next_entry, next_entry_diff); \
		if ((next_entry_diff) > (entry_diff)) { \
			(write_diff) = (((next_entry_diff) - (entry_diff)) / MB_to_Bytes); \
		} else { \
			(write_diff) = 0; \
		} \
	} while(0)

#define RECORD_CONDITION(cancelled_write, read_diff, write_diff) \
	((cancelled_write) || \
		((read_diff) >= record_threshold) || \
		((write_diff) >= record_threshold))

struct nt_task_exit_event_st {
	void *msg_buffer;
	unsigned int fifo_size;
	atomic_t is_fifo_overflow;
	struct kfifo msg_fifo;
};
static struct nt_task_exit_event_st nt_task_exit_event;

struct nt_task_io_info {
	pid_t pid;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	char name[NT_TASK_NAME_MAX_LEN];
	unsigned long pre_read_bytes;
	unsigned long pre_write_bytes;
	unsigned long pre_cancelled_write_bytes;
	unsigned long read_bytes;
	unsigned long write_bytes;
	unsigned long cancelled_write_bytes;
	int comm_res;
	bool task_exit;
	int task_exit_record;
};

struct nt_task_snapshot {
	struct nt_task_io_info io_info;
	struct hlist_node node;
};

static DEFINE_HASHTABLE(io_records_0, NT_HASH_BITS);
static DEFINE_HASHTABLE(io_records_1, NT_HASH_BITS);
static DEFINE_HASHTABLE(io_records_2, NT_HASH_BITS);
static struct hlist_head (*io_records_ptr[NT_MAX_RECORDS])[NT_HASH_SIZE] = {
	&io_records_0,
	&io_records_1,
	&io_records_2,
};

static DEFINE_HASHTABLE(io_records_diff_0, NT_HASH_BITS);
static DEFINE_HASHTABLE(io_records_diff_1, NT_HASH_BITS);
static struct hlist_head (*io_records_diff_ptr[NT_MAX_DIFF_RECORD])[NT_HASH_SIZE] = {
	&io_records_diff_0,
	&io_records_diff_1,
};

static int record_index = 0;
static int oldest_entry_index = 0;
static int oldest_next_entry_index = 0;
static int hash_list_record_count = 0;
static bool cal_diff_start = false;
static struct workqueue_struct *wq;
static struct delayed_work io_stat_work;
static struct timespec64 ts_array[NT_MAX_RECORDS] = {0};
static char timestamp_arry[NT_MAX_DIFF_RECORD][NT_TIMESTAMP_MAX_LEN] = {0};
static int cur_record_round = 0;
static int pre_trigger_dump_round = 0;

struct proc_dir_entry *root = NULL;
struct proc_dir_entry *enable_node = NULL;
struct proc_dir_entry *schedule_period_node = NULL;
struct proc_dir_entry *record_threshold_node = NULL;
struct proc_dir_entry *debug_log_enable_node = NULL;
struct proc_dir_entry *trigger_scan_node = NULL;
static char schedule_period_proc_data[(sizeof(int) * 2 + 1)] = {'\0'};
static char record_threshold_proc_data[(sizeof(int) * 2 + 1)] = {'\0'};
static char debug_log_enable_proc_data[(sizeof(int) * 2 + 1)] = {'\0'};
static char trigger_scan_proc_data[(sizeof(int) * 2 + 1)] = {'\0'};
static int schedule_period = NT_INTERVAL;
char *record_index_list[] = {"PID", "TGID", "Comm", "Pre_Read(MB)", "Read(MB)", "Pre_Write(MB)",
	"Write(MB)", "Pre_Cancel_Write(MB)", "Cancel_Write(MB)", "TaskExit", "Command"};
static int kfifo_len = NT_SNAPSHOT_KFIFO_MAX_LEN;
static bool kfifo_len_low = false;
static bool enable_task_io = false;

#define TASK_EXIT_EVENT(entry) \
	((entry->io_info.task_exit) && \
	(entry->io_info.task_exit_record < cur_record_round))

int get_task_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	int res = 0;
	unsigned int len;
	struct mm_struct *mm = get_task_mm(task);
	unsigned long arg_start, arg_end, env_start, env_end;
	if (!mm)
		goto out;
	if (!mm->arg_end)
		goto out_mm;	/* Shh! No looking before we're done */

	spin_lock(&mm->arg_lock);
	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	env_start = mm->env_start;
	env_end = mm->env_end;
	spin_unlock(&mm->arg_lock);

	len = arg_end - arg_start;

	if (len > buflen)
		len = buflen;

	res = access_process_vm(task, arg_start, buffer, len, FOLL_FORCE);

	/*
	 * If the nul at the end of args has been overwritten, then
	 * assume application is using setproctitle(3).
	 */
	if (res > 0 && buffer[res-1] != '\0' && len < buflen) {
		len = strnlen(buffer, res);
		if (len < res) {
			res = len;
		} else {
			len = env_end - env_start;
			if (len > buflen - res)
				len = buflen - res;
			res += access_process_vm(task, env_start,
						 buffer+res, len,
						 FOLL_FORCE);
			res = strnlen(buffer, res);
		}
	}
out_mm:
	mmput(mm);
out:
	return res;
}

static void store_io_diff(int oldest_entry_index,
		int oldest_next_entry_index, int store_index)
{
	int oldest_index, oldest_next_index;
	unsigned long read_diff, write_diff;
	struct nt_task_snapshot *oldest_entry, *oldest_next_entry, *diff_entry;

	for (oldest_index = 0; oldest_index < NT_HASH_SIZE; oldest_index++) {
		hlist_for_each_entry(oldest_entry,
		&(*io_records_ptr[oldest_entry_index])[oldest_index], node){
			for (oldest_next_index = 0;
			oldest_next_index < NT_HASH_SIZE;
			oldest_next_index++) {
				hlist_for_each_entry(oldest_next_entry,
				&(*io_records_ptr[oldest_next_entry_index])[oldest_next_index],
					node) {
					if (TASK_EXIT_EVENT(oldest_entry)) {
						diff_entry =
							vzalloc(sizeof(struct nt_task_snapshot));
						if (!diff_entry) {
							NT_task_err("%s vzalloc faild %d\n",
								__func__, __LINE__);
							continue;
						}
						COPY_IO_INFO(&diff_entry->io_info,
							&oldest_entry->io_info);
						hash_add((*io_records_diff_ptr[store_index]),
							&diff_entry->node,
							diff_entry->io_info.pid);
						oldest_entry->io_info.task_exit_record =
							cur_record_round;
					} else if (TASK_EXIT_EVENT(oldest_next_entry)) {
						diff_entry =
							vzalloc(sizeof(struct nt_task_snapshot));
						if (!diff_entry) {
							NT_task_err("%s vzalloc faild %d\n",
								__func__, __LINE__);
							continue;
						}
						COPY_IO_INFO(&diff_entry->io_info,
							&oldest_next_entry->io_info);
						hash_add((*io_records_diff_ptr[store_index]),
							&diff_entry->node,
							diff_entry->io_info.pid);
						oldest_next_entry->io_info.task_exit_record =
							cur_record_round;
					} else if (SAME_PID_AND_NO_EXIT_EVENT(oldest_entry, oldest_next_entry)) {
						bool cancelled_write = false;
						read_diff = READ_DIFF(oldest_entry,
							oldest_next_entry);
						if (CANCEL_WRITE_BYTES_THRESHOLD(oldest_entry,
							oldest_next_entry))
							cancelled_write = true;
						WRITE_DIFF(oldest_entry,
							oldest_next_entry, write_diff);
						if (RECORD_CONDITION(cancelled_write,
							read_diff, write_diff)) {
							diff_entry =
								vzalloc(sizeof(struct nt_task_snapshot));
							if (!diff_entry) {
								NT_task_err(
									"%s vzalloc faild %d\n",
									__func__, __LINE__);
									continue;
							}
							COPY_IO_INFO(&diff_entry->io_info,
								&oldest_next_entry->io_info);
							COPY_PRE_IO_INFO(&diff_entry->io_info,
								&oldest_entry->io_info);
							hash_add(
								(*io_records_diff_ptr[store_index]),
								&diff_entry->node,
								diff_entry->io_info.pid);
						}
					}
				}
			}
		}
	}
}

static void cal_io_diff(void)
{
	int cal_time, bkt;
	struct nt_task_snapshot *diff_entry;
	struct hlist_node *tmp;
	struct tm oldest_record_tm, oldest_next_record_tm;

	for (cal_time = 0; cal_time < (hash_list_record_count - 1); cal_time++) {
		//step 1. find the oldest_entry_index
		if (!cal_diff_start) {
			cal_diff_start = true;
			if (hash_list_record_count == NT_MAX_RECORDS) {
				oldest_entry_index = record_index;
				oldest_next_entry_index =
					(oldest_entry_index + 1)% NT_MAX_RECORDS;
			} else {
				oldest_entry_index = record_index - 2;
				oldest_next_entry_index = record_index - 1;
			}
		} else {
			oldest_entry_index = oldest_next_entry_index;
			oldest_next_entry_index = (oldest_entry_index + 1) % NT_MAX_RECORDS;
		}

		//step 2. saving the timestamp format
		if (ts_array[oldest_entry_index].tv_sec < 0 ||
			ts_array[oldest_next_entry_index].tv_sec < 0) {
			NT_task_err("ts_array is not correct\n");
			continue;
		} else {
			time64_to_tm(ts_array[oldest_entry_index].tv_sec,
				0, &oldest_record_tm);
			time64_to_tm(ts_array[oldest_next_entry_index].tv_sec,
				0, &oldest_next_record_tm);
			snprintf(timestamp_arry[cal_time], NT_TIMESTAMP_MAX_LEN,
				"%04ld-%02d-%02d %02d:%02d:%02d to %04ld-%02d-%02d %02d:%02d:%02d",
				oldest_record_tm.tm_year + 1900, oldest_record_tm.tm_mon + 1,
				oldest_record_tm.tm_mday, oldest_record_tm.tm_hour,
				oldest_record_tm.tm_min, oldest_record_tm.tm_sec,
				oldest_next_record_tm.tm_year + 1900,
				oldest_next_record_tm.tm_mon + 1,
				oldest_next_record_tm.tm_mday, oldest_next_record_tm.tm_hour,
				oldest_next_record_tm.tm_min, oldest_next_record_tm.tm_sec
			);
		}

		//step 3. Delete hash list before use
		hash_for_each_safe((*io_records_diff_ptr[cal_time]),
								bkt, tmp, diff_entry, node) {
			hash_del(&diff_entry->node);
			vfree(diff_entry);
		}

		//step 4. Find below conditions in oldest_record and oldest_next_record list
		//        1. the common pid number in oldest_record 
		//           and oldest_next_record list without task_exit
		//        2. task_exit flag is true
		store_io_diff( oldest_entry_index, oldest_next_entry_index, cal_time);
	}
	cal_diff_start = false;
}

static void pop_nt_task_io_info_msgfifo(struct nt_task_io_info *info)
{
	unsigned int len = 0;

	len =  kfifo_out(&nt_task_exit_event.msg_fifo, info, sizeof(*info));

	if (len != sizeof(*info))
		NT_task_err("%s: len:%d, info_size:%ld unexpect\n",
			__func__, len, sizeof(*info));

	kfifo_len++;
}

static unsigned int chk_nt_task_snapshot_msgfifo_empty(void)
{
	return (kfifo_len(&nt_task_exit_event.msg_fifo)
			>= sizeof(struct nt_task_io_info));
}

static void handle_kfifo(void)
{
	struct nt_task_io_info info;
	struct nt_task_snapshot *new_record_entry;

	if (unlikely(atomic_cmpxchg(&nt_task_exit_event.is_fifo_overflow, 1, 0)))
		NT_task_err("%s :detect io snapshot msg fifo is overflow\n", __func__);

	while (chk_nt_task_snapshot_msgfifo_empty()) {
		pop_nt_task_io_info_msgfifo(&info);

		new_record_entry = vzalloc(sizeof(struct nt_task_snapshot));
		if (!new_record_entry) {
			NT_task_err("%s: vzalloc fail\n", __func__);
			continue;
		}

		new_record_entry->io_info.pid = info.pid;
		new_record_entry->io_info.tgid = info.tgid;
		strscpy_pad(new_record_entry->io_info.comm, info.comm, TASK_COMM_LEN);
		strscpy_pad(new_record_entry->io_info.name, info.name, NT_TASK_NAME_MAX_LEN);
		new_record_entry->io_info.read_bytes = info.read_bytes;
		new_record_entry->io_info.write_bytes = info.write_bytes;
		new_record_entry->io_info.task_exit = info.task_exit;
		new_record_entry->io_info.task_exit_record = info.task_exit_record;
		new_record_entry->io_info.comm_res = info.comm_res;
		new_record_entry->io_info.cancelled_write_bytes = info.cancelled_write_bytes;

		if (debug_log_enable)
			NT_task_info("%s: %d, %s, %lu, %lu, %s, %d, %d, %lu\n",
				__func__, record_index, new_record_entry->io_info.comm,
				(new_record_entry->io_info.read_bytes) / MB_to_Bytes,
				(new_record_entry->io_info.write_bytes) / MB_to_Bytes,
				new_record_entry->io_info.name,
				new_record_entry->io_info.task_exit,
				new_record_entry->io_info.comm_res,
				(new_record_entry->io_info.cancelled_write_bytes) / MB_to_Bytes);

		hash_add((*io_records_ptr[record_index]),
			&new_record_entry->node, new_record_entry->io_info.pid);
	}
}

static void collect_io_stats(struct work_struct *work)
{
	struct task_struct *task;
	struct nt_task_snapshot *entry, *new_record_entry;
	int bkt;
	struct hlist_node *tmp;
	bool cancelled_write;
	unsigned long read_bytes, write_bytes;

	hash_for_each_safe((*io_records_ptr[record_index]), bkt, tmp, entry, node) {
		hash_del(&entry->node);
		vfree(entry);
	}

	handle_kfifo();

	ktime_get_real_ts64(&ts_array[record_index]);
	for_each_process(task) {
		cancelled_write = false;

		if ((task->ioac.cancelled_write_bytes / MB_to_Bytes) > record_threshold)
			cancelled_write = true;

		read_bytes = (task->ioac.read_bytes) / MB_to_Bytes;

		if (task->ioac.write_bytes > task->ioac.cancelled_write_bytes)
			write_bytes =
				(task->ioac.write_bytes -
				task->ioac.cancelled_write_bytes) / MB_to_Bytes;
		else
			write_bytes = 0;

		//the read_bytes and write_bytes below threshold, skip record
		if (!(RECORD_CONDITION(cancelled_write, read_bytes, write_bytes)))
			continue;

		new_record_entry = vzalloc(sizeof(struct nt_task_snapshot));
		if (!new_record_entry) {
			NT_task_err("%s: vzalloc fail\n", __func__);
			continue;
		}

		new_record_entry->io_info.pid = task->pid;
		new_record_entry->io_info.tgid = task->tgid;
		strscpy_pad(new_record_entry->io_info.comm, task->comm, TASK_COMM_LEN);
		new_record_entry->io_info.read_bytes = task->ioac.read_bytes;
		new_record_entry->io_info.write_bytes = task->ioac.write_bytes;
		new_record_entry->io_info.cancelled_write_bytes = task->ioac.cancelled_write_bytes;
		new_record_entry->io_info.comm_res =
		get_task_cmdline(task, new_record_entry->io_info.name,
			NT_TASK_NAME_MAX_LEN);
		new_record_entry->io_info.task_exit = false;

		if (debug_log_enable)
			NT_task_info("%d , insert pid:%d, comm: %s, r: %lu, w: %llu, cmd: %s, w_c:%llu\n",
				record_index, new_record_entry->io_info.pid,
				new_record_entry->io_info.comm,
				(new_record_entry->io_info.read_bytes) / MB_to_Bytes,
				(task->ioac.write_bytes) / MB_to_Bytes,
				new_record_entry->io_info.name,
				(task->ioac.cancelled_write_bytes) / MB_to_Bytes);

		hash_add((*io_records_ptr[record_index]),
			&new_record_entry->node, new_record_entry->io_info.pid);
	}

	record_index = (record_index + 1) % NT_MAX_RECORDS;
	if (hash_list_record_count < NT_MAX_RECORDS)
		hash_list_record_count++;

	cur_record_round++;
	kfifo_len_low = false;
	if (trigger_scan) {
		trigger_scan = 0;
		sprintf(trigger_scan_proc_data, "%d", trigger_scan);
		NT_task_info("%s: trigger scan done\n", __func__);
	}
	if (debug_log_enable)
		NT_task_info("collect_io_stats do next %d wq\n", cur_record_round);
	queue_delayed_work(wq, &io_stat_work, (schedule_period * HZ));
}

static int show_task_io_thread(struct seq_file *m, void *p)
{
	int index, diff_index, bkt;
	struct nt_task_snapshot *entry, *read_write_diff_entry;
	struct hlist_node *tmp;
	struct tm tm;
	char timestamp[NT_TIMESTAMP_MAX_LEN] = {0};

	if (pre_trigger_dump_round != cur_record_round) {
		cal_io_diff();
		pre_trigger_dump_round = cur_record_round;
	}

	seq_printf(m,"[SHOW_TASK_IO_THREAD_START]\n");

	for (index = 0; index < sizeof(record_index_list)/sizeof(record_index_list[0]); index++)
		seq_printf(m, "%s, ", record_index_list[index]);
	seq_printf(m,"\n");
	seq_printf(m, "======================================================================\n");

	if (debug_log_enable) {
		for (index = 0; index < NT_MAX_RECORDS; index++) {
			if (ts_array[index].tv_sec > 0) {
				time64_to_tm(ts_array[index].tv_sec, 0, &tm);
				snprintf(timestamp, NT_TIMESTAMP_MAX_LEN,
					"%04ld-%02d-%02d %02d:%02d:%02d",
					tm.tm_year + 1900, tm.tm_mon + 1,
					tm.tm_mday, tm.tm_hour,
					tm.tm_min, tm.tm_sec);
				seq_printf(m, "%s\n", timestamp);
			} else
				NT_task_err("%s ts_array index %d is not correct\n", __func__, index);
			hash_for_each_safe((*io_records_ptr[index]), bkt, tmp, entry, node) {
				seq_printf(m,
					"[Record%d] %d, %d, %s, %lu, %lu, %lu, %lu, %lu, %lu",
					index, entry->io_info.pid,
					entry->io_info.tgid,
					entry->io_info.comm,
					(entry->io_info.pre_read_bytes) / MB_to_Bytes,
					(entry->io_info.read_bytes) / MB_to_Bytes,
					(entry->io_info.pre_write_bytes) / MB_to_Bytes,
					(entry->io_info.write_bytes) / MB_to_Bytes,
					(entry->io_info.pre_cancelled_write_bytes) / MB_to_Bytes,
					(entry->io_info.cancelled_write_bytes) / MB_to_Bytes);
				if (entry->io_info.comm_res == 0)
					seq_printf(m, "\n");
				else
					seq_printf(m, ", %s\n", entry->io_info.name);
			}
		}
		seq_printf(m, "======================================================================\n");
	}

	for (diff_index = 0; diff_index < NT_MAX_DIFF_RECORD; diff_index++) {
		seq_printf(m, "[%s]\n", timestamp_arry[diff_index]);
		hash_for_each_safe((*io_records_diff_ptr[diff_index]),
			bkt, tmp, read_write_diff_entry, node) {
			seq_printf(m,
				"%d, %d, %s, %lu, %lu, %lu, %lu, %lu, %lu, %d",
				read_write_diff_entry->io_info.pid,
				read_write_diff_entry->io_info.tgid,
				read_write_diff_entry->io_info.comm,
				(read_write_diff_entry->io_info.pre_read_bytes) / MB_to_Bytes,
				(read_write_diff_entry->io_info.read_bytes) / MB_to_Bytes,
				(read_write_diff_entry->io_info.pre_write_bytes) / MB_to_Bytes,
				(read_write_diff_entry->io_info.write_bytes) / MB_to_Bytes,
				(read_write_diff_entry->io_info.pre_cancelled_write_bytes) / MB_to_Bytes,
				(read_write_diff_entry->io_info.cancelled_write_bytes) / MB_to_Bytes,
				read_write_diff_entry->io_info.task_exit);
			if (read_write_diff_entry->io_info.comm_res == 0)
				seq_printf(m, "\n");
			else
				seq_printf(m, ", %s\n", read_write_diff_entry->io_info.name);
		}
		seq_printf(m,
			"======================================================================\n");
	}

	seq_printf(m,"[SHOW_TASK_IO_THREAD_END]\n");
	return 0;
}

static void reset_task_io(bool enable)
{
	enable_task_io = enable;
	NT_task_info("%s:enable task io:%d\n", __func__, enable_task_io);
	if (enable) {
		queue_delayed_work(wq, &io_stat_work, (schedule_period * HZ));
	} else {
		cancel_delayed_work(&io_stat_work);
		kfifo_reset(&nt_task_exit_event.msg_fifo);
		memset(ts_array, 0, sizeof(ts_array));
		memset(timestamp_arry, 0, sizeof(timestamp_arry));
		cur_record_round = 0;
		pre_trigger_dump_round = 0;
		hash_list_record_count = 0;
		oldest_next_entry_index = 0;
		oldest_entry_index = 0;
		record_index = 0;
	}
}

static int show_task_io_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_task_io_thread, NULL);
}

static ssize_t show_task_io_write(struct file *file,
	const char __user *user_buffer, size_t count, loff_t *ppos) {
	char buffer[SZ_128] = {0};
	int new_enable_task_io = 0;
	ssize_t ret;

	ret = simple_write_to_buffer(buffer, sizeof(buffer), ppos, user_buffer, count);
	if (ret < 0) {
		NT_task_err("%s: show_task_io_write fail, ret:%zd\n",
									__func__, ret);
		return ret;
	}
	buffer[ret] = '\0';
	if (sscanf(buffer, "%d", &new_enable_task_io) == 1) {
		if ((new_enable_task_io == 1) || (new_enable_task_io == 0))
			reset_task_io(new_enable_task_io);
	} else{
		NT_task_err("%s: parsing error\n", __func__);
	}

	return ret;
}

static const struct proc_ops show_task_io_fops = {
	.proc_open      = show_task_io_open,
	.proc_read      = seq_read,
	.proc_write     = show_task_io_write,
	.proc_lseek     = seq_lseek,
	.proc_release   = single_release,
};

static ssize_t schedule_period_read(struct file *file,
	char __user *user_buffer, size_t count, loff_t *ppos) {
	return simple_read_from_buffer(user_buffer, count,
		ppos, schedule_period_proc_data, strlen(schedule_period_proc_data));
}

static ssize_t schedule_period_write(struct file *file,
	const char __user *user_buffer, size_t count, loff_t *ppos) {
	char buffer[SZ_128] = {0};
	int new_schedule_period = 0;
	ssize_t ret;

	ret = simple_write_to_buffer(buffer, sizeof(buffer), ppos, user_buffer, count);
	if (ret < 0) {
		NT_task_err("%s: simple_write_to_buffer fail, ret:%zd\n",
									__func__, ret);
		return ret;
	}
	buffer[ret] = '\0';
	if (sscanf(buffer, "%d", &new_schedule_period) == 1) {
		if (new_schedule_period >= NT_TASK_IO_LOWER_THRESHOLD) {
			NT_task_info("%s: schedule_period change from %d s to %d s\n",
				__func__, schedule_period, new_schedule_period);
			schedule_period = new_schedule_period;
			sprintf(schedule_period_proc_data, "%d", schedule_period);
			if (enable_task_io) {
				cancel_delayed_work(&io_stat_work);
				queue_delayed_work(wq, &io_stat_work, (schedule_period * HZ));
			}
		} else {
			NT_task_err("%s: new_schedule_period :%d invalid\n",
								__func__, new_schedule_period);
		}
	} else{
		NT_task_err("%s: parsing error\n", __func__);
	}

	return ret;
}

static const struct proc_ops schedule_period_fops = {
	.proc_read = schedule_period_read,
	.proc_write = schedule_period_write,
};

static ssize_t record_threshold_read(struct file *file,
	char __user *user_buffer, size_t count, loff_t *ppos) {
	return simple_read_from_buffer(user_buffer, count,
		ppos, record_threshold_proc_data, strlen(record_threshold_proc_data));
}

static ssize_t record_threshold_write(struct file *file,
	const char __user *user_buffer, size_t count, loff_t *ppos) {
	char buffer[SZ_128] = {0};
	int new_record_threshold = 0;
	ssize_t ret;

	ret = simple_write_to_buffer(buffer, sizeof(buffer), ppos, user_buffer, count);
	if (ret < 0) {
		NT_task_err("%s: simple_write_to_buffer fail, ret:%zd\n",
			__func__, ret);
		return ret;
	}
	buffer[ret] = '\0';
	if (sscanf(buffer, "%d", &new_record_threshold) == 1) {
		if (new_record_threshold > 0) {
			NT_task_info("%s: new_record_threshold change from %d MB to %d MB\n",
				__func__, record_threshold, new_record_threshold);
			record_threshold = new_record_threshold;
			sprintf(record_threshold_proc_data, "%d", record_threshold);
		} else {
			NT_task_err("%s: new_record_threshold :%d invalid\n",
				__func__, new_record_threshold);
		}
	} else{
		NT_task_err("%s: parsing error\n", __func__);
	}
	return ret;
}

static const struct proc_ops record_threshold_fops = {
	.proc_read = record_threshold_read,
	.proc_write = record_threshold_write,
};

static ssize_t debug_log_enable_write(struct file *file,
	const char __user *user_buffer, size_t count, loff_t *ppos) {
	char buffer[SZ_128] = {0};
	int new_debug_log_enable = 0;
	ssize_t ret;

	ret = simple_write_to_buffer(buffer, sizeof(buffer), ppos, user_buffer, count);
	if (ret < 0) {
		NT_task_err("%s: simple_write_to_buffer fail, ret:%zd\n",
			__func__, ret);
		return ret;
	}
	buffer[ret] = '\0';
	if (sscanf(buffer, "%d", &new_debug_log_enable) == 1) {
		if (new_debug_log_enable == 1) {
			NT_task_info("%s: enable debug log\n",
				__func__);
			debug_log_enable = new_debug_log_enable;
			sprintf(debug_log_enable_proc_data, "%d", new_debug_log_enable);
		} else if (new_debug_log_enable == 0){
			NT_task_info("%s: disalbe debug log\n",
				__func__);
			debug_log_enable = new_debug_log_enable;
			sprintf(debug_log_enable_proc_data, "%d", new_debug_log_enable);
		}else {
			NT_task_err("%s: debug_log_enable :%d invalid\n",
				__func__, new_debug_log_enable);
		}
	} else{
		NT_task_err("%s: parsing error\n", __func__);
	}
	return ret;
}

static ssize_t debug_log_enable_read(struct file *file,
	char __user *user_buffer, size_t count, loff_t *ppos) {
	return simple_read_from_buffer(user_buffer, count,
		ppos, debug_log_enable_proc_data, strlen(debug_log_enable_proc_data));
}

static const struct proc_ops debug_log_enable_fops = {
	.proc_read = debug_log_enable_read,
	.proc_write = debug_log_enable_write,
};

static ssize_t trigger_scan_write(struct file *file,
	const char __user *user_buffer, size_t count, loff_t *ppos) {
	char buffer[SZ_128] = {0};
	int new_trigger_scan = 0;
	ssize_t ret;

	ret = simple_write_to_buffer(buffer, sizeof(buffer), ppos, user_buffer, count);
	if (ret < 0) {
		NT_task_err("%s: simple_write_to_buffer fail, ret:%zd\n",
			__func__, ret);
		return ret;
	}
	buffer[ret] = '\0';
	if (sscanf(buffer, "%d", &new_trigger_scan) == 1) {
		if ((new_trigger_scan == 1) && (trigger_scan == 0)) {
			trigger_scan = new_trigger_scan;
			sprintf(trigger_scan_proc_data, "%d", new_trigger_scan);
			cancel_delayed_work(&io_stat_work);
			queue_delayed_work(wq, &io_stat_work, (0 * HZ));
			NT_task_info("%s: trigger scan worker immediately\n", __func__);
		} else if (trigger_scan == 1) {
			NT_task_info("%s: trigger scaning\n", __func__);
		} else {
			NT_task_err("%s: trigger_scan :%d invalid\n",
				__func__, new_trigger_scan);
		}
	} else {
		NT_task_err("%s: parsing error\n", __func__);
	}
	return ret;
}

static ssize_t trigger_scan_read(struct file *file,
	char __user *user_buffer, size_t count, loff_t *ppos) {
	return simple_read_from_buffer(user_buffer, count,
		ppos, trigger_scan_proc_data, strlen(trigger_scan_proc_data));
}

static const struct proc_ops trigger_scan_fops = {
	.proc_read = trigger_scan_read,
	.proc_write = trigger_scan_write,
};

static void record_nt_task_io_info(struct task_struct *task,
			struct nt_task_io_info *info){
	info->pid = task->pid;
	info->tgid = task->tgid;
	strscpy_pad(info->comm, task->comm, TASK_COMM_LEN);
	info->comm_res= get_task_cmdline(task, info->name, NT_TASK_NAME_MAX_LEN);
	info->read_bytes = task->ioac.read_bytes;
	info->write_bytes = task->ioac.write_bytes;
	info->cancelled_write_bytes = task->ioac.cancelled_write_bytes;
	info->task_exit = true;
	info->task_exit_record = cur_record_round;
	if (debug_log_enable)
		NT_task_info(
			"%s: pid:%d, tgid:%d, comm:%s, cmd:%s, r: %lu, w: %lu, w_c:%lu, task_exit_record:%d\n",
			__func__, info->pid, info->tgid,
			info->comm, info->name,
			(info->read_bytes) / MB_to_Bytes,
			(info->write_bytes) / MB_to_Bytes,
			(info->cancelled_write_bytes) / MB_to_Bytes,
			info->task_exit_record);
}

static int push_nt_task_io_info_msgfifo(struct nt_task_io_info *info) {
	int len = 0;

	if (unlikely(kfifo_avail(&nt_task_exit_event.msg_fifo) < sizeof(*info))) {
		atomic_set(&nt_task_exit_event.is_fifo_overflow, 1);
		NT_task_err("%s: fifo over flow\n", __func__);
		return -1;
	}

	len =  kfifo_in(&nt_task_exit_event.msg_fifo, info, sizeof(*info));
	if (len != sizeof(*info)) {
		NT_task_err("%s: len:%d, info_size:%lu unexpect\n", __func__, len, sizeof(*info));
		return -1;
	}

	kfifo_len--;

	return 0;
}

static int nt_process_exit_notifier(struct notifier_block *self,
			unsigned long cmd, void *v){
	struct task_struct *task = v;
	unsigned long read_bytes, write_bytes;
	struct nt_task_io_info info = {0};
	bool cancelled_write = false;

	/*
	** record condition as below:
	** read_bytes or write_bytes or cancelled_write_bytes > record_threshold
	*/

	if (!task || !enable_task_io)
		return NOTIFY_OK;

	if ((task->ioac.cancelled_write_bytes / MB_to_Bytes) > record_threshold)
		cancelled_write = true;

	if (task->ioac.write_bytes > task->ioac.cancelled_write_bytes)
		write_bytes =
			(task->ioac.write_bytes - task->ioac.cancelled_write_bytes) / MB_to_Bytes;
	else
		write_bytes = 0;

	read_bytes = (task->ioac.read_bytes) / MB_to_Bytes;

	if (!(RECORD_CONDITION(cancelled_write, read_bytes, write_bytes)))
		return NOTIFY_OK;

	record_nt_task_io_info(task, &info);
	if (push_nt_task_io_info_msgfifo(&info) == 0) {
		if (debug_log_enable)
			NT_task_info("%s: push to fifo success\n", __func__);
	} else
		NT_task_err("%s: push to fifo fail\n", __func__);

	if (kfifo_len < (NT_SNAPSHOT_KFIFO_MAX_LEN / 2)  && (!kfifo_len_low)) {
		kfifo_len_low = true;
		cancel_delayed_work(&io_stat_work);
		queue_delayed_work(wq, &io_stat_work,
			(NT_RESCHEDULE * HZ));
		NT_task_info("%s: kfifo len %d is below %d\n",
			__func__, kfifo_len,
			NT_SNAPSHOT_KFIFO_MAX_LEN / 2);
	}
	return NOTIFY_OK;
}

static struct notifier_block process_notifier_block = {
	.notifier_call	= nt_process_exit_notifier,
};

static void nt_task_exit_event_init(void)
{
	int ret = 0;

	/* init/setup fifo size for below dynamic mem alloc using */
	nt_task_exit_event.fifo_size = roundup_pow_of_two(
		NT_SNAPSHOT_KFIFO_MAX_LEN * sizeof(struct nt_task_io_info));
	if (likely(nt_task_exit_event.msg_buffer == NULL)) {
		nt_task_exit_event.msg_buffer = kzalloc(nt_task_exit_event.fifo_size, GFP_ATOMIC);
		if (!nt_task_exit_event.msg_buffer) {
			NT_task_err("kzalloc memory faill %d", __LINE__);
			nt_task_exit_event.msg_buffer = vmalloc(nt_task_exit_event.fifo_size);
		}
		if (unlikely(nt_task_exit_event.msg_buffer == NULL))
			NT_task_err(
				"ERROR: irq msg_buffer:%p allocate memory failed, fifo_size:%u\n",
				nt_task_exit_event.msg_buffer,
				nt_task_exit_event.fifo_size
			);
		else {
			ret = kfifo_init(&nt_task_exit_event.msg_fifo,
				nt_task_exit_event.msg_buffer,
				nt_task_exit_event.fifo_size);
			if (unlikely(ret != 0)) {
				NT_task_err(
					"%s init failed,ret:%d,msg_buffer:%p,fifo_size:(%u/%lu)\n",
					__func__, ret, nt_task_exit_event.msg_buffer,
					nt_task_exit_event.fifo_size,
					sizeof(struct nt_task_io_info));
				return;
			}
			atomic_set(&nt_task_exit_event.is_fifo_overflow, 0);
			NT_task_info(
				"%s init done,ret:%d,msg_buffer:%p,fifo_size:%u(%lu)\n",
				__func__, ret, nt_task_exit_event.msg_buffer,
				nt_task_exit_event.fifo_size,
				sizeof(struct nt_task_io_info));
		}
	}
}

static void nt_task_exit_event_uninit(void)
{
	kfifo_free(&nt_task_exit_event.msg_fifo);

	if (likely(nt_task_exit_event.msg_buffer != NULL)) {
		kfree(nt_task_exit_event.msg_buffer);
		nt_task_exit_event.msg_buffer = NULL;
		NT_task_info("%s msg_buffer:%p is freed\n",
			__func__, nt_task_exit_event.msg_buffer);
	}
}

static int create_file_node(void)
{
	root = proc_mkdir("nt_task_stats", NULL);
	if (!root){
		NT_task_err("mkdir nt_task_stats node failed!\n");
		return 1;
	}

	enable_node = proc_create("task_io_stats", S_IRUGO, root, &show_task_io_fops);
	if (!enable_node) {
		NT_task_err("create task_io_stats node failed!\n");
		proc_remove(root);
		return 1;
	}

	schedule_period_node = proc_create("task_schedule_period",
		S_IRUGO, root, &schedule_period_fops);
	if (!schedule_period_node) {
		NT_task_err("create task_schedule_period node failed!\n");
		return 1;
	}

	record_threshold_node = proc_create("task_record_threshold",
		S_IRUGO, root, &record_threshold_fops);
	if (!record_threshold_node) {
		NT_task_err("create task_record_threshold node failed!\n");
		return 1;
	}

	debug_log_enable_node = proc_create("debug_log_enable",
		S_IRUGO, root, &debug_log_enable_fops);
	if (!debug_log_enable_node) {
		NT_task_err("create debug_log_enable node failed!\n");
		return 1;
	}

	trigger_scan_node = proc_create("trigger_scan",
		S_IRUGO, root, &trigger_scan_fops);
	if (!trigger_scan_node) {
		NT_task_err("create trigger_scan_node node failed!\n");
		return 1;
	}

	sprintf(schedule_period_proc_data, "%d", schedule_period);
	sprintf(record_threshold_proc_data, "%d", record_threshold);
	sprintf(debug_log_enable_proc_data, "%d", debug_log_enable);
	sprintf(trigger_scan_proc_data, "%d", trigger_scan);

	return 0;
}

static int __init nt_task_io_init(void)
{
	int i;

	wq = alloc_workqueue("nt_task_io_wq", WQ_UNBOUND , 0);
	if (!wq)
		return -ENOMEM;

	for (i = 0; i < NT_MAX_RECORDS; i++)
		hash_init((*io_records_ptr[i]));

	for (i = 0; i < NT_MAX_DIFF_RECORD; i++)
		hash_init((*io_records_diff_ptr[i]));

	nt_task_exit_event_init();

	INIT_DELAYED_WORK(&io_stat_work, collect_io_stats);
	profile_event_register(PROFILE_TASK_EXIT, &process_notifier_block);
	cal_diff_start = false;
	create_file_node();

	NT_task_info("IO Monitor Module Loaded\n");

	return 0;
}

static void __exit nt_task_io_exit(void)
{
	int i;
	struct nt_task_snapshot *entry;
	struct hlist_node *tmp;
	int bkt;

	if (trigger_scan_node)
		proc_remove(trigger_scan_node);

	if (debug_log_enable_node)
		proc_remove(debug_log_enable_node);

	if (schedule_period_node)
		proc_remove(schedule_period_node);

	if (enable_node)
		proc_remove(enable_node);

	if (root)
		proc_remove(root);

	cancel_delayed_work_sync(&io_stat_work);
	destroy_workqueue(wq);

	nt_task_exit_event_uninit();

	for (i = 0; i < NT_MAX_RECORDS; i++) {
		hash_for_each_safe((*io_records_ptr[i]), bkt, tmp, entry, node) {
			hash_del(&entry->node);
			vfree(entry);
		}
	}

	for (i = 0; i < NT_MAX_RECORDS; i++) {
		hash_for_each_safe((*io_records_diff_ptr[i]), bkt, tmp, entry, node) {
		hash_del(&entry->node);
		vfree(entry);
	}
}

	cal_diff_start = false;

	NT_task_info("IO Monitor Module Unloaded\n");
}

module_init(nt_task_io_init);
module_exit(nt_task_io_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("<BSP_CORE@nothing.tech>");
MODULE_DESCRIPTION("NOTHING task IO");
