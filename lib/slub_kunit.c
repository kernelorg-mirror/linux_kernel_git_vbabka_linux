// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/percpu_counter.h>
#include <asm/tsc.h>
#include "../mm/slab.h"

static struct kunit_resource resource;
static int slab_errors;

/*
 * Wrapper function for kmem_cache_create(), which reduces 2 parameters:
 * 'align' and 'ctor', and sets SLAB_SKIP_KFENCE flag to avoid getting an
 * object from kfence pool, where the operation could be caught by both
 * our test and kfence sanity check.
 */
static struct kmem_cache *test_kmem_cache_create(const char *name,
				unsigned int size, slab_flags_t flags)
{
	struct kmem_cache *s = kmem_cache_create(name, size, 0,
					(flags | SLAB_NO_USER_FLAGS), NULL);
	s->flags |= SLAB_SKIP_KFENCE;
	return s;
}

static void test_clobber_zone(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_RZ_alloc", 64,
							SLAB_RED_ZONE);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kasan_disable_current();
	p[64] = 0x12;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kasan_enable_current();
	kmem_cache_free(s, p);
	kmem_cache_destroy(s);
}

#ifndef CONFIG_KASAN
static void test_next_pointer(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_next_ptr_free",
							64, SLAB_POISON);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);
	unsigned long tmp;
	unsigned long *ptr_addr;

	kmem_cache_free(s, p);

	ptr_addr = (unsigned long *)(p + s->offset);
	tmp = *ptr_addr;
	p[s->offset] = 0x12;

	/*
	 * Expecting three errors.
	 * One for the corrupted freechain and the other one for the wrong
	 * count of objects in use. The third error is fixing broken cache.
	 */
	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 3, slab_errors);

	/*
	 * Try to repair corrupted freepointer.
	 * Still expecting two errors. The first for the wrong count
	 * of objects in use.
	 * The second error is for fixing broken cache.
	 */
	*ptr_addr = tmp;
	slab_errors = 0;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	/*
	 * Previous validation repaired the count of objects in use.
	 * Now expecting no error.
	 */
	slab_errors = 0;
	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 0, slab_errors);

	kmem_cache_destroy(s);
}

static void test_first_word(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_1th_word_free",
							64, SLAB_POISON);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	*p = 0x78;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kmem_cache_destroy(s);
}

static void test_clobber_50th_byte(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_50th_word_free",
							64, SLAB_POISON);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	p[50] = 0x9a;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kmem_cache_destroy(s);
}
#endif

static void test_clobber_redzone_free(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_RZ_free", 64,
							SLAB_RED_ZONE);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kasan_disable_current();
	kmem_cache_free(s, p);
	p[64] = 0xab;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kasan_enable_current();
	kmem_cache_destroy(s);
}

static void test_kmalloc_redzone_access(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_RZ_kmalloc", 32,
				SLAB_KMALLOC|SLAB_STORE_USER|SLAB_RED_ZONE);
	u8 *p = kmalloc_trace(s, GFP_KERNEL, 18);

	kasan_disable_current();

	/* Suppress the -Warray-bounds warning */
	OPTIMIZER_HIDE_VAR(p);
	p[18] = 0xab;
	p[19] = 0xab;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kasan_enable_current();
	kmem_cache_free(s, p);
	kmem_cache_destroy(s);
}

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

typedef union {
	struct {
		unsigned long counter;
		void *dummy;
	};
	u128 full;
} counter_ptr_t;

struct test_pcp {
	local_lock_t llock;
	spinlock_t slock;
	unsigned long counter;
	counter_ptr_t counter_ptr;
};

static bool __dummy;

static DEFINE_PER_CPU(struct test_pcp, test_pcps) = {
        .llock = INIT_LOCAL_LOCK(llock),
	.slock = __SPIN_LOCK_UNLOCKED(stock_lock),
};

#define TIMING_ITERATIONS 1000000000

static void test_lock_timings(struct kunit *test)
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
	pr_info("%-25s %12llu cycles", "this_cpu_inc_return", after - before);

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
	pr_info("%-25s %12llu cycles", "this_cpu_try_cmpxchg", after - before);

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
	pr_info("%-25s %12llu cycles", "raw+this_cpu_try_cmpxchg", after - before);

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
	pr_info("%-25s %12llu cycles", "this_cpu_try_cmpxchg128", after - before);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		percpu_counter_inc(&pcpc);
	}

	after = rdtsc_ordered();

	cond_resched();
	pr_info("%-25s %12llu cycles", "percpu_counter_inc", after - before);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		percpu_counter_add_local(&pcpc, 1);
	}

	after = rdtsc_ordered();

	cond_resched();
	pr_info("%-25s %12llu cycles", "percpu_counter_inc_local", after - before);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		local_lock(&test_pcps.llock);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		local_unlock(&test_pcps.llock);
	}

	after = rdtsc_ordered();

	cond_resched();
	pr_info("%-25s %12llu cycles", "local_lock", after - before);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		if (likely(!in_interrupt())) {
			local_lock(&test_pcps.llock);

			pcp = this_cpu_ptr(&test_pcps);

			pcp->counter++;

			local_unlock(&test_pcps.llock);
		}
	}

	after = rdtsc_ordered();

	cond_resched();
	pr_info("%-25s %12llu cycles", "local_lock+in_intr()", after - before);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		local_lock_irq(&test_pcps.llock);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		local_unlock_irq(&test_pcps.llock);
	}

	after = rdtsc_ordered();

	cond_resched();
	pr_info("%-25s %12llu cycles", "local_lock_irq", after - before);

	before = rdtsc_ordered();

	for (unsigned long i = 0; i < TIMING_ITERATIONS; i++) {
		local_lock_irqsave(&test_pcps.llock, flags);

		pcp = this_cpu_ptr(&test_pcps);

		pcp->counter++;

		local_unlock_irqrestore(&test_pcps.llock, flags);
	}

	after = rdtsc_ordered();

	cond_resched();
	pr_info("%-25s %12llu cycles", "local_lock_irqsave", after - before);

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
	pr_info("%-25s %12llu cycles", "pcpu_spin_trylock", after - before);

	percpu_counter_destroy(&pcpc);
}

static int test_init(struct kunit *test)
{
	slab_errors = 0;

	kunit_add_named_resource(test, NULL, NULL, &resource,
					"slab_errors", &slab_errors);
	return 0;
}

static struct kunit_case test_cases[] = {
	KUNIT_CASE(test_clobber_zone),

#ifndef CONFIG_KASAN
	KUNIT_CASE(test_next_pointer),
	KUNIT_CASE(test_first_word),
	KUNIT_CASE(test_clobber_50th_byte),
#endif

	KUNIT_CASE(test_clobber_redzone_free),
	KUNIT_CASE(test_kmalloc_redzone_access),

	KUNIT_CASE(test_lock_timings),
	{}
};

static struct kunit_suite test_suite = {
	.name = "slub_test",
	.init = test_init,
	.test_cases = test_cases,
};
kunit_test_suite(test_suite);

MODULE_LICENSE("GPL");
