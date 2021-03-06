/*
 *  kernel/sched.c
 *
 *  Kernel scheduler and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *
 *  1996-12-23  Modified by Dave Grothe to fix bugs in semaphores and
 *		make semaphores SMP safe
 *  1998-11-19	Implemented schedule_timeout() and related stuff
 *		by Andrea Arcangeli
 *  2002-01-04	New ultra-scalable O(1) scheduler by Ingo Molnar:
 *		hybrid priority-list and round-robin design with
 *		an array-switch method of distributing timeslices
 *		and per-CPU runqueues.  Cleanups and useful suggestions
 *		by Davide Libenzi, preemptible kernel bits by Robert Love.
 *  2003-09-03	Interactivity tuning by Con Kolivas.
 *  2004-04-02	Scheduler domains code by Nick Piggin
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <linux/highmem.h>
#include <linux/smp_lock.h>
#include <asm/mmu_context.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/kernel_stat.h>
#include <linux/security.h>
#include <linux/notifier.h>
#include <linux/profile.h>
#include <linux/suspend.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/timer.h>
#include <linux/rcupdate.h>
#include <linux/cpu.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/times.h>
#include <asm/tlb.h>

#include <asm/unistd.h>

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static priority [ MAX_RT_PRIO..MAX_PRIO-1 ],
 * and back.
 */
#define NICE_TO_PRIO(nice)	(MAX_RT_PRIO + (nice) + 20)
#define PRIO_TO_NICE(prio)	((prio) - MAX_RT_PRIO - 20)
#define TASK_NICE(p)		PRIO_TO_NICE((p)->static_prio)

/*
 * 'User priority' is the nice value converted to something we
 * can work with better when scaling various scheduler parameters,
 * it's a [ 0 ... 39 ] range.
 */
#define USER_PRIO(p)		((p)-MAX_RT_PRIO)
#define TASK_USER_PRIO(p)	USER_PRIO((p)->static_prio)
#define MAX_USER_PRIO		(USER_PRIO(MAX_PRIO))

/*
 * Some helpers for converting nanosecond timing to jiffy resolution
 */
#define NS_TO_JIFFIES(TIME)	((TIME) / (1000000000 / HZ))
#define JIFFIES_TO_NS(TIME)	((TIME) * (1000000000 / HZ))

/*
 * These are the 'tuning knobs' of the scheduler:
 *
 * Minimum timeslice is 5 msecs (or 1 jiffy, whichever is larger),
 * default timeslice is 100 msecs, maximum timeslice is 800 msecs.
 * Timeslices get refilled after they expire.
 */
#define MIN_TIMESLICE		max(5 * HZ / 1000, 1)
#define DEF_TIMESLICE		(100 * HZ / 1000)
#define ON_RUNQUEUE_WEIGHT	 30
#define CHILD_PENALTY		 95
#define PARENT_PENALTY		100
#define EXIT_WEIGHT		  3
#define PRIO_BONUS_RATIO	 25
#define MAX_BONUS		(MAX_USER_PRIO * PRIO_BONUS_RATIO / 100)
#define INTERACTIVE_DELTA	  2
#define MAX_SLEEP_AVG		(DEF_TIMESLICE * MAX_BONUS)
#define STARVATION_LIMIT	(MAX_SLEEP_AVG)
#define NS_MAX_SLEEP_AVG	(JIFFIES_TO_NS(MAX_SLEEP_AVG))

/*
 * If a task is 'interactive' then we reinsert it in the active
 * array after it has expired its current timeslice. (it will not
 * continue to run immediately, it will still roundrobin with
 * other interactive tasks.)
 *
 * This part scales the interactivity limit depending on niceness.
 *
 * We scale it linearly, offset by the INTERACTIVE_DELTA delta.
 * Here are a few examples of different nice levels:
 *
 *  TASK_INTERACTIVE(-20): [1,1,1,1,1,1,1,1,1,0,0]
 *  TASK_INTERACTIVE(-10): [1,1,1,1,1,1,1,0,0,0,0]
 *  TASK_INTERACTIVE(  0): [1,1,1,1,0,0,0,0,0,0,0]
 *  TASK_INTERACTIVE( 10): [1,1,0,0,0,0,0,0,0,0,0]
 *  TASK_INTERACTIVE( 19): [0,0,0,0,0,0,0,0,0,0,0]
 *
 * (the X axis represents the possible -5 ... 0 ... +5 dynamic
 *  priority range a task can explore, a value of '1' means the
 *  task is rated interactive.)
 *
 * Ie. nice +19 tasks can never get 'interactive' enough to be
 * reinserted into the active array. And only heavily CPU-hog nice -20
 * tasks will be expired. Default nice 0 tasks are somewhere between,
 * it takes some effort for them to get interactive, but it's not
 * too hard.
 */

/**
 * 本宏计算进程原来的平均睡眠时间的bonus值。
 */
#define CURRENT_BONUS(p) \
	(NS_TO_JIFFIES((p)->sleep_avg) * MAX_BONUS / \
		MAX_SLEEP_AVG)

#define GRANULARITY	(10 * HZ / 1000 ? : 1)

#ifdef CONFIG_SMP
#define TIMESLICE_GRANULARITY(p)	(GRANULARITY * \
		(1 << (((MAX_BONUS - CURRENT_BONUS(p)) ? : 1) - 1)) * \
			num_online_cpus())
#else
#define TIMESLICE_GRANULARITY(p)	(GRANULARITY * \
		(1 << (((MAX_BONUS - CURRENT_BONUS(p)) ? : 1) - 1)))
#endif

#define SCALE(v1,v1_max,v2_max) \
	(v1) * (v2_max) / (v1_max)

#define DELTA(p) \
	(SCALE(TASK_NICE(p), 40, MAX_BONUS) + INTERACTIVE_DELTA)

#define TASK_INTERACTIVE(p) \
	((p)->prio <= (p)->static_prio - DELTA(p))

#define INTERACTIVE_SLEEP(p) \
	(JIFFIES_TO_NS(MAX_SLEEP_AVG * \
		(MAX_BONUS / 2 + DELTA((p)) + 1) / MAX_BONUS - 1))

#define TASK_PREEMPTS_CURR(p, rq) \
	((p)->prio < (rq)->curr->prio)

/*
 * task_timeslice() scales user-nice values [ -20 ... 0 ... 19 ]
 * to time slice values: [800ms ... 100ms ... 5ms]
 *
 * The higher a thread's priority, the bigger timeslices
 * it gets during one round of execution. But even the lowest
 * priority thread gets MIN_TIMESLICE worth of execution time.
 */

#define SCALE_PRIO(x, prio) \
	max(x * (MAX_PRIO - prio) / (MAX_USER_PRIO/2), MIN_TIMESLICE)

static unsigned int task_timeslice(task_t *p)
{
	if (p->static_prio < NICE_TO_PRIO(0))
		return SCALE_PRIO(DEF_TIMESLICE*4, p->static_prio);
	else
		return SCALE_PRIO(DEF_TIMESLICE, p->static_prio);
}
#define task_hot(p, now, sd) ((long long) ((now) - (p)->last_ran)	\
				< (long long) (sd)->cache_hot_time)

/*
 * These are the runqueue data structures:
 */

#define BITMAP_SIZE ((((MAX_PRIO+1+7)/8)+sizeof(long)-1)/sizeof(long))

typedef struct runqueue runqueue_t;

/**
 * 进程优先级数组。每个CPU对应一个此结构。
 */
struct prio_array {
	/**
	 * 链表中进程描述符的数量。
	 */
	unsigned int nr_active;
	/**
	 * 优先权数组。当且仅当某个优先权的进程链表不为空时设置相应的位标志。
	 */
	unsigned long bitmap[BITMAP_SIZE];
	/**
	 * 140个优先权队列的头结点。
	 */
	struct list_head queue[MAX_PRIO];
};

/*
 * This is the main, per-CPU runqueue data structure.
 *
 * Locking rule: those places that want to lock multiple runqueues
 * (such as the load balancing or the thread migration code), lock
 * acquire operations must be ordered by ascending &runqueue.
 */
/**
 * CPU任务运行队列:每个CPU核都有一个，per-cpu变量
 */
struct runqueue {
	/**
	 * 保护进程链表的自旋锁
	 */ 
	spinlock_t lock;

	/*
	 * nr_running and cpu_load should be in the same cacheline because
	 * remote CPUs use both these fields when doing load calculation.
	 */
	/**
	 * 进程链表中可运行进程的数量。
	 */
	unsigned long nr_running;
#ifdef CONFIG_SMP
	/**
	 * 基于运行队列中进程的平均数量的CPU负载因子。
	 */
	unsigned long cpu_load;
#endif
	/**
	 * CPU执行进程切换的次数。
	 */
	unsigned long long nr_switches;

	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	/**
	 * 先前在运行队列链表中而现在 睡眠在TASK_UNINTERRUPTIBLE状态的进程的数量。
	 */
	unsigned long nr_uninterruptible;

	/**
	 * 过期队列中最老的进程被插入队列的时间。
	 */
	unsigned long expired_timestamp;
	/**
	 * 最近一次定时器中断的时间戳的值。
	 */
	unsigned long long timestamp_last_tick;
	/**
	 * curr-当前正在运行进程的进程描述符指针(对于本地CPU来说，它与current相同)。
	 * idle-当前CPU的swapper进程的进程描述符指针。
	 */
	task_t *curr, *idle;
	/**
	 * 在进程切换期间用来存放被替换进程的内存描述符的地址。
	 */
	struct mm_struct *prev_mm;
	/**
	 * active-指向活动进程的链表数组
	 * expired-指向过期进程的链表数组
	 * arrays-活动进程和过期进程的两个集合
	 */
	prio_array_t *active, *expired, arrays[2];
	/**
	 * 过期进程中静态优先级最高的进程(权值最小)
	 */
	int best_expired_prio;
	/**
	 * 先前在运行队列的链表中，而现在正等待IO操作结束的进程的数量。
	 */
	atomic_t nr_iowait;

#ifdef CONFIG_SMP
	/**
	 * 当前CPU的基本调度域
	 */
	struct sched_domain *sd;

	/* For active balancing */
	/**
	 * 如果要把一些进程从本地运行队列迁移到另外的运行队列，就设置这个标志。
	 */
	int active_balance;
	/**
	 * 未使用.
	 * xie.baoyou注:从代码上看，应该是指需要将本运行队列中，向哪个CPU迁移任务。
	 */
	int push_cpu;

	/**
	 * 迁移内核线程的进程描述符指针
	 */
	task_t *migration_thread;
	/**
	 * 从运行队列中被删除的进程的链表。
	 */
	struct list_head migration_queue;
#endif

#ifdef CONFIG_SCHEDSTATS
	/* latency stats */
	struct sched_info rq_sched_info;

	/* sys_sched_yield() stats */
	unsigned long yld_exp_empty;
	unsigned long yld_act_empty;
	unsigned long yld_both_empty;
	unsigned long yld_cnt;

	/* schedule() stats */
	unsigned long sched_noswitch;
	unsigned long sched_switch;
	unsigned long sched_cnt;
	unsigned long sched_goidle;

	/* pull_task() stats */
	unsigned long pt_gained[MAX_IDLE_TYPES];
	unsigned long pt_lost[MAX_IDLE_TYPES];

	/* active_load_balance() stats */
	unsigned long alb_cnt;
	unsigned long alb_lost;
	unsigned long alb_gained;
	unsigned long alb_failed;

	/* try_to_wake_up() stats */
	unsigned long ttwu_cnt;
	unsigned long ttwu_attempts;
	unsigned long ttwu_moved;

	/* wake_up_new_task() stats */
	unsigned long wunt_cnt;
	unsigned long wunt_moved;

	/* sched_migrate_task() stats */
	unsigned long smt_cnt;

	/* sched_balance_exec() stats */
	unsigned long sbe_cnt;
#endif
};

/**
 * 所有CPU的运行队列。
 */
static DEFINE_PER_CPU(struct runqueue, runqueues);

#define for_each_domain(cpu, domain) \
	for (domain = cpu_rq(cpu)->sd; domain; domain = domain->parent)

/**
 * 该宏产生索引为n的CPU的运行队列的地址。
 */
#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
/**
 * 该宏产生本地CPU的运行队列的地址。
 */
#define this_rq()		(&__get_cpu_var(runqueues))
/**
 * 该宏产生任务所在的运行队列的地址。
 */
#define task_rq(p)		cpu_rq(task_cpu(p))
/**
 * 该宏产生索引为n的CPU的运行队列的当前任务。
 */
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)

/*
 * Default context-switch locking:
 */
#ifndef prepare_arch_switch
# define prepare_arch_switch(rq, next)	do { } while (0)
# define finish_arch_switch(rq, next)	spin_unlock_irq(&(rq)->lock)
# define task_running(rq, p)		((rq)->curr == (p))
#endif

/*
 * task_rq_lock - lock the runqueue a given task resides on and disable
 * interrupts.  Note the ordering: we can safely lookup the task_rq without
 * explicitly disabling preemption.
 */
static runqueue_t *task_rq_lock(task_t *p, unsigned long *flags)
	__acquires(rq->lock)
{
	struct runqueue *rq;

repeat_lock_task:
	local_irq_save(*flags);
	rq = task_rq(p);
	spin_lock(&rq->lock);
	if (unlikely(rq != task_rq(p))) {
		spin_unlock_irqrestore(&rq->lock, *flags);
		goto repeat_lock_task;
	}
	return rq;
}

static inline void task_rq_unlock(runqueue_t *rq, unsigned long *flags)
	__releases(rq->lock)
{
	spin_unlock_irqrestore(&rq->lock, *flags);
}

#ifdef CONFIG_SCHEDSTATS
/*
 * bump this up when changing the output format or the meaning of an existing
 * format, so that tools can adapt (or abort)
 */
#define SCHEDSTAT_VERSION 10

static int show_schedstat(struct seq_file *seq, void *v)
{
	int cpu;
	enum idle_type itype;

	seq_printf(seq, "version %d\n", SCHEDSTAT_VERSION);
	seq_printf(seq, "timestamp %lu\n", jiffies);
	for_each_online_cpu(cpu) {
		runqueue_t *rq = cpu_rq(cpu);
#ifdef CONFIG_SMP
		struct sched_domain *sd;
		int dcnt = 0;
#endif

		/* runqueue-specific stats */
		seq_printf(seq,
		    "cpu%d %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu "
		    "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
		    cpu, rq->yld_both_empty,
		    rq->yld_act_empty, rq->yld_exp_empty,
		    rq->yld_cnt, rq->sched_noswitch,
		    rq->sched_switch, rq->sched_cnt, rq->sched_goidle,
		    rq->alb_cnt, rq->alb_gained, rq->alb_lost,
		    rq->alb_failed,
		    rq->ttwu_cnt, rq->ttwu_moved, rq->ttwu_attempts,
		    rq->wunt_cnt, rq->wunt_moved,
		    rq->smt_cnt, rq->sbe_cnt, rq->rq_sched_info.cpu_time,
		    rq->rq_sched_info.run_delay, rq->rq_sched_info.pcnt);

		for (itype = SCHED_IDLE; itype < MAX_IDLE_TYPES; itype++)
			seq_printf(seq, " %lu %lu", rq->pt_gained[itype],
						    rq->pt_lost[itype]);
		seq_printf(seq, "\n");

#ifdef CONFIG_SMP
		/* domain-specific stats */
		for_each_domain(cpu, sd) {
			char mask_str[NR_CPUS];

			cpumask_scnprintf(mask_str, NR_CPUS, sd->span);
			seq_printf(seq, "domain%d %s", dcnt++, mask_str);
			for (itype = SCHED_IDLE; itype < MAX_IDLE_TYPES;
						itype++) {
				seq_printf(seq, " %lu %lu %lu %lu %lu",
				    sd->lb_cnt[itype],
				    sd->lb_failed[itype],
				    sd->lb_imbalance[itype],
				    sd->lb_nobusyq[itype],
				    sd->lb_nobusyg[itype]);
			}
			seq_printf(seq, " %lu %lu %lu %lu\n",
			    sd->sbe_pushed, sd->sbe_attempts,
			    sd->ttwu_wake_affine, sd->ttwu_wake_balance);
		}
#endif
	}
	return 0;
}

static int schedstat_open(struct inode *inode, struct file *file)
{
	unsigned int size = PAGE_SIZE * (1 + num_online_cpus() / 32);
	char *buf = kmalloc(size, GFP_KERNEL);
	struct seq_file *m;
	int res;

	if (!buf)
		return -ENOMEM;
	res = single_open(file, show_schedstat, NULL);
	if (!res) {
		m = file->private_data;
		m->buf = buf;
		m->size = size;
	} else
		kfree(buf);
	return res;
}

