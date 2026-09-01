/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Oxide Computer Company
 */

/*
 * See sys/rawmem.h for the design.  In short: a single vmem arena over
 * PFN-space (quantum 1, so arena "addresses" are literally PFNs), built
 * once from phys_rawmem and never resized.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/sysmacros.h>
#include <sys/vmem.h>
#include <sys/memlist.h>
#include <sys/bootconf.h>
#include <sys/rawmem.h>
#include <vm/page.h>

static vmem_t *rawmem_arena;
static pgcnt_t rawmem_total_pages;

void
rawmem_init(void)
{
	struct memlist *ml;

	/*
	 * phys_rawmem is always a valid, non-NULL pointer once
	 * perform_allocations() has run -- it's part of the unconditionally
	 * sized valloc_base region.  When PHYS_RAWMEM_SIZE_PROP is unset (or
	 * clamped to 0), rawmem_filter() never actually writes any entries
	 * into it, leaving it pointing at raw, zeroed memory (ml_address ==
	 * 0, ml_size == 0).  rawmem_pages -- not phys_rawmem's pointer
	 * value -- is the real signal for "was anything reserved".
	 */
	if (rawmem_pages == 0)
		return;

	rawmem_arena = vmem_create("rawmem", NULL, 0, 1,
	    NULL, NULL, NULL, 0, VM_SLEEP);

	for (ml = phys_rawmem; ml != NULL; ml = ml->ml_next) {
		pgcnt_t pages = btop(ml->ml_size);

		(void) vmem_add(rawmem_arena,
		    (void *)(uintptr_t)btop(ml->ml_address), pages, VM_SLEEP);
		rawmem_total_pages += pages;
	}
}

int
rawmem_alloc(pgcnt_t npages, uint_t align_pages, int vmflag, pfn_t *pfnp)
{
	void *res;

	if (rawmem_arena == NULL)
		return (ENXIO);

	res = vmem_xalloc(rawmem_arena, npages, MAX(align_pages, 1), 0, 0,
	    NULL, NULL, vmflag);
	if (res == NULL)
		return (ENOMEM);

	*pfnp = (pfn_t)(uintptr_t)res;
	return (0);
}

void
rawmem_free(pfn_t pfn, pgcnt_t npages)
{
	vmem_xfree(rawmem_arena, (void *)(uintptr_t)pfn, npages);
}

void
rawmem_query(pgcnt_t *totalp, pgcnt_t *freep)
{
	if (totalp != NULL)
		*totalp = rawmem_arena != NULL ? rawmem_total_pages : 0;
	if (freep != NULL)
		*freep = rawmem_arena != NULL ?
		    vmem_size(rawmem_arena, VMEM_FREE) : 0;
}
