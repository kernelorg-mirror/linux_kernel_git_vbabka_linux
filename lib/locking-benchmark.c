// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/percpu_counter.h>
#include <asm/tsc.h>

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/*
 * On SMP, spin_trylock is sufficient protection.
 * On PREEMPT_RT, spin_trylock is equivalent on both SMP and UP.
 */
#define pcp_trylock_prepare(flags)	do { } while (0)
#define pcp_trylock_finish(flag)	do { } while (0)
#else

/* UP spin_trylock always succeeds so disable IRQs to prevent re-entrancy. */
#define pcp_trylock_prepare(flags)	local_irq_save(flags)
#define pcp_trylock_finish(flags)	local_irq_restore(flags)
#endif

/*
 * Locking a pcp requires a PCP lookup followed by a spinlock. To avoid
 * a migration causing the wrong PCP to be locked and remote memory being
 * potentially allocated, pin the task to the CPU for the lookup+lock.
 * preempt_disable is used on !RT because it is faster than migrate_disable.
 * migrate_disable is used on RT because otherwise RT spinlock usage is
 * interfered with and a high priority task cannot preempt the allocator.
 */
#ifndef CONFIG_PREEMPT_RT
#define pcpu_task_pin()		preempt_disable()
#define pcpu_task_unpin()	preempt_enable()
#else
#define pcpu_task_pin()		migrate_disable()
#define pcpu_task_unpin()	migrate_enable()
#endif

/*
 * Generic helper to lookup and a per-cpu variable with an embedded spinlock.
 * Return value should be used with equivalent unlock helper.
 */
#define pcpu_spin_lock(type, member, ptr)				\
({									\
	type *_ret;							\
	pcpu_task_pin();						\
	_ret = this_cpu_ptr(ptr);					\
	spin_lock(&_ret->member);					\
	_ret;								\
})

#define pcpu_spin_trylock(type, member, ptr)				\
({									\
	type *_ret;							\
	pcpu_task_pin();						\
	_ret = this_cpu_ptr(ptr);					\
	if (!spin_trylock(&_ret->member)) {				\
		pcpu_task_unpin();					\
		_ret = NULL;						\
	}								\
	_ret;								\
})

#define pcpu_spin_unlock(member, ptr)					\
({									\
	spin_unlock(&ptr->member);					\
	pcpu_task_unpin();						\
})

/* struct per_cpu_pages specific helpers. */
#define pcp_spin_lock(ptr)						\
	pcpu_spin_lock(struct per_cpu_pages, lock, ptr)

#define pcp_spin_trylock(ptr)						\
	pcpu_spin_trylock(struct per_cpu_pages, lock, ptr)

#define pcp_spin_unlock(ptr)						\
	pcpu_spin_unlock(lock, ptr)

typedef union {
	struct {
		unsigned long counter;
		void *dummy;
	};
	u128 full;
} counter_ptr_t;

struct test_pcp {
	local_lock_t llock;
	localtry_lock_t ltlock;
	spinlock_t slock;
	unsigned long counter;
	counter_ptr_t counter_ptr;
};

static bool __dummy;

static DEFINE_PER_CPU(struct test_pcp, test_pcps) = {
        .llock = INIT_LOCAL_LOCK(llock),
	.ltlock = INIT_LOCALTRY_LOCK(ltlock),
	.slock = __SPIN_LOCK_UNLOCKED(slock),
};

static counter_ptr_t counter_ptr;

struct test_bsl {
	unsigned long page_flags;
	unsigned long counter;
};

static struct test_bsl bsl = {};

#define TIMING_ITERATIONS 1000000000

#define print_result(name) \
	pr_info("%-35s %12llu cycles\n", name, after - before)