struct file_operations proc_schedstat_operations = {
	.open    = schedstat_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

# define schedstat_inc(rq, field)	do { (rq)->field++; } while (0)
# define schedstat_add(rq, field, amt)	do { (rq)->field += (amt); } while (0)
#else /* !CONFIG_SCHEDSTATS */
# define schedstat_inc(rq, field)	do { } while (0)
# define schedstat_add(rq, field, amt)	do { } while (0)
#endif

/*
 * rq_lock - lock a given runqueue and disable interrupts.
 */
static runqueue_t *this_rq_lock(void)
	__acquires(rq->lock)
{
	runqueue_t *rq;

	local_irq_disable();
	rq = this_rq();
	spin_lock(&rq->lock);

	return rq;
}

#ifdef CONFIG_SCHED_SMT
static int cpu_and_siblings_are_idle(int cpu)
{
	int sib;
	for_each_cpu_mask(sib, cpu_sibling_map[cpu]) {
		if (idle_cpu(sib))
			continue;
		return 0;
	}

	return 1;
}
#else
#define cpu_and_siblings_are_idle(A) idle_cpu(A)
#endif

#ifdef CONFIG_SCHEDSTATS
/*
 * Called when a process is dequeued from the active array and given
 * the cpu.  We should note that with the exception of interactive
 * tasks, the expired queue will become the active queue after the active
 * queue is empty, without explicitly dequeuing and requeuing tasks in the
 * expired queue.  (Interactive tasks may be requeued directly to the
 * active queue, thus delaying tasks in the expired queue from running;
 * see scheduler_tick()).
 *
 * This function is only called from sched_info_arrive(), rather than
 * dequeue_task(). Even though a task may be queued and dequeued multiple
 * times as it is shuffled about, we're really interested in knowing how
 * long it was from the *first* time it was queued to the time that it
 * finally hit a cpu.
 */
static inline void sched_info_dequeued(task_t *t)
{
	t->sched_info.last_queued = 0;
}

/*
 * Called when a task finally hits the cpu.  We can now calculate how
 * long it was waiting to run.  We also note when it began so that we
 * can keep stats on how long its timeslice is.
 */
static inline void sched_info_arrive(task_t *t)
{
	unsigned long now = jiffies, diff = 0;
	struct runqueue *rq = task_rq(t);

	if (t->sched_info.last_queued)
		diff = now - t->sched_info.last_queued;
	sched_info_dequeued(t);
	t->sched_info.run_delay += diff;
	t->sched_info.last_arrival = now;
	t->sched_info.pcnt++;

	if (!rq)
		return;

	rq->rq_sched_info.run_delay += diff;
	rq->rq_sched_info.pcnt++;
}

/*
 * Called when a process is queued into either the active or expired
 * array.  The time is noted and later used to determine how long we
 * had to wait for us to reach the cpu.  Since the expired queue will
 * become the active queue after active queue is empty, without dequeuing
 * and requeuing any tasks, we are interested in queuing to either. It
 * is unusual but not impossible for tasks to be dequeued and immediately
 * requeued in the same or another array: this can happen in sched_yield(),
 * set_user_nice(), and even load_balance() as it moves tasks from runqueue
 * to runqueue.
 *
 * This function is only called from enqueue_task(), but also only updates
 * the timestamp if it is already not set.  It's assumed that
 * sched_info_dequeued() will clear that stamp when appropriate.
 */
static inline void sched_info_queued(task_t *t)
{
	if (!t->sched_info.last_queued)
		t->sched_info.last_queued = jiffies;
}

/*
 * Called when a process ceases being the active-running process, either
 * voluntarily or involuntarily.  Now we can calculate how long we ran.
 */
static inline void sched_info_depart(task_t *t)
{
	struct runqueue *rq = task_rq(t);
	unsigned long diff = jiffies - t->sched_info.last_arrival;

	t->sched_info.cpu_time += diff;

	if (rq)
		rq->rq_sched_info.cpu_time += diff;
}

/*
 * Called when tasks are switched involuntarily due, typically, to expiring
 * their time slice.  (This may also be called when switching to or from
 * the idle task.)  We are only called when prev != next.
 */
static inline void sched_info_switch(task_t *prev, task_t *next)
{
	struct runqueue *rq = task_rq(prev);

	/*
	 * prev now departs the cpu.  It's not interesting to record
	 * stats about how efficient we were at scheduling the idle
	 * process, however.
	 */
	if (prev != rq->idle)
		sched_info_depart(prev);

	if (next != rq->idle)
		sched_info_arrive(next);
}
#else
#define sched_info_queued(t)		do { } while (0)
#define sched_info_switch(t, next)	do { } while (0)
#endif /* CONFIG_SCHEDSTATS */

/*
 * Adding/removing a task to/from a priority array:
 */
/**
 * 从运行队列的链表中删除一个进程的描述符
 */
static void dequeue_task(struct task_struct *p, prio_array_t *array)
{
	array->nr_active--;
	list_del(&p->run_list);
	if (list_empty(array->queue + p->prio))
		__clear_bit(p->prio, array->bitmap);
}

/**
 * 把进程描述符插入某个运行队列的链表。
 */
static void enqueue_task(struct task_struct *p, prio_array_t *array)
{
	sched_info_queued(p);
	list_add_tail(&p->run_list, array->queue + p->prio);
	__set_bit(p->prio, array->bitmap);
	array->nr_active++;
	p->array = array;
}

/*
 * Put task to the end of the run list without the overhead of dequeue
 * followed by enqueue.
 */
static void requeue_task(struct task_struct *p, prio_array_t *array)
{
	list_move_tail(&p->run_list, array->queue + p->prio);
}

static inline void enqueue_task_head(struct task_struct *p, prio_array_t *array)
{
	list_add(&p->run_list, array->queue + p->prio);
	__set_bit(p->prio, array->bitmap);
	array->nr_active++;
	p->array = array;
}

/*
 * effective_prio - return the priority that is based on the static
 * priority but is modified by bonuses/penalties.
 *
 * We scale the actual sleep average [0 .... MAX_SLEEP_AVG]
 * into the -5 ... 0 ... +5 bonus/penalty range.
 *
 * We use 25% of the full 0...39 priority range so that:
 *
 * 1) nice +19 interactive tasks do not preempt nice 0 CPU hogs.
 * 2) nice -20 CPU hogs do not get preempted by nice 0 tasks.
 *
 * Both properties are important to certain workloads.
 */
/**
 * 读取current的static_prio和sleep_avg字段，并根据公司计算进程的动态优先级。
 */
static int effective_prio(task_t *p)
{
	int bonus, prio;

	if (rt_task(p))
		return p->prio;

	bonus = CURRENT_BONUS(p) - MAX_BONUS / 2;

	prio = p->static_prio - bonus;
	if (prio < MAX_RT_PRIO)
		prio = MAX_RT_PRIO;
	if (prio > MAX_PRIO-1)
		prio = MAX_PRIO-1;
	return prio;
}

/*
 * __activate_task - move a task to the runqueue.
 */ //把一个新的进程或线程 加入到当前的CPU的 活动进程链表， rq->nr_running++;
static inline void __activate_task(task_t *p, runqueue_t *rq)
{
	enqueue_task(p, rq->active);
	rq->nr_running++;
}

/*
 * __activate_idle_task - move idle task to the _front_ of runqueue.
 */
static inline void __activate_idle_task(task_t *p, runqueue_t *rq)
{
	enqueue_task_head(p, rq->active);
	rq->nr_running++;
}

/**
 * 更新进程的动态优先级和平均睡眠时间。
 * p-进程描述符
 * now-由函数sched_clock计算出的当前时间戳。
 */
static void recalc_task_prio(task_t *p, unsigned long long now)
{
	/**
	 * timestamp是进程进入睡眠状态的进程切换的时间戳。
	 * __sleep_time是进程从上次被切换出去后，经过的时间。
	 */
	unsigned long long __sleep_time = now - p->timestamp;
	unsigned long sleep_time;

	/**
	 * 如果进程睡眠时间超过1秒，就算成1秒。
	 */
	if (__sleep_time > NS_MAX_SLEEP_AVG)
		sleep_time = NS_MAX_SLEEP_AVG;
	else
		sleep_time = (unsigned long)__sleep_time;

	/**
	 * 只有当进程睡眠时间大于0才需要更新平均睡眠时间。
	 */
	if (likely(sleep_time > 0)) {
		/*
		 * User tasks that sleep a long time are categorised as
		 * idle and will get just interactive status to stay active &
		 * prevent them suddenly becoming cpu hogs and starving
		 * other processes.
		 */
		/**
		 * 如果进程不是内核线程(mm!=NULL)，并且进程不是从TASK_UNINTERRUPTIBLE状态被唤醒，并且睡眠时间超过给定的睡眠时间极限
		 * 就把平均睡眠时间设置为相当于900个时钟节拍的值。这仅仅是经验值而已，没有什么科学道理。
		 * 睡眠时间极限依赖于进程的静态优先级，这个规则的目的是保证已经在不可中断模式(通常是等待磁盘IO)上，睡眠了很久的进程获得足够长的平均睡眠时间，以使这些进程尽快得到服务。
		 */
		if (p->mm && p->activated != -1 &&
			sleep_time > INTERACTIVE_SLEEP(p)) {
				p->sleep_avg = JIFFIES_TO_NS(MAX_SLEEP_AVG -
						DEF_TIMESLICE);
		} else {
			/*
			 * The lower the sleep avg a task has the more
			 * rapidly it will rise with sleep time.
			 */
			/**
			 * CURRENT_BONUS宏计算进程原来的平均睡眠时间的bonus值。如果10-bonus>0,就用这个值与sleep_time相乘。
			 * 所以平均睡眠时间越短，它增加得越快。
			 */
			sleep_time *= (MAX_BONUS - CURRENT_BONUS(p)) ? : 1;

			/*
			 * Tasks waking from uninterruptible sleep are
			 * limited in their sleep_avg rise as they
			 * are likely to be waiting on I/O
			 */
			/**
			 * 如果不是内核线程，并且处于TASK_UNINTERRUPTIBLE状态。以下两种情况是为了不对睡眠时间很长的批处理进程给予太多的奖励。
			 */
			if (p->activated == -1 && p->mm) {
				/**
				 * 如果平均睡眠时间已经超过极限了，就不用再调整平均睡眠时间了。
				 */
				if (p->sleep_avg >= INTERACTIVE_SLEEP(p))
					sleep_time = 0;
				else if (p->sleep_avg + sleep_time >=
						INTERACTIVE_SLEEP(p)) {/* 如果加上本次sleep_time时间大于极限睡眠时间，就将睡眠时间设置为最大 */
					p->sleep_avg = INTERACTIVE_SLEEP(p);
					sleep_time = 0;
				}
			}

			/*
			 * This code gives a bonus to interactive tasks.
			 *
			 * The boost works by updating the 'average sleep time'
			 * value here, based on ->timestamp. The more time a
			 * task spends sleeping, the higher the average gets -
			 * and the higher the priority boost gets as well.
			 */
			/**
			 * 将本次睡眠时间加到平均睡眠时间上。
			 */
			p->sleep_avg += sleep_time;

			/**
			 * 如果平均睡眠时间超过1秒，那么就将它调整为1秒。
			 */
			if (p->sleep_avg > NS_MAX_SLEEP_AVG)
				p->sleep_avg = NS_MAX_SLEEP_AVG;
		}
	}

	/**
	 * 更新进程的动态优先级。
	 */
	p->prio = effective_prio(p);
}

/*
 * activate_task - move a task to the runqueue and do priority recalculation
 *
 * Update all the scheduling statistics stuff. (sleep average
 * calculation, priority modifiers, etc.)
 */
static void activate_task(task_t *p, runqueue_t *rq, int local)
{
	unsigned long long now;

	now = sched_clock();
#ifdef CONFIG_SMP
	if (!local) {
		/* Compensate for drifting sched_clock */
		runqueue_t *this_rq = this_rq();
		now = (now - this_rq->timestamp_last_tick)
			+ rq->timestamp_last_tick;
	}
#endif

	recalc_task_prio(p, now);

	/*
	 * This checks to make sure it's not an uninterruptible task
	 * that is now waking up.
	 */
	if (!p->activated) {
		/*
		 * Tasks which were woken up by interrupts (ie. hw events)
		 * are most likely of interactive nature. So we give them
		 * the credit of extending their sleep time to the period
		 * of time they spend on the runqueue, waiting for execution
		 * on a CPU, first time around:
		 */
		if (in_interrupt())
			p->activated = 2;
		else {
			/*
			 * Normal first-time wakeups get a credit too for
			 * on-runqueue time, but it will be weighted down:
			 */
			p->activated = 1;
		}
	}
	p->timestamp = now;

	__activate_task(p, rq);
}

/*
 * deactivate_task - remove a task from the runqueue.
 */
static void deactivate_task(struct task_struct *p, runqueue_t *rq)
{
	rq->nr_running--;
	dequeue_task(p, p->array);
	p->array = NULL;
}

/*
 * resched_task - mark a task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
#ifdef CONFIG_SMP
static void resched_task(task_t *p)
{
	int need_resched, nrpolling;

	assert_spin_locked(&task_rq(p)->lock);

	/* minimise the chance of sending an interrupt to poll_idle() */
	nrpolling = test_tsk_thread_flag(p,TIF_POLLING_NRFLAG);
	need_resched = test_and_set_tsk_thread_flag(p,TIF_NEED_RESCHED);
	nrpolling |= test_tsk_thread_flag(p,TIF_POLLING_NRFLAG);

	if (!need_resched && !nrpolling && (task_cpu(p) != smp_processor_id()))
		smp_send_reschedule(task_cpu(p));
}
#else
static inline void resched_task(task_t *p)
{
	set_tsk_need_resched(p);
}
#endif

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 */
inline int task_curr(const task_t *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

#ifdef CONFIG_SMP
enum request_type {
	REQ_MOVE_TASK,
	REQ_SET_DOMAIN,
};

typedef struct {
	struct list_head list;
	enum request_type type;

	/* For REQ_MOVE_TASK */
	task_t *task;
	int dest_cpu;

	/* For REQ_SET_DOMAIN */
	struct sched_domain *sd;

	struct completion done;
} migration_req_t;

/*
 * The task's runqueue lock must be held.
 * Returns true if you have to wait for migration thread.
 */
static int migrate_task(task_t *p, int dest_cpu, migration_req_t *req)
{
	runqueue_t *rq = task_rq(p);

	/*
	 * If the task is not on a runqueue (and not running), then
	 * it is sufficient to simply update the task's cpu field.
	 */
	if (!p->array && !task_running(rq, p)) {
		set_task_cpu(p, dest_cpu);
		return 0;
	}

	init_completion(&req->done);
	req->type = REQ_MOVE_TASK;
	req->task = p;
	req->dest_cpu = dest_cpu;
	list_add(&req->list, &rq->migration_queue);
	return 1;
}

/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time. This function can't
 * be called with interrupts off, or it may introduce deadlock with
 * smp_call_function() if an IPI is sent by the same process we are
 * waiting to become inactive.
 */
void wait_task_inactive(task_t * p)
{
	unsigned long flags;
	runqueue_t *rq;
	int preempted;

repeat:
	rq = task_rq_lock(p, &flags);
	/* Must be off runqueue entirely, not preempted. */
	if (unlikely(p->array || task_running(rq, p))) {
		/* If it's preempted, we yield.  It could be a while. */
		preempted = !task_running(rq, p);
		task_rq_unlock(rq, &flags);
		cpu_relax();
		if (preempted)
			yield();
		goto repeat;
	}
	task_rq_unlock(rq, &flags);
}

/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesnt have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
void kick_process(task_t *p)
{
	int cpu;

	preempt_disable();
	cpu = task_cpu(p);
	if ((cpu != smp_processor_id()) && task_curr(p))
		smp_send_reschedule(cpu);
	preempt_enable();
}

/*
 * Return a low guess at the load of a migration-source cpu.
 *
 * We want to under-estimate the load of migration sources, to
 * balance conservatively.
 */
static inline unsigned long source_load(int cpu)
{
	runqueue_t *rq = cpu_rq(cpu);
	unsigned long load_now = rq->nr_running * SCHED_LOAD_SCALE;

	return min(rq->cpu_load, load_now);
}

/*
 * Return a high guess at the load of a migration-target cpu
 */
static inline unsigned long target_load(int cpu)
{
	runqueue_t *rq = cpu_rq(cpu);
	unsigned long load_now = rq->nr_running * SCHED_LOAD_SCALE;

	return max(rq->cpu_load, load_now);
}

#endif

/*
 * wake_idle() will wake a task on an idle cpu if task->cpu is
 * not idle and an idle cpu is available.  The span of cpus to
 * search starts with cpus closest then further out as needed,
 * so we always favor a closer, idle cpu.
 *
 * Returns the CPU we should wake onto.
 */
#if defined(ARCH_HAS_SCHED_WAKE_IDLE)
static int wake_idle(int cpu, task_t *p)
{
	cpumask_t tmp;
	struct sched_domain *sd;
	int i;

	if (idle_cpu(cpu))
		return cpu;

	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_IDLE) {
			cpus_and(tmp, sd->span, cpu_online_map);
			cpus_and(tmp, tmp, p->cpus_allowed);
			for_each_cpu_mask(i, tmp) {
				if (idle_cpu(i))
					return i;
			}
		}
		else break;
	}
	return cpu;
}
#else
static inline int wake_idle(int cpu, task_t *p)
{
	return cpu;
}
#endif

/***
 * try_to_wake_up - wake up a thread
 * @p: the to-be-woken-up thread
 * @state: the mask of task states that can be woken
 * @sync: do a synchronous wakeup?
 *
 * Put it on the run-queue if it's not already there. The "current"
 * thread is always on the run-queue (except when the actual
 * re-schedule is in progress), and as such you're allowed to do
 * the simpler "current->state = TASK_RUNNING" to mark yourself
 * runnable without the overhead of this.
 *
 * returns failure only if the task is already active.
 */
/**
 * 通过把进程状态设置为TASK_RUNNING，并把该进程插入本地CPU的运行队列来唤醒睡眠或停止的进程
 * p-被唤醒进程的描述符
 * state-可以被唤醒的进程状态掩码。
 * sync-一个标志，用来禁止被唤醒的进程抢占本地CPU上正在运行的进程。
 */
static int try_to_wake_up(task_t * p, unsigned int state, int sync)
{
	int cpu, this_cpu, success = 0;
	unsigned long flags;
	long old_state;
	runqueue_t *rq;
#ifdef CONFIG_SMP
	unsigned long load, this_load;
	struct sched_domain *sd;
	int new_cpu;
#endif

	/**
	 * 调用task_rq_lock来禁止中断，并获得进程所在CPU上的运行队列的锁(可能与当前CPU的运行队列不一样，并且被唤醒的进程可能并不在队列上)
	 */
	rq = task_rq_lock(p, &flags);
	schedstat_inc(rq, ttwu_cnt);
	old_state = p->state;
	/**
	 * 只唤醒state对应状态的进程。如果被唤醒的进程状态不在state中，直接退出。本次唤醒无效。
	 * 例如:通过信号就不会唤醒TASK_UNINTERRUPTIBLE状态的进程。
	 */
	if (!(old_state & state))
		goto out;

	/**
	 * 如果进程已经属于某个运行队列，就跳转到out_running，将它的状态修改为TASK_RUNNING状态后退出。
	 */
	if (p->array)
		goto out_running;

	cpu = task_cpu(p);
	this_cpu = smp_processor_id();

#ifdef CONFIG_SMP
	/**
	 * 在SMP上，需要检查被唤醒的进程是否应该从最近运行的CPU的运行队列迁移到另外一个CPU的运行队列。
	 */

	/**
	 * 被唤醒任务正在CPU上运行，不必考虑迁移了。
	 */
	if (unlikely(task_running(rq, p)))
		goto out_activate;

	/**
	 * 优先将进程放到进程所在CPU上运行。
	 */
	new_cpu = cpu;

	/**
	 * 如果进程所在CPU就是当前进程所在CPU，或者被唤醒进程不允许在当前进程所在CPU上运行，那么跳转到out_set_cpu
	 */
	if (cpu == this_cpu || unlikely(!cpu_isset(this_cpu, p->cpus_allowed)))
		goto out_set_cpu;

	load = source_load(cpu);
	this_load = target_load(this_cpu);

	/*
	 * If sync wakeup then subtract the (maximum possible) effect of
	 * the currently running task from the load of the current CPU:
	 */
	if (sync)
		this_load -= SCHED_LOAD_SCALE;

	/* Don't pull the task off an idle CPU to a busy one */
	/**
	 * 如果被唤醒任务所在的CPU工作量小于当前CPU的工作量，也跳转到out_set_cpu
	 */
	if (load < SCHED_LOAD_SCALE/2 && this_load > SCHED_LOAD_SCALE/2)
		goto out_set_cpu;

	/**
	 * 试图将进程迁移到本地CPU。
	 */
	new_cpu = this_cpu; /* Wake to this CPU if we can */

	/*
	 * Scan domains for affine wakeup and passive balancing
	 * possibilities.
	 */
	for_each_domain(this_cpu, sd) {
		unsigned int imbalance;
		/*
		 * Start passive balancing when half the imbalance_pct
		 * limit is reached.
		 */
		imbalance = sd->imbalance_pct + (sd->imbalance_pct - 100) / 2;

		if ((sd->flags & SD_WAKE_AFFINE) &&
				!task_hot(p, rq->timestamp_last_tick, sd)) {
			/*
			 * This domain has SD_WAKE_AFFINE and p is cache cold
			 * in this domain.
			 */
			if (cpu_isset(cpu, sd->span)) {
				schedstat_inc(sd, ttwu_wake_affine);
				goto out_set_cpu;
			}
		} else if ((sd->flags & SD_WAKE_BALANCE) &&
				imbalance*this_load <= 100*load) {
			/*
			 * This domain has SD_WAKE_BALANCE and there is
			 * an imbalance.
			 */
			if (cpu_isset(cpu, sd->span)) {
				schedstat_inc(sd, ttwu_wake_balance);
				goto out_set_cpu;
			}
		}
	}

	new_cpu = cpu; /* Could not wake to this_cpu. Wake to cpu instead */
out_set_cpu:
	schedstat_inc(rq, ttwu_attempts);
	new_cpu = wake_idle(new_cpu, p);
	if (new_cpu != cpu) {
		schedstat_inc(rq, ttwu_moved);
		set_task_cpu(p, new_cpu);
		task_rq_unlock(rq, &flags);
		/* might preempt at this point */
		rq = task_rq_lock(p, &flags);
		old_state = p->state;
		if (!(old_state & state))
			goto out;
		if (p->array)
			goto out_running;

		this_cpu = smp_processor_id();
		cpu = task_cpu(p);
	}

out_activate:
#endif /* CONFIG_SMP */
	/**
	 * 如果是TASK_UNINTERRUPTIBLE，就递减nr_uninterruptible
	 * 并将activated设为-1,表示进程是从TASK_UNINTERRUPTIBLE状态被唤醒这个事实。
	 */
	if (old_state == TASK_UNINTERRUPTIBLE) {
		rq->nr_uninterruptible--;
		/*
		 * Tasks on involuntary sleep don't earn
		 * sleep_avg beyond just interactive state.
		 */
		p->activated = -1;
	}

	/*
	 * Sync wakeups (i.e. those types of wakeups where the waker
	 * has indicated that it will leave the CPU in short order)
	 * don't trigger a preemption, if the woken up task will run on
	 * this cpu. (in this case the 'I will reschedule' promise of
	 * the waker guarantees that the freshly woken up task is going
	 * to be considered on this CPU.)
	 */
	/**
	 * activate_task函数依次执行以下步骤澹�
	 *     1:调用sched_clock获得当前时间戳，如果目标CPU不是本地CPU，那么还会补偿时钟中断的偏差。
	 *     2:调用recalc_task_prio，计算进程的动态优先级。
	 *     3:根据情况设置activated
	 *     4:设置进程的时间戳。
	 *     5:将进程插入进程集合。
	 */
	activate_task(p, rq, cpu == this_cpu);
	/**
	 * 如果目标CPU不是本地CPU，或者没有SYNC标志，就检查新进程的动态优先级是否比运行队列中当前进程的优先级高。
	 */
	if (!sync || cpu != this_cpu) {
		if (TASK_PREEMPTS_CURR(p, rq))/* 进程的优先级比所在队列的当前进程优先级高，需要抢占。 */
			/**
			 * resched_task函数进行进程抢占。
			 * 在单处理器上，它仅仅设置TIF_NEED_RESCHED标志。 
			 * 在多处理器上，它可能会发送IPI，强制让CPU产生调度。
			 */
			resched_task(rq->curr);
	}
	success = 1;

out_running:
	/**
	 * 将进程状态设置为为TASK_RUNNING，注意两个流程会走到这里。
	 */
	p->state = TASK_RUNNING;
out:
	/**
	 * 开中断并打开运行队列的锁。
	 */
	task_rq_unlock(rq, &flags);

	/**
	 * 返回0:进程没有被唤醒。否则返回1,进程被唤醒。
	 */
	return success;
}

int fastcall wake_up_process(task_t * p)
{
	return try_to_wake_up(p, TASK_STOPPED | TASK_TRACED |
				 TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE, 0);
}

EXPORT_SYMBOL(wake_up_process);

