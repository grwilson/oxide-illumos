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
 * ktest suite for the VMM memory reservoir (see vmm_reservoir.c), with a
 * particular focus on its bootmem-tiered sourcing (see sys/bootmem.h).
 *
 * These tests exercise the reservoir purely through its public API
 * (vmmr_alloc()/vmmr_free()/vmmr_region_pfn_at()), using transient
 * allocations to grow and shrink it on demand via the same
 * vmmr_add()/vmmr_remove() paths that back the VMM_RESV_SET_TARGET ioctl.
 * That API is fully usable the moment the vmm module is loaded -- unlike
 * the vmmctl device node, it does not require HMA/VT-x/AMD-V support or a
 * successful driver attach, which makes it usable for testing on systems
 * (e.g. nested VMs without exposed hardware virtualization) where
 * /dev/vmmctl is never created.
 */

#include <sys/ktest.h>
#include <sys/modctl.h>
#include <sys/sysmacros.h>
#include <sys/bootmem.h>
#include <sys/vmm_reservoir.h>

static void
vmmr_ktest_basic_alloc_free(ktest_ctx_hdl_t *ctx)
{
	const uint_t npages = 4;
	const size_t sz = npages << PAGESHIFT;
	vmmr_region_t *region = NULL;

	KT_EASSERT0(vmmr_alloc(sz, true, &region), ctx);

	pfn_t pfns[4];
	for (uint_t i = 0; i < npages; i++) {
		pfns[i] = vmmr_region_pfn_at(region, i << PAGESHIFT);
	}
	for (uint_t i = 0; i < npages; i++) {
		for (uint_t j = i + 1; j < npages; j++) {
			if (pfns[i] == pfns[j]) {
				KT_FAIL(ctx,
				    "duplicate pfn %lu at offsets %u and %u",
				    pfns[i], i, j);
				vmmr_free(region);
				return;
			}
		}
	}

	vmmr_free(region);
	KT_ASSERT(vmmr_is_empty(), ctx);

	KT_PASS(ctx);
}

/*
 * Grow the reservoir by an amount that fits within the bootmem pool's
 * current free capacity, and confirm that capacity drops by exactly that
 * amount -- proving the growth was actually sourced from bootmem, rather
 * than merely succeeding via the ordinary page_t-backed paths.  Shrinking
 * the reservoir back down should then restore the original free capacity.
 */
static void
vmmr_ktest_bootmem_engaged(ktest_ctx_hdl_t *ctx)
{
	pgcnt_t total, free_before;

	bootmem_query(&total, &free_before);
	if (total == 0) {
		KT_SKIP(ctx, "no bootmem reservation configured");
		return;
	}
	if (free_before == 0) {
		KT_SKIP(ctx, "bootmem pool already fully consumed");
		return;
	}

	const pgcnt_t req_pages = MIN(free_before, 64);
	const size_t sz = req_pages << PAGESHIFT;

	vmmr_region_t *region = NULL;
	KT_EASSERT0(vmmr_alloc(sz, true, &region), ctx);

	/* Touch the endpoints to exercise the bootmem PFN-lookup path. */
	(void) vmmr_region_pfn_at(region, 0);
	(void) vmmr_region_pfn_at(region, sz - (1 << PAGESHIFT));

	pgcnt_t free_after;
	bootmem_query(NULL, &free_after);
	KT_ASSERT3UG(free_before - free_after, ==, req_pages, ctx, cleanup);

	vmmr_free(region);
	region = NULL;

	pgcnt_t free_restored;
	bootmem_query(NULL, &free_restored);
	KT_ASSERT3U(free_restored, ==, free_before, ctx);

	KT_PASS(ctx);
	return;

cleanup:
	vmmr_free(region);
}

/*
 * Grow the reservoir by more than the bootmem pool's total capacity, and
 * confirm the allocation still succeeds (the excess falling through to the
 * ordinary page_t-backed paths) while bootmem's free capacity bottoms out
 * at zero rather than producing an error.
 */
static void
vmmr_ktest_bootmem_overflow(ktest_ctx_hdl_t *ctx)
{
	pgcnt_t total, free_before;

	bootmem_query(&total, &free_before);
	if (total == 0) {
		KT_SKIP(ctx, "no bootmem reservation configured");
		return;
	}

	const pgcnt_t req_pages = total + 16;
	const size_t sz = req_pages << PAGESHIFT;

	vmmr_region_t *region = NULL;
	const int err = vmmr_alloc(sz, true, &region);
	if (err != 0) {
		KT_SKIP(ctx, "reservoir limit too small for overflow test");
		return;
	}

	pgcnt_t free_after;
	bootmem_query(NULL, &free_after);
	KT_ASSERT3UG(free_after, ==, 0, ctx, cleanup);

	(void) vmmr_region_pfn_at(region, 0);
	(void) vmmr_region_pfn_at(region, sz - (1 << PAGESHIFT));

	vmmr_free(region);
	region = NULL;

	pgcnt_t free_restored;
	bootmem_query(NULL, &free_restored);
	KT_ASSERT3U(free_restored, ==, free_before, ctx);

	KT_PASS(ctx);
	return;

cleanup:
	vmmr_free(region);
}

static struct modlmisc vmm_ktest_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "vmm reservoir ktest module"
};

static struct modlinkage vmm_ktest_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &vmm_ktest_modlmisc, NULL }
};

int
_init(void)
{
	int ret;
	ktest_module_hdl_t *km = NULL;
	ktest_suite_hdl_t *ks = NULL;

	VERIFY0(ktest_create_module("vmm", &km));
	VERIFY0(ktest_add_suite(km, "reservoir", &ks));
	VERIFY0(ktest_add_test(ks, "vmmr_ktest_basic_alloc_free",
	    vmmr_ktest_basic_alloc_free, KTEST_FLAG_NONE));
	VERIFY0(ktest_add_test(ks, "vmmr_ktest_bootmem_engaged",
	    vmmr_ktest_bootmem_engaged, KTEST_FLAG_NONE));
	VERIFY0(ktest_add_test(ks, "vmmr_ktest_bootmem_overflow",
	    vmmr_ktest_bootmem_overflow, KTEST_FLAG_NONE));

	if ((ret = ktest_register_module(km)) != 0) {
		ktest_free_module(km);
		return (ret);
	}

	if ((ret = mod_install(&vmm_ktest_modlinkage)) != 0) {
		ktest_unregister_module("vmm");
		return (ret);
	}

	return (0);
}

int
_fini(void)
{
	ktest_unregister_module("vmm");
	return (mod_remove(&vmm_ktest_modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&vmm_ktest_modlinkage, modinfop));
}
