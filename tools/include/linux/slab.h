/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TOOLS_SLAB_H
#define _TOOLS_SLAB_H

#include <linux/types.h>
#include <linux/gfp.h>

#define SLAB_PANIC 2
#define SLAB_RECLAIM_ACCOUNT    0x00020000UL            /* Objects are reclaimable */
#define SLAB_NO_MERGE		0x01000000UL		/* Prevent merging with compatible kmem caches */

#define kzalloc_node(size, flags, node) kmalloc(size, flags)

void *kmalloc(size_t size, gfp_t gfp);
void kfree(void *p);

bool slab_is_available(void);

enum slab_state {
	DOWN,
	PARTIAL,
	PARTIAL_NODE,
	UP,
	FULL
};

static inline void *kzalloc(size_t size, gfp_t gfp)
{
	return kmalloc(size, gfp | __GFP_ZERO);
}

struct list_lru;

void *kmem_cache_alloc_lru(struct kmem_cache *cachep, struct list_lru *, int flags);
static inline void *kmem_cache_alloc(struct kmem_cache *cachep, int flags)
{
	return kmem_cache_alloc_lru(cachep, NULL, flags);
}
void kmem_cache_free(struct kmem_cache *cachep, void *objp);

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
			unsigned int align, unsigned int flags,
			void (*ctor)(void *));

void kmem_cache_free_bulk(struct kmem_cache *cachep, size_t size, void **list);
int kmem_cache_alloc_bulk(struct kmem_cache *cachep, gfp_t gfp, size_t size,
			  void **list);

int kmem_cache_setup_percpu_array(struct kmem_cache *s, unsigned int count);
int kmem_cache_prefill_percpu_array(struct kmem_cache *s, unsigned int count,
		gfp_t gfp);
#endif		/* _TOOLS_SLAB_H */