int fastcall wake_up_state(task_t *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

#ifdef CONFIG_SMP
static int find_idlest_cpu(struct task_struct *p, int this_cpu,
			   struct sched_domain *sd);
#endif

/*
 * Perform scheduler related setup for a newly forked process p.
 * p is forked by current.
 */
void fastcall sched_fork(task_t *p)
{
	/*
	 * We mark the process as running here, but have not actually
	 * inserted it onto the runqueue yet. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->state = TASK_RUNNING;
	INIT_LIST_HEAD(&p->run_list);
	p->array = NULL;
	spin_lock_init(&p->switch_lock);
#ifdef CONFIG_SCHEDSTATS
	memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif
#ifdef CONFIG_PREEMPT
	/*
	 * During context-switch we hold precisely one spinlock, which
	 * schedule_tail drops. (in the common case it's this_rq()->lock,
	 * but it also can be p->switch_lock.) So we compensate with a count
	 * of 1. Also, we want to start with kernel preemption disabled.
	 */
	p->thread_info->preempt_count = 1;
#endif
	/*
	 * Share the timeslice between parent and child, thus the
	 * total amount of pending timeslices in the system doesn't change,
	 * resulting in more scheduling fairness.
	 */
	local_irq_disable();
	p->time_slice = (current->time_slice + 1) >> 1;
	/*
	 * The remainder of the first timeslice might be recovered by
	 * the parent if the child exits early enough.
	 */
	/**
	 * 因为子进程没有用完它的时间片(现在还没有运行，并且子进程的时间片总是大于0,当然就没有用完了)
	 * 这样，设置first_time_slice，如果子进程在它的第一个时间片内终止或者执行新的程序，就把子进程的剩余时间奖励给父进程。
	 */
	p->first_time_slice = 1;
	current->time_slice >>= 1;
	p->timestamp = sched_clock();
	/**
	 * 如果当前进程的时间片为1,那么这一个tick将被分给子进程，而当前进程的时间片变成1.
	 */
	if (unlikely(!current->time_slice)) {
		/*
		 * This case is rare, it happens when the parent has only
		 * a single jiffy left from its timeslice. Taking the
		 * runqueue lock is not a problem.
		 */
		/**
		 * 强制将time_slice设置成1,然后调用scheduler_tick递减为0.
		 */
		current->time_slice = 1;
		preempt_disable();
		scheduler_tick();
		local_irq_enable();
		preempt_enable();
	} else
		local_irq_enable();
}

/*
 * wake_up_new_task - wake up a newly created task for the first time.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created context, then puts the task
 * on the runqueue and wakes it.
 */
void fastcall wake_up_new_task(task_t * p, unsigned long clone_flags)
{
	unsigned long flags;
	int this_cpu, cpu;
	runqueue_t *rq, *this_rq;

	rq = task_rq_lock(p, &flags);
	cpu = task_cpu(p);
	this_cpu = smp_processor_id();

	BUG_ON(p->state != TASK_RUNNING);

	schedstat_inc(rq, wunt_cnt);
	/*
	 * We decrease the sleep average of forking parents
	 * and children as well, to keep max-interactive tasks
	 * from forking tasks that are max-interactive. The parent
	 * (current) is done further down, under its lock.
	 */
	p->sleep_avg = JIFFIES_TO_NS(CURRENT_BONUS(p) *
		CHILD_PENALTY / 100 * MAX_SLEEP_AVG / MAX_BONUS);

	p->prio = effective_prio(p);

	if (likely(cpu == this_cpu)) {
		if (!(clone_flags & CLONE_VM)) {
			/*
			 * The VM isn't cloned, so we're in a good position to
			 * do child-runs-first in anticipation of an exec. This
			 * usually avoids a lot of COW overhead.
			 */
			if (unlikely(!current->array))
				__activate_task(p, rq);
			else {
				p->prio = current->prio;
				list_add_tail(&p->run_list, &current->run_list);
				p->array = current->array;
				p->array->nr_active++;
				rq->nr_running++;
			}
			set_need_resched();
		} else
			/* Run child last */
			__activate_task(p, rq);
		/*
		 * We skip the following code due to cpu == this_cpu
	 	 *
		 *   task_rq_unlock(rq, &flags);
		 *   this_rq = task_rq_lock(current, &flags);
		 */
		this_rq = rq;
	} else {
		this_rq = cpu_rq(this_cpu);

		/*
		 * Not the local CPU - must adjust timestamp. This should
		 * get optimised away in the !CONFIG_SMP case.
		 */
		p->timestamp = (p->timestamp - this_rq->timestamp_last_tick)
					+ rq->timestamp_last_tick;
		__activate_task(p, rq);
		if (TASK_PREEMPTS_CURR(p, rq))
			resched_task(rq->curr);

		schedstat_inc(rq, wunt_moved);
		/*
		 * Parent and child are on different CPUs, now get the
		 * parent runqueue to update the parent's ->sleep_avg:
		 */
		task_rq_unlock(rq, &flags);
		this_rq = task_rq_lock(current, &flags);
	}
	current->sleep_avg = JIFFIES_TO_NS(CURRENT_BONUS(current) *
		PARENT_PENALTY / 100 * MAX_SLEEP_AVG / MAX_BONUS);
	task_rq_unlock(this_rq, &flags);
}

/*
 * Potentially available exiting-child timeslices are
 * retrieved here - this way the parent does not get
 * penalized for creating too many threads.
 *
 * (this cannot be used to 'generate' timeslices
 * artificially, because any timeslice recovered here
 * was given away by the parent in the first place.)
 */
void fastcall sched_exit(task_t * p)
{
	unsigned long flags;
	runqueue_t *rq;

	/*
	 * If the child was a (relative-) CPU hog then decrease
	 * the sleep_avg of the parent as well.
	 */
	rq = task_rq_lock(p->parent, &flags);
	if (p->first_time_slice) {
		p->parent->time_slice += p->time_slice;
		if (unlikely(p->parent->time_slice > task_timeslice(p)))
			p->parent->time_slice = task_timeslice(p);
	}
	if (p->sleep_avg < p->parent->sleep_avg)
		p->parent->sleep_avg = p->parent->sleep_avg /
		(EXIT_WEIGHT + 1) * EXIT_WEIGHT + p->sleep_avg /
		(EXIT_WEIGHT + 1);
	task_rq_unlock(rq, &flags);
}

/**
 * finish_task_switch - clean up after a task-switch
 * @prev: the thread we just switched away from.
 *
 * We enter this with the runqueue still locked, and finish_arch_switch()
 * will unlock it along with doing any other architecture-specific cleanup
 * actions.
 *
 * Note that we may have delayed dropping an mm in context_switch(). If
 * so, we finish that here outside of the runqueue lock.  (Doing it
 * with the lock held can cause deadlocks; see schedule() for
 * details.)
 */
/**
 * 进程切换回来后，执行的代码。
 */
static void finish_task_switch(task_t *prev)
	__releases(rq->lock)
{
	runqueue_t *rq = this_rq();
	/**
	 * 如果prev是一个内核线程，那么rq->prev_mm字段存放借给prev的内存描述符的地址。
	 */
	struct mm_struct *mm = rq->prev_mm;
	unsigned long prev_task_flags;

	rq->prev_mm = NULL;

	/*
	 * A task struct has one reference for the use as "current".
	 * If a task dies, then it sets EXIT_ZOMBIE in tsk->exit_state and
	 * calls schedule one last time. The schedule call will never return,
	 * and the scheduled task must drop that reference.
	 * The test for EXIT_ZOMBIE must occur while the runqueue locks are
	 * still held, otherwise prev could be scheduled on another cpu, die
	 * there before we look at prev->state, and then the reference would
	 * be dropped twice.
	 *		Manfred Spraul <manfred@colorfullife.com>
	 */
	prev_task_flags = prev->flags;
	/**
	 * 打开中断并释放自旋锁。
	 */
	finish_arch_switch(rq, prev);
	/**
	 * 前一个线程是内核线程，它借用了某一个用户线程的地址空间，在此将mm_struct引用计数减1.
	 */
	if (mm)
		mmdrop(mm);
	/**
	 * 前一个线程可能是一个僵死进程了，释放它的描述符的虚拟存储器。
	 */
	if (unlikely(prev_task_flags & PF_DEAD))
		put_task_struct(prev);
}

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
asmlinkage void schedule_tail(task_t *prev)
	__releases(rq->lock)
{
	finish_task_switch(prev);

	if (current->set_child_tid)
		put_user(current->pid, current->set_child_tid);
}

/*
 * context_switch - switch to the new MM and the new
 * thread's register state.
 */
/**
 * 建立next的地址空间
 */
static inline
task_t * context_switch(runqueue_t *rq, task_t *prev, task_t *next)
{
	struct mm_struct *mm = next->mm;
	struct mm_struct *oldmm = prev->active_mm;

	/**
	 * 如果是切换到一个内核线程，新进程就使用pre的地址空间，避免了TLB的切换
	 */
	if (unlikely(!mm)) {
		next->active_mm = oldmm;
		atomic_inc(&oldmm->mm_count);
		/**
		 * 作为更进一步的优化措施，如果新进程是内核线程，就将进程设置为懒惰TLB模式
		 * xie.baoyou注：请想一下，如果内核线程切换出去后，可能又会回到上一个进程，此时就根本不需要切换地址空间。
		 * 皆大欢喜，大家都省事了，这叫“懒人有懒福”
		 */
		enter_lazy_tlb(oldmm, next);
	} else/* 否则就需要切换内地址空间。 */
		switch_mm(oldmm, mm, next);

	/**
	 * 如果上一个线程是内核线程，就把prev内存描述符的指针保存到运行队列的prev_mm中。
	 * 并清空rq->prev_mm
	 */
	if (unlikely(!prev->mm)) {
		prev->active_mm = NULL;
		WARN_ON(rq->prev_mm);
		rq->prev_mm = oldmm;
	}

	/* Here we just switch the register state and the stack. */
	/**
	 * 终于可以真正的切换了。
	 */	
	switch_to(prev, next, prev);

	return prev;
}

/*
 * nr_running, nr_uninterruptible and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, current number of uninterruptible-sleeping threads, total
 * number of context switches performed since bootup.
 */
unsigned long nr_running(void)
{
	unsigned long i, sum = 0;

	for_each_online_cpu(i)
		sum += cpu_rq(i)->nr_running; //runqueues->nr_running  每个CPU中可运行进程的数量 R 

	return sum;
}

unsigned long nr_uninterruptible(void)
{
	unsigned long i, sum = 0;

	for_each_cpu(i)
		sum += cpu_rq(i)->nr_uninterruptible; //runqueue->nr_uninterruptible  每个CPU中不可中断睡眠进程的数量 D

	/*
	 * Since we read the counters lockless, it might be slightly
	 * inaccurate. Do not allow it to go below zero though:
	 */
	if (unlikely((long)sum < 0))
		sum = 0;

	return sum;
}

unsigned long long nr_context_switches(void)
{
	unsigned long long i, sum = 0;

	for_each_cpu(i)
		sum += cpu_rq(i)->nr_switches;

	return sum;
}

unsigned long nr_iowait(void)
{
	unsigned long i, sum = 0;

	for_each_cpu(i)
		sum += atomic_read(&cpu_rq(i)->nr_iowait);

	return sum;
}

#ifdef CONFIG_SMP

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static void double_rq_lock(runqueue_t *rq1, runqueue_t *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	if (rq1 == rq2) {
		spin_lock(&rq1->lock);
		__acquire(rq2->lock);	/* Fake it out ;) */
	} else {
		if (rq1 < rq2) {
			spin_lock(&rq1->lock);
			spin_lock(&rq2->lock);
		} else {
			spin_lock(&rq2->lock);
			spin_lock(&rq1->lock);
		}
	}
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static void double_rq_unlock(runqueue_t *rq1, runqueue_t *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	spin_unlock(&rq1->lock);
	if (rq1 != rq2)
		spin_unlock(&rq2->lock);
	else
		__release(rq2->lock);
}

/*
 * double_lock_balance - lock the busiest runqueue, this_rq is locked already.
 */
static void double_lock_balance(runqueue_t *this_rq, runqueue_t *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	if (unlikely(!spin_trylock(&busiest->lock))) {
		if (busiest < this_rq) {
			spin_unlock(&this_rq->lock);
			spin_lock(&busiest->lock);
			spin_lock(&this_rq->lock);
		} else
			spin_lock(&busiest->lock);
	}
}

/*
 * find_idlest_cpu - find the least busy runqueue.
 */
static int find_idlest_cpu(struct task_struct *p, int this_cpu,
			   struct sched_domain *sd)
{
	unsigned long load, min_load, this_load;
	int i, min_cpu;
	cpumask_t mask;

	min_cpu = UINT_MAX;
	min_load = ULONG_MAX;

	cpus_and(mask, sd->span, p->cpus_allowed);

	for_each_cpu_mask(i, mask) {
		load = target_load(i);

		if (load < min_load) {
			min_cpu = i;
			min_load = load;

			/* break out early on an idle CPU: */
			if (!min_load)
				break;
		}
	}

	/* add +1 to account for the new task */
	this_load = source_load(this_cpu) + SCHED_LOAD_SCALE;

	/*
	 * Would with the addition of the new task to the
	 * current CPU there be an imbalance between this
	 * CPU and the idlest CPU?
	 *
	 * Use half of the balancing threshold - new-context is
	 * a good opportunity to balance.
	 */
	if (min_load*(100 + (sd->imbalance_pct-100)/2) < this_load*100)
		return min_cpu;

	return this_cpu;
}

/*
 * If dest_cpu is allowed for this process, migrate the task to it.
 * This is accomplished by forcing the cpu_allowed mask to only
 * allow dest_cpu, which will force the cpu onto dest_cpu.  Then
 * the cpu_allowed mask is restored.
 */
static void sched_migrate_task(task_t *p, int dest_cpu)
{
	migration_req_t req;
	runqueue_t *rq;
	unsigned long flags;

	rq = task_rq_lock(p, &flags);
	if (!cpu_isset(dest_cpu, p->cpus_allowed)
	    || unlikely(cpu_is_offline(dest_cpu)))
		goto out;

	schedstat_inc(rq, smt_cnt);
	/* force the process onto the specified CPU */
	if (migrate_task(p, dest_cpu, &req)) {
		/* Need to wait for migration thread (might exit: take ref). */
		struct task_struct *mt = rq->migration_thread;
		get_task_struct(mt);
		task_rq_unlock(rq, &flags);
		wake_up_process(mt);
		put_task_struct(mt);
		wait_for_completion(&req.done);
		return;
	}
out:
	task_rq_unlock(rq, &flags);
}

/*
 * sched_exec(): find the highest-level, exec-balance-capable
 * domain and try to migrate the task to the least loaded CPU.
 *
 * execve() is a valuable balancing opportunity, because at this point
 * the task has the smallest effective memory and cache footprint.
 */
void sched_exec(void)
{
	struct sched_domain *tmp, *sd = NULL;
	int new_cpu, this_cpu = get_cpu();

	schedstat_inc(this_rq(), sbe_cnt);
	/* Prefer the current CPU if there's only this task running */
	if (this_rq()->nr_running <= 1)
		goto out;

	for_each_domain(this_cpu, tmp)
		if (tmp->flags & SD_BALANCE_EXEC)
			sd = tmp;

	if (sd) {
		schedstat_inc(sd, sbe_attempts);
		new_cpu = find_idlest_cpu(current, this_cpu, sd);
		if (new_cpu != this_cpu) {
			schedstat_inc(sd, sbe_pushed);
			put_cpu();
			sched_migrate_task(current, new_cpu);
			return;
		}
	}
out:
	put_cpu();
}

/*
 * pull_task - move a task from a remote runqueue to the local runqueue.
 * Both runqueues must be locked.
 */
static inline
void pull_task(runqueue_t *src_rq, prio_array_t *src_array, task_t *p,
	       runqueue_t *this_rq, prio_array_t *this_array, int this_cpu)
{
	dequeue_task(p, src_array);
	src_rq->nr_running--;
	set_task_cpu(p, this_cpu);
	this_rq->nr_running++;
	enqueue_task(p, this_array);
	p->timestamp = (p->timestamp - src_rq->timestamp_last_tick)
				+ this_rq->timestamp_last_tick;
	/*
	 * Note that idle threads have a prio of MAX_PRIO, for this test
	 * to be always true for them.
	 */
	if (TASK_PREEMPTS_CURR(p, this_rq))
		resched_task(this_rq->curr);
}

/*
 * can_migrate_task - may task p from runqueue rq be migrated to this_cpu?
 */
/**
 * 当下列条件都满足时，函数返回1，否则表示不能漂移任务到其他CPU：
 *     进程没有在运行（不是running状态），而是没有在rq所在的CPU上占用CPU。
 *         因为这样的进程切换到其他的CPU上，会使其页表缓存失效，达不到提高性能的目的。
 *     本地CPU包含在进程描述符的cpus_allowed位掩码中
 *     并且满足以下条件之一：
 *         本地CPU空闲。如果支持超线程技术，则本地物理芯片中的逻辑CPU必须空闲。
 *         内核在平衡调度域时因反复进行进程迁移都不成功而陷入困境
 *         被迁移的进程不是“高速缓存命中”的（最近不曾在远程CPU上运行，因此远程CPU上的硬件高速缓存中没有该进程的数据。）
 * 如果can_migrate_task返回1，move_tasks就调用pull_task函数把候选进程迁移到本地运行队列中。
 */
static inline
int can_migrate_task(task_t *p, runqueue_t *rq, int this_cpu,
		     struct sched_domain *sd, enum idle_type idle)
{
	/*
	 * We do not migrate tasks that are:
	 * 1) running (obviously), or
	 * 2) cannot be migrated to this CPU due to cpus_allowed, or
	 * 3) are cache-hot on their current CPU.
	 */
	if (task_running(rq, p))
		return 0;
	if (!cpu_isset(this_cpu, p->cpus_allowed))
		return 0;

	/*
	 * Aggressive migration if:
	 * 1) the [whole] cpu is idle, or
	 * 2) too many balance attempts have failed.
	 */

	if (cpu_and_siblings_are_idle(this_cpu) || \
			sd->nr_balance_failed > sd->cache_nice_tries)
		return 1;

	if (task_hot(p, rq->timestamp_last_tick, sd))
			return 0;
	return 1;
}

/*
 * move_tasks tries to move up to max_nr_move tasks from busiest to this_rq,
 * as part of a balancing operation within "domain". Returns the number of
 * tasks moved.
 *
 * Called with both runqueues locked.
 */
/**
 * 把进程从源运行队列迁移到本地运行队列
 * this_rq:本地运行队列描述符。
 * this_cpu:本地CPU下标。
 * busiest:源运行队列描述符。
 * max_nr_move:被迁移进程的最大数。
 * sd:在其中执行平衡操作的调度域的描述符地址。
 * idle:IDLE标志。可以为SCHED_IDLE和NOT_IDLE，当函数被idle_balance间接调用时，还可能是NEWLY_IDLE。
 */
static int move_tasks(runqueue_t *this_rq, int this_cpu, runqueue_t *busiest,
		      unsigned long max_nr_move, struct sched_domain *sd,
		      enum idle_type idle)
{
	prio_array_t *array, *dst_array;
	struct list_head *head, *curr;
	int idx, pulled = 0;
	task_t *tmp;

	if (max_nr_move <= 0 || busiest->nr_running <= 1)
		goto out;

	/*
	 * We first consider expired tasks. Those will likely not be
	 * executed in the near future, and they are most likely to
	 * be cache-cold, thus switching CPUs has the least effect
	 * on them.
	 */
	/**
	 * 首先分析busiest的过期进程。这是因为过期进程没有占用缓存。拉活动线程有点划不来。
	 */
	if (busiest->expired->nr_active) {
		array = busiest->expired;
		dst_array = this_rq->expired;
	} else {
		array = busiest->active;
		dst_array = this_rq->active;
	}

new_array:
	/* Start searching at priority 0: */
	idx = 0;
skip_bitmap:
	/**
	 * 在运行队列中搜索可被迁移的进程。从优先级最高的线程开始查找。
	 */
	if (!idx)
		idx = sched_find_first_bit(array->bitmap);
	else
		idx = find_next_bit(array->bitmap, MAX_PRIO, idx);
	/**
	 * 如果已经搜索完队列。
	 */
	if (idx >= MAX_PRIO) {
		/**
		 * 如果当前是在过期队列中搜索，并且活动队列中有进程。
		 * 就准备在活动队列中搜索。
		 */
		if (array == busiest->expired && busiest->active->nr_active) {
			array = busiest->active;
			dst_array = this_rq->active;
			goto new_array;
		}
		/**
		 * 两个队列中都搜索过了，退出。
		 */
		goto out;
	}

	/**
	 * 对进程链表中的每个进程进行遍历。
	 */
	head = array->queue + idx;
	curr = head->prev;
skip_queue:
	tmp = list_entry(curr, task_t, run_list);

	curr = curr->prev;

	/**
	 * 对链表中的每个线程，调用can_migrate_task，判断它是否适合迁移到本地CPU。
	 */
	if (!can_migrate_task(tmp, busiest, this_cpu, sd, idle)) {
		if (curr != head)
			goto skip_queue;
		idx++;
		goto skip_bitmap;
	}

	/*
	 * Right now, this is the only place pull_task() is called,
	 * so we can safely collect pull_task() stats here rather than
	 * inside pull_task().
	 */
	schedstat_inc(this_rq, pt_gained[idle]);
	schedstat_inc(busiest, pt_lost[idle]);

	/**
	 * 当前线程适于迁移到本地CPU。调用pull_task执行迁移操作。
	 * 本质上，它执行dequeue_task从远程运行队列删除进程。
	 * 然后执行enqueue_task把进程插入本地运行队列。如果刚被迁移的进程比当前进程拥有更高的优先级，
	 * 就调用resched_task抢占本地CPU的当前进程。
	 */
	pull_task(busiest, array, tmp, this_rq, dst_array, this_cpu);
	pulled++;

	/* We only want to steal up to the prescribed number of tasks. */
	if (pulled < max_nr_move) {
		if (curr != head)
			goto skip_queue;
		idx++;
		goto skip_bitmap;
	}
out:
	return pulled;
}

/* 找出当前调度域sd中最忙的调度组sg
 * find_busiest_group finds and returns the busiest CPU group within the
 * domain. It calculates and returns the number of tasks which should be
 * moved to restore balance via the imbalance parameter.
 */
static struct sched_group *
find_busiest_group(struct sched_domain *sd, int this_cpu,
		   unsigned long *imbalance, enum idle_type idle)
{
	struct sched_group *busiest = NULL, *this = NULL, *group = sd->groups;
	unsigned long max_load, avg_load, total_load, this_load, total_pwr;

	max_load = this_load = total_load = total_pwr = 0;

	do {
		unsigned long load;
		int local_group;
		int i, nr_cpus = 0;

		local_group = cpu_isset(this_cpu, group->cpumask);

		/* Tally up the load of all CPUs in the group */
		avg_load = 0;

		for_each_cpu_mask(i, group->cpumask) {
			/* Bias balancing toward cpus of our domain */
			if (local_group)
				load = target_load(i);
			else
				load = source_load(i);

			nr_cpus++;
			avg_load += load;
		}

		if (!nr_cpus)
			goto nextgroup;

		total_load += avg_load;
		total_pwr += group->cpu_power;

		/* Adjust by relative CPU power of the group */
		avg_load = (avg_load * SCHED_LOAD_SCALE) / group->cpu_power;

		if (local_group) {
			this_load = avg_load;
			this = group;
			goto nextgroup;
		} else if (avg_load > max_load) {
			max_load = avg_load;
			busiest = group;
		}
nextgroup:
		group = group->next;
	} while (group != sd->groups);

	if (!busiest || this_load >= max_load)
		goto out_balanced;

	avg_load = (SCHED_LOAD_SCALE * total_load) / total_pwr; //平均负载

	if (this_load >= avg_load ||
			100*max_load <= sd->imbalance_pct*this_load)
		goto out_balanced;

	/*
	 * We're trying to get all the cpus to the average_load, so we don't
	 * want to push ourselves above the average load, nor do we wish to
	 * reduce the max loaded cpu below the average load, as either of these
	 * actions would just result in more rebalancing later, and ping-pong
	 * tasks around. Thus we look for the minimum possible imbalance.
	 * Negative imbalances (*we* are more loaded than anyone else) will
	 * be counted as no imbalance for these purposes -- we can't fix that
	 * by pulling tasks to us.  Be careful of negative numbers as they'll
	 * appear as very large values with unsigned longs.
	 */
	*imbalance = min(max_load - avg_load, avg_load - this_load);

	/* How much load to actually move to equalise the imbalance */
	*imbalance = (*imbalance * min(busiest->cpu_power, this->cpu_power))
				/ SCHED_LOAD_SCALE;

	if (*imbalance < SCHED_LOAD_SCALE - 1) {
		unsigned long pwr_now = 0, pwr_move = 0;
		unsigned long tmp;

		if (max_load - this_load >= SCHED_LOAD_SCALE*2) {
			*imbalance = 1;
			return busiest;
		}

		/*
		 * OK, we don't have enough imbalance to justify moving tasks,
		 * however we may be able to increase total CPU power used by
		 * moving them.
		 */

		pwr_now += busiest->cpu_power*min(SCHED_LOAD_SCALE, max_load);
		pwr_now += this->cpu_power*min(SCHED_LOAD_SCALE, this_load);
		pwr_now /= SCHED_LOAD_SCALE;

		/* Amount of load we'd subtract */
		tmp = SCHED_LOAD_SCALE*SCHED_LOAD_SCALE/busiest->cpu_power;
		if (max_load > tmp)
			pwr_move += busiest->cpu_power*min(SCHED_LOAD_SCALE,
							max_load - tmp);

		/* Amount of load we'd add */
		tmp = SCHED_LOAD_SCALE*SCHED_LOAD_SCALE/this->cpu_power;
		if (max_load < tmp)
			tmp = max_load;
		pwr_move += this->cpu_power*min(SCHED_LOAD_SCALE, this_load + tmp);
		pwr_move /= SCHED_LOAD_SCALE;

		/* Move if we gain another 8th of a CPU worth of throughput */
		if (pwr_move < pwr_now + SCHED_LOAD_SCALE / 8)
			goto out_balanced;

		*imbalance = 1;
		return busiest;
	}

	/* Get rid of the scaling factor, rounding down as we divide */
	*imbalance = (*imbalance + 1) / SCHED_LOAD_SCALE;

	return busiest;

out_balanced:
	if (busiest && (idle == NEWLY_IDLE ||
			(idle == SCHED_IDLE && max_load > SCHED_LOAD_SCALE)) ) {
		*imbalance = 1;
		return busiest;
	}

	*imbalance = 0;
	return NULL;
}

/*
 * find_busiest_queue - find the busiest runqueue among the cpus in group.
 */
static runqueue_t *find_busiest_queue(struct sched_group *group)
{
	unsigned long load, max_load = 0;
	runqueue_t *busiest = NULL;
	int i;

	for_each_cpu_mask(i, group->cpumask) {
		load = source_load(i);

		if (load > max_load) {
			max_load = load;
			busiest = cpu_rq(i);
		}
	}

	return busiest;
}

/*
 * Check this_cpu to ensure it is balanced within domain. Attempt to move
 * tasks if there is an imbalance.
 *
 * Called with this_rq unlocked.
 */
/**
 * 维持多处理器系统中运行队列的平衡。
 * this_cpu-本地CPU的下标。
 * this_rq-本地运行队列的描述符的地址。
 * sd-指向被检查的调度域的描述符。
 * idle-取值为SCHED_IDLE或NOT_IDLE。
 */
static int load_balance(int this_cpu, runqueue_t *this_rq,
			struct sched_domain *sd, enum idle_type idle)
{
	struct sched_group *group;
	runqueue_t *busiest;
	unsigned long imbalance;
	int nr_moved;

	/**
	 * 获得运行队列的自旋锁。
	 */
	spin_lock(&this_rq->lock);
	schedstat_inc(sd, lb_cnt[idle]);

	/**
	 * find_busiest_group函数分析调度域中各组的工作量。
	 * 它返回最繁忙的组的sched_group描述符的地址。假设这个组不包括本地CPU，函数还返回为了恢复平衡而被迁移到
	 * 本地运行队列中的进程数。如果最繁忙的组包括本地CPU或者所有的组本来就是平衡的，函数返回NULL。
	 */
	group = find_busiest_group(sd, this_cpu, &imbalance, idle);
	/**
	 * 如果find_busiest_group在调度域中没有找到既不包括本地CPU又非常繁忙的组。就释放自旋锁，调整调度域描述符的参数，以延迟本地CPU下一次对load_balance的调度。
	 */
	if (!group) {
		schedstat_inc(sd, lb_nobusyg[idle]);
		goto out_balanced;
	}

	/**
	 * find_busiest_queue找到组中最繁忙的CPU，函数返回相应运行队列的描述符地址busiest。
	 */
	busiest = find_busiest_queue(group);
	if (!busiest) {
		schedstat_inc(sd, lb_nobusyq[idle]);
		goto out_balanced;
	}

	/*
	 * This should be "impossible", but since load
	 * balancing is inherently racy and statistical,
	 * it could happen in theory.
	 */
	/**
	 * 这里为了保险起见，确保busiest != this_rq
	 * 因为现在已经获得了this->lock。并且将要获得busiest->lock
	 * 如果这两个值相等，会导致系统死锁。
	 */
	if (unlikely(busiest == this_rq)) {
		WARN_ON(1);
		goto out_balanced;
	}

	schedstat_add(sd, lb_imbalance[idle], imbalance);

	nr_moved = 0;
	if (busiest->nr_running > 1) {
		/*
		 * Attempt to move tasks. If find_busiest_group has found
		 * an imbalance but busiest->nr_running <= 1, the group is
		 * still unbalanced. nr_moved simply stays zero, so it is
		 * correctly treated as an imbalance.
		 */
		/**
		 * 获得busiest->lock.
		 * 注意:这一步必须非常小心，因为极有可能死锁。
		 * 当不能立即获得busiest->lock时，需要比较this_rq和busiest，确保按顺序获得锁。
		 * 请细心领会double_lock_balance函数。
		 */
		double_lock_balance(this_rq, busiest);
		/**
		 * 调用move_tasks，尝试从最繁忙的运行队列中把一些进程迁移到本地运行队列this_rq中。
		 */
		nr_moved = move_tasks(this_rq, this_cpu, busiest,
						imbalance, sd, idle);
		/**
		 * 释放busiest->lock，但是仍然保留this_rq->lock
		 */
		spin_unlock(&busiest->lock);
	}
	spin_unlock(&this_rq->lock);

	/**
	 * 没有迁移到任何线程到本地CPU。队列仍然不平衡。
	 */
	if (!nr_moved) {
		schedstat_inc(sd, lb_failed[idle]);
		sd->nr_balance_failed++;

		if (unlikely(sd->nr_balance_failed > sd->cache_nice_tries+2)) {
			int wake = 0;

			spin_lock(&busiest->lock);
			if (!busiest->active_balance) {
				/**
				 * 设置active_balance标志，表示需要将任务进行迁移。
				 */
				busiest->active_balance = 1;
				busiest->push_cpu = this_cpu;
				wake = 1;
			}
			spin_unlock(&busiest->lock);
			/**
			 * 需要唤醒migration内核线程。这个内核线程顺着调度域的链搜索。寻找空闲CPU。
			 * 如果找到一个空闲CPU，就调用move_tasks把一个进程迁移到空闲运行队列。
			 */
			if (wake)
				wake_up_process(busiest->migration_thread);

			/*
			 * We've kicked active balancing, reset the failure
			 * counter.
			 */
			sd->nr_balance_failed = sd->cache_nice_tries;
		}

		/*
		 * We were unbalanced, but unsuccessful in move_tasks(),
		 * so bump the balance_interval to lessen the lock contention.
		 */
		if (sd->balance_interval < sd->max_interval)
			sd->balance_interval++;
	} else {
		sd->nr_balance_failed = 0;

		/* We were unbalanced, so reset the balancing interval */
		sd->balance_interval = sd->min_interval;
	}

	return nr_moved;

out_balanced:
	spin_unlock(&this_rq->lock);

	/* tune up the balancing interval */
	if (sd->balance_interval < sd->max_interval)
		sd->balance_interval *= 2;

	return 0;
}

/*
 * Check this_cpu to ensure it is balanced within domain. Attempt to move
 * tasks if there is an imbalance.
 *
 * Called from schedule when this_rq is about to become idle (NEWLY_IDLE).
 * this_rq is locked.
 */
static int load_balance_newidle(int this_cpu, runqueue_t *this_rq,
				struct sched_domain *sd)
{
	struct sched_group *group;
	runqueue_t *busiest = NULL;
	unsigned long imbalance;
	int nr_moved = 0;

	schedstat_inc(sd, lb_cnt[NEWLY_IDLE]);
	group = find_busiest_group(sd, this_cpu, &imbalance, NEWLY_IDLE); //从当前调度域sd中找到最忙的调度组sg
	if (!group) {
		schedstat_inc(sd, lb_nobusyg[NEWLY_IDLE]);
		goto out;
	}

	busiest = find_busiest_queue(group); //从调度组sg中找到最忙的调度队列rq
	if (!busiest || busiest == this_rq) {
		schedstat_inc(sd, lb_nobusyq[NEWLY_IDLE]);
		goto out;
	}

	/* Attempt to move tasks */
	double_lock_balance(this_rq, busiest);

	schedstat_add(sd, lb_imbalance[NEWLY_IDLE], imbalance);
	nr_moved = move_tasks(this_rq, this_cpu, busiest,
					imbalance, sd, NEWLY_IDLE);
	if (!nr_moved)
		schedstat_inc(sd, lb_failed[NEWLY_IDLE]);

	spin_unlock(&busiest->lock);

out:
	return nr_moved;
}

/* 当本CPU上运行队列没有可运行的task,就从其它的CPU上迁移一些可运行的task到本地rq
 * idle_balance is called by schedule() if this_cpu is about to become
 * idle. Attempts to pull tasks from other CPUs.
 */
static inline void idle_balance(int this_cpu, runqueue_t *this_rq)
{
	struct sched_domain *sd;

	for_each_domain(this_cpu, sd) {  //从下到上遍历所有CPU 调度域
		if (sd->flags & SD_BALANCE_NEWIDLE) {
			if (load_balance_newidle(this_cpu, this_rq, sd)) {
				/* We've pulled tasks over so stop searching */
				break;
			}
		}
	}
}

/*
 * active_load_balance is run by migration threads. It pushes running tasks
 * off the busiest CPU onto idle CPUs. It requires at least 1 task to be
 * running on each physical CPU where possible, and avoids physical /
 * logical imbalances.
 *
 * Called with busiest_rq locked.
 */
static void active_load_balance(runqueue_t *busiest_rq, int busiest_cpu)
{
	struct sched_domain *sd;
	struct sched_group *cpu_group;
	runqueue_t *target_rq;
	cpumask_t visited_cpus;
	int cpu;

	schedstat_inc(busiest_rq, alb_cnt);
	/*
	 * Search for suitable CPUs to push tasks to in successively higher
	 * domains with SD_LOAD_BALANCE set.
	 */
	visited_cpus = CPU_MASK_NONE;
	for_each_domain(busiest_cpu, sd) {
		if (!(sd->flags & SD_LOAD_BALANCE))
			/* no more domains to search */
			break;

		cpu_group = sd->groups;
		do {
			for_each_cpu_mask(cpu, cpu_group->cpumask) {
				if (busiest_rq->nr_running <= 1)
					/* no more tasks left to move */
					return;
				if (cpu_isset(cpu, visited_cpus))
					continue;
				cpu_set(cpu, visited_cpus);
				if (!cpu_and_siblings_are_idle(cpu) || cpu == busiest_cpu)
					continue;

				target_rq = cpu_rq(cpu);
				/*
				 * This condition is "impossible", if it occurs
				 * we need to fix it.  Originally reported by
				 * Bjorn Helgaas on a 128-cpu setup.
				 */
				BUG_ON(busiest_rq == target_rq);

				/* move a task from busiest_rq to target_rq */
				double_lock_balance(busiest_rq, target_rq);
				if (move_tasks(target_rq, cpu, busiest_rq,
						1, sd, SCHED_IDLE)) {
					schedstat_inc(busiest_rq, alb_lost);
					schedstat_inc(target_rq, alb_gained);
				} else {
					schedstat_inc(busiest_rq, alb_failed);
				}
				spin_unlock(&target_rq->lock);
			}
			cpu_group = cpu_group->next;
		} while (cpu_group != sd->groups);
	}
}

/*
 * rebalance_tick will get called every timer tick, on every CPU.
 *
 * It checks each scheduling domain to see if it is due to be balanced,
 * and initiates a balancing operation if so.
 *
 * Balancing parameters are set up in arch_init_sched_domains.
 */

/* Don't have all balancing operations going off at once */
#define CPU_OFFSET(cpu) (HZ * cpu / NR_CPUS)

/**
 * 运行队列平衡函数。每次经过一次时钟节拍时，被scheduler_tick调用。
 * this_cpu-本地CPU下标。
 * this_rq-本地运行队列的地址。
 * idle-是否idle进程。SCHED_IDLE:当前CPU空闲，即current是swapper进程。
 *                    NOT_IDLE:当前CPU不空闲。即current不是swapper进程。
 */
static void rebalance_tick(int this_cpu, runqueue_t *this_rq,
			   enum idle_type idle)
{
	unsigned long old_load, this_load;
	unsigned long j = jiffies + CPU_OFFSET(this_cpu);
	struct sched_domain *sd;

	/* Update our load */
	/**
	 * 首先确定运行队列中的进程数。并更新运行队列的平均工作量。
	 */
	old_load = this_rq->cpu_load;
	this_load = this_rq->nr_running * SCHED_LOAD_SCALE;
	/*
	 * Round up the averaging division if load is increasing. This
	 * prevents us from getting stuck on 9 if the load is 10, for
	 * example.
	 */
	if (this_load > old_load)
		old_load++;
	this_rq->cpu_load = (old_load + this_load) / 2;

	/**
	 * 在所有调度域上循环。
	 * 循环路径从基本域(this->sd)到最上层域。
	 */
	for_each_domain(this_cpu, sd) {
		unsigned long interval;

		if (!(sd->flags & SD_LOAD_BALANCE))
			continue;

		/**
		 * 计算当前调度域的平衡间隔时间。由调度域参数及idle标志决定。
		 */
		interval = sd->balance_interval;
		/**
		 * 如果运行队列为空，rebalance_tick可以以很高的频率运行(大概一到两个TICK处理一次对应于逻辑和物理CPU的调度域)。
		 * 否则，将时间间隔乘以busy_factor.大概每10ms处理一次逻辑CPU对应的调度域。每100ms处理一次物理CPU对应的调度域。
		 */ 
		if (idle != SCHED_IDLE)
			interval *= sd->busy_factor;

		/* scale ms to jiffies */
		interval = msecs_to_jiffies(interval);
		if (unlikely(!interval))
			interval = 1;

		/**
		 * 如果已经到达了当前调度域的时限，就调用load_balance，从而在调度域上执行重新平衡的操作。
		 */
		if (j - sd->last_balance >= interval) {
			if (load_balance(this_cpu, this_rq, sd, idle)) {
				/* We've pulled tasks over so no longer idle */
				idle = NOT_IDLE;
			}
			sd->last_balance += interval;
		}
	}
}
#else
/*
 * on UP we do not need to balance between CPUs:
 */
static inline void rebalance_tick(int cpu, runqueue_t *rq, enum idle_type idle)
{
}
static inline void idle_balance(int cpu, runqueue_t *rq)
{
}
#endif

static inline int wake_priority_sleeper(runqueue_t *rq)
{
	int ret = 0;
#ifdef CONFIG_SCHED_SMT
	spin_lock(&rq->lock);
	/*
	 * If an SMT sibling task has been put to sleep for priority
	 * reasons reschedule the idle task to see if it can now run.
	 */
	if (rq->nr_running) {
		resched_task(rq->idle);
		ret = 1;
	}
	spin_unlock(&rq->lock);
#endif
	return ret;
}

DEFINE_PER_CPU(struct kernel_stat, kstat);

EXPORT_PER_CPU_SYMBOL(kstat);

/*
 * We place interactive tasks back into the active array, if possible.
 *
 * To guarantee that this does not starve expired tasks we ignore the
 * interactivity of a task if the first expired task had to wait more
 * than a 'reasonable' amount of time. This deadline timeout is
 * load-dependent, as the frequency of array switched decreases with
 * increasing number of running tasks. We also ignore the interactivity
 * if a better static_prio task has expired:
 */
/**
 * 如果当前进程的静态优先级大于一个过期进程的静态优先级，则返回1
 */
#define EXPIRED_STARVING(rq) \
	((STARVATION_LIMIT && ((rq)->expired_timestamp && \
		(jiffies - (rq)->expired_timestamp >= \
			STARVATION_LIMIT * ((rq)->nr_running) + 1))) || \
			((rq)->curr->static_prio > (rq)->best_expired_prio))

/*
 * Do the virtual cpu time signal calculations.
 * @p: the process that the cpu time gets accounted to
 * @cputime: the cpu time spent in user space since the last update
 */
/**
 * 检查进程定时器，请参见settimer和alarm系统调用
 */
static inline void account_it_virt(struct task_struct * p, cputime_t cputime)
{
	cputime_t it_virt = p->it_virt_value;

	if (cputime_gt(it_virt, cputime_zero) &&
	    cputime_gt(cputime, cputime_zero)) {
		if (cputime_ge(cputime, it_virt)) {
			it_virt = cputime_add(it_virt, p->it_virt_incr);
			send_sig(SIGVTALRM, p, 1);
		}
		it_virt = cputime_sub(it_virt, cputime);
		p->it_virt_value = it_virt;
	}
}

/*
 * Do the virtual profiling signal calculations.
 * @p: the process that the cpu time gets accounted to
 * @cputime: the cpu time spent in user and kernel space since the last update
 */
/**
 * 检查进程定时器，请参见settimer和alarm系统调用
 */
static void account_it_prof(struct task_struct *p, cputime_t cputime)
{
	cputime_t it_prof = p->it_prof_value;

	if (cputime_gt(it_prof, cputime_zero) &&
	    cputime_gt(cputime, cputime_zero)) {
		if (cputime_ge(cputime, it_prof)) {
			it_prof = cputime_add(it_prof, p->it_prof_incr);
			send_sig(SIGPROF, p, 1);
		}
		it_prof = cputime_sub(it_prof, cputime);
		p->it_prof_value = it_prof;
	}
}

/*
 * Check if the process went over its cputime resource limit after
 * some cpu time got added to utime/stime.
 * @p: the process that the cpu time gets accounted to
 * @cputime: the cpu time spent in user and kernel space since the last update
 */
/**
 * 检查是否已经到达CPU时限，如果是，向current进程发送SIGXCPU和SGKILL信号。
 * 这个限制是由进程描述符的signal->rlim[RLIMIT_CPU].rlim_cur字段控制的
 */ 
static void check_rlimit(struct task_struct *p, cputime_t cputime)
{
	cputime_t total, tmp;
	unsigned long secs;

	total = cputime_add(p->utime, p->stime);
	secs = cputime_to_secs(total);
	if (unlikely(secs >= p->signal->rlim[RLIMIT_CPU].rlim_cur)) {
		/* Send SIGXCPU every second. */
		tmp = cputime_sub(total, cputime);
		if (cputime_to_secs(tmp) < secs)
			send_sig(SIGXCPU, p, 1);
		/* and SIGKILL when we go over max.. */
		if (secs >= p->signal->rlim[RLIMIT_CPU].rlim_max)
			send_sig(SIGKILL, p, 1);
	}
}

/*
 * Account user cpu time to a process.
 * @p: the process that the cpu time gets accounted to
 * @hardirq_offset: the offset to subtract from hardirq_count()
 * @cputime: the cpu time spent in user space since the last update
 */
/**
 * 检查进程在用户态下运行了多长时间。
 */
void account_user_time(struct task_struct *p, cputime_t cputime)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime64_t tmp;

	/**
	 * 更新进程描述符的utime字段(进程在用户态下所经过的节拍数)
	 */
	p->utime = cputime_add(p->utime, cputime);

	/* Check for signals (SIGVTALRM, SIGPROF, SIGXCPU & SIGKILL). */
	
	/**
	 * 检查是否已经到达CPU时限，如果是，向current进程发送SIGXCPU和SGKILL信号。
	 * 这个限制是由进程描述符的signal->rlim[RLIMIT_CPU].rlim_cur字段控制的
	 */	
	check_rlimit(p, cputime);
	/**
	 * 检查进程定时器
	 */
	account_it_virt(p, cputime);
	account_it_prof(p, cputime);

	/* Add user time to cpustat. */
	tmp = cputime_to_cputime64(cputime);
	if (TASK_NICE(p) > 0)
		cpustat->nice = cputime64_add(cpustat->nice, tmp);
	else
		cpustat->user = cputime64_add(cpustat->user, tmp);
}

/*
 * Account system cpu time to a process.
 * @p: the process that the cpu time gets accounted to
 * @hardirq_offset: the offset to subtract from hardirq_count()
 * @cputime: the cpu time spent in kernel space since the last update
 */
/**
 * 检查进程在内核态运行了多长时间。
 */
void account_system_time(struct task_struct *p, int hardirq_offset,
			 cputime_t cputime)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	runqueue_t *rq = this_rq();
	cputime64_t tmp;

	/**
	 * 更新进程描述符的stime字段
	 */
	p->stime = cputime_add(p->stime, cputime);

	/* Check for signals (SIGPROF, SIGXCPU & SIGKILL). */
	if (likely(p->signal && p->exit_state < EXIT_ZOMBIE)) {
		/**
		 * 检查是否已经到达CPU时限，如果是，向current进程发送SIGXCPU和SGKILL信号。
		 * 这个限制是由进程描述符的signal->rlim[RLIMIT_CPU].rlim_cur字段控制的
		 */
		check_rlimit(p, cputime);
		account_it_prof(p, cputime);
	}

	/* Add system time to cpustat. */
	tmp = cputime_to_cputime64(cputime);
	if (hardirq_count() - hardirq_offset)
		cpustat->irq = cputime64_add(cpustat->irq, tmp);
	else if (softirq_count())
		cpustat->softirq = cputime64_add(cpustat->softirq, tmp);
	else if (p != rq->idle)
		cpustat->system = cputime64_add(cpustat->system, tmp);
	else if (atomic_read(&rq->nr_iowait) > 0)
		cpustat->iowait = cputime64_add(cpustat->iowait, tmp);
	else
		cpustat->idle = cputime64_add(cpustat->idle, tmp);
}

