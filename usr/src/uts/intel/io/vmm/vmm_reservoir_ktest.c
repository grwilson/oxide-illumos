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
 * (vmmr_alloc()/vmmr_free()/vmmr_region_pfn_at()/vmmr_is_empty()), using
 * transient allocations to grow and shrink it on demand via the same
 * vmmr_add()/vmmr_remove() paths that back the VMM_RESV_SET_TARGET ioctl.
 * That API is fully usable the moment the vmm module is loaded -- unlike
 * the vmmctl device node, it does not require HMA/VT-x/AMD-V support or a
 * successful driver attach, which makes it usable for testing on systems
 * (e.g. nested VMs without exposed hardware virtualization) where
 * /dev/vmmctl is never created.
 *
 * None of vmmr_alloc()/vmmr_free()/vmmr_region_pfn_at()/vmmr_is_empty() are
 * exported by vmm.mapfile (they are only reachable from within the vmm
 * module itself), so they cannot be resolved via an ordinary -N drv/vmm
 * link-time dependency.  Instead, each test resolves them at run time via
 * ktest_hold_mod()/ktest_get_fn(), which -- per their documentation -- can
 * reach a module's non-exported functions directly, the same technique
 * mac_ktest.c uses for mac's internals.
 */

#include <sys/ktest.h>
#include <sys/modctl.h>
#include <sys/sysmacros.h>
#include <sys/bootmem.h>
#include <sys/vmm_reservoir.h>

typedef int (*vmmr_alloc_fn_t)(size_t, bool, vmmr_region_t **);
typedef void (*vmmr_free_fn_t)(vmmr_region_t *);
typedef pfn_t (*vmmr_region_pfn_at_fn_t)(vmmr_region_t *, uintptr_t);
typedef bool (*vmmr_is_empty_fn_t)(void);

typedef struct vmmr_ktest_fns {
	vmmr_alloc_fn_t		vkf_alloc;
	vmmr_free_fn_t		vkf_free;
	vmmr_region_pfn_at_fn_t	vkf_pfn_at;
	vmmr_is_empty_fn_t	vkf_is_empty;
} vmmr_ktest_fns_t;

static int
vmmr_ktest_resolve(ddi_modhandle_t hdl, vmmr_ktest_fns_t *fns)
{
	if (ktest_get_fn(hdl, "vmmr_alloc", (void **)&fns->vkf_alloc) != 0)
		return (-1);
	if (ktest_get_fn(hdl, "vmmr_free", (void **)&fns->vkf_free) != 0)
		return (-1);
	if (ktest_get_fn(hdl, "vmmr_region_pfn_at",
	    (void **)&fns->vkf_pfn_at) != 0)
		return (-1);
	if (ktest_get_fn(hdl, "vmmr_is_empty",
	    (void **)&fns->vkf_is_empty) != 0)
		return (-1);
	return (0);
}

static void
vmmr_ktest_basic_alloc_free(ktest_ctx_hdl_t *ctx)
{
	ddi_modhandle_t hdl = NULL;
	vmmr_ktest_fns_t fns = { 0 };
	vmmr_region_t *region = NULL;

	if (ktest_hold_mod("drv/vmm", &hdl) != 0) {
		KT_ERROR(ctx, "failed to hold 'vmm' module");
		return;
	}
	if (vmmr_ktest_resolve(hdl, &fns) != 0) {
		KT_ERROR(ctx, "failed to resolve vmm reservoir symbols");
		goto cleanup;
	}

	const uint_t npages = 4;
	const size_t sz = npages << PAGESHIFT;

	KT_EASSERT0G(fns.vkf_alloc(sz, true, &region), ctx, cleanup);

	pfn_t pfns[4];
	for (uint_t i = 0; i < npages; i++) {
		pfns[i] = fns.vkf_pfn_at(region, i << PAGESHIFT);
	}
	for (uint_t i = 0; i < npages; i++) {
		for (uint_t j = i + 1; j < npages; j++) {
			if (pfns[i] == pfns[j]) {
				KT_FAIL(ctx,
				    "duplicate pfn %lu at offsets %u and %u",
				    pfns[i], i, j);
				fns.vkf_free(region);
				goto cleanup;
			}
		}
	}

	fns.vkf_free(region);
	KT_ASSERTG(fns.vkf_is_empty(), ctx, cleanup);

	KT_PASS(ctx);

cleanup:
	if (hdl != NULL) {
		ktest_release_mod(hdl);
	}
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

	ddi_modhandle_t hdl = NULL;
	vmmr_ktest_fns_t fns = { 0 };
	vmmr_region_t *region = NULL;

	if (ktest_hold_mod("drv/vmm", &hdl) != 0) {
		KT_ERROR(ctx, "failed to hold 'vmm' module");
		return;
	}
	if (vmmr_ktest_resolve(hdl, &fns) != 0) {
		KT_ERROR(ctx, "failed to resolve vmm reservoir symbols");
		goto cleanup;
	}

	const pgcnt_t req_pages = MIN(free_before, 64);
	const size_t sz = req_pages << PAGESHIFT;

	KT_EASSERT0G(fns.vkf_alloc(sz, true, &region), ctx, cleanup);

	/* Touch the endpoints to exercise the bootmem PFN-lookup path. */
	(void) fns.vkf_pfn_at(region, 0);
	(void) fns.vkf_pfn_at(region, sz - (1 << PAGESHIFT));

	pgcnt_t free_after;
	bootmem_query(NULL, &free_after);
	KT_ASSERT3UG(free_before - free_after, ==, req_pages, ctx, cleanup_region);

	fns.vkf_free(region);
	region = NULL;

	pgcnt_t free_restored;
	bootmem_query(NULL, &free_restored);
	KT_ASSERT3UG(free_restored, ==, free_before, ctx, cleanup);

	KT_PASS(ctx);
	goto cleanup;

cleanup_region:
	fns.vkf_free(region);
cleanup:
	if (hdl != NULL) {
		ktest_release_mod(hdl);
	}
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

	ddi_modhandle_t hdl = NULL;
	vmmr_ktest_fns_t fns = { 0 };
	vmmr_region_t *region = NULL;

	if (ktest_hold_mod("drv/vmm", &hdl) != 0) {
		KT_ERROR(ctx, "failed to hold 'vmm' module");
		return;
	}
	if (vmmr_ktest_resolve(hdl, &fns) != 0) {
		KT_ERROR(ctx, "failed to resolve vmm reservoir symbols");
		goto cleanup;
	}

	const pgcnt_t req_pages = total + 16;
	const size_t sz = req_pages << PAGESHIFT;

	const int err = fns.vkf_alloc(sz, true, &region);
	if (err != 0) {
		KT_SKIP(ctx, "reservoir limit too small for overflow test");
		goto cleanup;
	}

	pgcnt_t free_after;
	bootmem_query(NULL, &free_after);
	KT_ASSERT3UG(free_after, ==, 0, ctx, cleanup_region);

	(void) fns.vkf_pfn_at(region, 0);
	(void) fns.vkf_pfn_at(region, sz - (1 << PAGESHIFT));

	fns.vkf_free(region);
	region = NULL;

	pgcnt_t free_restored;
	bootmem_query(NULL, &free_restored);
	KT_ASSERT3UG(free_restored, ==, free_before, ctx, cleanup);

	KT_PASS(ctx);
	goto cleanup;

cleanup_region:
	fns.vkf_free(region);
cleanup:
	if (hdl != NULL) {
		ktest_release_mod(hdl);
	}
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
