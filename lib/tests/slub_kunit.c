// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/delay.h>
#include <asm/timex.h>
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
	p[s->offset] = ~p[s->offset];

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
	u8 *p = alloc_hooks(__kmalloc_cache_noprof(s, GFP_KERNEL, 18));

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

struct test_kfree_rcu_struct {
	struct rcu_head rcu;
};

static void test_kfree_rcu(struct kunit *test)
{
	struct kmem_cache *s;
	struct test_kfree_rcu_struct *p;

	if (IS_BUILTIN(CONFIG_SLUB_KUNIT_TEST))
		kunit_skip(test, "can't do kfree_rcu() when test is built-in");

	s = test_kmem_cache_create("TestSlub_kfree_rcu",
				   sizeof(struct test_kfree_rcu_struct),
				   SLAB_NO_MERGE);
	p = kmem_cache_alloc(s, GFP_KERNEL);

	kfree_rcu(p, rcu);
	kmem_cache_destroy(s);

	KUNIT_EXPECT_EQ(test, 0, slab_errors);
}

struct cache_destroy_work {
	struct work_struct work;
	struct kmem_cache *s;
};

static void cache_destroy_workfn(struct work_struct *w)
{
	struct cache_destroy_work *cdw;

	cdw = container_of(w, struct cache_destroy_work, work);
	kmem_cache_destroy(cdw->s);
}

#define KMEM_CACHE_DESTROY_NR 10

static void test_kfree_rcu_wq_destroy(struct kunit *test)
{
	struct test_kfree_rcu_struct *p;
	struct cache_destroy_work cdw;
	struct workqueue_struct *wq;
	struct kmem_cache *s;
	unsigned int delay;
	int i;

	if (IS_BUILTIN(CONFIG_SLUB_KUNIT_TEST))
		kunit_skip(test, "can't do kfree_rcu() when test is built-in");

	INIT_WORK_ONSTACK(&cdw.work, cache_destroy_workfn);
	wq = alloc_workqueue("test_kfree_rcu_destroy_wq",
			WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 0);

	if (!wq)
		kunit_skip(test, "failed to alloc wq");

	for (i = 0; i < KMEM_CACHE_DESTROY_NR; i++) {
		s = test_kmem_cache_create("TestSlub_kfree_rcu_wq_destroy",
				sizeof(struct test_kfree_rcu_struct),
				SLAB_NO_MERGE);

		if (!s)
			kunit_skip(test, "failed to create cache");

		delay = get_random_u8();
		p = kmem_cache_alloc(s, GFP_KERNEL);
		kfree_rcu(p, rcu);

		cdw.s = s;

		msleep(delay);
		queue_work(wq, &cdw.work);
		flush_work(&cdw.work);
	}

	destroy_workqueue(wq);
	KUNIT_EXPECT_EQ(test, 0, slab_errors);
}

static void test_leak_destroy(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_leak_destroy",
							64, SLAB_NO_MERGE);
	kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_destroy(s);

	KUNIT_EXPECT_EQ(test, 2, slab_errors);
}

static void test_krealloc_redzone_zeroing(struct kunit *test)
{
	u8 *p;
	int i;
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_krealloc", 64,
				SLAB_KMALLOC|SLAB_STORE_USER|SLAB_RED_ZONE);

	p = alloc_hooks(__kmalloc_cache_noprof(s, GFP_KERNEL, 48));
	memset(p, 0xff, 48);

	kasan_disable_current();
	OPTIMIZER_HIDE_VAR(p);

	/* Test shrink */
	p = krealloc(p, 40, GFP_KERNEL | __GFP_ZERO);
	for (i = 40; i < 64; i++)
		KUNIT_EXPECT_EQ(test, p[i], SLUB_RED_ACTIVE);

	/* Test grow within the same 64B kmalloc object */
	p = krealloc(p, 56, GFP_KERNEL | __GFP_ZERO);
	for (i = 40; i < 56; i++)
		KUNIT_EXPECT_EQ(test, p[i], 0);
	for (i = 56; i < 64; i++)
		KUNIT_EXPECT_EQ(test, p[i], SLUB_RED_ACTIVE);

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 0, slab_errors);

	memset(p, 0xff, 56);
	/* Test grow with allocating a bigger 128B object */
	p = krealloc(p, 112, GFP_KERNEL | __GFP_ZERO);
	for (i = 0; i < 56; i++)
		KUNIT_EXPECT_EQ(test, p[i], 0xff);
	for (i = 56; i < 112; i++)
		KUNIT_EXPECT_EQ(test, p[i], 0);

	kfree(p);
	kasan_enable_current();
	kmem_cache_destroy(s);
}

static void shuffled_array(unsigned int *list, unsigned int count)
{
	unsigned int rand;
	unsigned int i;

	for (i = 0; i < count; i++)
		list[i] = i;

	/* Fisher-Yates shuffle */
	for (i = count - 1; i > 0; i--) {
		rand = get_random_u32_below(i + 1);
		swap(list[i], list[rand]);
	}
}

