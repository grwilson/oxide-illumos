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
 * ktest suite for the rawmem allocator itself, decoupled from the
 * VMM reservoir.
 *
 * Every test here is careful to only ever hold rawmem capacity transiently
 * (alloc, then free before returning), so it never disturbs whatever a real
 * consumer (currently just the VMM reservoir) may be doing with the pool
 * concurrently, beyond briefly borrowing some of its free capacity.
 */

#include <sys/ktest.h>
#include <sys/sysmacros.h>
#include <sys/errno.h>
#include <sys/vmem.h>
#include <sys/rawmem.h>

static void
rawmem_ktest_basic_alloc_free(ktest_ctx_hdl_t *ctx)
{
	pgcnt_t total, free_before;

	rawmem_query(&total, &free_before);
	if (total == 0) {
		KT_SKIP(ctx, "no rawmem reservation configured");
		return;
	}

	const pgcnt_t npages = MIN(free_before, 4);
	if (npages == 0) {
		KT_SKIP(ctx, "rawmem pool already fully consumed");
		return;
	}

	pfn_t pfn;
	KT_EASSERT0(rawmem_alloc(npages, 1, VM_NOSLEEP, &pfn), ctx);

	pgcnt_t free_after;
	rawmem_query(NULL, &free_after);
	KT_ASSERT3UG(free_before - free_after, ==, npages, ctx, cleanup);

	rawmem_free(pfn, npages);

	pgcnt_t free_restored;
	rawmem_query(NULL, &free_restored);
	KT_ASSERT3U(free_restored, ==, free_before, ctx);

	KT_PASS(ctx);
	return;

cleanup:
	rawmem_free(pfn, npages);
}

/*
 * Confirm rawmem_alloc()'s alignment argument is honored, not just its size.
 */
static void
rawmem_ktest_alignment(ktest_ctx_hdl_t *ctx)
{
	pgcnt_t total, free_before;

	rawmem_query(&total, &free_before);
	if (total == 0) {
		KT_SKIP(ctx, "no rawmem reservation configured");
		return;
	}

	const uint_t align_pages = 8;
	const pgcnt_t npages = 2;
	if (free_before < align_pages + npages) {
		KT_SKIP(ctx, "rawmem pool too small for alignment test");
		return;
	}

	pfn_t pfn;
	KT_EASSERT0(rawmem_alloc(npages, align_pages, VM_NOSLEEP, &pfn), ctx);

	KT_ASSERT3UG(pfn % align_pages, ==, 0, ctx, cleanup);

	KT_PASS(ctx);

cleanup:
	rawmem_free(pfn, npages);
}

/*
 * A request larger than the pool's total capacity must fail cleanly, with
 * no effect on the pool's free count.
 */
static void
rawmem_ktest_oversized_enomem(ktest_ctx_hdl_t *ctx)
{
	pgcnt_t total, free_before;

	rawmem_query(&total, &free_before);
	if (total == 0) {
		KT_SKIP(ctx, "no rawmem reservation configured");
		return;
	}

	pfn_t pfn;
	const int err = rawmem_alloc(total + 1, 1, VM_NOSLEEP, &pfn);
	KT_ASSERT3SG(err, ==, ENOMEM, ctx, cleanup);

	pgcnt_t free_after;
	rawmem_query(NULL, &free_after);
	KT_ASSERT3U(free_after, ==, free_before, ctx);

	KT_PASS(ctx);
	return;

cleanup:
	if (err == 0) {
		rawmem_free(pfn, total + 1);
	}
}

/*
 * rawmem_free() should coalesce adjacent ranges back together: two
 * separately allocated chunks that happen to land contiguously, once both
 * are freed, must be reusable as a single larger contiguous allocation --
 * proving the arena doesn't stay needlessly fragmented after ordinary use.
 *
 * Adjacency of the two chunks isn't part of rawmem_alloc()'s contract, just
 * the typical behavior of a freshly-created, lightly used arena, so this
 * skips the coalescing check (without failing) if they don't land that way.
 */
static void
rawmem_ktest_coalesce(ktest_ctx_hdl_t *ctx)
{
	pgcnt_t total, free_before;

	rawmem_query(&total, &free_before);
	if (total == 0) {
		KT_SKIP(ctx, "no rawmem reservation configured");
		return;
	}

	const pgcnt_t npages = MIN(free_before / 2, 4);
	if (npages == 0) {
		KT_SKIP(ctx, "rawmem pool too small for coalesce test");
		return;
	}

	pfn_t pfn_a, pfn_b;
	KT_EASSERT0(rawmem_alloc(npages, 1, VM_NOSLEEP, &pfn_a), ctx);
	if (rawmem_alloc(npages, 1, VM_NOSLEEP, &pfn_b) != 0) {
		KT_ERROR(ctx, "second rawmem_alloc() failed");
		goto cleanup_a;
	}

	if (pfn_b != pfn_a + npages) {
		KT_SKIP(ctx,
		    "allocations did not land contiguously; can't test "
		    "coalescing");
		goto cleanup_both;
	}

	rawmem_free(pfn_a, npages);
	rawmem_free(pfn_b, npages);

	pfn_t pfn_merged;
	KT_ASSERT0(rawmem_alloc(2 * npages, 1, VM_NOSLEEP, &pfn_merged), ctx);
	rawmem_free(pfn_merged, 2 * npages);

	pgcnt_t free_restored;
	rawmem_query(NULL, &free_restored);
	KT_ASSERT3U(free_restored, ==, free_before, ctx);

	KT_PASS(ctx);
	return;

cleanup_both:
	rawmem_free(pfn_b, npages);
cleanup_a:
	rawmem_free(pfn_a, npages);
}

/*
 * Register the "rawmem" suite into an already-created ktest module.  Called
 * from vmm_reservoir_ktest.c's _init(), before that module's own
 * ktest_register_module() call.
 */
int
rawmem_ktest_register(ktest_module_hdl_t *km)
{
	ktest_suite_hdl_t *ks = NULL;

	if (ktest_add_suite(km, "rawmem", &ks) != 0)
		return (-1);
	if (ktest_add_test(ks, "rawmem_ktest_basic_alloc_free",
	    rawmem_ktest_basic_alloc_free, KTEST_FLAG_NONE) != 0)
		return (-1);
	if (ktest_add_test(ks, "rawmem_ktest_alignment",
	    rawmem_ktest_alignment, KTEST_FLAG_NONE) != 0)
		return (-1);
	if (ktest_add_test(ks, "rawmem_ktest_oversized_enomem",
	    rawmem_ktest_oversized_enomem, KTEST_FLAG_NONE) != 0)
		return (-1);
	if (ktest_add_test(ks, "rawmem_ktest_coalesce",
	    rawmem_ktest_coalesce, KTEST_FLAG_NONE) != 0)
		return (-1);

	return (0);
}