/*
 * Account for involuntary wait time.
 * @p: the process from which the cpu time has been stolen
 * @steal: the cpu time spent in involuntary wait
 */
void account_steal_time(struct task_struct *p, cputime_t steal)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime64_t tmp = cputime_to_cputime64(steal);
	runqueue_t *rq = this_rq();

	if (p == rq->idle) {
		p->stime = cputime_add(p->stime, steal);
		if (atomic_read(&rq->nr_iowait) > 0)
			cpustat->iowait = cputime64_add(cpustat->iowait, tmp);
		else
			cpustat->idle = cputime64_add(cpustat->idle, tmp);
	} else
		cpustat->steal = cputime64_add(cpustat->steal, tmp);
}

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 *
 * It also gets called by the fork code, when changing the parent's
 * timeslices.
 */
/**
 * 维持当前最新的time_slice计数器
 * 每次时钟节拍到来时，scheduler_tick函数将被调用，以执行与调度相关的操作。
 */
void scheduler_tick(void)
{
	int cpu = smp_processor_id();
	runqueue_t *rq = this_rq();
	task_t *p = current;

	/**
	 * 把转换为纳秒的TSC的当前值存入本地运行队列的timestamp_last_tick中。这个时间戳由sched_clock获得。
	 */
	rq->timestamp_last_tick = sched_clock();

	/**
	 * 检查当前进程是否是idle进程。
	 */
	if (p == rq->idle) {
		/**
		 * 检查运行队列中除了IDLE进程外，是否还有其他可运行进程。
		 * 如果有，就设置当前进程的TIF_NEED_SCHEDULED字段，以强迫进行调度。
		 */
		if (wake_priority_sleeper(rq))
			goto out;
		rebalance_tick(cpu, rq, SCHED_IDLE);
		/**
		 * 没有必要更新IDLE进程的时间片计数器，所以此处直接返回。
		 */
		return;
	}

	/* Task might have expired already, but not scheduled off yet */
	/**
	 * 检查current->array是否指向本地运行队列的活动链表。
	 * 如果不是，说明进程已经过期但还没有被替换，设置TIF_NEED_SCHEDULED标志，以强制进行重新调度。
	 */ 
	if (p->array != rq->active) {
		set_tsk_need_resched(p);
		goto out;
	}
	/**
	 * 获得运行队列的自旋锁。
	 */
	spin_lock(&rq->lock);
	/*
	 * The task was running during this tick - update the
	 * time slice counter. Note: we do not update a thread's
	 * priority until it either goes to sleep or uses up its
	 * timeslice. This makes it possible for interactive tasks
	 * to use up their timeslices at their highest priority levels.
	 */
	/**
	 * 递减当前进程的时间片计数器，并检查是否已经用完时间片。
	 * 由于进程的调度类型不同，函数所执行的操作也有很大差别。
	 */
	if (rt_task(p)) {/* 如果是实时进程，就进一步根据是FIFO还是RR类型的实时进程 */
		/*
		 * RR tasks need a special form of timeslice management.
		 * FIFO tasks have no timeslices.
		 */
		/**
		 * 对SCHED_RR类型的实时进程，需要递减它的时间片。
		 * 对SCHED_FIFO类型的实时进程，什么都不做，退出。
		 */
		if ((p->policy == SCHED_RR) && !--p->time_slice) {
			/**
			 * 对SCHED_RR类型的实时进程，如果它的时间片已经用完，就执行此下动作，以达到抢占当前进程的目的。
			 * 如果必要的话，就尽快抢占。
			 */
			p->time_slice = task_timeslice(p);/* 重新计算它的时间片,它根据进程的静态优先级来计算它的时间片。 */
			/**
			 * 直到这里，说明进程一定不是第一次运行了，它已经用完了一次它的时间片，将first_time_slice置为0.
			 * 这样，它即使退出，也不会将剩余的时间片还给父进程了。
			 */
			p->first_time_slice = 0;
			/**
			 * 设置调度标志，以达到尽快抢占的目的。
			 */
			set_tsk_need_resched(p);

			/* put it at the end of the queue: */
			/**
			 * 将实时进程放到队列末尾。这样，如此链表中还有其他同优先级的RR进程，其他进程就能够得到运行了。
			 */
			requeue_task(p, rq->active);
		}
		goto out_unlock;
	}

	/**
	 * 运行到此，说明进程是普通进程。现在开始更新普通进程的时间片。
	 */
	if (!--p->time_slice) {/* 首先递减普通进程的时间片计数器。如果用完，继续执行以下操作 */
		/**
		 * 既然用完了，就将当前进程从活动集合中摘除。
		 */
		dequeue_task(p, rq->active);
		/**
		 * 当然，当前进程既然已经过期，就必须设置重新调度标志，以便在中断返回前调用schedule选择另外一个进程来运行。
		 */
		set_tsk_need_resched(p);
		/**
		 * 更新当前进程的动态优先级。
		 * effective_prio根据当前进程的static_prio和sleep_avg字段，计算进程的动态优先级。
		 */
		p->prio = effective_prio(p);
		/**
		 * 重填进程的时间片
		 */
		p->time_slice = task_timeslice(p);
		/**
		 * 既然当前进程的一个时间片已经用完，当然就需要清除first_time_slice标志了。
		 */
		p->first_time_slice = 0;

		/**
		 * 如果本地运行队列的expired_timestamp为0,表示过期进程集合为空。
		 * 并且当前进程马上就会变成过期进程，那么将当前jiffies赋给expired_timestamp
		 * expired_timestamp表示当前队列中，过期队列中最老进程被插入过期队列的时间。
		 */
		if (!rq->expired_timestamp)
			rq->expired_timestamp = jiffies;
		/**
		 * 把当前进程插入过期集合或者活动集合。
		 * TASK_INTERACTIVE判断当前进程是否是一个交互式进程。
		 * TASK_INTERACTIVE宏检查运行队列中的第一个过期进程的等待时间是否已经超过1000个时钟节拍乘以运行队列中的可运行进程数+1，如果是返回1.
		 *                     如果当前进程的静态优先级大于过期进程的静态优先级，也返回1.
		 */
		if (!TASK_INTERACTIVE(p) || EXPIRED_STARVING(rq)) {
			/**
			 * 当前进程不是交互式进程，或者过期队列中有优先级更高的进程，那么将当前进程插入到过期队列。
			 */
			enqueue_task(p, rq->expired);
			/**
			 * 如果当前进程是过期队列中优先级最高的低，就更新过期队列的最高优先级。
			 */
			if (p->static_prio < rq->best_expired_prio)
				rq->best_expired_prio = p->static_prio;
		} else
			enqueue_task(p, rq->active);/* 进程是交互式进程，并且比过期队列中所有进程的静态优先级高，那么就将它加到活动队列中。这实际上是对交互式进程的优待。 */
	} else {/* 普通进程的时间片还没有用完，需要进一步检查是否时间片太长 */
		/*
		 * Prevent a too long timeslice allowing a task to monopolize
		 * the CPU. We do this by splitting up the timeslice into
		 * smaller pieces.
		 *
		 * Note: this does not mean the task's timeslices expire or
		 * get lost in any way, they just might be preempted by
		 * another task of equal priority. (one with higher
		 * priority would have preempted this task already.) We
		 * requeue this task to the end of the list on this priority
		 * level, which is in essence a round-robin of tasks with
		 * equal priority.
		 *
		 * This only applies to tasks in the interactive
		 * delta range with at least TIMESLICE_GRANULARITY to requeue.
		 */
		/**
		 * 检查当前进程的时间片是否太长，因为对于交互式进程来说，它时间片用完后，可能会再插入到活动队列，可能导致这种进程的时间片特别长。
		 */
		if (TASK_INTERACTIVE(p) && !((task_timeslice(p) -
			p->time_slice) % TIMESLICE_GRANULARITY(p)) &&
			(p->time_slice >= TIMESLICE_GRANULARITY(p)) &&
			(p->array == rq->active)) {

			requeue_task(p, rq->active);
			set_tsk_need_resched(p);
		}
	}
out_unlock:
	/**
	 * 释放自旋锁。
	 */
	spin_unlock(&rq->lock);
out:
	/**
	 * 调用rebalance_tick函数，该函数应该保证不同CPU的运行队列包含数量基本相同的可运行进程。
	 */
	rebalance_tick(cpu, rq, NOT_IDLE);
}

