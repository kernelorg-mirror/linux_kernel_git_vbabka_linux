/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A generic stack depot implementation
 *
 * Author: Alexander Potapenko <glider@google.com>
 * Copyright (C) 2016 Google, Inc.
 *
 * Based on code by Dmitry Chernenkov.
 */

#ifndef _LINUX_STACKDEPOT_H
#define _LINUX_STACKDEPOT_H

#include <linux/gfp.h>

typedef u32 depot_stack_handle_t;

extern bool stack_depot_want_early_init;

depot_stack_handle_t __stack_depot_save(unsigned long *entries,
					unsigned int nr_entries,
					gfp_t gfp_flags, bool can_alloc);

/*
 * Every user of stack depot has to call this during its own init when it's
 * decided that it will be calling stack_depot_save() later.
 *
 * The alternative is to select STACKDEPOT_ALWAYS_INIT to have stack depot
 * enabled as part of mm_init(), for subsystems where it's known at compile time
 * that stack depot will be used.
 *
 * Another alternative is to set stack_depot_want_early_init as true, when the
 * decision to use stack depot is taken e.g. when evaluating kernel boot
 * parameters, which precedes the call to stack_depot_want_early_init().
 */
int stack_depot_init(void);

#ifdef CONFIG_STACKDEPOT
static inline int stack_depot_early_init(void)
{
	if (IS_ENABLED(CONFIG_STACKDEPOT_ALWAYS_INIT)
	    || stack_depot_want_early_init)
		return stack_depot_init();
	return 0;
}
#else
static inline int stack_depot_early_init(void)	{ return 0; }
#endif

depot_stack_handle_t stack_depot_save(unsigned long *entries,
				      unsigned int nr_entries, gfp_t gfp_flags);

unsigned int stack_depot_fetch(depot_stack_handle_t handle,
			       unsigned long **entries);

int stack_depot_snprint(depot_stack_handle_t handle, char *buf, size_t size,
		       int spaces);

void stack_depot_print(depot_stack_handle_t stack);

#endif
