/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RCUPDATE_H
#define _RCUPDATE_H

#include <urcu.h>

#define rcu_dereference_raw(p) rcu_dereference(p)
#define rcu_dereference_protected(p, cond) rcu_dereference(p)
#define rcu_dereference_check(p, cond) rcu_dereference(p)
#define RCU_INIT_POINTER(p, v)	do { (p) = (v); } while (0)

void kmem_cache_free_active(void *objp);
static unsigned long kfree_cb_offset = 0;

static inline void kfree_rcu_cb(struct rcu_head *head)
{
	void *objp = (void *) ((unsigned long)head - kfree_cb_offset);

	kmem_cache_free_active(objp);
}

#ifndef offsetof
#define offsetof(TYPE, MEMBER)	__builtin_offsetof(TYPE, MEMBER)
#endif

#define kfree_rcu(ptr, rhv)						\
do {									\
	if (!kfree_cb_offset)						\
		kfree_cb_offset = offsetof(typeof(*(ptr)), rhv);	\
									\
	call_rcu(&ptr->rhv, kfree_rcu_cb);				\
} while (0)

#endif