#ifdef CONFIG_SCHED_SMT
static inline void wake_sleeping_dependent(int this_cpu, runqueue_t *this_rq)
{
	struct sched_domain *sd = this_rq->sd;
	cpumask_t sibling_map;
	int i;

	if (!(sd->flags & SD_SHARE_CPUPOWER))
		return;

	/*
	 * Unlock the current runqueue because we have to lock in
	 * CPU order to avoid deadlocks. Caller knows that we might
	 * unlock. We keep IRQs disabled.
	 */
	spin_unlock(&this_rq->lock);

	sibling_map = sd->span;

	for_each_cpu_mask(i, sibling_map)
		spin_lock(&cpu_rq(i)->lock);
	/*
	 * We clear this CPU from the mask. This both simplifies the
	 * inner loop and keps this_rq locked when we exit:
	 */
	cpu_clear(this_cpu, sibling_map);

	for_each_cpu_mask(i, sibling_map) {
		runqueue_t *smt_rq = cpu_rq(i);

		/*
		 * If an SMT sibling task is sleeping due to priority
		 * reasons wake it up now.
		 */
		if (smt_rq->curr == smt_rq->idle && smt_rq->nr_running)
			resched_task(smt_rq->idle);
	}

	for_each_cpu_mask(i, sibling_map)
		spin_unlock(&cpu_rq(i)->lock);
	/*
	 * We exit with this_cpu's rq still held and IRQs
	 * still disabled:
	 */
}

/**
 * schedule函数会检查运行队列中剩余的可运行进程数。如果有可运行的进程，它用调用dependent_sleeper
 * dependent_sleeper在绝大多数情况下会立即返回0。
 * 但是，如果内核支持超线程技术，函数检查要被选中执行的进程，其优先级是否比已经在相同物理CPU的某个逻辑CPU上运行的兄弟进程优先级低
 * 在这种特殊情况下，schedule拒绝选择低优先级的进程，而去执行swapper进程。
 */
static inline int dependent_sleeper(int this_cpu, runqueue_t *this_rq)
{
	struct sched_domain *sd = this_rq->sd;
	cpumask_t sibling_map;
	prio_array_t *array;
	int ret = 0, i;
	task_t *p;

	/**
	 * 不支持超线程
	 */
	if (!(sd->flags & SD_SHARE_CPUPOWER))
		return 0;

	/*
	 * The same locking rules and details apply as for
	 * wake_sleeping_dependent():
	 */
	spin_unlock(&this_rq->lock);
	sibling_map = sd->span;
	for_each_cpu_mask(i, sibling_map)
		spin_lock(&cpu_rq(i)->lock);
	cpu_clear(this_cpu, sibling_map);

	/*
	 * Establish next task to be run - it might have gone away because
	 * we released the runqueue lock above:
	 */
	if (!this_rq->nr_running)
		goto out_unlock;
	array = this_rq->active;
	if (!array->nr_active)
		array = this_rq->expired;
	BUG_ON(!array->nr_active);

	p = list_entry(array->queue[sched_find_first_bit(array->bitmap)].next,
		task_t, run_list);

	for_each_cpu_mask(i, sibling_map) {
		runqueue_t *smt_rq = cpu_rq(i);
		task_t *smt_curr = smt_rq->curr;

		/*
		 * If a user task with lower static priority than the
		 * running task on the SMT sibling is trying to schedule,
		 * delay it till there is proportionately less timeslice
		 * left of the sibling task to prevent a lower priority
		 * task from using an unfair proportion of the
		 * physical cpu's resources. -ck
		 */
		if (((smt_curr->time_slice * (100 - sd->per_cpu_gain) / 100) >
			task_timeslice(p) || rt_task(smt_curr)) &&
			p->mm && smt_curr->mm && !rt_task(p))
				ret = 1;

		/*
		 * Reschedule a lower priority task on the SMT sibling,
		 * or wake it up if it has been put to sleep for priority
		 * reasons.
		 */
		if ((((p->time_slice * (100 - sd->per_cpu_gain) / 100) >
			task_timeslice(smt_curr) || rt_task(p)) &&
			smt_curr->mm && p->mm && !rt_task(smt_curr)) ||
			(smt_curr == smt_rq->idle && smt_rq->nr_running))
				resched_task(smt_curr);
	}
out_unlock:
	for_each_cpu_mask(i, sibling_map)
		spin_unlock(&cpu_rq(i)->lock);
	return ret;
}
#else
static inline void wake_sleeping_dependent(int this_cpu, runqueue_t *this_rq)
{
}

static inline int dependent_sleeper(int this_cpu, runqueue_t *this_rq)
{
	return 0;
}
#endif

#if defined(CONFIG_PREEMPT) && defined(CONFIG_DEBUG_PREEMPT)

void fastcall add_preempt_count(int val)
{
	/*
	 * Underflow?
	 */
	BUG_ON(((int)preempt_count() < 0));
	preempt_count() += val;
	/*
	 * Spinlock count overflowing soon?
	 */
	BUG_ON((preempt_count() & PREEMPT_MASK) >= PREEMPT_MASK-10);
}
EXPORT_SYMBOL(add_preempt_count);

void fastcall sub_preempt_count(int val)
{
	/*
	 * Underflow?
	 */
	BUG_ON(val > preempt_count());
	/*
	 * Is the spinlock portion underflowing?
	 */
	BUG_ON((val < PREEMPT_MASK) && !(preempt_count() & PREEMPT_MASK));
	preempt_count() -= val;
}
EXPORT_SYMBOL(sub_preempt_count);

#endif

/*
 * schedule() is the main scheduler function.
 */
/**
 * 调度函数。
 */
asmlinkage void __sched schedule(void)
{
	long *switch_count;
	/**
	 * next指向被选中的进程，这个进程将取代当前进程在CPU上执行。
	 * 如果系统中没有优先级高于当前进程，那么next会和current相等。不发生任何切换。
	 */
	task_t *prev, *next;
	runqueue_t *rq;
	prio_array_t *array;
	struct list_head *queue;
	unsigned long long now;
	unsigned long run_time;
	int cpu, idx;

	/*
	 * Test if we are atomic.  Since do_exit() needs to call into
	 * schedule() atomically, we ignore that path for now.
	 * Otherwise, whine if we are scheduling when we should not be.
	 */
	if (likely(!current->exit_state)) {
		if (unlikely(in_atomic())) {
			printk(KERN_ERR "scheduling while atomic: "
				"%s/0x%08x/%d\n",
				current->comm, preempt_count(), current->pid);
			dump_stack();
		}
	}
	profile_hit(SCHED_PROFILING, __builtin_return_address(0));

need_resched:
	/**
	 * 先禁止抢占，再初始化一些变量。
	 * 此处需要禁止抢占，因为后面需要访问任务的运行队列。禁止抢占后可以防止进程飘移。
	 */
	preempt_disable();
	prev = current;
	/**
	 * 释放大内核锁。当内核抢占打开时，并且当前中断正在抢占当前进程，那么会将lock_depth置为-1.
	 * 这样，不会释放内核锁。只有当进程获得了大内核锁并且是主动调度出来时，才会释放锁。
	 * 注意，释放锁并不会修改lock_depth。当进程恢复执行后，如果lock_depth>=0，就会再次获得大内核锁。
	 */
	release_kernel_lock(prev);
need_resched_nonpreemptible:
	rq = this_rq();

	/*
	 * The idle thread is not allowed to schedule!
	 * Remove this check after it has been exercised a bit.
	 */
	if (unlikely(prev == rq->idle) && prev->state != TASK_RUNNING) {
		printk(KERN_ERR "bad: scheduling from the idle thread!\n");
		dump_stack();
	}

	schedstat_inc(rq, sched_cnt);
	/**
	 * 计算当前进程的运行时间。不超过1秒。
	 */
	now = sched_clock();
	if (likely(now - prev->timestamp < NS_MAX_SLEEP_AVG))
		run_time = now - prev->timestamp;
	else
		run_time = NS_MAX_SLEEP_AVG;

	/*
	 * Tasks charged proportionately less run_time at high sleep_avg to
	 * delay them losing their interactive status
	 */
	/**
	 * 对有较长睡眠时间的进程，进行一定奖励。
	 */
	run_time /= (CURRENT_BONUS(prev) ? : 1);

	/**
	 * 在开始寻找可运行进程之前，需要关中断并获得保护运行队列的自旋锁。
	 */
	spin_lock_irq(&rq->lock);

	/**
	 * 当前进程可能是一个正在准备被终止的进程。可能现在是通过do_exit进入schedule函数。
	 */
	if (unlikely(prev->flags & PF_DEAD))
		prev->state = EXIT_DEAD;

	switch_count = &prev->nivcsw;
	/**
	 * 如果进程不是TASK_RUNNING状态，并且没有被内核抢占。就把该进程从运行队列中删除。
	 */ 
	if (prev->state && !(preempt_count() & PREEMPT_ACTIVE)) {
		switch_count = &prev->nvcsw;
		/**
		 * 如果进程是被信号打断的，就将它设置成TASK_RUNNING
		 */
		if (unlikely((prev->state & TASK_INTERRUPTIBLE) &&
				unlikely(signal_pending(prev))))
			prev->state = TASK_RUNNING;
		else {/* 将它从运行队列中删除 */
			if (prev->state == TASK_UNINTERRUPTIBLE)
				rq->nr_uninterruptible++;
			deactivate_task(prev, rq);
		}
	}

	cpu = smp_processor_id();
	/**
	 * 检查是否有可运行的进程。
	 */
	if (unlikely(!rq->nr_running)) {/* 没有了 */
go_idle:
		/**
		 * 运行队列中没有可运行的进程存在，调用idle_balance，从另外一个运行队列迁移一些可运行进程到本地运行队列中。
		 */
		idle_balance(cpu, rq);
		if (!rq->nr_running) {/* 没有迁移新进程到本运行队列。 */
			next = rq->idle;
			rq->expired_timestamp = 0;
			/**
			 * wake_sleeping_dependent重新调度空闲CPU中的可运行进程。主要是处于超线程的情况。
			 */
			wake_sleeping_dependent(cpu, rq);
			/*
			 * wake_sleeping_dependent() might have released
			 * the runqueue, so break out if we got new
			 * tasks meanwhile:
			 */
			if (!rq->nr_running)/* 如果支持超线程，并且其他逻辑CPU也没有可运行进程，那么只好运行IDLE进程了。 */
				goto switch_tasks;
		}
	} else {/* 有可能运行的进程 */
		/**
		 * dependent_sleeper一般返回为0,但是如果内核支持超线程技术，函数检查要被选中执行的进程。
		 * 其优先级是否比当前已经在相同物理CPU的逻辑CPU上运行的兄弟进程的优先级，如果新进程优先级低，就拒绝选择低优先级进程，而去执行swapper进程。
		 */
		if (dependent_sleeper(cpu, rq)) {
			next = rq->idle;
			goto switch_tasks;
		}
		/*
		 * dependent_sleeper() releases and reacquires the runqueue
		 * lock, hence go into the idle loop if the rq went
		 * empty meanwhile:
		 */
		if (unlikely(!rq->nr_running))
			goto go_idle;
	}

	/**
	 * 运行到此，说明运行队列中有线程可被运行。
	 */
	array = rq->active;
	if (unlikely(!array->nr_active)) {
		/**
		 * 活动队列中没有可运行进程了。交换活动集合和过期集合。
		 */
		/*
		 * Switch the active and expired arrays.
		 */
		schedstat_inc(rq, sched_switch);
		rq->active = rq->expired;
		rq->expired = array;
		array = rq->active;
		rq->expired_timestamp = 0;
		rq->best_expired_prio = MAX_PRIO;
	} else
		schedstat_inc(rq, sched_noswitch);

	/**
	 * 现在开始在活动集合中搜索一个可运行的进程。
	 * 首先搜索第一个非0位，并找到对应的链表。
	 */
	idx = sched_find_first_bit(array->bitmap);
	queue = array->queue + idx;
	/**
	 * 将下一个可运行进程描述符放到next中
	 */
	next = list_entry(queue->next, task_t, run_list);

	/**
	 * 如果进程是一个普通进程，并且是从TASK_INTERRUPTIBLE或者TASK_STOPPED状态被唤醒。
	 * 就把自从进程插入运行队列开始所经过的纳秒数加到平均睡眠时间中。
	 */
	if (!rt_task(next) && next->activated > 0) {
		unsigned long long delta = now - next->timestamp;

		/**
		 * 如果是被系统调用服务例程或者内核线程所唤醒，就只增加部分睡眠时间(30%)
		 * 否则增加100%的睡眠时间。这样，交互式进程由于经常被中断打断，它的睡眠时间会增加得更快。
		 */
		if (next->activated == 1)
			delta = delta * (ON_RUNQUEUE_WEIGHT * 128 / 100) / 128;

		array = next->array;
		dequeue_task(next, array);
		recalc_task_prio(next, next->timestamp + delta);
		enqueue_task(next, array);
	}
	next->activated = 0;
switch_tasks:
	/**
	 * 运行到这里，开始进行进程切换了。
	 */
	if (next == rq->idle)
		schedstat_inc(rq, sched_goidle);
	/**
	 * prefetch提示CPU控制单元把next的进程描述符的第一部分字段的内容装入硬件高速缓存。
	 * 这改善了schedule的性能。
	 */
	prefetch(next);
	/**
	 * 清除TIF_NEED_RESCHED标志。
	 */
	clear_tsk_need_resched(prev);
	/**
	 * 记录CPU正在经历静止状态。主要与RCU相关。
	 */
	rcu_qsctr_inc(task_cpu(prev));

	/**
	 * 减少prev的平均睡眠时间
	 */
	prev->sleep_avg -= run_time;
	if ((long)prev->sleep_avg <= 0)
		prev->sleep_avg = 0;
	/**
	 * 更新进程的时间戳
	 */
	prev->timestamp = prev->last_ran = now;

	sched_info_switch(prev, next);
	if (likely(prev != next)) {/* prev和next不同，需要切换 */
		next->timestamp = now;
		rq->nr_switches++;
		rq->curr = next;
		++*switch_count;

		prepare_arch_switch(rq, next);
		/**
		 * context_switch执行真正的进程切换
		 */
		prev = context_switch(rq, prev, next);

		/**
		 * 当进程再次被切换进来后，以下代码被接着运行。
		 * 但是此时prev并不指向当前进程，而是指代码从哪一个进程切换到本进程。
		 * 由于此时已经进行了进程空间的切换，寄存器中缓存的变量等都不再有效，所以用barrier产生一个优化屏障。
		 */
		barrier();

		/**
		 * 对前一个进程进行一些收尾工作，比如减少它的mm_struct,task_struct的引用计数等。
		 */
		finish_task_switch(prev);
	} else/* 如果prev和next是同一个进程，就不做进程切换。当prev仍然是当前活动集合中的最高优先级进程时，这是有可能发生的。 */
		spin_unlock_irq(&rq->lock);

	/**
	 * 在前几句中(context_switch之后)，prev代表的是从哪个进程切换到本进程。
	 * 在继续进行调度之前(因此在context_switch中开了中断，可能刚切回本进程就来了中断，并需要重新调度)，将prev设置成当前进程。
	 */
	prev = current;
	/**
	 * 重新获得大内核锁。
	 */
	if (unlikely(reacquire_kernel_lock(prev) < 0))
		goto need_resched_nonpreemptible;
	/**
	 * 打开抢占，并检查是否需要重新调度。
	 */
	preempt_enable_no_resched();
	if (unlikely(test_thread_flag(TIF_NEED_RESCHED)))
		goto need_resched;
}