static int __init locking_bench(void)
{
	unsigned long long before, after;
	unsigned long __maybe_unused UP_flags;
	struct test_pcp *pcp;
	struct percpu_counter pcpc;
	unsigned long flags;

	percpu_counter_init(&pcpc, 0, GFP_KERNEL);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		if (this_cpu_inc_return(test_pcps.counter) == 0)
			__dummy = true;
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("this_cpu_inc_return");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		unsigned long old, new;
		do {
			old = this_cpu_read(test_pcps.counter);
			new = old + 1;
		} while (!this_cpu_try_cmpxchg(test_pcps.counter, &old, new));
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("this_cpu_try_cmpxchg");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		unsigned long old, new;
		do {
			old = raw_cpu_read(test_pcps.counter);
			new = old + 1;
		} while (!this_cpu_try_cmpxchg(test_pcps.counter, &old, new));
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("raw+this_cpu_try_cmpxchg");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		counter_ptr_t old, new;
		do {
			struct test_pcp *pcp = raw_cpu_ptr(&test_pcps);
			old.full = pcp->counter_ptr.full;
			new.counter = old.counter + 1;
			new.dummy = old.dummy;
		} while (!this_cpu_try_cmpxchg128(test_pcps.counter_ptr.full,
						  &old.full, new.full));
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("this_cpu_try_cmpxchg128");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		counter_ptr_t *test;
		counter_ptr_t old, new;
		do {
			test = &counter_ptr;
			old.full = test->full;
			new.counter = old.counter + 1;
			new.dummy = old.dummy;
		} while (!try_cmpxchg128(&test->full,
					 &old.full, new.full));
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("try_cmpxchg128");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		struct test_bsl *test = &bsl;

		bit_spin_lock(PG_locked, &test->page_flags);
		test->counter++;
		bit_spin_unlock(PG_locked, &test->page_flags);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("bit_spin_lock");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		percpu_counter_inc(&pcpc);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("percpu_counter_inc");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		percpu_counter_add_local(&pcpc, 1);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("percpu_counter_inc_local");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		local_lock(&test_pcps.llock);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		local_unlock(&test_pcps.llock);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("local_lock");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		local_lock_irq(&test_pcps.llock);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		local_unlock_irq(&test_pcps.llock);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("local_lock_irq");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		local_lock_irqsave(&test_pcps.llock, flags);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		local_unlock_irqrestore(&test_pcps.llock, flags);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("local_lock_irqsave");


	before = rdtsc_ordered();

	for (unsigned j = 0; j < 10; j++) {

		local_irq_disable();

		for (unsigned long i = 0; i < TIMING_ITERATIONS/10; i++) {
			local_lock_irqsave(&test_pcps.llock, flags);

			pcp = this_cpu_ptr(&test_pcps);

			pcp->counter++;

			local_unlock_irqrestore(&test_pcps.llock, flags);
		}

		local_irq_enable();
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("irq_dis(local_lock_irqsave)");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		localtry_lock(&test_pcps.ltlock);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		localtry_unlock(&test_pcps.ltlock);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("localtry_lock");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		localtry_lock_irq(&test_pcps.ltlock);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		localtry_unlock_irq(&test_pcps.ltlock);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("localtry_lock_irq");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		localtry_lock_irqsave(&test_pcps.ltlock, flags);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		localtry_unlock_irqrestore(&test_pcps.ltlock, flags);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("localtry_lock_irqsave");

	before = rdtsc_ordered();

	for (unsigned j = 0; j < 10; j++) {

		local_irq_disable();

		for (unsigned long i = 0; i < TIMING_ITERATIONS/10; i++) {
			localtry_lock_irqsave(&test_pcps.ltlock, flags);

			pcp = this_cpu_ptr(&test_pcps);

			pcp->counter++;

			localtry_unlock_irqrestore(&test_pcps.ltlock, flags);
		}

		local_irq_enable();
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("irq_dis(localtry_lock_irqsave)");

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {

		pcp_trylock_prepare(UP_flags);

		pcp = pcpu_spin_trylock(struct test_pcp, slock, &test_pcps);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		pcpu_spin_unlock(slock, pcp);
		pcp_trylock_finish(UP_flags);
	}

	after = rdtsc_ordered();

	cond_resched();
	print_result("pcpu_spin_trylock");

	percpu_counter_destroy(&pcpc);

	/*
	 * Everything is OK. Return error just to let user run benchmark
	 * again without annoying rmmod.
	 */
	return -EINVAL;
}

module_init(locking_bench);

MODULE_DESCRIPTION("Benchmark for (pcp) locking schemes");
MODULE_LICENSE("GPL");
