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

#ifndef	_SYS_RAWMEM_H
#define	_SYS_RAWMEM_H

/*
 * Generic, consumer-agnostic allocator over phys_rawmem -- the flat,
 * PFN-range reservation carved out of page_t/memseg management at boot
 * (see PHYS_RAWMEM_SIZE_PROP in sys/bootconf.h, and rawmem_filter()
 * below and avail_filter() in startup.c).  Hands out PFNs directly;
 * callers with no page_t to work with must use hat_kpm_pfn2va() (or
 * similar) if they need a kernel VA, and are responsible for their own
 * zeroing policy -- this layer knows nothing about vnodes, EPT, or any
 * particular consumer.
 *
 * Where phys_rawmem sits in the physical address space:
 *
 *                    high pfn
 *              +----------------------+
 *              |     phys_rawmem      |   rawmem_alloc()/_free()/_query()
 *              |  (no page_t; PFN-    |   operate here.  size = rawmem_pages,
 *              |   addressed only)    |   capped at rawmem_max_pct of total.
 *              +----------------------+
 *              |      phys_avail      |   general pool -- every page here
 *              |  (page_t-backed;     |   still carries a page_t, page-hash
 *              |   general use)       |   bucket, and pse mutex.
 *              +----------------------+
 *              | kernel text/data,    |   trimmed by trim_kernel_range();
 *              | pfn 0 (BIOS-reserved)|   excluded from both pools above.
 *              +----------------------+
 *                    pfn 0
 *
 * Fixed for the life of the boot: the arena is built once, from
 * phys_rawmem, and never resized -- there is no path back into the
 * general page_t-managed pool.
 */

#include <sys/types.h>
#include <sys/memlist.h>
#include <vm/page.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * The flat PFN-range reservation itself, built by startup_memlist() (see
 * rawmem_filter() below) before rawmem_init() ever runs.  NULL if
 * PHYS_RAWMEM_SIZE_PROP was unset or clamped to 0.
 */
extern struct memlist *phys_rawmem;

/*
 * Callback for copy_memlist_filter(), called from startup_memlist() to
 * build phys_rawmem: the flat reservation of the top rawmem_resv pages.
 * Identical on every platform, unlike avail_filter() in startup.c, so
 * it lives here instead.  Unlike avail_filter(), a zero size does not
 * complete the scan -- see the implementation for why.
 */
extern void rawmem_filter(uint64_t *addr, uint64_t *size);

/*
 * Called once, after phys_rawmem exists and after the kmem/vmem
 * allocators are up (i.e. after startup_kmem()), to build the arena.
 * A no-op if phys_rawmem is empty (PHYS_RAWMEM_SIZE_PROP unset or clamped
 * to 0).
 */
extern void rawmem_init(void);

/*
 * Allocate npages contiguous PFNs, aligned to align_pages (1 for no
 * particular alignment).  Returns 0 and sets *pfnp on success, or a
 * nonzero errno (ENOMEM if the arena doesn't have a big enough run,
 * ENXIO if rawmem_init() never built an arena at all) otherwise.
 * vmflag is passed straight through to the underlying vmem_xalloc() (e.g.
 * VM_SLEEP/VM_NOSLEEP).
 */
extern int rawmem_alloc(pgcnt_t npages, uint_t align_pages, int vmflag,
    pfn_t *pfnp);

/*
 * Return npages PFNs starting at pfn (as returned by a prior
 * rawmem_alloc()) to the arena.
 */
extern void rawmem_free(pfn_t pfn, pgcnt_t npages);

/*
 * Report the arena's total and currently-free page counts.  Either
 * pointer may be NULL.  Both report 0 if rawmem_init() never built an
 * arena.
 */
extern void rawmem_query(pgcnt_t *totalp, pgcnt_t *freep);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_RAWMEM_H */