EXPORT_SYMBOL(schedule);

#ifdef CONFIG_PREEMPT
/*
 * this is is the entry point to schedule() from in-kernel preemption
 * off of preempt_enable.  Kernel preemptions off return from interrupt
 * occur there and call schedule directly.
 */
asmlinkage void __sched preempt_schedule(void)
{
	struct thread_info *ti = current_thread_info();
#ifdef CONFIG_PREEMPT_BKL
	struct task_struct *task = current;
	int saved_lock_depth;
#endif
	/*
	 * If there is a non-zero preempt_count or interrupts are disabled,
	 * we do not want to preempt the current task.  Just return..
	 */
	/**
	 * 检查是否允许本地中断，当前抢占计数是否为0，两个条件都满足的话，就执行调度
	 * 否则返回。
	 */
	if (unlikely(ti->preempt_count || irqs_disabled()))
		return;

need_resched:
	add_preempt_count(PREEMPT_ACTIVE);
	/*
	 * We keep the big kernel semaphore locked, but we
	 * clear ->lock_depth so that schedule() doesnt
	 * auto-release the semaphore:
	 */
#ifdef CONFIG_PREEMPT_BKL
	saved_lock_depth = task->lock_depth;
	/**
	 * 因为此时不是主动调度出去，而是被抢占出去的，所以临时将lock_depth设置成-1.
	 * 这样schedule函数会误认为没有大内核锁，出就不会up(kernel_sem)了。
	 */
	task->lock_depth = -1;
#endif
	schedule();
#ifdef CONFIG_PREEMPT_BKL
	task->lock_depth = saved_lock_depth;
#endif
	sub_preempt_count(PREEMPT_ACTIVE);

	/* we could miss a preemption opportunity between schedule and now */
	barrier();
	if (unlikely(test_thread_flag(TIF_NEED_RESCHED)))
		goto need_resched;
}

EXPORT_SYMBOL(preempt_schedule);

/*
 * this is is the entry point to schedule() from kernel preemption
 * off of irq context.
 * Note, that this is called and return with irqs disabled. This will
 * protect us against recursive calling from irq.
 */
asmlinkage void __sched preempt_schedule_irq(void)
{
	struct thread_info *ti = current_thread_info();
#ifdef CONFIG_PREEMPT_BKL
	struct task_struct *task = current;
	int saved_lock_depth;
#endif
	/* Catch callers which need to be fixed*/
	BUG_ON(ti->preempt_count || !irqs_disabled());

need_resched:
	add_preempt_count(PREEMPT_ACTIVE);
	/*
	 * We keep the big kernel semaphore locked, but we
	 * clear ->lock_depth so that schedule() doesnt
	 * auto-release the semaphore:
	 */
#ifdef CONFIG_PREEMPT_BKL
	saved_lock_depth = task->lock_depth;
	task->lock_depth = -1;
#endif
	local_irq_enable();
	schedule();
	local_irq_disable();
#ifdef CONFIG_PREEMPT_BKL
	task->lock_depth = saved_lock_depth;
#endif
	sub_preempt_count(PREEMPT_ACTIVE);

	/* we could miss a preemption opportunity between schedule and now */
	barrier();
	if (unlikely(test_thread_flag(TIF_NEED_RESCHED)))
		goto need_resched;
}

#endif /* CONFIG_PREEMPT */

/**
 * 非互斥进程由default_wake_function唤醒。它是try_to_wake_up的一个简单封装。
 */
int default_wake_function(wait_queue_t *curr, unsigned mode, int sync, void *key)
{
	task_t *p = curr->task;
	return try_to_wake_up(p, mode, sync);
}

EXPORT_SYMBOL(default_wake_function);

/*
 * The core wakeup function.  Non-exclusive wakeups (nr_exclusive == 0) just
 * wake everything up.  If it's an exclusive wakeup (nr_exclusive == small +ve
 * number) then we wake all the non-exclusive tasks and one exclusive task.
 *
 * There are circumstances in which we can try to wake a task which has already
 * started to run but is not in state TASK_RUNNING.  try_to_wake_up() returns
 * zero in this (rare) case, and we handle it by continuing to scan the queue.
 */
static void __wake_up_common(wait_queue_head_t *q, unsigned int mode,
			     int nr_exclusive, int sync, void *key)
{
	struct list_head *tmp, *next;

	list_for_each_safe(tmp, next, &q->task_list) {
		wait_queue_t *curr;
		unsigned flags;
		curr = list_entry(tmp, wait_queue_t, task_list);
		flags = curr->flags;
		if (curr->func(curr, mode, sync, key) &&
		    (flags & WQ_FLAG_EXCLUSIVE) &&
		    !--nr_exclusive)
			break;
	}
}

/**
 * __wake_up - wake up threads blocked on a waitqueue.
 * @q: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 */
void fastcall __wake_up(wait_queue_head_t *q, unsigned int mode,
				int nr_exclusive, void *key)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_common(q, mode, nr_exclusive, 0, key);
	spin_unlock_irqrestore(&q->lock, flags);
}

EXPORT_SYMBOL(__wake_up);

/*
 * Same as __wake_up but called with the spinlock in wait_queue_head_t held.
 */
void fastcall __wake_up_locked(wait_queue_head_t *q, unsigned int mode)
{
	__wake_up_common(q, mode, 1, 0, NULL);
}

/**
 * __wake_up - sync- wake up threads blocked on a waitqueue.
 * @q: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 *
 * The sync wakeup differs that the waker knows that it will schedule
 * away soon, so while the target thread will be woken up, it will not
 * be migrated to another CPU - ie. the two threads are 'synchronized'
 * with each other. This can prevent needless bouncing between CPUs.
 *
 * On UP it can prevent extra preemption.
 */
void fastcall __wake_up_sync(wait_queue_head_t *q, unsigned int mode, int nr_exclusive)
{
	unsigned long flags;
	int sync = 1;

	if (unlikely(!q))
		return;

	if (unlikely(!nr_exclusive))
		sync = 0;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_common(q, mode, nr_exclusive, sync, NULL);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL_GPL(__wake_up_sync);	/* For internal use only */

/**
 * 完成原语的up方法。接收completion的地址为参数。
 */
void fastcall complete(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	/**
	 * 递增done并唤醒wait等待队列上睡眠的互斥进程。
	 */
	x->done++;
	__wake_up_common(&x->wait, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE,
			 1, 0, NULL);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete);

void fastcall complete_all(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	x->done += UINT_MAX/2;
	__wake_up_common(&x->wait, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE,
			 0, 0, NULL);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete_all);

/**
 * 完成原语的down操作。
 */
void fastcall __sched wait_for_completion(struct completion *x)
{
	might_sleep();
	/**
	 * 注意锁的用法，这是它与信号量的差别所在。xie.baoyou注
	 * 它的锁保护了整个函数体，并且在complete中也是这样，确保了complete和wait_for_completion在SMP中不会并发执行。
	 * 而信号量主要是保护down
	 */
	spin_lock_irq(&x->wait.lock);
	/**
	 * 判断x->done,这是因为在SMP中，可能有其他进程已经调用了complete。
	 * 必须先检查
	 */
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);

		wait.flags |= WQ_FLAG_EXCLUSIVE;
		/**
		 * 没有up操作，才加入到等待队列上。
		 */
		__add_wait_queue_tail(&x->wait, &wait);
		do {
			__set_current_state(TASK_UNINTERRUPTIBLE);
			spin_unlock_irq(&x->wait.lock);
			schedule();
			spin_lock_irq(&x->wait.lock);
		} while (!x->done);/* 注意:判断条件在自旋锁的保护中 */
		/**
		 * 还在锁的保护中，所以退到外层调用spin_unlock_irq是对的
		 */
		__remove_wait_queue(&x->wait, &wait);
	}
	x->done--;
	spin_unlock_irq(&x->wait.lock);
}
EXPORT_SYMBOL(wait_for_completion);

unsigned long fastcall __sched
wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	might_sleep();

	spin_lock_irq(&x->wait.lock);
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);

		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_tail(&x->wait, &wait);
		do {
			__set_current_state(TASK_UNINTERRUPTIBLE);
			spin_unlock_irq(&x->wait.lock);
			timeout = schedule_timeout(timeout);
			spin_lock_irq(&x->wait.lock);
			if (!timeout) {
				__remove_wait_queue(&x->wait, &wait);
				goto out;
			}
		} while (!x->done);
		__remove_wait_queue(&x->wait, &wait);
	}
	x->done--;
out:
	spin_unlock_irq(&x->wait.lock);
	return timeout;
}
EXPORT_SYMBOL(wait_for_completion_timeout);

int fastcall __sched wait_for_completion_interruptible(struct completion *x)
{
	int ret = 0;

	might_sleep();

	spin_lock_irq(&x->wait.lock);
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);

		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_tail(&x->wait, &wait);
		do {
			if (signal_pending(current)) {
				ret = -ERESTARTSYS;
				__remove_wait_queue(&x->wait, &wait);
				goto out;
			}
			__set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&x->wait.lock);
			schedule();
			spin_lock_irq(&x->wait.lock);
		} while (!x->done);
		__remove_wait_queue(&x->wait, &wait);
	}
	x->done--;
out:
	spin_unlock_irq(&x->wait.lock);

	return ret;
}
EXPORT_SYMBOL(wait_for_completion_interruptible);

unsigned long fastcall __sched
wait_for_completion_interruptible_timeout(struct completion *x,
					  unsigned long timeout)
{
	might_sleep();

	spin_lock_irq(&x->wait.lock);
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);

		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_tail(&x->wait, &wait);
		do {
			if (signal_pending(current)) {
				timeout = -ERESTARTSYS;
				__remove_wait_queue(&x->wait, &wait);
				goto out;
			}
			__set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&x->wait.lock);
			timeout = schedule_timeout(timeout);
			spin_lock_irq(&x->wait.lock);
			if (!timeout) {
				__remove_wait_queue(&x->wait, &wait);
				goto out;
			}
		} while (!x->done);
		__remove_wait_queue(&x->wait, &wait);
	}
	x->done--;
out:
	spin_unlock_irq(&x->wait.lock);
	return timeout;
}
EXPORT_SYMBOL(wait_for_completion_interruptible_timeout);


#define	SLEEP_ON_VAR					\
	unsigned long flags;				\
	wait_queue_t wait;				\
	init_waitqueue_entry(&wait, current);

#define SLEEP_ON_HEAD					\
	spin_lock_irqsave(&q->lock,flags);		\
	__add_wait_queue(q, &wait);			\
	spin_unlock(&q->lock);

#define	SLEEP_ON_TAIL					\
	spin_lock_irq(&q->lock);			\
	__remove_wait_queue(q, &wait);			\
	spin_unlock_irqrestore(&q->lock, flags);

/**
 * 与sleep_on函数是一样的。
 * 但稍有不同，它把当前进程的状态设置为TASK_INTERRUPTIBLE而不是TASK_UNINTERRUPTIBLE。
 * 因此，授受一个信号就可以唤醒当前进程。
 */
void fastcall __sched interruptible_sleep_on(wait_queue_head_t *q)
{
	SLEEP_ON_VAR

	current->state = TASK_INTERRUPTIBLE;

	SLEEP_ON_HEAD
	schedule();
	SLEEP_ON_TAIL
}

EXPORT_SYMBOL(interruptible_sleep_on);

long fastcall __sched interruptible_sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	SLEEP_ON_VAR

	current->state = TASK_INTERRUPTIBLE;

	SLEEP_ON_HEAD
	timeout = schedule_timeout(timeout);
	SLEEP_ON_TAIL

	return timeout;
}

EXPORT_SYMBOL(interruptible_sleep_on_timeout);

/**
 * 该函数把当前进程的状态设置为TASK_UNINTERRUPTIBLE，并把它插入到特定的等待队列。
 * 然后，它调用调度程序，重新开始另外一个程序的执行。
 * 当睡眠进程被唤醒时，调度程序重新开始执行sleep_on函数，把该进程从等待队列中删除。
 */
void fastcall __sched sleep_on(wait_queue_head_t *q)
{
	SLEEP_ON_VAR

	current->state = TASK_UNINTERRUPTIBLE;

	SLEEP_ON_HEAD
	schedule();
	SLEEP_ON_TAIL
}

EXPORT_SYMBOL(sleep_on);

long fastcall __sched sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	SLEEP_ON_VAR

	current->state = TASK_UNINTERRUPTIBLE;

	SLEEP_ON_HEAD
	timeout = schedule_timeout(timeout);
	SLEEP_ON_TAIL

	return timeout;
}

EXPORT_SYMBOL(sleep_on_timeout);

void set_user_nice(task_t *p, long nice)
{
	unsigned long flags;
	prio_array_t *array;
	runqueue_t *rq;
	int old_prio, new_prio, delta;

	if (TASK_NICE(p) == nice || nice < -20 || nice > 19)
		return;
	/*
	 * We have to be careful, if called from sys_setpriority(),
	 * the task might be in the middle of scheduling on another CPU.
	 */
	rq = task_rq_lock(p, &flags);
	/*
	 * The RT priorities are set via sched_setscheduler(), but we still
	 * allow the 'normal' nice value to be set - but as expected
	 * it wont have any effect on scheduling until the task is
	 * not SCHED_NORMAL:
	 */
	if (rt_task(p)) {
		p->static_prio = NICE_TO_PRIO(nice);
		goto out_unlock;
	}
	array = p->array;
	if (array)
		dequeue_task(p, array);

	old_prio = p->prio;
	new_prio = NICE_TO_PRIO(nice);
	delta = new_prio - old_prio;
	p->static_prio = NICE_TO_PRIO(nice);
	p->prio += delta;

	if (array) {
		enqueue_task(p, array);
		/*
		 * If the task increased its priority or is running and
		 * lowered its priority, then reschedule its CPU:
		 */
		if (delta < 0 || (delta > 0 && task_running(rq, p)))
			resched_task(rq->curr);
	}
out_unlock:
	task_rq_unlock(rq, &flags);
}

EXPORT_SYMBOL(set_user_nice);

#ifdef __ARCH_WANT_SYS_NICE

/*
 * sys_nice - change the priority of the current process.
 * @increment: priority increment
 *
 * sys_setpriority is a more generic, but much slower function that
 * does similar things.
 */
/**
 * 处理nice系统调用
 */
asmlinkage long sys_nice(int increment)
{
	int retval;
	long nice;

	/*
	 * Setpriority might change our priority at the same moment.
	 * We don't have to worry. Conceptually one call occurs first
	 * and we have a single winner.
	 */
	/**
	 * 负值相当于请求优先级增加。并请求超级用户特权。
	 */
	if (increment < 0) {
		/**
		 * 调用capable核实进程是否有CAP_SYS_NICE权能。
		 */
		if (!capable(CAP_SYS_NICE))
			return -EPERM;
		if (increment < -40)
			increment = -40;
	}
	/**
	 * 尽管increment参数可以有任何值。但是大于40的值会被截为40。
	 */
	if (increment > 40)
		increment = 40;

	/**
	 * 将进程的static_prio转换为nice值,并加上increment
	 */
	nice = PRIO_TO_NICE(current->static_prio) + increment;
	if (nice < -20)
		nice = -20;
	if (nice > 19)
		nice = 19;

	/**
	 * 调用security_task_setnice安全钩子。
	 */
	retval = security_task_setnice(current, nice);
	if (retval)
		return retval;

	/**
	 * set_user_nice真正设置进程的静态优先级。它首先会获得本地运行队列的锁。
	 * 如果需要，会调用resched_task函数以允许其他进程抢占当前进程。
	 */
	set_user_nice(current, nice);
	return 0;
}

#endif

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * This is the priority value as seen by users in /proc.
 * RT tasks are offset by -200. Normal tasks are centered
 * around 0, value goes from -16 to +15.
 */
int task_prio(const task_t *p)
{
	return p->prio - MAX_RT_PRIO;
}

/**
 * task_nice - return the nice value of a given task.
 * @p: the task in question.
 */
int task_nice(const task_t *p)
{
	return TASK_NICE(p);
}

/*
 * The only users of task_nice are binfmt_elf and binfmt_elf32.
 * binfmt_elf is no longer modular, but binfmt_elf32 still is.
 * Therefore, task_nice is needed if there is a compat_mode.
 */
#ifdef CONFIG_COMPAT
EXPORT_SYMBOL_GPL(task_nice);
#endif

/**
 * idle_cpu - is a given cpu idle currently?
 * @cpu: the processor in question.
 */
int idle_cpu(int cpu)
{
	return cpu_curr(cpu) == cpu_rq(cpu)->idle;
}

EXPORT_SYMBOL_GPL(idle_cpu);

/**
 * idle_task - return the idle task for a given cpu.
 * @cpu: the processor in question.
 */
task_t *idle_task(int cpu)
{
	return cpu_rq(cpu)->idle;
}

/**
 * find_process_by_pid - find a process with a matching PID value.
 * @pid: the pid in question.
 */
static inline task_t *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_pid(pid) : current;
}

/* Actually do priority change: must hold rq lock. */
static void __setscheduler(struct task_struct *p, int policy, int prio)
{
	BUG_ON(p->array);
	p->policy = policy;
	p->rt_priority = prio;
	if (policy != SCHED_NORMAL)
		p->prio = MAX_USER_RT_PRIO-1 - p->rt_priority;
	else
		p->prio = p->static_prio;
}

/**
 * sched_setscheduler - change the scheduling policy and/or RT priority of
 * a thread.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 */
int sched_setscheduler(struct task_struct *p, int policy, struct sched_param *param)
{
	int retval;
	int oldprio, oldpolicy = -1;
	prio_array_t *array;
	unsigned long flags;
	runqueue_t *rq;

recheck:
	/* double check policy once rq lock held */
	if (policy < 0)
		policy = oldpolicy = p->policy;
	else if (policy != SCHED_FIFO && policy != SCHED_RR &&
				policy != SCHED_NORMAL)
			return -EINVAL;
	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are
	 * 1..MAX_USER_RT_PRIO-1, valid priority for SCHED_NORMAL is 0.
	 */
	if (param->sched_priority < 0 ||
	    param->sched_priority > MAX_USER_RT_PRIO-1)
		return -EINVAL;
	if ((policy == SCHED_NORMAL) != (param->sched_priority == 0))
		return -EINVAL;

	if ((policy == SCHED_FIFO || policy == SCHED_RR) &&
	    !capable(CAP_SYS_NICE))
		return -EPERM;
	if ((current->euid != p->euid) && (current->euid != p->uid) &&
	    !capable(CAP_SYS_NICE))
		return -EPERM;

	retval = security_task_setscheduler(p, policy, param);
	if (retval)
		return retval;
	/*
	 * To be able to change p->policy safely, the apropriate
	 * runqueue lock must be held.
	 */
	rq = task_rq_lock(p, &flags);
	/* recheck policy now with rq lock held */
	if (unlikely(oldpolicy != -1 && oldpolicy != p->policy)) {
		policy = oldpolicy = -1;
		task_rq_unlock(rq, &flags);
		goto recheck;
	}
	array = p->array;
	if (array)
		deactivate_task(p, rq);
	oldprio = p->prio;
	__setscheduler(p, policy, param->sched_priority);
	if (array) {
		__activate_task(p, rq);
		/*
		 * Reschedule if we are currently running on this runqueue and
		 * our priority decreased, or if we are not currently running on
		 * this runqueue and our priority is higher than the current's
		 */
		if (task_running(rq, p)) {
			if (p->prio > oldprio)
				resched_task(rq->curr);
		} else if (TASK_PREEMPTS_CURR(p, rq))
			resched_task(rq->curr);
	}
	task_rq_unlock(rq, &flags);
	return 0;
}
EXPORT_SYMBOL_GPL(sched_setscheduler);

/**
 * 实现sys_sched_setscheduler系统调用。
 * 它检查由参数policy指定的调度策略和由参数param->sched_priority指定的新优先级是否有效，
 * 它还检查进程是否有CAP_SYS_NICE权能。或者进程的拥有者是否有超级用户的权限。
 * 如果满足，就把进程从它的运行队列（如果是可运行的）中删除，更新进程的静态优先级。实时优先级和动态优先级。把进程插回到运行队列。
 * 最后，如果需要，就调用resched_task抢占运行队列的当前进程。
 */