static unsigned long long
do_bench(struct kmem_cache *s, int iters, int total, int batch, bool shuffle)
{
	void **ptrs;
	cycles_t start, end;
	cycles_t sum = 0;
	unsigned int *rndidx;

	ptrs = kmalloc_array(batch, sizeof(void *), GFP_KERNEL);
	if (shuffle)
		rndidx = kmalloc_array(batch, sizeof(unsigned int), GFP_KERNEL);

	for (int iter = 0; iter < iters + 1; iter++) {
		if (shuffle)
			shuffled_array(rndidx, batch);

		start = get_cycles();

		if (!shuffle) {
			for (int i = 0; i < total / batch; i++) {
				for (int j = 0; j < batch; j++)
					ptrs[j] = kmem_cache_alloc(s, GFP_KERNEL);
				for (int j = 0; j < batch; j++)
					kmem_cache_free(s, ptrs[j]);
			}
		} else {
			for (int i = 0; i < total / batch; i++) {
				for (int j = 0; j < batch; j++)
					ptrs[j] = kmem_cache_alloc(s, GFP_KERNEL);
				for (int j = 0; j < batch; j++)
					kmem_cache_free(s, ptrs[rndidx[j]]);
			}
		}

		end = get_cycles();

		pr_info("iteration: %02d time %llu\n", iter,
				(unsigned long long) end - start);
		if (iter != 0)
			sum += end - start;
	}

	pr_info("average (excl. iter 0): %llu\n",
			(unsigned long long) sum / iters);

	if (shuffle)
		kfree(rndidx);
	kfree(ptrs);

	kmem_cache_print_stats(s);

	return sum / iters;
}

#ifdef SLUB_HAS_SHEAVES
static void print_improvement(unsigned long long base, unsigned long long sheaves)
{
	bool improved = sheaves < base;
	unsigned long long diff;

	if (improved)
		diff = base - sheaves;
	else
		diff = sheaves - base;

	diff = div_u64(diff * 1000, base);

	pr_info("sheaves %s by %llu.%llu%%\n", improved ? "better" : "worse",
			diff / 10, diff % 10);
}
#endif

static void test_bench(struct kunit *test)
{
	struct kmem_cache *s;
	bool shuffle = false;
	unsigned long long base;
#ifdef SLUB_HAS_SHEAVES
	struct kmem_cache_args args = {
		.sheaf_capacity = 32,
	};
	unsigned long long sheaves;
#endif

shuffled:
	for (int batch = 1; batch <= 1000; batch *= 10) {

		if (batch == 1 && shuffle)
			continue;

		pr_info("\n---------------------------------\n");
		pr_info("BATCH SIZE: %d SHUFFLED: %s\n", batch, shuffle ? "YES" : "NO");
		pr_info("---------------------------------\n\n");

		s = test_kmem_cache_create("TestSlub_bench", 64, SLAB_NO_MERGE);

		pr_info("bench: no memcg, no sheaves\n");
		base = do_bench(s, 10, 1000000, batch, shuffle);

		kmem_cache_destroy(s);

#ifdef SLUB_HAS_SHEAVES
		s = kmem_cache_create("TestSlub_bench", 64, &args, SLAB_NO_MERGE);

		pr_info("bench: no memcg, sheaves\n");
		sheaves = do_bench(s, 10, 1000000, batch, shuffle);

		kmem_cache_destroy(s);

		print_improvement(base, sheaves);
#endif

		s = test_kmem_cache_create("TestSlub_bench", 64, SLAB_NO_MERGE | SLAB_ACCOUNT);

		pr_info("bench: memcg, no sheaves\n");
		base = do_bench(s, 10, 1000000, batch, shuffle);

		kmem_cache_destroy(s);

#ifdef SLUB_HAS_SHEAVES
		s = kmem_cache_create("TestSlub_bench", 64, &args, SLAB_NO_MERGE | SLAB_ACCOUNT);

		pr_info("bench: memcg, sheaves\n");
		sheaves = do_bench(s, 10, 1000000, batch, shuffle);

		kmem_cache_destroy(s);

		print_improvement(base, sheaves);
#endif
	}

	if (!shuffle) {
		shuffle = true;
		goto shuffled;
	}
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
	KUNIT_CASE(test_kfree_rcu),
	KUNIT_CASE(test_kfree_rcu_wq_destroy),
	KUNIT_CASE(test_leak_destroy),
	KUNIT_CASE(test_krealloc_redzone_zeroing),
	KUNIT_CASE(test_bench),
	{}
};

static struct kunit_suite test_suite = {
	.name = "slub_test",
	.init = test_init,
	.test_cases = test_cases,
};
kunit_test_suite(test_suite);

MODULE_DESCRIPTION("Kunit tests for slub allocator");
MODULE_LICENSE("GPL");
