
#ifndef _NOTHING_NAMED_THREAD_AFFINITY_H
#define _NOTHING_NAMED_THREAD_AFFINITY_H

int nt_named_thread_affinity_init(void);
void set_thread_affinity_on_set_name(void *, int , struct task_struct *);
void check_setaffinity_skip_by_named_thread_affinity(void *, struct task_struct *, const struct cpumask *, bool *);

#endif /* _NOTHING_NAMED_THREAD_AFFINITY_H */