static int do_sched_setscheduler(pid_t pid, int policy, struct sched_param __user *param)
{
	int retval;
	struct sched_param lparam;
	struct task_struct *p;

	if (!param || pid < 0)
		return -EINVAL;
	if (copy_from_user(&lparam, param, sizeof(struct sched_param)))
		return -EFAULT;
	read_lock_irq(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (!p) {
		read_unlock_irq(&tasklist_lock);
		return -ESRCH;
	}
	retval = sched_setscheduler(p, policy, &lparam);
	read_unlock_irq(&tasklist_lock);
	return retval;
}

/**
 * sys_sched_setscheduler - set/change the scheduler policy and RT priority
 * @pid: the pid in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 */
asmlinkage long sys_sched_setscheduler(pid_t pid, int policy,
				       struct sched_param __user *param)
{
	return do_sched_setscheduler(pid, policy, param);
}

/**
 * sys_sched_setparam - set/change the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the new RT priority.
 */
asmlinkage long sys_sched_setparam(pid_t pid, struct sched_param __user *param)
{
	return do_sched_setscheduler(pid, -1, param);
}

/**
 * sys_sched_getscheduler - get the policy (scheduling class) of a thread
 * @pid: the pid in question.
 */
asmlinkage long sys_sched_getscheduler(pid_t pid)
{
	int retval = -EINVAL;
	task_t *p;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (p) {
		retval = security_task_getscheduler(p);
		if (!retval)
			retval = p->policy;
	}
	read_unlock(&tasklist_lock);

out_nounlock:
	return retval;
}

/**
 * sys_sched_getscheduler - get the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the RT priority.
 */
asmlinkage long sys_sched_getparam(pid_t pid, struct sched_param __user *param)
{
	struct sched_param lp;
	int retval = -EINVAL;
	task_t *p;

	if (!param || pid < 0)
		goto out_nounlock;

	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	lp.sched_priority = p->rt_priority;
	read_unlock(&tasklist_lock);

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	retval = copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;

out_nounlock:
	return retval;

out_unlock:
	read_unlock(&tasklist_lock);
	return retval;
}

long sched_setaffinity(pid_t pid, cpumask_t new_mask)
{
	task_t *p;
	int retval;

	lock_cpu_hotplug();
	read_lock(&tasklist_lock);

	p = find_process_by_pid(pid);
	if (!p) {
		read_unlock(&tasklist_lock);
		unlock_cpu_hotplug();
		return -ESRCH;
	}

	/*
	 * It is not safe to call set_cpus_allowed with the
	 * tasklist_lock held.  We will bump the task_struct's
	 * usage count and then drop tasklist_lock.
	 */
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	retval = -EPERM;
	if ((current->euid != p->euid) && (current->euid != p->uid) &&
			!capable(CAP_SYS_NICE))
		goto out_unlock;

	retval = set_cpus_allowed(p, new_mask);

out_unlock:
	put_task_struct(p);
	unlock_cpu_hotplug();
	return retval;
}

static int get_user_cpu_mask(unsigned long __user *user_mask_ptr, unsigned len,
			     cpumask_t *new_mask)
{
	if (len < sizeof(cpumask_t)) {
		memset(new_mask, 0, sizeof(cpumask_t));
	} else if (len > sizeof(cpumask_t)) {
		len = sizeof(cpumask_t);
	}
	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}

/**
 * sys_sched_setaffinity - set the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to the new cpu mask
 */
asmlinkage long sys_sched_setaffinity(pid_t pid, unsigned int len,
				      unsigned long __user *user_mask_ptr)
{
	cpumask_t new_mask;
	int retval;

	retval = get_user_cpu_mask(user_mask_ptr, len, &new_mask);
	if (retval)
		return retval;

	return sched_setaffinity(pid, new_mask);
}

/*
 * Represents all cpu's present in the system
 * In systems capable of hotplug, this map could dynamically grow
 * as new cpu's are detected in the system via any platform specific
 * method, such as ACPI for e.g.
 */

cpumask_t cpu_present_map;
EXPORT_SYMBOL(cpu_present_map);

#ifndef CONFIG_SMP
cpumask_t cpu_online_map = CPU_MASK_ALL;
cpumask_t cpu_possible_map = CPU_MASK_ALL;
#endif

long sched_getaffinity(pid_t pid, cpumask_t *mask)
{
	int retval;
	task_t *p;

	lock_cpu_hotplug();
	read_lock(&tasklist_lock);

	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = 0;
	cpus_and(*mask, p->cpus_allowed, cpu_possible_map);

out_unlock:
	read_unlock(&tasklist_lock);
	unlock_cpu_hotplug();
	if (retval)
		return retval;

	return 0;
}

/**
 * sys_sched_getaffinity - get the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to hold the current cpu mask
 */
asmlinkage long sys_sched_getaffinity(pid_t pid, unsigned int len,
				      unsigned long __user *user_mask_ptr)
{
	int ret;
	cpumask_t mask;

	if (len < sizeof(cpumask_t))
		return -EINVAL;

	ret = sched_getaffinity(pid, &mask);
	if (ret < 0)
		return ret;

	if (copy_to_user(user_mask_ptr, &mask, sizeof(cpumask_t)))
		return -EFAULT;

	return sizeof(cpumask_t);
}

/**
 * sys_sched_yield - yield the current processor to other threads.
 *
 * this function yields the current CPU by moving the calling thread
 * to the expired array. If there are no other threads running on this
 * CPU then this function will return.
 */
asmlinkage long sys_sched_yield(void)
{
	runqueue_t *rq = this_rq_lock();
	prio_array_t *array = current->array;
	prio_array_t *target = rq->expired;

	schedstat_inc(rq, yld_cnt);
	/*
	 * We implement yielding by moving the task into the expired
	 * queue.
	 *
	 * (special rule: RT tasks will just roundrobin in the active
	 *  array.)
	 */
	if (rt_task(current))
		target = rq->active;

	if (current->array->nr_active == 1) {
		schedstat_inc(rq, yld_act_empty);
		if (!rq->expired->nr_active)
			schedstat_inc(rq, yld_both_empty);
	} else if (!rq->expired->nr_active)
		schedstat_inc(rq, yld_exp_empty);

	if (array != target) {
		dequeue_task(current, array);
		enqueue_task(current, target);
	} else
		/*
		 * requeue_task is cheaper so perform that if possible.
		 */
		requeue_task(current, array);

	/*
	 * Since we are going to call schedule() anyway, there's
	 * no need to preempt or enable interrupts:
	 */
	__release(rq->lock);
	_raw_spin_unlock(&rq->lock);
	preempt_enable_no_resched();

	schedule();

	return 0;
}

static inline void __cond_resched(void)
{
	do {
		add_preempt_count(PREEMPT_ACTIVE);
		schedule();
		sub_preempt_count(PREEMPT_ACTIVE);
	} while (need_resched());
}

/**
 * 如果有必要，就重新调度
 */
int __sched cond_resched(void)
{
	if (need_resched()) {
		__cond_resched();
		return 1;
	}
	return 0;
}

EXPORT_SYMBOL(cond_resched);

/*
 * cond_resched_lock() - if a reschedule is pending, drop the given lock,
 * call schedule, and on return reacquire the lock.
 *
 * This works OK both with and without CONFIG_PREEMPT.  We do strange low-level
 * operations here to prevent schedule() from being called twice (once via
 * spin_unlock(), once by hand).
 */
int cond_resched_lock(spinlock_t * lock)
{
#if defined(CONFIG_SMP) && defined(CONFIG_PREEMPT)
	if (lock->break_lock) {
		lock->break_lock = 0;
		spin_unlock(lock);
		cpu_relax();
		spin_lock(lock);
	}
#endif
	if (need_resched()) {
		_raw_spin_unlock(lock);
		preempt_enable_no_resched();
		__cond_resched();
		spin_lock(lock);
		return 1;
	}
	return 0;
}

EXPORT_SYMBOL(cond_resched_lock);

int __sched cond_resched_softirq(void)
{
	BUG_ON(!in_softirq());

	if (need_resched()) {
		__local_bh_enable();
		__cond_resched();
		local_bh_disable();
		return 1;
	}
	return 0;
}

EXPORT_SYMBOL(cond_resched_softirq);


/**
 * yield - yield the current processor to other threads.
 *
 * this is a shortcut for kernel-space yielding - it marks the
 * thread runnable and calls sys_sched_yield().
 */
void __sched yield(void)
{
	set_current_state(TASK_RUNNING);
	sys_sched_yield();
}

EXPORT_SYMBOL(yield);

/*
 * This task is about to go to sleep on IO.  Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 *
 * But don't do that if it is a deliberate, throttling IO wait (this task
 * has set its backing_dev_info: the queue against which it should throttle)
 */
void __sched io_schedule(void)
{
	struct runqueue *rq = &per_cpu(runqueues, _smp_processor_id());

	atomic_inc(&rq->nr_iowait);
	schedule();
	atomic_dec(&rq->nr_iowait);
}

EXPORT_SYMBOL(io_schedule);

long __sched io_schedule_timeout(long timeout)
{
	struct runqueue *rq = &per_cpu(runqueues, _smp_processor_id());
	long ret;

	atomic_inc(&rq->nr_iowait);
	ret = schedule_timeout(timeout);
	atomic_dec(&rq->nr_iowait);
	return ret;
}

/**
 * sys_sched_get_priority_max - return maximum RT priority.
 * @policy: scheduling class.
 *
 * this syscall returns the maximum rt_priority that can be used
 * by a given scheduling class.
 */
asmlinkage long sys_sched_get_priority_max(int policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = MAX_USER_RT_PRIO-1;
		break;
	case SCHED_NORMAL:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_get_priority_min - return minimum RT priority.
 * @policy: scheduling class.
 *
 * this syscall returns the minimum rt_priority that can be used
 * by a given scheduling class.
 */
asmlinkage long sys_sched_get_priority_min(int policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	case SCHED_NORMAL:
		ret = 0;
	}
	return ret;
}

/**
 * sys_sched_rr_get_interval - return the default timeslice of a process.
 * @pid: pid of the process.
 * @interval: userspace pointer to the timeslice value.
 *
 * this syscall writes the default timeslice value of a given process
 * into the user-space timespec buffer. A value of '0' means infinity.
 */
asmlinkage
long sys_sched_rr_get_interval(pid_t pid, struct timespec __user *interval)
{
	int retval = -EINVAL;
	struct timespec t;
	task_t *p;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	jiffies_to_timespec(p->policy & SCHED_FIFO ?
				0 : task_timeslice(p), &t);
	read_unlock(&tasklist_lock);
	retval = copy_to_user(interval, &t, sizeof(t)) ? -EFAULT : 0;
out_nounlock:
	return retval;
out_unlock:
	read_unlock(&tasklist_lock);
	return retval;
}

static inline struct task_struct *eldest_child(struct task_struct *p)
{
	if (list_empty(&p->children)) return NULL;
	return list_entry(p->children.next,struct task_struct,sibling);
}

static inline struct task_struct *older_sibling(struct task_struct *p)
{
	if (p->sibling.prev==&p->parent->children) return NULL;
	return list_entry(p->sibling.prev,struct task_struct,sibling);
}

static inline struct task_struct *younger_sibling(struct task_struct *p)
{
	if (p->sibling.next==&p->parent->children) return NULL;
	return list_entry(p->sibling.next,struct task_struct,sibling);
}

static void show_task(task_t * p)
{
	task_t *relative;
	unsigned state;
	unsigned long free = 0;
	static const char *stat_nam[] = { "R", "S", "D", "T", "t", "Z", "X" };

	printk("%-13.13s ", p->comm);
	state = p->state ? __ffs(p->state) + 1 : 0;
	if (state < ARRAY_SIZE(stat_nam))
		printk(stat_nam[state]);
	else
		printk("?");
#if (BITS_PER_LONG == 32)
	if (state == TASK_RUNNING)
		printk(" running ");
	else
		printk(" %08lX ", thread_saved_pc(p));
#else
	if (state == TASK_RUNNING)
		printk("  running task   ");
	else
		printk(" %016lx ", thread_saved_pc(p));
#endif
#ifdef CONFIG_DEBUG_STACK_USAGE
	{
		unsigned long * n = (unsigned long *) (p->thread_info+1);
		while (!*n)
			n++;
		free = (unsigned long) n - (unsigned long)(p->thread_info+1);
	}
#endif
	printk("%5lu %5d %6d ", free, p->pid, p->parent->pid);
	if ((relative = eldest_child(p)))
		printk("%5d ", relative->pid);
	else
		printk("      ");
	if ((relative = younger_sibling(p)))
		printk("%7d", relative->pid);
	else
		printk("       ");
	if ((relative = older_sibling(p)))
		printk(" %5d", relative->pid);
	else
		printk("      ");
	if (!p->mm)
		printk(" (L-TLB)\n");
	else
		printk(" (NOTLB)\n");

	if (state != TASK_RUNNING)
		show_stack(p, NULL);
}

void show_state(void)
{
	task_t *g, *p;

#if (BITS_PER_LONG == 32)
	printk("\n"
	       "                                               sibling\n");
	printk("  task             PC      pid father child younger older\n");
#else
	printk("\n"
	       "                                                       sibling\n");
	printk("  task                 PC          pid father child younger older\n");
#endif
	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take alot of time:
		 */
		touch_nmi_watchdog();
		show_task(p);
	} while_each_thread(g, p);

	read_unlock(&tasklist_lock);
}

void __devinit init_idle(task_t *idle, int cpu)
{
	runqueue_t *rq = cpu_rq(cpu);
	unsigned long flags;

	idle->sleep_avg = 0;
	idle->array = NULL;
	idle->prio = MAX_PRIO;
	idle->state = TASK_RUNNING;
	set_task_cpu(idle, cpu);

	spin_lock_irqsave(&rq->lock, flags);
	rq->curr = rq->idle = idle;
	set_tsk_need_resched(idle);
	spin_unlock_irqrestore(&rq->lock, flags);

	/* Set the preempt count _outside_ the spinlocks! */
#if defined(CONFIG_PREEMPT) && !defined(CONFIG_PREEMPT_BKL)
	idle->thread_info->preempt_count = (idle->lock_depth >= 0);
#else
	idle->thread_info->preempt_count = 0;
#endif
}

/*
 * In a system that switches off the HZ timer nohz_cpu_mask
 * indicates which cpus entered this state. This is used
 * in the rcu update to wait only for active cpus. For system
 * which do not switch off the HZ timer nohz_cpu_mask should
 * always be CPU_MASK_NONE.
 */
cpumask_t nohz_cpu_mask = CPU_MASK_NONE;

#ifdef CONFIG_SMP
/*
 * This is how migration works:
 *
 * 1) we queue a migration_req_t structure in the source CPU's
 *    runqueue and wake up that CPU's migration thread.
 * 2) we down() the locked semaphore => thread blocks.
 * 3) migration thread wakes up (implicitly it forces the migrated
 *    thread off the CPU)
 * 4) it gets the migration request and checks whether the migrated
 *    task is still in the wrong runqueue.
 * 5) if it's in the wrong runqueue then the migration thread removes
 *    it and puts it into the right queue.
 * 6) migration thread up()s the semaphore.
 * 7) we wake up and the migration is done.
 */

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * proper CPU and schedule it away if the CPU it's executing on
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely.  The
 * call is not atomic; no spinlocks may be held.
 */
int set_cpus_allowed(task_t *p, cpumask_t new_mask)
{
	unsigned long flags;
	int ret = 0;
	migration_req_t req;
	runqueue_t *rq;

	rq = task_rq_lock(p, &flags);
	if (!cpus_intersects(new_mask, cpu_online_map)) {
		ret = -EINVAL;
		goto out;
	}

	p->cpus_allowed = new_mask;
	/* Can the task run on the task's current CPU? If so, we're done */
	if (cpu_isset(task_cpu(p), new_mask))
		goto out;

	if (migrate_task(p, any_online_cpu(new_mask), &req)) {
		/* Need help from migration thread: drop lock and wait. */
		task_rq_unlock(rq, &flags);
		wake_up_process(rq->migration_thread);
		wait_for_completion(&req.done);
		tlb_migrate_finish(p->mm);
		return 0;
	}
out:
	task_rq_unlock(rq, &flags);
	return ret;
}

EXPORT_SYMBOL_GPL(set_cpus_allowed);

/*
 * Move (not current) task off this cpu, onto dest cpu.  We're doing
 * this because either it can't run here any more (set_cpus_allowed()
 * away from this CPU, or CPU going down), or because we're
 * attempting to rebalance this task on exec (sched_exec).
 *
 * So we race with normal scheduler movements, but that's OK, as long
 * as the task is no longer on this CPU.
 */
static void __migrate_task(struct task_struct *p, int src_cpu, int dest_cpu)
{
	runqueue_t *rq_dest, *rq_src;

	if (unlikely(cpu_is_offline(dest_cpu)))
		return;

	rq_src = cpu_rq(src_cpu);
	rq_dest = cpu_rq(dest_cpu);

	double_rq_lock(rq_src, rq_dest);
	/* Already moved. */
	if (task_cpu(p) != src_cpu)
		goto out;
	/* Affinity changed (again). */
	if (!cpu_isset(dest_cpu, p->cpus_allowed))
		goto out;

	set_task_cpu(p, dest_cpu);
	if (p->array) {
		/*
		 * Sync timestamp with rq_dest's before activating.
		 * The same thing could be achieved by doing this step
		 * afterwards, and pretending it was a local activate.
		 * This way is cleaner and logically correct.
		 */
		p->timestamp = p->timestamp - rq_src->timestamp_last_tick
				+ rq_dest->timestamp_last_tick;
		deactivate_task(p, rq_src);
		activate_task(p, rq_dest, 0);
		if (TASK_PREEMPTS_CURR(p, rq_dest))
			resched_task(rq_dest->curr);
	}

out:
	double_rq_unlock(rq_src, rq_dest);
}

/*
 * migration_thread - this is a highprio system thread that performs
 * thread migration by bumping thread off CPU then 'pushing' onto
 * another runqueue.
 */
static int migration_thread(void * data)
{
	runqueue_t *rq;
	int cpu = (long)data;

	rq = cpu_rq(cpu);
	BUG_ON(rq->migration_thread != current);

	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		struct list_head *head;
		migration_req_t *req;

		if (current->flags & PF_FREEZE)
			refrigerator(PF_FREEZE);

		spin_lock_irq(&rq->lock);

		if (cpu_is_offline(cpu)) {
			spin_unlock_irq(&rq->lock);
			goto wait_to_die;
		}

		if (rq->active_balance) {
			active_load_balance(rq, cpu);
			rq->active_balance = 0;
		}

		head = &rq->migration_queue;

		if (list_empty(head)) {
			spin_unlock_irq(&rq->lock);
			schedule();
			set_current_state(TASK_INTERRUPTIBLE);
			continue;
		}
		req = list_entry(head->next, migration_req_t, list);
		list_del_init(head->next);

		if (req->type == REQ_MOVE_TASK) {
			spin_unlock(&rq->lock);
			__migrate_task(req->task, cpu, req->dest_cpu);
			local_irq_enable();
		} else if (req->type == REQ_SET_DOMAIN) {
			rq->sd = req->sd;
			spin_unlock_irq(&rq->lock);
		} else {
			spin_unlock_irq(&rq->lock);
			WARN_ON(1);
		}

		complete(&req->done);
	}
	__set_current_state(TASK_RUNNING);
	return 0;

wait_to_die:
	/* Wait for kthread_stop */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/* Figure out where task on dead CPU should go, use force if neccessary. */
static void move_task_off_dead_cpu(int dead_cpu, struct task_struct *tsk)
{
	int dest_cpu;
	cpumask_t mask;

	/* On same node? */
	mask = node_to_cpumask(cpu_to_node(dead_cpu));
	cpus_and(mask, mask, tsk->cpus_allowed);
	dest_cpu = any_online_cpu(mask);

	/* On any allowed CPU? */
	if (dest_cpu == NR_CPUS)
		dest_cpu = any_online_cpu(tsk->cpus_allowed);

	/* No more Mr. Nice Guy. */
	if (dest_cpu == NR_CPUS) {
		cpus_setall(tsk->cpus_allowed);
		dest_cpu = any_online_cpu(tsk->cpus_allowed);

		/*
		 * Don't tell them about moving exiting tasks or
		 * kernel threads (both mm NULL), since they never
		 * leave kernel.
		 */
		if (tsk->mm && printk_ratelimit())
			printk(KERN_INFO "process %d (%s) no "
			       "longer affine to cpu%d\n",
			       tsk->pid, tsk->comm, dead_cpu);
	}
	__migrate_task(tsk, dead_cpu, dest_cpu);
}

/*
 * While a dead CPU has no uninterruptible tasks queued at this point,
 * it might still have a nonzero ->nr_uninterruptible counter, because
 * for performance reasons the counter is not stricly tracking tasks to
 * their home CPUs. So we just add the counter to another CPU's counter,
 * to keep the global sum constant after CPU-down:
 */
static void migrate_nr_uninterruptible(runqueue_t *rq_src)
{
	runqueue_t *rq_dest = cpu_rq(any_online_cpu(CPU_MASK_ALL));
	unsigned long flags;

	local_irq_save(flags);
	double_rq_lock(rq_src, rq_dest);
	rq_dest->nr_uninterruptible += rq_src->nr_uninterruptible;
	rq_src->nr_uninterruptible = 0;
	double_rq_unlock(rq_src, rq_dest);
	local_irq_restore(flags);
}

/* Run through task list and migrate tasks from the dead cpu. */
static void migrate_live_tasks(int src_cpu)
{
	struct task_struct *tsk, *t;

	write_lock_irq(&tasklist_lock);

	do_each_thread(t, tsk) {
		if (tsk == current)
			continue;

		if (task_cpu(tsk) == src_cpu)
			move_task_off_dead_cpu(src_cpu, tsk);
	} while_each_thread(t, tsk);

	write_unlock_irq(&tasklist_lock);
}

/* Schedules idle task to be the next runnable task on current CPU.
 * It does so by boosting its priority to highest possible and adding it to
 * the _front_ of runqueue. Used by CPU offline code.
 */
void sched_idle_next(void)
{
	int cpu = smp_processor_id();
	runqueue_t *rq = this_rq();
	struct task_struct *p = rq->idle;
	unsigned long flags;

	/* cpu has to be offline */
	BUG_ON(cpu_online(cpu));

	/* Strictly not necessary since rest of the CPUs are stopped by now
	 * and interrupts disabled on current cpu.
	 */
	spin_lock_irqsave(&rq->lock, flags);

	__setscheduler(p, SCHED_FIFO, MAX_RT_PRIO-1);
	/* Add idle task to _front_ of it's priority queue */
	__activate_idle_task(p, rq);

	spin_unlock_irqrestore(&rq->lock, flags);
}

/* Ensures that the idle task is using init_mm right before its cpu goes
 * offline.
 */
void idle_task_exit(void)
{
	struct mm_struct *mm = current->active_mm;

	BUG_ON(cpu_online(smp_processor_id()));

	if (mm != &init_mm)
		switch_mm(mm, &init_mm, current);
	mmdrop(mm);
}

static void migrate_dead(unsigned int dead_cpu, task_t *tsk)
{
	struct runqueue *rq = cpu_rq(dead_cpu);

	/* Must be exiting, otherwise would be on tasklist. */
	BUG_ON(tsk->exit_state != EXIT_ZOMBIE && tsk->exit_state != EXIT_DEAD);

	/* Cannot have done final schedule yet: would have vanished. */
	BUG_ON(tsk->flags & PF_DEAD);

	get_task_struct(tsk);

	/*
	 * Drop lock around migration; if someone else moves it,
	 * that's OK.  No task can be added to this CPU, so iteration is
	 * fine.
	 */
	spin_unlock_irq(&rq->lock);
	move_task_off_dead_cpu(dead_cpu, tsk);
	spin_lock_irq(&rq->lock);

	put_task_struct(tsk);
}

/* release_task() removes task from tasklist, so we won't find dead tasks. */
static void migrate_dead_tasks(unsigned int dead_cpu)
{
	unsigned arr, i;
	struct runqueue *rq = cpu_rq(dead_cpu);

	for (arr = 0; arr < 2; arr++) {
		for (i = 0; i < MAX_PRIO; i++) {
			struct list_head *list = &rq->arrays[arr].queue[i];
			while (!list_empty(list))
				migrate_dead(dead_cpu,
					     list_entry(list->next, task_t,
							run_list));
		}
	}
}
#endif /* CONFIG_HOTPLUG_CPU */

/*
 * migration_call - callback that gets triggered when a CPU is added.
 * Here we can start up the necessary migration thread for the new CPU.
 */
static int migration_call(struct notifier_block *nfb, unsigned long action,
			  void *hcpu)
{
	int cpu = (long)hcpu;
	struct task_struct *p;
	struct runqueue *rq;
	unsigned long flags;

	switch (action) {
	case CPU_UP_PREPARE:
		p = kthread_create(migration_thread, hcpu, "migration/%d",cpu);
		if (IS_ERR(p))
			return NOTIFY_BAD;
		p->flags |= PF_NOFREEZE;
		kthread_bind(p, cpu);
		/* Must be high prio: stop_machine expects to yield to it. */
		rq = task_rq_lock(p, &flags);
		__setscheduler(p, SCHED_FIFO, MAX_RT_PRIO-1);
		task_rq_unlock(rq, &flags);
		cpu_rq(cpu)->migration_thread = p;
		break;
	case CPU_ONLINE:
		/* Strictly unneccessary, as first user will wake it. */
		wake_up_process(cpu_rq(cpu)->migration_thread);
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
		/* Unbind it from offline cpu so it can run.  Fall thru. */
		kthread_bind(cpu_rq(cpu)->migration_thread,smp_processor_id());
		kthread_stop(cpu_rq(cpu)->migration_thread);
		cpu_rq(cpu)->migration_thread = NULL;
		break;
	case CPU_DEAD:
		migrate_live_tasks(cpu);
		rq = cpu_rq(cpu);
		kthread_stop(rq->migration_thread);
		rq->migration_thread = NULL;
		/* Idle task back to normal (off runqueue, low prio) */
		rq = task_rq_lock(rq->idle, &flags);
		deactivate_task(rq->idle, rq);
		rq->idle->static_prio = MAX_PRIO;
		__setscheduler(rq->idle, SCHED_NORMAL, 0);
		migrate_dead_tasks(cpu);
		task_rq_unlock(rq, &flags);
		migrate_nr_uninterruptible(rq);
		BUG_ON(rq->nr_running != 0);

		/* No need to migrate the tasks: it was best-effort if
		 * they didn't do lock_cpu_hotplug().  Just wake up
		 * the requestors. */
		spin_lock_irq(&rq->lock);
		while (!list_empty(&rq->migration_queue)) {
			migration_req_t *req;
			req = list_entry(rq->migration_queue.next,
					 migration_req_t, list);
			BUG_ON(req->type != REQ_MOVE_TASK);
			list_del_init(&req->list);
			complete(&req->done);
		}
		spin_unlock_irq(&rq->lock);
		break;
#endif
	}
	return NOTIFY_OK;
}

/* Register at highest priority so that task migration (migrate_all_tasks)
 * happens before everything else.
 */
static struct notifier_block __devinitdata migration_notifier = {
	.notifier_call = migration_call,
	.priority = 10
};

int __init migration_init(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	/* Start one for boot CPU. */
	migration_call(&migration_notifier, CPU_UP_PREPARE, cpu);
	migration_call(&migration_notifier, CPU_ONLINE, cpu);
	register_cpu_notifier(&migration_notifier);
	return 0;
}
#endif

#ifdef CONFIG_SMP
#define SCHED_DOMAIN_DEBUG
#ifdef SCHED_DOMAIN_DEBUG
static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
	int level = 0;

	printk(KERN_DEBUG "CPU%d attaching sched-domain:\n", cpu);

	do {
		int i;
		char str[NR_CPUS];
		struct sched_group *group = sd->groups;
		cpumask_t groupmask;

		cpumask_scnprintf(str, NR_CPUS, sd->span);
		cpus_clear(groupmask);

		printk(KERN_DEBUG);
		for (i = 0; i < level + 1; i++)
			printk(" ");
		printk("domain %d: ", level);

		if (!(sd->flags & SD_LOAD_BALANCE)) {
			printk("does not load-balance\n");
			if (sd->parent)
				printk(KERN_ERR "ERROR: !SD_LOAD_BALANCE domain has parent");
			break;
		}

		printk("span %s\n", str);

		if (!cpu_isset(cpu, sd->span))
			printk(KERN_ERR "ERROR: domain->span does not contain CPU%d\n", cpu);
		if (!cpu_isset(cpu, group->cpumask))
			printk(KERN_ERR "ERROR: domain->groups does not contain CPU%d\n", cpu);

		printk(KERN_DEBUG);
		for (i = 0; i < level + 2; i++)
			printk(" ");
		printk("groups:");
		do {
			if (!group) {
				printk("\n");
				printk(KERN_ERR "ERROR: group is NULL\n");
				break;
			}

			if (!group->cpu_power) {
				printk("\n");
				printk(KERN_ERR "ERROR: domain->cpu_power not set\n");
			}

			if (!cpus_weight(group->cpumask)) {
				printk("\n");
				printk(KERN_ERR "ERROR: empty group\n");
			}

			if (cpus_intersects(groupmask, group->cpumask)) {
				printk("\n");
				printk(KERN_ERR "ERROR: repeated CPUs\n");
			}

			cpus_or(groupmask, groupmask, group->cpumask);

			cpumask_scnprintf(str, NR_CPUS, group->cpumask);
			printk(" %s", str);

			group = group->next;
		} while (group != sd->groups);
		printk("\n");

		if (!cpus_equal(sd->span, groupmask))
			printk(KERN_ERR "ERROR: groups don't span domain->span\n");

		level++;
		sd = sd->parent;

		if (sd) {
			if (!cpus_subset(groupmask, sd->span))
				printk(KERN_ERR "ERROR: parent span is not a superset of domain->span\n");
		}

	} while (sd);
}
#else
#define sched_domain_debug(sd, cpu) {}
#endif

/*
 * Attach the domain 'sd' to 'cpu' as its base domain.  Callers must
 * hold the hotplug lock.
 */
void __devinit cpu_attach_domain(struct sched_domain *sd, int cpu)
{
	migration_req_t req;
	unsigned long flags;
	runqueue_t *rq = cpu_rq(cpu);
	int local = 1;

	sched_domain_debug(sd, cpu);

	spin_lock_irqsave(&rq->lock, flags);

	if (cpu == smp_processor_id() || !cpu_online(cpu)) {
		rq->sd = sd;
	} else {
		init_completion(&req.done);
		req.type = REQ_SET_DOMAIN;
		req.sd = sd;
		list_add(&req.list, &rq->migration_queue);
		local = 0;
	}

	spin_unlock_irqrestore(&rq->lock, flags);

	if (!local) {
		wake_up_process(rq->migration_thread);
		wait_for_completion(&req.done);
	}
}

/* cpus with isolated domains */
cpumask_t __devinitdata cpu_isolated_map = CPU_MASK_NONE;

/* Setup the mask of cpus configured for isolated domains */
static int __init isolated_cpu_setup(char *str)
{
	int ints[NR_CPUS], i;

	str = get_options(str, ARRAY_SIZE(ints), ints);
	cpus_clear(cpu_isolated_map);
	for (i = 1; i <= ints[0]; i++)
		if (ints[i] < NR_CPUS)
			cpu_set(ints[i], cpu_isolated_map);
	return 1;
}

__setup ("isolcpus=", isolated_cpu_setup);

/*
 * init_sched_build_groups takes an array of groups, the cpumask we wish
 * to span, and a pointer to a function which identifies what group a CPU
 * belongs to. The return value of group_fn must be a valid index into the
 * groups[] array, and must be >= 0 and < NR_CPUS (due to the fact that we
 * keep track of groups covered with a cpumask_t).
 *
 * init_sched_build_groups will build a circular linked list of the groups
 * covered by the given span, and will set each group's ->cpumask correctly,
 * and ->cpu_power to 0.
 */
void __devinit init_sched_build_groups(struct sched_group groups[],
			cpumask_t span, int (*group_fn)(int cpu))
{
	struct sched_group *first = NULL, *last = NULL;
	cpumask_t covered = CPU_MASK_NONE;
	int i;

	for_each_cpu_mask(i, span) {
		int group = group_fn(i);
		struct sched_group *sg = &groups[group];
		int j;

		if (cpu_isset(i, covered))
			continue;

		sg->cpumask = CPU_MASK_NONE;
		sg->cpu_power = 0;

		for_each_cpu_mask(j, span) {
			if (group_fn(j) != group)
				continue;

			cpu_set(j, covered);
			cpu_set(j, sg->cpumask);
		}
		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
	}
	last->next = first;
}


#ifdef ARCH_HAS_SCHED_DOMAIN
extern void __devinit arch_init_sched_domains(void);
extern void __devinit arch_destroy_sched_domains(void);
#else
#ifdef CONFIG_SCHED_SMT
/**
 * 如果支持超线程技术，那么cpu_domains就是底层调度域。
 */
static DEFINE_PER_CPU(struct sched_domain, cpu_domains);
static struct sched_group sched_group_cpus[NR_CPUS];
static int __devinit cpu_to_cpu_group(int cpu)
{
	return cpu;
}
#endif

/**
 * 所有物理CPU的调度域描述符。
 */
static DEFINE_PER_CPU(struct sched_domain, phys_domains);
static struct sched_group sched_group_phys[NR_CPUS];
static int __devinit cpu_to_phys_group(int cpu)
{
#ifdef CONFIG_SCHED_SMT
	return first_cpu(cpu_sibling_map[cpu]);
#else
	return cpu;
#endif
}

#ifdef CONFIG_NUMA

static DEFINE_PER_CPU(struct sched_domain, node_domains);
static struct sched_group sched_group_nodes[MAX_NUMNODES];
static int __devinit cpu_to_node_group(int cpu)
{
	return cpu_to_node(cpu);
}
#endif

#if defined(CONFIG_SCHED_SMT) && defined(CONFIG_NUMA)
/*
 * The domains setup code relies on siblings not spanning
 * multiple nodes. Make sure the architecture has a proper
 * siblings map:
 */
static void check_sibling_maps(void)
{
	int i, j;

	for_each_online_cpu(i) {
		for_each_cpu_mask(j, cpu_sibling_map[i]) {
			if (cpu_to_node(i) != cpu_to_node(j)) {
				printk(KERN_INFO "warning: CPU %d siblings map "
					"to different node - isolating "
					"them.\n", i);
				cpu_sibling_map[i] = cpumask_of_cpu(i);
				break;
			}
		}
	}
}
#endif

/*
 * Set up scheduler domains and groups.  Callers must hold the hotplug lock.
 */
static void __devinit arch_init_sched_domains(void)
{
	int i;
	cpumask_t cpu_default_map;

#if defined(CONFIG_SCHED_SMT) && defined(CONFIG_NUMA)
	check_sibling_maps();
#endif
	/*
	 * Setup mask for cpus without special case scheduling requirements.
	 * For now this just excludes isolated cpus, but could be used to
	 * exclude other special cases in the future.
	 */
	cpus_complement(cpu_default_map, cpu_isolated_map);
	cpus_and(cpu_default_map, cpu_default_map, cpu_online_map);

	/*
	 * Set up domains. Isolated domains just stay on the dummy domain.
	 */
	for_each_cpu_mask(i, cpu_default_map) {
		int group;
		struct sched_domain *sd = NULL, *p;
		cpumask_t nodemask = node_to_cpumask(cpu_to_node(i));

		cpus_and(nodemask, nodemask, cpu_default_map);

#ifdef CONFIG_NUMA
		sd = &per_cpu(node_domains, i);
		group = cpu_to_node_group(i);
		*sd = SD_NODE_INIT;
		sd->span = cpu_default_map;
		sd->groups = &sched_group_nodes[group];
#endif

		p = sd;
		sd = &per_cpu(phys_domains, i);
		group = cpu_to_phys_group(i);
		*sd = SD_CPU_INIT;
		sd->span = nodemask;
		sd->parent = p;
		sd->groups = &sched_group_phys[group];

#ifdef CONFIG_SCHED_SMT
		p = sd;
		sd = &per_cpu(cpu_domains, i);
		group = cpu_to_cpu_group(i);
		*sd = SD_SIBLING_INIT;
		sd->span = cpu_sibling_map[i];
		cpus_and(sd->span, sd->span, cpu_default_map);
		sd->parent = p;
		sd->groups = &sched_group_cpus[group];
#endif
	}

#ifdef CONFIG_SCHED_SMT
	/* Set up CPU (sibling) groups */
	for_each_online_cpu(i) {
		cpumask_t this_sibling_map = cpu_sibling_map[i];
		cpus_and(this_sibling_map, this_sibling_map, cpu_default_map);
		if (i != first_cpu(this_sibling_map))
			continue;

		init_sched_build_groups(sched_group_cpus, this_sibling_map,
						&cpu_to_cpu_group);
	}
#endif

	/* Set up physical groups */
	for (i = 0; i < MAX_NUMNODES; i++) {
		cpumask_t nodemask = node_to_cpumask(i);

		cpus_and(nodemask, nodemask, cpu_default_map);
		if (cpus_empty(nodemask))
			continue;

		init_sched_build_groups(sched_group_phys, nodemask,
						&cpu_to_phys_group);
	}

#ifdef CONFIG_NUMA
	/* Set up node groups */
	init_sched_build_groups(sched_group_nodes, cpu_default_map,
					&cpu_to_node_group);
#endif

	/* Calculate CPU power for physical packages and nodes */
	for_each_cpu_mask(i, cpu_default_map) {
		int power;
		struct sched_domain *sd;
#ifdef CONFIG_SCHED_SMT
		sd = &per_cpu(cpu_domains, i);
		power = SCHED_LOAD_SCALE;
		sd->groups->cpu_power = power;
#endif

		sd = &per_cpu(phys_domains, i);
		power = SCHED_LOAD_SCALE + SCHED_LOAD_SCALE *
				(cpus_weight(sd->groups->cpumask)-1) / 10;
		sd->groups->cpu_power = power;

#ifdef CONFIG_NUMA
		if (i == first_cpu(sd->groups->cpumask)) {
			/* Only add "power" once for each physical package. */
			sd = &per_cpu(node_domains, i);
			sd->groups->cpu_power += power;
		}
#endif
	}

	/* Attach the domains */
	for_each_online_cpu(i) {
		struct sched_domain *sd;
#ifdef CONFIG_SCHED_SMT
		sd = &per_cpu(cpu_domains, i);
#else
		sd = &per_cpu(phys_domains, i);
#endif
		cpu_attach_domain(sd, i);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static void __devinit arch_destroy_sched_domains(void)
{
	/* Do nothing: everything is statically allocated. */
}
#endif

#endif /* ARCH_HAS_SCHED_DOMAIN */

/*
 * Initial dummy domain for early boot and for hotplug cpu. Being static,
 * it is initialized to zero, so all balancing flags are cleared which is
 * what we want.
 */
static struct sched_domain sched_domain_dummy;

#ifdef CONFIG_HOTPLUG_CPU
/*
 * Force a reinitialization of the sched domains hierarchy.  The domains
 * and groups cannot be updated in place without racing with the balancing
 * code, so we temporarily attach all running cpus to a "dummy" domain
 * which will prevent rebalancing while the sched domains are recalculated.
 */
static int update_sched_domains(struct notifier_block *nfb,
				unsigned long action, void *hcpu)
{
	int i;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_DOWN_PREPARE:
		for_each_online_cpu(i)
			cpu_attach_domain(&sched_domain_dummy, i);
		arch_destroy_sched_domains();
		return NOTIFY_OK;

	case CPU_UP_CANCELED:
	case CPU_DOWN_FAILED:
	case CPU_ONLINE:
	case CPU_DEAD:
		/*
		 * Fall through and re-initialise the domains.
		 */
		break;
	default:
		return NOTIFY_DONE;
	}

	/* The hotplug lock is already held by cpu_up/cpu_down */
	arch_init_sched_domains();

	return NOTIFY_OK;
}
#endif

void __init sched_init_smp(void)
{
	lock_cpu_hotplug();
	arch_init_sched_domains();
	unlock_cpu_hotplug();
	/* XXX: Theoretical race here - CPU may be hotplugged now */
	hotcpu_notifier(update_sched_domains, 0);
}
#else
void __init sched_init_smp(void)
{
}
#endif /* CONFIG_SMP */

int in_sched_functions(unsigned long addr)
{
	/* Linker adds these: start and end of __sched functions */
	extern char __sched_text_start[], __sched_text_end[];
	return in_lock_functions(addr) ||
		(addr >= (unsigned long)__sched_text_start
		&& addr < (unsigned long)__sched_text_end);
}
//调度器初始化: O(0)调度,新版上使用CFG调度器,struct runqueue rq;
void __init sched_init(void)
{
	runqueue_t *rq;
	int i, j, k;

	for (i = 0; i < NR_CPUS; i++) {
		prio_array_t *array;

		rq = cpu_rq(i);
		spin_lock_init(&rq->lock);
		rq->active = rq->arrays;
		rq->expired = rq->arrays + 1;
		rq->best_expired_prio = MAX_PRIO;

#ifdef CONFIG_SMP
		rq->sd = &sched_domain_dummy;
		rq->cpu_load = 0;
		rq->active_balance = 0;
		rq->push_cpu = 0;
		rq->migration_thread = NULL;
		INIT_LIST_HEAD(&rq->migration_queue);
#endif
		atomic_set(&rq->nr_iowait, 0);

		for (j = 0; j < 2; j++) {
			array = rq->arrays + j;
			for (k = 0; k < MAX_PRIO; k++) {
				INIT_LIST_HEAD(array->queue + k);
				__clear_bit(k, array->bitmap);
			}
			// delimiter for bitsearch
			__set_bit(MAX_PRIO, array->bitmap);
		}
	}

	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	atomic_inc(&init_mm.mm_count);
	enter_lazy_tlb(&init_mm, current);

	/*
	 * Make us the idle thread. Technically, schedule() should not be
	 * called from this thread, however somewhere below it might be,
	 * but because we are the idle thread, we just pick up running again
	 * when this runqueue becomes "idle".
	 */
	init_idle(current, smp_processor_id());
}

#ifdef CONFIG_DEBUG_SPINLOCK_SLEEP
void __might_sleep(char *file, int line)
{
#if defined(in_atomic)
	static unsigned long prev_jiffy;	/* ratelimiting */

	if ((in_atomic() || irqs_disabled()) &&
	    system_state == SYSTEM_RUNNING && !oops_in_progress) {
		if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
			return;
		prev_jiffy = jiffies;
		printk(KERN_ERR "Debug: sleeping function called from invalid"
				" context at %s:%d\n", file, line);
		printk("in_atomic():%d, irqs_disabled():%d\n",
			in_atomic(), irqs_disabled());
		dump_stack();
	}
#endif
}
EXPORT_SYMBOL(__might_sleep);
#endif

#ifdef CONFIG_MAGIC_SYSRQ
void normalize_rt_tasks(void)
{
	struct task_struct *p;
	prio_array_t *array;
	unsigned long flags;
	runqueue_t *rq;

	read_lock_irq(&tasklist_lock);
	for_each_process (p) {
		if (!rt_task(p))
			continue;

		rq = task_rq_lock(p, &flags);

		array = p->array;
		if (array)
			deactivate_task(p, task_rq(p));
		__setscheduler(p, SCHED_NORMAL, 0);
		if (array) {
			__activate_task(p, task_rq(p));
			resched_task(rq->curr);
		}

		task_rq_unlock(rq, &flags);
	}
	read_unlock_irq(&tasklist_lock);
}

#endif /* CONFIG_MAGIC_SYSRQ */
