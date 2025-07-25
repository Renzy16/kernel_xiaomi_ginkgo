/* Copyright (c) 2011-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022,2025, Qualcomm Innovation Center, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/msm_kgsl.h>
#include <linux/ratelimit.h>
#include <linux/of_platform.h>
#include <linux/random.h>
#include <soc/qcom/scm.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/compat.h>

#include "kgsl.h"
#include "kgsl_device.h"
#include "kgsl_mmu.h"
#include "kgsl_sharedmem.h"
#include "kgsl_iommu.h"
#include "adreno_pm4types.h"
#include "adreno.h"
#include "kgsl_trace.h"
#include "kgsl_pwrctrl.h"

#define _IOMMU_PRIV(_mmu) (&((_mmu)->priv.iommu))

#define ADDR_IN_GLOBAL(_mmu, _a) \
	(((_a) >= KGSL_IOMMU_GLOBAL_MEM_BASE(_mmu)) && \
	 ((_a) < (KGSL_IOMMU_GLOBAL_MEM_BASE(_mmu) + \
	 KGSL_IOMMU_GLOBAL_MEM_SIZE)))

/*
 * Flag to set SMMU memory attributes required to
 * enable system cache for GPU transactions.
 */
#ifndef IOMMU_USE_UPSTREAM_HINT
#define IOMMU_USE_UPSTREAM_HINT 0
#endif

static struct kgsl_mmu_pt_ops iommu_pt_ops;
static bool need_iommu_sync;

const unsigned int kgsl_iommu_reg_list[KGSL_IOMMU_REG_MAX] = {
	0x0,/* SCTLR */
	0x20,/* TTBR0 */
	0x34,/* CONTEXTIDR */
	0x58,/* FSR */
	0x60,/* FAR_0 */
	0x618,/* TLBIALL */
	0x008,/* RESUME */
	0x68,/* FSYNR0 */
	0x6C,/* FSYNR1 */
	0x7F0,/* TLBSYNC */
	0x7F4,/* TLBSTATUS */
};

/*
 * struct kgsl_iommu_addr_entry - entry in the kgsl_iommu_pt rbtree.
 * @base: starting virtual address of the entry
 * @size: size of the entry
 * @node: the rbtree node
 *
 */
struct kgsl_iommu_addr_entry {
	uint64_t base;
	uint64_t size;
	struct rb_node node;
};

static struct kmem_cache *addr_entry_cache;

/*
 * There are certain memory allocations (ringbuffer, memstore, etc) that need to
 * be present at the same address in every pagetable. We call these "global"
 * pagetable entries. There are relatively few of these and they are mostly
 * stable (defined at init time) but the actual number of globals can differ
 * slight depending on the target and implementation.
 *
 * Here we define an array and a simple allocator to keep track of the currently
 * active global entries. Each entry is assigned a unique address inside of a
 * MMU implementation specific "global" region. We use a simple bitmap based
 * allocator for the region to allow for both fixed and dynamic addressing.
 */

#define GLOBAL_PT_ENTRIES 32

struct global_pt_entry {
	struct kgsl_memdesc *memdesc;
	char name[32];
};

#define GLOBAL_MAP_PAGES (KGSL_IOMMU_GLOBAL_MEM_SIZE >> PAGE_SHIFT)

static struct global_pt_entry global_pt_entries[GLOBAL_PT_ENTRIES];
static DECLARE_BITMAP(global_map, GLOBAL_MAP_PAGES);

static int secure_global_size;
static int global_pt_count;
static struct kgsl_memdesc gpu_qdss_desc;
static struct kgsl_memdesc gpu_qtimer_desc;
static unsigned int context_bank_number;

void kgsl_print_global_pt_entries(struct seq_file *s)
{
	int i;

	for (i = 0; i < global_pt_count; i++) {
		struct kgsl_memdesc *memdesc = global_pt_entries[i].memdesc;

		if (memdesc == NULL)
			continue;

		seq_printf(s, "0x%pK-0x%pK %16llu %s\n",
			(uint64_t *)(uintptr_t) memdesc->gpuaddr,
			(uint64_t *)(uintptr_t) (memdesc->gpuaddr +
			memdesc->size - 1), memdesc->size,
			global_pt_entries[i].name);
	}
}

static void kgsl_iommu_unmap_globals(struct kgsl_pagetable *pagetable)
{
	unsigned int i;

	for (i = 0; i < global_pt_count; i++) {
		if (global_pt_entries[i].memdesc != NULL)
			kgsl_mmu_unmap(pagetable,
					global_pt_entries[i].memdesc);
	}
}

static int kgsl_iommu_map_globals(struct kgsl_pagetable *pagetable)
{
	unsigned int i;

	for (i = 0; i < global_pt_count; i++) {
		if (global_pt_entries[i].memdesc != NULL) {
			int ret = kgsl_mmu_map(pagetable,
					global_pt_entries[i].memdesc);

			if (ret) {
				kgsl_iommu_unmap_globals(pagetable);
				return ret;
			}
		}
	}

	return 0;
}

void kgsl_iommu_unmap_global_secure_pt_entry(struct kgsl_device *device,
				struct kgsl_memdesc *memdesc)
{
	if (!kgsl_mmu_is_secured(&device->mmu) || memdesc == NULL)
		return;

	/* Check if an empty memdesc got passed in */
	if ((memdesc->gpuaddr == 0) || (memdesc->size == 0))
		return;

	if (memdesc->pagetable) {
		if (memdesc->pagetable->name == KGSL_MMU_SECURE_PT)
			kgsl_mmu_unmap(memdesc->pagetable, memdesc);
	}
}

int kgsl_iommu_map_global_secure_pt_entry(struct kgsl_device *device,
				struct kgsl_memdesc *entry)
{
	int ret = 0;

	if (!kgsl_mmu_is_secured(&device->mmu))
		return -ENOTSUPP;

	if (entry != NULL) {
		struct kgsl_pagetable *pagetable = device->mmu.securepagetable;

		entry->pagetable = pagetable;
		entry->gpuaddr = device->mmu.secure_base +
			secure_global_size;

		ret = kgsl_mmu_map(pagetable, entry);
		if (ret == 0)
			secure_global_size += entry->size;
	}
	return ret;
}

static void kgsl_iommu_remove_global(struct kgsl_mmu *mmu,
		struct kgsl_memdesc *memdesc)
{
	int i;

	if (memdesc->gpuaddr == 0 || !(memdesc->priv & KGSL_MEMDESC_GLOBAL))
		return;

	for (i = 0; i < global_pt_count; i++) {
		if (global_pt_entries[i].memdesc == memdesc) {
			u64 offset = memdesc->gpuaddr -
				KGSL_IOMMU_GLOBAL_MEM_BASE(mmu);

			bitmap_clear(global_map, offset >> PAGE_SHIFT,
				kgsl_memdesc_footprint(memdesc) >> PAGE_SHIFT);

			memdesc->gpuaddr = 0;
			memdesc->priv &= ~KGSL_MEMDESC_GLOBAL;
			global_pt_entries[i].memdesc = NULL;
			return;
		}
	}
}

static void kgsl_iommu_add_global(struct kgsl_mmu *mmu,
		struct kgsl_memdesc *memdesc, const char *name)
{
	u32 bit, start = 0;
	u64 size = kgsl_memdesc_footprint(memdesc);

	if (memdesc->gpuaddr != 0)
		return;

	if (WARN_ON(global_pt_count >= GLOBAL_PT_ENTRIES))
		return;

	if (WARN_ON(size > KGSL_IOMMU_GLOBAL_MEM_SIZE))
		return;

	if (memdesc->priv & KGSL_MEMDESC_RANDOM) {
		u32 range = GLOBAL_MAP_PAGES - (size >> PAGE_SHIFT);

		start = get_random_int() % range;
	}

	while (start >= 0) {
		bit = bitmap_find_next_zero_area(global_map, GLOBAL_MAP_PAGES,
			start, size >> PAGE_SHIFT, 0);

		if (bit < GLOBAL_MAP_PAGES)
			break;

		start--;
	}

	if (WARN_ON(start < 0))
		return;

	memdesc->gpuaddr =
		KGSL_IOMMU_GLOBAL_MEM_BASE(mmu) + (bit << PAGE_SHIFT);

	bitmap_set(global_map, bit, size >> PAGE_SHIFT);

	memdesc->priv |= KGSL_MEMDESC_GLOBAL;

	global_pt_entries[global_pt_count].memdesc = memdesc;
	strlcpy(global_pt_entries[global_pt_count].name, name,
			sizeof(global_pt_entries[global_pt_count].name));
	global_pt_count++;
}

struct kgsl_memdesc *kgsl_iommu_get_qdss_global_entry(void)
{
	return &gpu_qdss_desc;
}

static void kgsl_setup_qdss_desc(struct kgsl_device *device)
{
	int result = 0;
	uint32_t gpu_qdss_entry[2];

	if (!of_find_property(device->pdev->dev.of_node,
		"qcom,gpu-qdss-stm", NULL))
		return;

	if (of_property_read_u32_array(device->pdev->dev.of_node,
				"qcom,gpu-qdss-stm", gpu_qdss_entry, 2)) {
		KGSL_CORE_ERR("Failed to read gpu qdss dts entry\n");
		return;
	}

	kgsl_memdesc_init(device, &gpu_qdss_desc, 0);
	gpu_qdss_desc.priv = 0;
	gpu_qdss_desc.physaddr = gpu_qdss_entry[0];
	gpu_qdss_desc.size = gpu_qdss_entry[1];
	gpu_qdss_desc.pagetable = NULL;
	gpu_qdss_desc.ops = NULL;
	gpu_qdss_desc.hostptr = NULL;

	result = memdesc_sg_dma(&gpu_qdss_desc, gpu_qdss_desc.physaddr,
			gpu_qdss_desc.size);
	if (result) {
		KGSL_CORE_ERR("memdesc_sg_dma failed: %d\n", result);
		return;
	}

	kgsl_mmu_add_global(device, &gpu_qdss_desc, "gpu-qdss");
}

static inline void kgsl_cleanup_qdss_desc(struct kgsl_mmu *mmu)
{
	kgsl_iommu_remove_global(mmu, &gpu_qdss_desc);
	kgsl_sharedmem_free(&gpu_qdss_desc);
}

struct kgsl_memdesc *kgsl_iommu_get_qtimer_global_entry(void)
{
	return &gpu_qtimer_desc;
}

static void kgsl_setup_qtimer_desc(struct kgsl_device *device)
{
	int result = 0;
	uint32_t gpu_qtimer_entry[2];

	if (!of_find_property(device->pdev->dev.of_node,
		"qcom,gpu-qtimer", NULL))
		return;

	if (of_property_read_u32_array(device->pdev->dev.of_node,
				"qcom,gpu-qtimer", gpu_qtimer_entry, 2)) {
		KGSL_CORE_ERR("Failed to read gpu qtimer dts entry\n");
		return;
	}

	kgsl_memdesc_init(device, &gpu_qtimer_desc, 0);
	gpu_qtimer_desc.priv = 0;
	gpu_qtimer_desc.physaddr = gpu_qtimer_entry[0];
	gpu_qtimer_desc.size = gpu_qtimer_entry[1];
	gpu_qtimer_desc.pagetable = NULL;
	gpu_qtimer_desc.ops = NULL;
	gpu_qtimer_desc.hostptr = NULL;

	result = memdesc_sg_dma(&gpu_qtimer_desc, gpu_qtimer_desc.physaddr,
			gpu_qtimer_desc.size);
	if (result) {
		KGSL_CORE_ERR("memdesc_sg_dma failed: %d\n", result);
		return;
	}

	kgsl_mmu_add_global(device, &gpu_qtimer_desc, "gpu-qtimer");
}

static inline void kgsl_cleanup_qtimer_desc(struct kgsl_mmu *mmu)
{
	kgsl_iommu_remove_global(mmu, &gpu_qtimer_desc);
	kgsl_sharedmem_free(&gpu_qtimer_desc);
}

static inline void _iommu_sync_mmu_pc(bool lock)
{
	if (need_iommu_sync == false)
		return;

	if (lock)
		mutex_lock(&kgsl_mmu_sync);
	else
		mutex_unlock(&kgsl_mmu_sync);
}

static void _detach_pt(struct kgsl_iommu_pt *iommu_pt,
			  struct kgsl_iommu_context *ctx)
{
	if (iommu_pt->attached) {
		_iommu_sync_mmu_pc(true);
		iommu_detach_device(iommu_pt->domain, ctx->dev);
		_iommu_sync_mmu_pc(false);
		iommu_pt->attached = false;
	}
}

static int _attach_pt(struct kgsl_iommu_pt *iommu_pt,
			struct kgsl_iommu_context *ctx)
{
	int ret;

	if (iommu_pt->attached)
		return 0;

	_iommu_sync_mmu_pc(true);
	ret = iommu_attach_device(iommu_pt->domain, ctx->dev);
	_iommu_sync_mmu_pc(false);

	if (ret == 0)
		iommu_pt->attached = true;

	return ret;
}

static int _iommu_map_single_page_sync_pc(struct kgsl_pagetable *pt,
		uint64_t gpuaddr, phys_addr_t physaddr, int times,
		unsigned int flags)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	size_t mapped = 0;
	int i;
	int ret = 0;

	_iommu_sync_mmu_pc(true);

	for (i = 0; i < times; i++) {
		ret = iommu_map(iommu_pt->domain, gpuaddr + mapped,
				physaddr, PAGE_SIZE, flags);
		if (ret)
			break;
		mapped += PAGE_SIZE;
	}

	if (ret)
		iommu_unmap(iommu_pt->domain, gpuaddr, mapped);

	_iommu_sync_mmu_pc(false);

	if (ret) {
		KGSL_CORE_ERR("map err: 0x%016llX, 0x%lx, 0x%x, %d\n",
			gpuaddr, PAGE_SIZE * times, flags, ret);
		return -ENODEV;
	}

	return 0;
}

static int _iommu_unmap_sync_pc(struct kgsl_pagetable *pt,
		uint64_t addr, uint64_t size)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(pt->mmu);
	size_t unmapped = 0;

	_iommu_sync_mmu_pc(true);

	/*
	 * Take iommu unmap fast path if CX GDSC is in OFF state.
	 */
	if (iommu->vddcx_regulator &&
			(!regulator_is_enabled(iommu->vddcx_regulator)))
		unmapped = iommu_unmap_fast(iommu_pt->domain, addr, size);
	else
		unmapped = iommu_unmap(iommu_pt->domain, addr, size);

	_iommu_sync_mmu_pc(false);

	if (unmapped != size) {
		KGSL_CORE_ERR("unmap err: 0x%016llx, 0x%llx, %zd\n",
			addr, size, unmapped);
		return -ENODEV;
	}

	return 0;
}

static int _iommu_map_sg_offset_sync_pc(struct kgsl_pagetable *pt,
		uint64_t addr, struct scatterlist *sg, int nents,
		uint64_t offset, uint64_t size, unsigned int flags)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	uint64_t offset_tmp = offset;
	uint64_t size_tmp = size;
	size_t mapped = 0;
	unsigned int i;
	struct scatterlist *s;
	phys_addr_t physaddr;
	int ret;

	_iommu_sync_mmu_pc(true);

	for_each_sg(sg, s, nents, i) {
		/* Iterate until we find the offset */
		if (offset_tmp >= s->length) {
			offset_tmp -= s->length;
			continue;
		}

		/* How much mapping is needed in this sg? */
		if (size < s->length - offset_tmp)
			size_tmp = size;
		else
			size_tmp = s->length - offset_tmp;

		/* Get the phys addr for the offset page */
		if (offset_tmp != 0) {
			physaddr = page_to_phys(nth_page(sg_page(s),
					offset_tmp >> PAGE_SHIFT));
			/* Reset offset_tmp */
			offset_tmp = 0;
		} else
			physaddr = page_to_phys(sg_page(s));

		/* Do the map for this sg */
		ret = iommu_map(iommu_pt->domain, addr + mapped,
				physaddr, size_tmp, flags);
		if (ret)
			break;

		mapped += size_tmp;
		size -= size_tmp;

		if (size == 0)
			break;
	}

	_iommu_sync_mmu_pc(false);

	if (size != 0) {
		/* Cleanup on error */
		_iommu_unmap_sync_pc(pt, addr, mapped);
		KGSL_CORE_ERR(
			"map sg offset err: 0x%016llX, %d, %x, %zd\n",
			addr, nents, flags, mapped);
		return  -ENODEV;
	}

	return 0;
}

static int _iommu_map_sg_sync_pc(struct kgsl_pagetable *pt,
		uint64_t addr, struct scatterlist *sg, int nents,
		unsigned int flags)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	size_t mapped;

	_iommu_sync_mmu_pc(true);

	mapped = iommu_map_sg(iommu_pt->domain, addr, sg, nents, flags);

	_iommu_sync_mmu_pc(false);

	if (mapped == 0) {
		KGSL_CORE_ERR("map sg err: 0x%016llX, %d, %x, %zd\n",
			addr, nents, flags, mapped);
		return  -ENODEV;
	}

	return 0;
}

/*
 * One page allocation for a guard region to protect against over-zealous
 * GPU pre-fetch
 */

static struct page *kgsl_guard_page;
static struct page *kgsl_secure_guard_page;

/*
 * The dummy page is a placeholder/extra page to be used for sparse mappings.
 * This page will be mapped to all virtual sparse bindings that are not
 * physically backed.
 */
static struct page *kgsl_dummy_page;

/* These functions help find the nearest allocated memory entries on either side
 * of a faulting address. If we know the nearby allocations memory we can
 * get a better determination of what we think should have been located in the
 * faulting region
 */

/*
 * A local structure to make it easy to store the interesting bits for the
 * memory entries on either side of the faulting address
 */

struct _mem_entry {
	uint64_t gpuaddr;
	uint64_t size;
	uint64_t flags;
	unsigned int priv;
	int pending_free;
	pid_t pid;
	char name[32];
};

static void _get_global_entries(uint64_t faultaddr,
		struct _mem_entry *prev,
		struct _mem_entry *next)
{
	int i;
	uint64_t prevaddr = 0;
	struct global_pt_entry *p = NULL;

	uint64_t nextaddr = (uint64_t) -1;
	struct global_pt_entry *n = NULL;

	for (i = 0; i < global_pt_count; i++) {
		uint64_t addr;

		if (global_pt_entries[i].memdesc == NULL)
			continue;

		addr = global_pt_entries[i].memdesc->gpuaddr;
		if ((addr < faultaddr) && (addr > prevaddr)) {
			prevaddr = addr;
			p = &global_pt_entries[i];
		}

		if ((addr > faultaddr) && (addr < nextaddr)) {
			nextaddr = addr;
			n = &global_pt_entries[i];
		}
	}

	if (p != NULL) {
		prev->gpuaddr = p->memdesc->gpuaddr;
		prev->size = p->memdesc->size;
		prev->flags = p->memdesc->flags;
		prev->priv = p->memdesc->priv;
		prev->pid = 0;
		strlcpy(prev->name, p->name, sizeof(prev->name));
	}

	if (n != NULL) {
		next->gpuaddr = n->memdesc->gpuaddr;
		next->size = n->memdesc->size;
		next->flags = n->memdesc->flags;
		next->priv = n->memdesc->priv;
		next->pid = 0;
		strlcpy(next->name, n->name, sizeof(next->name));
	}
}

void __kgsl_get_memory_usage(struct _mem_entry *entry)
{
	kgsl_get_memory_usage(entry->name, sizeof(entry->name), entry->flags);
}

static void _get_entries(struct kgsl_process_private *private,
		uint64_t faultaddr, struct _mem_entry *prev,
		struct _mem_entry *next)
{
	int id;
	struct kgsl_mem_entry *entry;

	uint64_t prevaddr = 0;
	struct kgsl_mem_entry *p = NULL;

	uint64_t nextaddr = (uint64_t) -1;
	struct kgsl_mem_entry *n = NULL;

	idr_for_each_entry(&private->mem_idr, entry, id) {
		uint64_t addr = entry->memdesc.gpuaddr;

		if ((addr < faultaddr) && (addr > prevaddr)) {
			prevaddr = addr;
			p = entry;
		}

		if ((addr > faultaddr) && (addr < nextaddr)) {
			nextaddr = addr;
			n = entry;
		}
	}

	if (p != NULL) {
		prev->gpuaddr = p->memdesc.gpuaddr;
		prev->size = p->memdesc.size;
		prev->flags = p->memdesc.flags;
		prev->priv = p->memdesc.priv;
		prev->pending_free = p->pending_free;
		prev->pid = pid_nr(private->pid);
		__kgsl_get_memory_usage(prev);
	}

	if (n != NULL) {
		next->gpuaddr = n->memdesc.gpuaddr;
		next->size = n->memdesc.size;
		next->flags = n->memdesc.flags;
		next->priv = n->memdesc.priv;
		next->pending_free = n->pending_free;
		next->pid = pid_nr(private->pid);
		__kgsl_get_memory_usage(next);
	}
}

static void _find_mem_entries(struct kgsl_mmu *mmu, uint64_t faultaddr,
		struct _mem_entry *preventry, struct _mem_entry *nextentry,
		struct kgsl_process_private *private)
{
	memset(preventry, 0, sizeof(*preventry));
	memset(nextentry, 0, sizeof(*nextentry));

	/* Set the maximum possible size as an initial value */
	nextentry->gpuaddr = (uint64_t) -1;

	if (ADDR_IN_GLOBAL(mmu, faultaddr)) {
		_get_global_entries(faultaddr, preventry, nextentry);
	} else if (private) {
		spin_lock(&private->mem_lock);
		_get_entries(private, faultaddr, preventry, nextentry);
		spin_unlock(&private->mem_lock);
	}
}

static void _print_entry(struct kgsl_device *device, struct _mem_entry *entry)
{
	KGSL_LOG_DUMP(device,
		"[%016llX - %016llX] %s %s (pid = %d) (%s)\n",
		entry->gpuaddr,
		entry->gpuaddr + entry->size,
		entry->priv & KGSL_MEMDESC_GUARD_PAGE ? "(+guard)" : "",
		entry->pending_free ? "(pending free)" : "",
		entry->pid, entry->name);
}

static void _check_if_freed(struct kgsl_iommu_context *ctx,
	uint64_t addr, pid_t ptname)
{
	uint64_t gpuaddr = addr;
	uint64_t size = 0;
	uint64_t flags = 0;
	pid_t pid;

	char name[32];

	memset(name, 0, sizeof(name));

	if (kgsl_memfree_find_entry(ptname, &gpuaddr, &size, &flags, &pid)) {
		kgsl_get_memory_usage(name, sizeof(name) - 1, flags);
		KGSL_LOG_DUMP(ctx->kgsldev, "---- premature free ----\n");
		KGSL_LOG_DUMP(ctx->kgsldev,
			"[%8.8llX-%8.8llX] (%s) was already freed by pid %d\n",
			gpuaddr, gpuaddr + size, name, pid);
	}
}

static bool
kgsl_iommu_uche_overfetch(struct kgsl_process_private *private,
		uint64_t faultaddr)
{
	int id;
	struct kgsl_mem_entry *entry = NULL;

	spin_lock(&private->mem_lock);
	idr_for_each_entry(&private->mem_idr, entry, id) {
		struct kgsl_memdesc *m = &entry->memdesc;

		if ((faultaddr >= (m->gpuaddr + m->size))
				&& (faultaddr < (m->gpuaddr + m->size + 64))) {
			spin_unlock(&private->mem_lock);
			return true;
		}
	}
	spin_unlock(&private->mem_lock);
	return false;
}

/*
 * Read pagefaults where the faulting address lies within the first 64 bytes
 * of a page (UCHE line size is 64 bytes) and the fault page is preceded by a
 * valid allocation are considered likely due to UCHE overfetch and suppressed.
 */

static bool kgsl_iommu_suppress_pagefault(uint64_t faultaddr, int write,
					struct kgsl_process_private *private)
{
	/*
	 * If there is no context associated with the pagefault then this
	 * could be a fault on a global buffer. We do not suppress faults
	 * on global buffers as they are mainly accessed by the CP bypassing
	 * the UCHE. Also, write pagefaults are never suppressed.
	 */
	if (!private || write)
		return false;

	return kgsl_iommu_uche_overfetch(private, faultaddr);
}

static struct kgsl_process_private *kgsl_iommu_identify_process(u64 ptbase)
{
	struct kgsl_process_private *p = NULL;
	struct kgsl_iommu_pt *iommu_pt;

	mutex_lock(&kgsl_driver.process_mutex);
	list_for_each_entry(p, &kgsl_driver.process_list, list) {
		iommu_pt = p->pagetable->priv;
		if (iommu_pt->ttbr0 == ptbase) {
			mutex_unlock(&kgsl_driver.process_mutex);
			return p;
		}
	}

	mutex_unlock(&kgsl_driver.process_mutex);
	return p;
}

static int kgsl_iommu_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long addr, int flags, void *token)
{
	int ret = 0;
	struct kgsl_pagetable *pt = token;
	struct kgsl_mmu *mmu = pt->mmu;
	struct kgsl_iommu *iommu;
	struct kgsl_iommu_context *ctx;
	u64 ptbase;
	u32 contextidr;
	pid_t pid = 0;
	pid_t ptname;
	struct _mem_entry prev, next;
	int write;
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;
	struct adreno_gpudev *gpudev;
	unsigned int no_page_fault_log = 0;
	char *fault_type = "unknown";
	struct kgsl_process_private *private;

	static DEFINE_RATELIMIT_STATE(_rs,
					DEFAULT_RATELIMIT_INTERVAL,
					DEFAULT_RATELIMIT_BURST);

	if (mmu == NULL)
		return ret;

	iommu = _IOMMU_PRIV(mmu);
	ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	device = KGSL_MMU_DEVICE(mmu);
	adreno_dev = ADRENO_DEVICE(device);
	gpudev = ADRENO_GPU_DEVICE(adreno_dev);

	write = (flags & IOMMU_FAULT_WRITE) ? 1 : 0;
	if (flags & IOMMU_FAULT_TRANSLATION)
		fault_type = "translation";
	else if (flags & IOMMU_FAULT_PERMISSION)
		fault_type = "permission";
	else if (flags & IOMMU_FAULT_EXTERNAL)
		fault_type = "external";
	else if (flags & IOMMU_FAULT_TRANSACTION_STALLED)
		fault_type = "transaction stalled";

	ptbase = KGSL_IOMMU_GET_CTX_REG_Q(ctx, TTBR0);
	private = kgsl_iommu_identify_process(ptbase);

	if (!kgsl_process_private_get(private))
		private = NULL;
	else
		pid = pid_nr(private->pid);

	if (kgsl_iommu_suppress_pagefault(addr, write, private)) {
		iommu->pagefault_suppression_count++;
		return ret;
	}

	if (pt->name == KGSL_MMU_SECURE_PT)
		ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_SECURE];

	if (test_bit(KGSL_FT_PAGEFAULT_GPUHALT_ENABLE,
		&adreno_dev->ft_pf_policy) &&
		(flags & IOMMU_FAULT_TRANSACTION_STALLED)) {
		/*
		 * Turn off GPU IRQ so we don't get faults from it too.
		 * The device mutex must be held to change power state
		 */
		mutex_lock(&device->mutex);
		kgsl_pwrctrl_change_state(device, KGSL_STATE_AWARE);
		mutex_unlock(&device->mutex);
	}

	contextidr = KGSL_IOMMU_GET_CTX_REG(ctx, CONTEXTIDR);
	ptname = MMU_FEATURE(mmu, KGSL_MMU_GLOBAL_PAGETABLE) ?
		KGSL_MMU_GLOBAL_PT : pid;
	/*
	 * Trace needs to be logged before searching the faulting
	 * address in free list as it takes quite long time in
	 * search and delays the trace unnecessarily.
	 */
	trace_kgsl_mmu_pagefault(ctx->kgsldev, addr,
			ptname,
			private != NULL ? private->comm : "unknown",
			write ? "write" : "read");

	if (test_bit(KGSL_FT_PAGEFAULT_LOG_ONE_PER_PAGE,
		&adreno_dev->ft_pf_policy))
		no_page_fault_log = kgsl_mmu_log_fault_addr(mmu, ptbase, addr);

	if (!no_page_fault_log && __ratelimit(&_rs)) {
		KGSL_MEM_CRIT(ctx->kgsldev,
			"GPU PAGE FAULT: addr = %lX pid= %d name=%s\n", addr,
			ptname,
			private != NULL ? private->comm : "unknown");
		KGSL_MEM_CRIT(ctx->kgsldev,
			"context=%s TTBR0=0x%llx CIDR=0x%x (%s %s fault)\n",
			ctx->name, ptbase, contextidr,
			write ? "write" : "read", fault_type);

		if (gpudev->iommu_fault_block) {
			unsigned int fsynr1;

			fsynr1 = KGSL_IOMMU_GET_CTX_REG(ctx, FSYNR1);
			KGSL_MEM_CRIT(ctx->kgsldev,
				"FAULTING BLOCK: %s\n",
				gpudev->iommu_fault_block(device, fsynr1));
		}

		/* Don't print the debug if this is a permissions fault */
		if (!(flags & IOMMU_FAULT_PERMISSION)) {
			_check_if_freed(ctx, addr, ptname);

			KGSL_LOG_DUMP(ctx->kgsldev,
				"---- nearby memory ----\n");

			_find_mem_entries(mmu, addr, &prev, &next, private);
			if (prev.gpuaddr)
				_print_entry(ctx->kgsldev, &prev);
			else
				KGSL_LOG_DUMP(ctx->kgsldev, "*EMPTY*\n");

			KGSL_LOG_DUMP(ctx->kgsldev, " <- fault @ %8.8lX\n",
				addr);

			if (next.gpuaddr != (uint64_t) -1)
				_print_entry(ctx->kgsldev, &next);
			else
				KGSL_LOG_DUMP(ctx->kgsldev, "*EMPTY*\n");
		}
	}


	/*
	 * We do not want the h/w to resume fetching data from an iommu
	 * that has faulted, this is better for debugging as it will stall
	 * the GPU and trigger a snapshot. Return EBUSY error.
	 */
	if (test_bit(KGSL_FT_PAGEFAULT_GPUHALT_ENABLE,
		&adreno_dev->ft_pf_policy) &&
		(flags & IOMMU_FAULT_TRANSACTION_STALLED)) {
		uint32_t sctlr_val;

		ret = -EBUSY;
		/*
		 * Disable context fault interrupts
		 * as we do not clear FSR in the ISR.
		 * Will be re-enabled after FSR is cleared.
		 */
		sctlr_val = KGSL_IOMMU_GET_CTX_REG(ctx, SCTLR);
		sctlr_val &= ~(0x1 << KGSL_IOMMU_SCTLR_CFIE_SHIFT);
		KGSL_IOMMU_SET_CTX_REG(ctx, SCTLR, sctlr_val);

		 /* This is used by reset/recovery path */
		ctx->stalled_on_fault = true;

		adreno_set_gpu_fault(adreno_dev, ADRENO_IOMMU_PAGE_FAULT);
		/* Go ahead with recovery*/
		adreno_dispatcher_schedule(device);
	}

	kgsl_process_private_put(private);

	return ret;
}

/*
 * kgsl_iommu_disable_clk() - Disable iommu clocks
 * Disable IOMMU clocks
 */
static void kgsl_iommu_disable_clk(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	int j;

	atomic_dec(&iommu->clk_enable_count);

	/*
	 * Make sure the clk refcounts are good. An unbalance may
	 * cause the clocks to be off when we need them on.
	 */
	WARN_ON(atomic_read(&iommu->clk_enable_count) < 0);

	for (j = (KGSL_IOMMU_MAX_CLKS - 1); j >= 0; j--)
		if (iommu->clks[j])
			clk_disable_unprepare(iommu->clks[j]);
}

/*
 * kgsl_iommu_enable_clk_prepare_enable - Enable the specified IOMMU clock
 * Try 4 times to enable it and then BUG() for debug
 */
static void kgsl_iommu_clk_prepare_enable(struct clk *clk)
{
	int num_retries = 4;

	while (num_retries--) {
		if (!clk_prepare_enable(clk))
			return;
	}

	/* Failure is fatal so BUG() to facilitate debug */
	KGSL_CORE_ERR("IOMMU clock enable failed\n");
	WARN(1, "IOMMU clock enable failed\n");
}

/*
 * kgsl_iommu_enable_clk - Enable iommu clocks
 * Enable all the IOMMU clocks
 */
static void kgsl_iommu_enable_clk(struct kgsl_mmu *mmu)
{
	int j;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);

	for (j = 0; j < KGSL_IOMMU_MAX_CLKS; j++) {
		if (iommu->clks[j])
			kgsl_iommu_clk_prepare_enable(iommu->clks[j]);
	}
	atomic_inc(&iommu->clk_enable_count);
}

/* kgsl_iommu_get_ttbr0 - Get TTBR0 setting for a pagetable */
static u64 kgsl_iommu_get_ttbr0(struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt ? pt->priv : NULL;

	if (WARN_ON(!iommu_pt))
		return 0;

	return iommu_pt->ttbr0;
}

static bool kgsl_iommu_pt_equal(struct kgsl_mmu *mmu,
				struct kgsl_pagetable *pt,
				u64 ttbr0)
{
	struct kgsl_iommu_pt *iommu_pt = pt ? pt->priv : NULL;
	u64 domain_ttbr0;

	if (iommu_pt == NULL)
		return 0;

	domain_ttbr0 = kgsl_iommu_get_ttbr0(pt);

	return (domain_ttbr0 == ttbr0);
}

/* kgsl_iommu_get_contextidr - query CONTEXTIDR setting for a pagetable */
static u32 kgsl_iommu_get_contextidr(struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt ? pt->priv : NULL;

	if (WARN_ON(!iommu_pt))
		return 0;

	return iommu_pt->contextidr;
}

/*
 * kgsl_iommu_destroy_pagetable - Free up reaources help by a pagetable
 * @mmu_specific_pt - Pointer to pagetable which is to be freed
 *
 * Return - void
 */
static void kgsl_iommu_destroy_pagetable(struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	struct kgsl_mmu *mmu = pt->mmu;
	struct kgsl_iommu *iommu;
	struct kgsl_iommu_context  *ctx;

	/*
	 * Make sure all allocations are unmapped before destroying
	 * the pagetable
	 */
	WARN_ON(!list_empty(&pt->list));

	iommu = _IOMMU_PRIV(mmu);

	if (pt->name == KGSL_MMU_SECURE_PT) {
		ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_SECURE];
	} else {
		ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
		kgsl_iommu_unmap_globals(pt);
		if (pt->name == KGSL_MMU_GLOBAL_PT)
			mmu->globalpt_mapped = false;
	}

	if (iommu_pt->domain) {
		trace_kgsl_pagetable_destroy(iommu_pt->ttbr0, pt->name);

		_detach_pt(iommu_pt, ctx);

		iommu_domain_free(iommu_pt->domain);
	}

	kfree(iommu_pt);
}

static void setup_64bit_pagetable(struct kgsl_mmu *mmu,
		struct kgsl_pagetable *pagetable,
		struct kgsl_iommu_pt *pt)
{
	if (mmu->secured && pagetable->name == KGSL_MMU_SECURE_PT) {
		pt->compat_va_start = mmu->secure_base;
		pt->compat_va_end = KGSL_IOMMU_SECURE_END(mmu);
		pt->va_start = mmu->secure_base;
		pt->va_end = KGSL_IOMMU_SECURE_END(mmu);
	} else {
		pt->compat_va_start = mmu->svm_base32;
		pt->compat_va_end = mmu->secure_base;
		pt->va_start = KGSL_IOMMU_VA_BASE64;
		pt->va_end = KGSL_IOMMU_VA_END64;
	}

	if (pagetable->name != KGSL_MMU_GLOBAL_PT &&
		pagetable->name != KGSL_MMU_SECURE_PT) {
		if (kgsl_is_compat_task()) {
			pt->svm_start = mmu->svm_base32;
			pt->svm_end = mmu->secure_base;
		} else {
			pt->svm_start = KGSL_IOMMU_SVM_BASE64;
			pt->svm_end = KGSL_IOMMU_SVM_END64;
		}
	}
}

static void setup_32bit_pagetable(struct kgsl_mmu *mmu,
		struct kgsl_pagetable *pagetable,
		struct kgsl_iommu_pt *pt)
{
	if (mmu->secured) {
		if (pagetable->name == KGSL_MMU_SECURE_PT) {
			pt->compat_va_start = mmu->secure_base;
			pt->compat_va_end = KGSL_IOMMU_SECURE_END(mmu);
			pt->va_start = mmu->secure_base;
			pt->va_end = KGSL_IOMMU_SECURE_END(mmu);
		} else {
			pt->va_start = mmu->svm_base32;
			pt->va_end = mmu->secure_base;
			pt->compat_va_start = pt->va_start;
			pt->compat_va_end = pt->va_end;
		}
	} else {
		pt->va_start = mmu->svm_base32;
		pt->va_end = KGSL_IOMMU_GLOBAL_MEM_BASE(mmu);
		pt->compat_va_start = pt->va_start;
		pt->compat_va_end = pt->va_end;
	}

	if (pagetable->name != KGSL_MMU_GLOBAL_PT &&
		pagetable->name != KGSL_MMU_SECURE_PT) {
		pt->svm_start = mmu->svm_base32;
		pt->svm_end = KGSL_IOMMU_SVM_END32;
	}
}


static struct kgsl_iommu_pt *
_alloc_pt(struct device *dev, struct kgsl_mmu *mmu, struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt;
	struct bus_type *bus = kgsl_mmu_get_bus(dev);

	if (bus == NULL)
		return ERR_PTR(-ENODEV);

	iommu_pt = kzalloc(sizeof(struct kgsl_iommu_pt), GFP_KERNEL);
	if (iommu_pt == NULL)
		return ERR_PTR(-ENOMEM);

	iommu_pt->domain = iommu_domain_alloc(bus);
	if (iommu_pt->domain == NULL) {
		kfree(iommu_pt);
		return ERR_PTR(-ENODEV);
	}

	pt->pt_ops = &iommu_pt_ops;
	pt->priv = iommu_pt;
	pt->fault_addr = ~0ULL;
	iommu_pt->rbtree = RB_ROOT;

	if (MMU_FEATURE(mmu, KGSL_MMU_64BIT))
		setup_64bit_pagetable(mmu, pt, iommu_pt);
	else
		setup_32bit_pagetable(mmu, pt, iommu_pt);


	return iommu_pt;
}

static void _free_pt(struct kgsl_iommu_context *ctx, struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;

	pt->pt_ops = NULL;
	pt->priv = NULL;

	if (iommu_pt == NULL)
		return;

	_detach_pt(iommu_pt, ctx);

	if (iommu_pt->domain != NULL)
		iommu_domain_free(iommu_pt->domain);
	kfree(iommu_pt);
}

void _enable_gpuhtw_llc(struct kgsl_mmu *mmu, struct kgsl_iommu_pt *iommu_pt)
{
	struct kgsl_device *device = KGSL_MMU_DEVICE(mmu);
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int gpuhtw_llc_enable = 1;
	int ret;

	/* GPU pagetable walk LLC slice not enabled */
	if (IS_ERR(adreno_dev->gpuhtw_llc_slice))
		return;

	/* Domain attribute to enable system cache for GPU pagetable walks */
	if (adreno_is_a640(adreno_dev) || adreno_is_a612(adreno_dev) ||
			adreno_is_a680(adreno_dev))
		ret = iommu_domain_set_attr(iommu_pt->domain,
			DOMAIN_ATTR_USE_LLC_NWA, &gpuhtw_llc_enable);
	else
		ret = iommu_domain_set_attr(iommu_pt->domain,
			DOMAIN_ATTR_USE_UPSTREAM_HINT, &gpuhtw_llc_enable);

	/*
	 * Warn that the system cache will not be used for GPU
	 * pagetable walks. This is not a fatal error.
	 */
	WARN_ONCE(ret,
		"System cache not enabled for GPU pagetable walks: %d\n", ret);
}

int kgsl_program_smmu_aperture(void)
{
	struct scm_desc desc = {0};

	desc.args[0] = 0xFFFF0000 | ((CP_APERTURE_REG & 0xff) << 8) |
			(context_bank_number & 0xff);
	desc.args[1] = 0xFFFFFFFF;
	desc.args[2] = 0xFFFFFFFF;
	desc.args[3] = 0xFFFFFFFF;
	desc.arginfo = SCM_ARGS(4);

	return scm_call2(SCM_SIP_FNID(SCM_SVC_MP, CP_SMMU_APERTURE_ID), &desc);
}

static int _init_global_pt(struct kgsl_mmu *mmu, struct kgsl_pagetable *pt)
{
	int ret = 0;
	struct kgsl_iommu_pt *iommu_pt = NULL;
	unsigned int cb_num;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];

	iommu_pt = _alloc_pt(ctx->dev, mmu, pt);

	if (IS_ERR(iommu_pt))
		return PTR_ERR(iommu_pt);

	if (kgsl_mmu_is_perprocess(mmu)) {
		ret = iommu_domain_set_attr(iommu_pt->domain,
				DOMAIN_ATTR_PROCID, &pt->name);
		if (ret) {
			KGSL_CORE_ERR("set DOMAIN_ATTR_PROCID failed: %d\n",
					ret);
			goto done;
		}
	}

	_enable_gpuhtw_llc(mmu, iommu_pt);

	ret = _attach_pt(iommu_pt, ctx);
	if (ret)
		goto done;

	iommu_set_fault_handler(iommu_pt->domain,
				kgsl_iommu_fault_handler, pt);

	ret = iommu_domain_get_attr(iommu_pt->domain,
				DOMAIN_ATTR_CONTEXT_BANK, &cb_num);
	if (ret) {
		KGSL_CORE_ERR("get DOMAIN_ATTR_CONTEXT_BANK failed: %d\n",
				ret);
		goto done;
	}
	context_bank_number = cb_num;
	if (!MMU_FEATURE(mmu, KGSL_MMU_GLOBAL_PAGETABLE) &&
		scm_is_call_available(SCM_SVC_MP, CP_SMMU_APERTURE_ID)) {
		ret = kgsl_program_smmu_aperture();
		if (ret) {
			pr_err("SMMU aperture programming call failed with error %d\n",
									ret);
			goto done;
		}
	}

	ctx->cb_num = cb_num;
	ctx->regbase = iommu->regbase + KGSL_IOMMU_CB0_OFFSET
			+ (cb_num << KGSL_IOMMU_CB_SHIFT);

	ret = iommu_domain_get_attr(iommu_pt->domain,
			DOMAIN_ATTR_TTBR0, &iommu_pt->ttbr0);
	if (ret) {
		KGSL_CORE_ERR("get DOMAIN_ATTR_TTBR0 failed: %d\n",
				ret);
		goto done;
	}
	ret = iommu_domain_get_attr(iommu_pt->domain,
			DOMAIN_ATTR_CONTEXTIDR, &iommu_pt->contextidr);
	if (ret) {
		KGSL_CORE_ERR("get DOMAIN_ATTR_CONTEXTIDR failed: %d\n",
				ret);
		goto done;
	}


done:
	if (ret)
		_free_pt(ctx, pt);

	return ret;
}

static int _init_secure_pt(struct kgsl_mmu *mmu, struct kgsl_pagetable *pt)
{
	int ret = 0;
	struct kgsl_iommu_pt *iommu_pt = NULL;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_SECURE];
	int secure_vmid = VMID_CP_PIXEL;
	unsigned int cb_num;

	if (!mmu->secured)
		return -EPERM;

	if (!MMU_FEATURE(mmu, KGSL_MMU_HYP_SECURE_ALLOC)) {
		if (!kgsl_mmu_bus_secured(ctx->dev))
			return -EPERM;
	}

	iommu_pt = _alloc_pt(ctx->dev, mmu, pt);

	if (IS_ERR(iommu_pt))
		return PTR_ERR(iommu_pt);

	ret = iommu_domain_set_attr(iommu_pt->domain,
				    DOMAIN_ATTR_SECURE_VMID, &secure_vmid);
	if (ret) {
		KGSL_CORE_ERR("set DOMAIN_ATTR_SECURE_VMID failed: %d\n", ret);
		goto done;
	}

	_enable_gpuhtw_llc(mmu, iommu_pt);

	ret = _attach_pt(iommu_pt, ctx);

	if (MMU_FEATURE(mmu, KGSL_MMU_HYP_SECURE_ALLOC))
		iommu_set_fault_handler(iommu_pt->domain,
					kgsl_iommu_fault_handler, pt);

	ret = iommu_domain_get_attr(iommu_pt->domain,
				DOMAIN_ATTR_CONTEXT_BANK, &cb_num);
	if (ret) {
		KGSL_CORE_ERR("get DOMAIN_ATTR_PROCID failed: %d\n",
				ret);
		goto done;
	}

	ctx->cb_num = cb_num;
	ctx->regbase = iommu->regbase + KGSL_IOMMU_CB0_OFFSET
			+ (cb_num << KGSL_IOMMU_CB_SHIFT);

done:
	if (ret)
		_free_pt(ctx, pt);
	return ret;
}

static int _init_per_process_pt(struct kgsl_mmu *mmu, struct kgsl_pagetable *pt)
{
	int ret = 0;
	struct kgsl_iommu_pt *iommu_pt = NULL;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	int dynamic = 1;
	unsigned int cb_num = ctx->cb_num;

	iommu_pt = _alloc_pt(ctx->dev, mmu, pt);

	if (IS_ERR(iommu_pt))
		return PTR_ERR(iommu_pt);

	ret = iommu_domain_set_attr(iommu_pt->domain,
				DOMAIN_ATTR_DYNAMIC, &dynamic);
	if (ret) {
		KGSL_CORE_ERR("set DOMAIN_ATTR_DYNAMIC failed: %d\n", ret);
		goto done;
	}
	ret = iommu_domain_set_attr(iommu_pt->domain,
				DOMAIN_ATTR_CONTEXT_BANK, &cb_num);
	if (ret) {
		KGSL_CORE_ERR("set DOMAIN_ATTR_CONTEXT_BANK failed: %d\n", ret);
		goto done;
	}

	ret = iommu_domain_set_attr(iommu_pt->domain,
				DOMAIN_ATTR_PROCID, &pt->name);
	if (ret) {
		KGSL_CORE_ERR("set DOMAIN_ATTR_PROCID failed: %d\n", ret);
		goto done;
	}

	_enable_gpuhtw_llc(mmu, iommu_pt);

	ret = _attach_pt(iommu_pt, ctx);
	if (ret)
		goto done;

	/* now read back the attributes needed for self programming */
	ret = iommu_domain_get_attr(iommu_pt->domain,
				DOMAIN_ATTR_TTBR0, &iommu_pt->ttbr0);
	if (ret) {
		KGSL_CORE_ERR("get DOMAIN_ATTR_TTBR0 failed: %d\n", ret);
		goto done;
	}

	ret = iommu_domain_get_attr(iommu_pt->domain,
				DOMAIN_ATTR_CONTEXTIDR, &iommu_pt->contextidr);
	if (ret) {
		KGSL_CORE_ERR("get DOMAIN_ATTR_CONTEXTIDR failed: %d\n", ret);
		goto done;
	}

	ret = kgsl_iommu_map_globals(pt);

done:
	if (ret)
		_free_pt(ctx, pt);

	return ret;
}

/* kgsl_iommu_init_pt - Set up an IOMMU pagetable */
static int kgsl_iommu_init_pt(struct kgsl_mmu *mmu, struct kgsl_pagetable *pt)
{
	if (pt == NULL)
		return -EINVAL;

	switch (pt->name) {
	case KGSL_MMU_GLOBAL_PT:
		return _init_global_pt(mmu, pt);

	case KGSL_MMU_SECURE_PT:
		return _init_secure_pt(mmu, pt);

	default:
		return _init_per_process_pt(mmu, pt);
	}
}

static struct kgsl_pagetable *kgsl_iommu_getpagetable(struct kgsl_mmu *mmu,
		unsigned long name)
{
	struct kgsl_pagetable *pt;

	if (!kgsl_mmu_is_perprocess(mmu) && (name != KGSL_MMU_SECURE_PT)) {
		name = KGSL_MMU_GLOBAL_PT;
		if (mmu->defaultpagetable != NULL)
			return mmu->defaultpagetable;
	}

	pt = kgsl_get_pagetable(name);
	if (pt == NULL)
		pt = kgsl_mmu_createpagetableobject(mmu, name);

	return pt;
}

/*
 * kgsl_iommu_get_reg_ahbaddr - Returns the ahb address of the register
 * @mmu - Pointer to mmu structure
 * @id - The context ID of the IOMMU ctx
 * @reg - The register for which address is required
 *
 * Return - The address of register which can be used in type0 packet
 */
static unsigned int kgsl_iommu_get_reg_ahbaddr(struct kgsl_mmu *mmu,
		int id, unsigned int reg)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[id];

	return ctx->gpu_offset + kgsl_iommu_reg_list[reg];
}

static void _detach_context(struct kgsl_iommu_context *ctx)
{
	struct kgsl_iommu_pt *iommu_pt;

	if (ctx->default_pt == NULL)
		return;

	iommu_pt = ctx->default_pt->priv;

	_detach_pt(iommu_pt, ctx);

	ctx->default_pt = NULL;
}

static void kgsl_iommu_close(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	int i;

	for (i = 0; i < KGSL_IOMMU_CONTEXT_MAX; i++)
		_detach_context(&iommu->ctx[i]);

	kgsl_mmu_putpagetable(mmu->defaultpagetable);
	mmu->defaultpagetable = NULL;

	kgsl_mmu_putpagetable(mmu->securepagetable);
	mmu->securepagetable = NULL;

	if (iommu->regbase != NULL)
		iounmap(iommu->regbase);

	kgsl_free_secure_page(kgsl_secure_guard_page);
	kgsl_secure_guard_page = NULL;

	if (kgsl_guard_page != NULL) {
		__free_page(kgsl_guard_page);
		kgsl_guard_page = NULL;
	}

	if (kgsl_dummy_page != NULL) {
		__free_page(kgsl_dummy_page);
		kgsl_dummy_page = NULL;
	}

	kgsl_iommu_remove_global(mmu, &iommu->setstate);
	kgsl_sharedmem_free(&iommu->setstate);
	kgsl_cleanup_qdss_desc(mmu);
	kgsl_cleanup_qtimer_desc(mmu);
}

static int _setstate_alloc(struct kgsl_device *device,
		struct kgsl_iommu *iommu)
{
	int ret;

	kgsl_memdesc_init(device, &iommu->setstate, 0);
	ret = kgsl_sharedmem_alloc_contig(device, &iommu->setstate, PAGE_SIZE);

	if (!ret) {
		/* Mark the setstate memory as read only */
		iommu->setstate.flags |= KGSL_MEMFLAGS_GPUREADONLY;

		kgsl_sharedmem_set(device, &iommu->setstate, 0, 0, PAGE_SIZE);
	}

	return ret;
}

static int kgsl_iommu_init(struct kgsl_mmu *mmu)
{
	struct kgsl_device *device = KGSL_MMU_DEVICE(mmu);
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	int status;

	mmu->features |= KGSL_MMU_PAGED;

	if (ctx->name == NULL) {
		KGSL_CORE_ERR("dt: gfx3d0_user context bank not found\n");
		return -EINVAL;
	}

	status = _setstate_alloc(device, iommu);
	if (status)
		return status;

	/* check requirements for per process pagetables */
	if (ctx->gpu_offset == UINT_MAX) {
		KGSL_CORE_ERR("missing qcom,gpu-offset forces global pt\n");
		mmu->features |= KGSL_MMU_GLOBAL_PAGETABLE;
	}

	if (iommu->version == 1 && iommu->micro_mmu_ctrl == UINT_MAX) {
		KGSL_CORE_ERR(
			"missing qcom,micro-mmu-control forces global pt\n");
		mmu->features |= KGSL_MMU_GLOBAL_PAGETABLE;
	}

	/* Check to see if we need to do the IOMMU sync dance */
	need_iommu_sync = of_property_read_bool(device->pdev->dev.of_node,
		"qcom,gpu-quirk-iommu-sync");

	iommu->regbase = ioremap(iommu->regstart, iommu->regsize);
	if (iommu->regbase == NULL) {
		KGSL_CORE_ERR("Could not map IOMMU registers 0x%lx:0x%x\n",
			iommu->regstart, iommu->regsize);
		status = -ENOMEM;
		goto done;
	}

	if (addr_entry_cache == NULL) {
		addr_entry_cache = KMEM_CACHE(kgsl_iommu_addr_entry, 0);
		if (addr_entry_cache == NULL) {
			status = -ENOMEM;
			goto done;
		}
	}

	kgsl_iommu_add_global(mmu, &iommu->setstate, "setstate");
	kgsl_setup_qdss_desc(device);
	kgsl_setup_qtimer_desc(device);

	mmu->defaultpagetable = kgsl_mmu_getpagetable(mmu,
				KGSL_MMU_GLOBAL_PT);
	/* if we don't have a default pagetable, nothing will work */
	if (IS_ERR(mmu->defaultpagetable)) {
		status = PTR_ERR(mmu->defaultpagetable);
		mmu->defaultpagetable = NULL;
		goto done;
	} else if (mmu->defaultpagetable == NULL) {
		status = -ENOMEM;
		goto done;
	}

	if (!mmu->secured)
		goto done;

	mmu->securepagetable = kgsl_mmu_getpagetable(mmu,
				KGSL_MMU_SECURE_PT);
	if (IS_ERR(mmu->securepagetable)) {
		status = PTR_ERR(mmu->securepagetable);
		mmu->securepagetable = NULL;
	} else if (mmu->securepagetable == NULL) {
		status = -ENOMEM;
	}

done:
	if (status)
		kgsl_iommu_close(mmu);

	return status;
}

static int _setup_user_context(struct kgsl_mmu *mmu)
{
	int ret = 0;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	struct kgsl_device *device = KGSL_MMU_DEVICE(mmu);
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_iommu_pt *iommu_pt = NULL;
	unsigned int  sctlr_val;

	if (mmu->defaultpagetable == NULL)
		return -ENOMEM;

	iommu_pt = mmu->defaultpagetable->priv;
	if (iommu_pt == NULL)
		return -ENODEV;

	ret = _attach_pt(iommu_pt, ctx);
	if (ret)
		return ret;

	ctx->default_pt = mmu->defaultpagetable;

	kgsl_iommu_enable_clk(mmu);

	sctlr_val = KGSL_IOMMU_GET_CTX_REG(ctx, SCTLR);

	/*
	 * If pagefault policy is GPUHALT_ENABLE,
	 * 1) Program CFCFG to 1 to enable STALL mode
	 * 2) Program HUPCF to 0 (Stall or terminate subsequent
	 *    transactions in the presence of an outstanding fault)
	 * else
	 * 1) Program CFCFG to 0 to disable STALL mode (0=Terminate)
	 * 2) Program HUPCF to 1 (Process subsequent transactions
	 *    independently of any outstanding fault)
	 */

	if (test_bit(KGSL_FT_PAGEFAULT_GPUHALT_ENABLE,
				&adreno_dev->ft_pf_policy)) {
		sctlr_val |= (0x1 << KGSL_IOMMU_SCTLR_CFCFG_SHIFT);
		sctlr_val &= ~(0x1 << KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
	} else {
		sctlr_val &= ~(0x1 << KGSL_IOMMU_SCTLR_CFCFG_SHIFT);
		sctlr_val |= (0x1 << KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
	}
	KGSL_IOMMU_SET_CTX_REG(ctx, SCTLR, sctlr_val);
	kgsl_iommu_disable_clk(mmu);

	return 0;
}

static int _setup_secure_context(struct kgsl_mmu *mmu)
{
	int ret;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_SECURE];
	unsigned int cb_num;

	struct kgsl_iommu_pt *iommu_pt;

	if (ctx->dev == NULL || !mmu->secured)
		return 0;

	if (mmu->securepagetable == NULL)
		return -ENOMEM;

	iommu_pt = mmu->securepagetable->priv;

	ret = _attach_pt(iommu_pt, ctx);
	if (ret)
		goto done;

	ctx->default_pt = mmu->securepagetable;

	ret = iommu_domain_get_attr(iommu_pt->domain, DOMAIN_ATTR_CONTEXT_BANK,
					&cb_num);
	if (ret) {
		KGSL_CORE_ERR("get CONTEXT_BANK attr, err %d\n", ret);
		goto done;
	}
	ctx->cb_num = cb_num;
done:
	if (ret)
		_detach_context(ctx);
	return ret;
}

static int kgsl_iommu_set_pt(struct kgsl_mmu *mmu, struct kgsl_pagetable *pt);

static int kgsl_iommu_start(struct kgsl_mmu *mmu)
{
	int status;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);

	status = _setup_user_context(mmu);
	if (status)
		return status;

	status = _setup_secure_context(mmu);
	if (status) {
		_detach_context(&iommu->ctx[KGSL_IOMMU_CONTEXT_USER]);
		return status;
	}

	if (mmu->defaultpagetable != NULL && !mmu->globalpt_mapped) {
		status = kgsl_iommu_map_globals(mmu->defaultpagetable);
		if (status)
			return status;

		mmu->globalpt_mapped = true;
	}

	/* Make sure the hardware is programmed to the default pagetable */
	kgsl_iommu_set_pt(mmu, mmu->defaultpagetable);
	set_bit(KGSL_MMU_STARTED, &mmu->flags);
	return 0;
}

static int
kgsl_iommu_unmap_offset(struct kgsl_pagetable *pt,
		struct kgsl_memdesc *memdesc, uint64_t addr,
		uint64_t offset, uint64_t size)
{
	if (size == 0 || (size + offset) > kgsl_memdesc_footprint(memdesc))
		return -EINVAL;
	/*
	 * All GPU addresses as assigned are page aligned, but some
	 * functions perturb the gpuaddr with an offset, so apply the
	 * mask here to make sure we have the right address.
	 */

	addr = PAGE_ALIGN(addr);
	if (addr == 0)
		return -EINVAL;

	return _iommu_unmap_sync_pc(pt, addr + offset, size);
}

static int
kgsl_iommu_unmap(struct kgsl_pagetable *pt, struct kgsl_memdesc *memdesc)
{
	if (memdesc->size == 0 || memdesc->gpuaddr == 0)
		return -EINVAL;

	return kgsl_iommu_unmap_offset(pt, memdesc, memdesc->gpuaddr, 0,
			kgsl_memdesc_footprint(memdesc));
}

/**
 * _iommu_map_guard_page - Map iommu guard page
 * @pt - Pointer to kgsl pagetable structure
 * @memdesc - memdesc to add guard page
 * @gpuaddr - GPU addr of guard page
 * @protflags - flags for mapping
 *
 * Return 0 on success, error on map fail
 */
static int _iommu_map_guard_page(struct kgsl_pagetable *pt,
				   struct kgsl_memdesc *memdesc,
				   uint64_t gpuaddr,
				   unsigned int protflags)
{
	uint64_t pad_size;
	phys_addr_t physaddr;

	pad_size = kgsl_memdesc_footprint(memdesc) - memdesc->size;
	if (!pad_size)
		return 0;

	/*
	 * Allocate guard page for secure buffers.
	 * This has to be done after we attach a smmu pagetable.
	 * Allocate the guard page when first secure buffer is.
	 * mapped to save 1MB of memory if CPZ is not used.
	 */
	if (kgsl_memdesc_is_secured(memdesc)) {
		if (!kgsl_secure_guard_page) {
			kgsl_secure_guard_page = kgsl_alloc_secure_page();
			if (!kgsl_secure_guard_page) {
				KGSL_CORE_ERR(
					"Secure guard page alloc failed\n");
				return -ENOMEM;
			}
		}

		physaddr = page_to_phys(kgsl_secure_guard_page);
	} else {
		if (kgsl_guard_page == NULL) {
			kgsl_guard_page = alloc_page(GFP_KERNEL | __GFP_ZERO |
					__GFP_NORETRY | __GFP_HIGHMEM);
			if (kgsl_guard_page == NULL)
				return -ENOMEM;
		}

		physaddr = page_to_phys(kgsl_guard_page);
	}

	if (!MMU_FEATURE(pt->mmu, KGSL_MMU_PAD_VA))
		protflags &= ~IOMMU_WRITE;

	return _iommu_map_single_page_sync_pc(pt, gpuaddr, physaddr,
			pad_size >> PAGE_SHIFT, protflags);
}

static unsigned int _get_protection_flags(struct kgsl_pagetable *pt,
	struct kgsl_memdesc *memdesc)
{
	unsigned int flags = IOMMU_READ | IOMMU_WRITE |
		IOMMU_NOEXEC;
	int ret, llc_nwa = 0, upstream_hint = 0;
	struct kgsl_iommu_pt *iommu_pt = pt->priv;

	ret = iommu_domain_get_attr(iommu_pt->domain,
				DOMAIN_ATTR_USE_UPSTREAM_HINT, &upstream_hint);

	if (!ret && upstream_hint)
		flags |= IOMMU_USE_UPSTREAM_HINT;

	ret = iommu_domain_get_attr(iommu_pt->domain,
				DOMAIN_ATTR_USE_LLC_NWA, &llc_nwa);

	if (!ret && llc_nwa)
		flags |= IOMMU_USE_LLC_NWA;

	if (memdesc->flags & KGSL_MEMFLAGS_GPUREADONLY)
		flags &= ~IOMMU_WRITE;

	if (memdesc->priv & KGSL_MEMDESC_PRIVILEGED)
		flags |= IOMMU_PRIV;

	if (memdesc->flags & KGSL_MEMFLAGS_IOCOHERENT)
		flags |= IOMMU_CACHE;

	if (memdesc->priv & KGSL_MEMDESC_UCODE)
		flags &= ~IOMMU_NOEXEC;

	return flags;
}

static int
kgsl_iommu_map(struct kgsl_pagetable *pt,
			struct kgsl_memdesc *memdesc)
{
	int ret;
	uint64_t addr = memdesc->gpuaddr;
	uint64_t size = memdesc->size;
	unsigned int flags = _get_protection_flags(pt, memdesc);
	struct sg_table *sgt = NULL;

	/*
	 * For paged memory allocated through kgsl, memdesc->pages is not NULL.
	 * Allocate sgt here just for its map operation. Contiguous memory
	 * already has its sgt, so no need to allocate it here.
	 */
	if (memdesc->pages != NULL)
		sgt = kgsl_alloc_sgt_from_pages(memdesc);
	else
		sgt = memdesc->sgt;

	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	ret = _iommu_map_sg_sync_pc(pt, addr, sgt->sgl, sgt->nents, flags);
	if (ret)
		goto done;

	ret = _iommu_map_guard_page(pt, memdesc, addr + size, flags);
	if (ret)
		_iommu_unmap_sync_pc(pt, addr, size);

done:
	if (memdesc->pages != NULL)
		kgsl_free_sgt(sgt);

	return ret;
}

static int kgsl_iommu_sparse_dummy_map(struct kgsl_pagetable *pt,
		struct kgsl_memdesc *memdesc, uint64_t offset, uint64_t size)
{
	int ret = 0, i;
	struct page **pages = NULL;
	struct sg_table sgt;
	int count = size >> PAGE_SHIFT;
	unsigned int map_flags;

	/* verify the offset is within our range */
	if (size + offset > kgsl_memdesc_footprint(memdesc))
		return -EINVAL;

	if (kgsl_dummy_page == NULL) {
		kgsl_dummy_page = alloc_page(GFP_KERNEL | __GFP_ZERO |
				__GFP_HIGHMEM);
		if (kgsl_dummy_page == NULL)
			return -ENOMEM;
	}

	map_flags = MMU_FEATURE(pt->mmu, KGSL_MMU_PAD_VA) ?
				_get_protection_flags(pt, memdesc) :
				IOMMU_READ | IOMMU_NOEXEC;

	pages = kcalloc(count, sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL)
		return -ENOMEM;

	for (i = 0; i < count; i++)
		pages[i] = kgsl_dummy_page;

	ret = sg_alloc_table_from_pages(&sgt, pages, count,
			0, size, GFP_KERNEL);
	if (ret == 0) {
		ret = _iommu_map_sg_sync_pc(pt, memdesc->gpuaddr + offset,
				sgt.sgl, sgt.nents, map_flags);
		sg_free_table(&sgt);
	}

	kfree(pages);

	return ret;
}

static int _map_to_one_page(struct kgsl_pagetable *pt, uint64_t addr,
		struct kgsl_memdesc *memdesc, uint64_t physoffset,
		uint64_t size, unsigned int map_flags)
{
	int ret = 0, i;
	int pg_sz = kgsl_memdesc_get_pagesize(memdesc);
	int count = size >> PAGE_SHIFT;
	struct page *page = NULL;
	struct page **pages = NULL;
	struct sg_page_iter sg_iter;
	struct sg_table sgt;

	/* Find our physaddr offset addr */
	if (memdesc->pages != NULL)
		page = memdesc->pages[physoffset >> PAGE_SHIFT];
	else {
		for_each_sg_page(memdesc->sgt->sgl, &sg_iter,
				memdesc->sgt->nents, physoffset >> PAGE_SHIFT) {
			page = sg_page_iter_page(&sg_iter);
			break;
		}
	}

	if (page == NULL)
		return -EINVAL;

	pages = kcalloc(count, sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		if (pg_sz != PAGE_SIZE) {
			struct page *tmp_page = page;
			int j;

			for (j = 0; j < 16; j++, tmp_page += PAGE_SIZE)
				pages[i++] = tmp_page;
		} else
			pages[i] = page;
	}

	ret = sg_alloc_table_from_pages(&sgt, pages, count,
			0, size, GFP_KERNEL);
	if (ret == 0) {
		ret = _iommu_map_sg_sync_pc(pt, addr, sgt.sgl,
				sgt.nents, map_flags);
		sg_free_table(&sgt);
	}

	kfree(pages);

	return ret;
}

static int kgsl_iommu_map_offset(struct kgsl_pagetable *pt,
		uint64_t virtaddr, uint64_t virtoffset,
		struct kgsl_memdesc *memdesc, uint64_t physoffset,
		uint64_t size, uint64_t feature_flag)
{
	int pg_sz;
	unsigned int protflags = _get_protection_flags(pt, memdesc);
	int ret;
	struct sg_table *sgt = NULL;

	pg_sz = kgsl_memdesc_get_pagesize(memdesc);
	if (!IS_ALIGNED(virtaddr | virtoffset | physoffset | size, pg_sz))
		return -EINVAL;

	if (size == 0)
		return -EINVAL;

	if (!(feature_flag & KGSL_SPARSE_BIND_MULTIPLE_TO_PHYS) &&
			size + physoffset > kgsl_memdesc_footprint(memdesc))
		return -EINVAL;

	/*
	 * For paged memory allocated through kgsl, memdesc->pages is not NULL.
	 * Allocate sgt here just for its map operation. Contiguous memory
	 * already has its sgt, so no need to allocate it here.
	 */
	if (memdesc->pages != NULL)
		sgt = kgsl_alloc_sgt_from_pages(memdesc);
	else
		sgt = memdesc->sgt;

	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	if (feature_flag & KGSL_SPARSE_BIND_MULTIPLE_TO_PHYS)
		ret = _map_to_one_page(pt, virtaddr + virtoffset,
				memdesc, physoffset, size, protflags);
	else
		ret = _iommu_map_sg_offset_sync_pc(pt, virtaddr + virtoffset,
				sgt->sgl, sgt->nents,
				physoffset, size, protflags);

	if (memdesc->pages != NULL)
		kgsl_free_sgt(sgt);

	return ret;
}

/* This function must be called with context bank attached */
static void kgsl_iommu_clear_fsr(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context  *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	unsigned int sctlr_val;

	if (ctx->default_pt != NULL && ctx->stalled_on_fault) {
		kgsl_iommu_enable_clk(mmu);
		KGSL_IOMMU_SET_CTX_REG(ctx, FSR, 0xffffffff);
		/*
		 * Re-enable context fault interrupts after clearing
		 * FSR to prevent the interrupt from firing repeatedly
		 */
		sctlr_val = KGSL_IOMMU_GET_CTX_REG(ctx, SCTLR);
		sctlr_val |= (0x1 << KGSL_IOMMU_SCTLR_CFIE_SHIFT);
		KGSL_IOMMU_SET_CTX_REG(ctx, SCTLR, sctlr_val);
		/*
		 * Make sure the above register writes
		 * are not reordered across the barrier
		 * as we use writel_relaxed to write them
		 */
		wmb();
		kgsl_iommu_disable_clk(mmu);
		ctx->stalled_on_fault = false;
	}
}

static void kgsl_iommu_pagefault_resume(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];

	if (ctx->default_pt != NULL && ctx->stalled_on_fault) {
		/*
		 * This will only clear fault bits in FSR. FSR.SS will still
		 * be set. Writing to RESUME (below) is the only way to clear
		 * FSR.SS bit.
		 */
		KGSL_IOMMU_SET_CTX_REG(ctx, FSR, 0xffffffff);
		/*
		 * Make sure the above register write is not reordered across
		 * the barrier as we use writel_relaxed to write it.
		 */
		wmb();

		/*
		 * Write 1 to RESUME.TnR to terminate the stalled transaction.
		 * This will also allow the SMMU to process new transactions.
		 */
		KGSL_IOMMU_SET_CTX_REG(ctx, RESUME, 1);
		/*
		 * Make sure the above register writes are not reordered across
		 * the barrier as we use writel_relaxed to write them.
		 */
		wmb();

	}
}

static void kgsl_iommu_stop(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	int i;

	/*
	 * If the iommu supports retention, we don't need
	 * to detach when stopping.
	 */
	if (!MMU_FEATURE(mmu, KGSL_MMU_RETENTION)) {
		for (i = 0; i < KGSL_IOMMU_CONTEXT_MAX; i++)
			_detach_context(&iommu->ctx[i]);
	}

	clear_bit(KGSL_MMU_STARTED, &mmu->flags);
}

static u64
kgsl_iommu_get_current_ttbr0(struct kgsl_mmu *mmu)
{
	u64 val;
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];

	/*
	 * We cannot enable or disable the clocks in interrupt context, this
	 * function is called from interrupt context if there is an axi error
	 */
	if (in_interrupt())
		return 0;

	if (ctx->regbase == NULL)
		return 0;

	kgsl_iommu_enable_clk(mmu);
	val = KGSL_IOMMU_GET_CTX_REG_Q(ctx, TTBR0);
	kgsl_iommu_disable_clk(mmu);
	return val;
}

/*
 * kgsl_iommu_set_pt - Change the IOMMU pagetable of the primary context bank
 * @mmu - Pointer to mmu structure
 * @pt - Pagetable to switch to
 *
 * Set the new pagetable for the IOMMU by doing direct register writes
 * to the IOMMU registers through the cpu
 *
 * Return - void
 */
static int kgsl_iommu_set_pt(struct kgsl_mmu *mmu, struct kgsl_pagetable *pt)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	uint64_t ttbr0, temp;
	unsigned int contextidr;
	unsigned long wait_for_flush;

	if ((pt != mmu->defaultpagetable) && !kgsl_mmu_is_perprocess(mmu))
		return 0;

	kgsl_iommu_enable_clk(mmu);

	ttbr0 = kgsl_mmu_pagetable_get_ttbr0(pt);
	contextidr = kgsl_mmu_pagetable_get_contextidr(pt);

	KGSL_IOMMU_SET_CTX_REG_Q(ctx, TTBR0, ttbr0);
	KGSL_IOMMU_SET_CTX_REG(ctx, CONTEXTIDR, contextidr);

	/* memory barrier before reading TTBR0 register */
	mb();
	temp = KGSL_IOMMU_GET_CTX_REG_Q(ctx, TTBR0);

	KGSL_IOMMU_SET_CTX_REG(ctx, TLBIALL, 1);
	/* make sure the TBLI write completes before we wait */
	mb();
	/*
	 * Wait for flush to complete by polling the flush
	 * status bit of TLBSTATUS register for not more than
	 * 2 s. After 2s just exit, at that point the SMMU h/w
	 * may be stuck and will eventually cause GPU to hang
	 * or bring the system down.
	 */
	wait_for_flush = jiffies + msecs_to_jiffies(2000);
	KGSL_IOMMU_SET_CTX_REG(ctx, TLBSYNC, 0);
	while (KGSL_IOMMU_GET_CTX_REG(ctx, TLBSTATUS) &
		(KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE)) {
		if (time_after(jiffies, wait_for_flush)) {
			KGSL_DRV_WARN(KGSL_MMU_DEVICE(mmu),
			"Wait limit reached for IOMMU tlb flush\n");
			break;
		}
		cpu_relax();
	}

	kgsl_iommu_disable_clk(mmu);
	return 0;
}

/*
 * kgsl_iommu_set_pf_policy() - Set the pagefault policy for IOMMU
 * @mmu: Pointer to mmu structure
 * @pf_policy: The pagefault polict to set
 *
 * Check if the new policy indicated by pf_policy is same as current
 * policy, if same then return else set the policy
 */
static int kgsl_iommu_set_pf_policy(struct kgsl_mmu *mmu,
				unsigned long pf_policy)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);
	struct kgsl_iommu_context *ctx = &iommu->ctx[KGSL_IOMMU_CONTEXT_USER];
	struct kgsl_device *device = KGSL_MMU_DEVICE(mmu);
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	if ((adreno_dev->ft_pf_policy &
		BIT(KGSL_FT_PAGEFAULT_GPUHALT_ENABLE)) ==
		(pf_policy & BIT(KGSL_FT_PAGEFAULT_GPUHALT_ENABLE)))
		return 0;

	/* If not attached, policy will be updated during the next attach */
	if (ctx->default_pt != NULL) {
		unsigned int sctlr_val;

		kgsl_iommu_enable_clk(mmu);

		sctlr_val = KGSL_IOMMU_GET_CTX_REG(ctx, SCTLR);

		if (test_bit(KGSL_FT_PAGEFAULT_GPUHALT_ENABLE, &pf_policy)) {
			sctlr_val |= (0x1 << KGSL_IOMMU_SCTLR_CFCFG_SHIFT);
			sctlr_val &= ~(0x1 << KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
		} else {
			sctlr_val &= ~(0x1 << KGSL_IOMMU_SCTLR_CFCFG_SHIFT);
			sctlr_val |= (0x1 << KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
		}

		KGSL_IOMMU_SET_CTX_REG(ctx, SCTLR, sctlr_val);

		kgsl_iommu_disable_clk(mmu);
	}

	return 0;
}

static struct kgsl_protected_registers *
kgsl_iommu_get_prot_regs(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = _IOMMU_PRIV(mmu);

	return &iommu->protect;
}

static struct kgsl_iommu_addr_entry *_find_gpuaddr(
		struct kgsl_pagetable *pagetable, uint64_t gpuaddr)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	struct rb_node *node = pt->rbtree.rb_node;

	while (node != NULL) {
		struct kgsl_iommu_addr_entry *entry = rb_entry(node,
			struct kgsl_iommu_addr_entry, node);

		if (gpuaddr < entry->base)
			node = node->rb_left;
		else if (gpuaddr > entry->base)
			node = node->rb_right;
		else
			return entry;
	}

	return NULL;
}

static int _remove_gpuaddr(struct kgsl_pagetable *pagetable,
		uint64_t gpuaddr)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	struct kgsl_iommu_addr_entry *entry;

	entry = _find_gpuaddr(pagetable, gpuaddr);

	if (entry != NULL) {
		rb_erase(&entry->node, &pt->rbtree);
		kmem_cache_free(addr_entry_cache, entry);
		return 0;
	}

	WARN(1, "Couldn't remove gpuaddr: 0x%llx\n", gpuaddr);
	return -ENOMEM;
}

static int _insert_gpuaddr(struct kgsl_pagetable *pagetable,
		uint64_t gpuaddr, uint64_t size)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	struct rb_node **node, *parent = NULL;
	struct kgsl_iommu_addr_entry *new =
		kmem_cache_alloc(addr_entry_cache, GFP_ATOMIC);

	if (new == NULL)
		return -ENOMEM;

	new->base = gpuaddr;
	new->size = size;

	node = &pt->rbtree.rb_node;

	while (*node != NULL) {
		struct kgsl_iommu_addr_entry *this;

		parent = *node;
		this = rb_entry(parent, struct kgsl_iommu_addr_entry, node);

		if (new->base < this->base)
			node = &parent->rb_left;
		else if (new->base > this->base)
			node = &parent->rb_right;
		else {
			/* Duplicate entry */
			WARN(1, "duplicate gpuaddr: 0x%llx\n", gpuaddr);
			kmem_cache_free(addr_entry_cache, new);
			return -EEXIST;
		}
	}

	rb_link_node(&new->node, parent, node);
	rb_insert_color(&new->node, &pt->rbtree);

	return 0;
}

static uint64_t _get_unmapped_area(struct kgsl_pagetable *pagetable,
		uint64_t bottom, uint64_t top, uint64_t size,
		uint64_t align)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	struct rb_node *node = rb_first(&pt->rbtree);
	uint64_t start;

	bottom = ALIGN(bottom, align);
	start = bottom;

	while (node != NULL) {
		uint64_t gap;
		struct kgsl_iommu_addr_entry *entry = rb_entry(node,
			struct kgsl_iommu_addr_entry, node);

		/*
		 * Skip any entries that are outside of the range, but make sure
		 * to account for some that might straddle the lower bound
		 */
		if (entry->base < bottom) {
			if (entry->base + entry->size > bottom)
				start = ALIGN(entry->base + entry->size, align);
			node = rb_next(node);
			continue;
		}

		/* Stop if we went over the top */
		if (entry->base >= top)
			break;

		/* Make sure there is a gap to consider */
		if (start < entry->base) {
			gap = entry->base - start;

			if (gap >= size)
				return start;
		}

		/* Stop if there is no more room in the region */
		if (entry->base + entry->size >= top)
			return (uint64_t) -ENOMEM;

		/* Start the next cycle at the end of the current entry */
		start = ALIGN(entry->base + entry->size, align);
		node = rb_next(node);
	}

	if (start + size <= top)
		return start;

	return (uint64_t) -ENOMEM;
}

static uint64_t _get_unmapped_area_topdown(struct kgsl_pagetable *pagetable,
		uint64_t bottom, uint64_t top, uint64_t size,
		uint64_t align)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	struct rb_node *node = rb_last(&pt->rbtree);
	uint64_t end = top;
	uint64_t mask = ~(align - 1);
	struct kgsl_iommu_addr_entry *entry;

	/* Make sure that the bottom is correctly aligned */
	bottom = ALIGN(bottom, align);

	/* Make sure the requested size will fit in the range */
	if (size > (top - bottom))
		return -ENOMEM;

	/* Walk back through the list to find the highest entry in the range */
	for (node = rb_last(&pt->rbtree); node != NULL; node = rb_prev(node)) {
		entry = rb_entry(node, struct kgsl_iommu_addr_entry, node);
		if (entry->base < top)
			break;
	}

	while (node != NULL) {
		uint64_t offset;

		entry = rb_entry(node, struct kgsl_iommu_addr_entry, node);

		/* If the entire entry is below the range the search is over */
		if ((entry->base + entry->size) < bottom)
			break;

		/* Get the top of the entry properly aligned */
		offset = ALIGN(entry->base + entry->size, align);

		/*
		 * Try to allocate the memory from the top of the gap,
		 * making sure that it fits between the top of this entry and
		 * the bottom of the previous one
		 */

		if ((end > size) && (offset < end)) {
			uint64_t chunk = (end - size) & mask;

			if (chunk >= offset)
				return chunk;
		}

		/*
		 * If we get here and the current entry is outside of the range
		 * then we are officially out of room
		 */

		if (entry->base < bottom)
			return (uint64_t) -ENOMEM;

		/* Set the top of the gap to the current entry->base */
		end = entry->base;

		/* And move on to the next lower entry */
		node = rb_prev(node);
	}

	/* If we get here then there are no more entries in the region */
	if ((end > size) && (((end - size) & mask) >= bottom))
		return (end - size) & mask;

	return (uint64_t) -ENOMEM;
}

static uint64_t kgsl_iommu_find_svm_region(struct kgsl_pagetable *pagetable,
		uint64_t start, uint64_t end, uint64_t size,
		uint64_t alignment)
{
	uint64_t addr;

	/* Avoid black holes */
	if (WARN(end <= start, "Bad search range: 0x%llx-0x%llx", start, end))
		return (uint64_t) -EINVAL;

	spin_lock(&pagetable->lock);
	addr = _get_unmapped_area_topdown(pagetable,
			start, end, size, alignment);
	spin_unlock(&pagetable->lock);
	return addr;
}

static bool iommu_addr_in_svm_ranges(struct kgsl_iommu_pt *pt,
	u64 gpuaddr, u64 size)
{
	u64 end = gpuaddr + size;

	/* Make sure size is not zero and we don't wrap around */
	if (end <= gpuaddr)
		return false;

	if ((gpuaddr >= pt->compat_va_start && gpuaddr < pt->compat_va_end) &&
		(end > pt->compat_va_start && end <= pt->compat_va_end))
		return true;

	if ((gpuaddr >= pt->svm_start && gpuaddr < pt->svm_end) &&
		(end > pt->svm_start && end <= pt->svm_end))
		return true;

	return false;
}

static int kgsl_iommu_set_svm_region(struct kgsl_pagetable *pagetable,
		uint64_t gpuaddr, uint64_t size)
{
	int ret = -ENOMEM;
	struct kgsl_iommu_pt *pt = pagetable->priv;
	struct rb_node *node;

	/* Make sure the requested address doesn't fall out of SVM range */
	if (!iommu_addr_in_svm_ranges(pt, gpuaddr, size))
		return -ENOMEM;

	spin_lock(&pagetable->lock);
	node = pt->rbtree.rb_node;

	while (node != NULL) {
		uint64_t start, end;
		struct kgsl_iommu_addr_entry *entry = rb_entry(node,
			struct kgsl_iommu_addr_entry, node);

		start = entry->base;
		end = entry->base + entry->size;

		if (gpuaddr  + size <= start)
			node = node->rb_left;
		else if (end <= gpuaddr)
			node = node->rb_right;
		else
			goto out;
	}

	ret = _insert_gpuaddr(pagetable, gpuaddr, size);
out:
	spin_unlock(&pagetable->lock);
	return ret;
}


static int kgsl_iommu_get_gpuaddr(struct kgsl_pagetable *pagetable,
		struct kgsl_memdesc *memdesc)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	int ret = 0;
	uint64_t addr, start, end, size;
	unsigned int align;

	if (WARN_ON(kgsl_memdesc_use_cpu_map(memdesc)))
		return -EINVAL;

	if (memdesc->flags & KGSL_MEMFLAGS_SECURE &&
			pagetable->name != KGSL_MMU_SECURE_PT)
		return -EINVAL;

	size = kgsl_memdesc_footprint(memdesc);

	align = max_t(uint64_t, 1 << kgsl_memdesc_get_align(memdesc),
			memdesc->pad_to);

	if (memdesc->flags & KGSL_MEMFLAGS_FORCE_32BIT) {
		start = pt->compat_va_start;
		end = pt->compat_va_end;
	} else {
		start = pt->va_start;
		end = pt->va_end;
	}

	/*
	 * When mapping secure buffers, adjust the start of the va range
	 * to the end of secure global buffers.
	 */
	if (kgsl_memdesc_is_secured(memdesc))
		start += secure_global_size;

	spin_lock(&pagetable->lock);

	addr = _get_unmapped_area(pagetable, start, end, size, align);

	if (addr == (uint64_t) -ENOMEM) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * This path is only called in a non-SVM path with locks so we can be
	 * sure we aren't racing with anybody so we don't need to worry about
	 * taking the lock
	 */
	ret = _insert_gpuaddr(pagetable, addr, size);
	if (ret == 0) {
		memdesc->gpuaddr = addr;
		memdesc->pagetable = pagetable;
	}

out:
	spin_unlock(&pagetable->lock);
	return ret;
}

static void kgsl_iommu_put_gpuaddr(struct kgsl_memdesc *memdesc)
{
	if (memdesc->pagetable == NULL)
		return;

	spin_lock(&memdesc->pagetable->lock);

	_remove_gpuaddr(memdesc->pagetable, memdesc->gpuaddr);

	spin_unlock(&memdesc->pagetable->lock);
}

static int kgsl_iommu_svm_range(struct kgsl_pagetable *pagetable,
		uint64_t *lo, uint64_t *hi, uint64_t memflags)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;
	bool gpu_compat = (memflags & KGSL_MEMFLAGS_FORCE_32BIT) != 0;

	if (lo != NULL)
		*lo = gpu_compat ? pt->compat_va_start : pt->svm_start;
	if (hi != NULL)
		*hi = gpu_compat ? pt->compat_va_end : pt->svm_end;

	return 0;
}

static bool kgsl_iommu_addr_in_range(struct kgsl_pagetable *pagetable,
		uint64_t gpuaddr, uint64_t size)
{
	struct kgsl_iommu_pt *pt = pagetable->priv;

	if (gpuaddr == 0)
		return false;

	if (gpuaddr >= pt->va_start && (gpuaddr + size) < pt->va_end)
		return true;

	if (gpuaddr >= pt->compat_va_start &&
		       (gpuaddr + size) < pt->compat_va_end)
		return true;

	if (gpuaddr >= pt->svm_start && (gpuaddr + size) < pt->svm_end)
		return true;

	return false;
}

static const struct {
	int id;
	char *name;
} kgsl_iommu_cbs[] = {
	{ KGSL_IOMMU_CONTEXT_USER, "gfx3d_user", },
	{ KGSL_IOMMU_CONTEXT_SECURE, "gfx3d_secure" },
	{ KGSL_IOMMU_CONTEXT_SECURE, "gfx3d_secure_alt" },
};

static int _kgsl_iommu_cb_probe(struct kgsl_device *device,
		struct kgsl_iommu *iommu, struct device_node *node)
{
	struct platform_device *pdev = of_find_device_by_node(node);
	struct kgsl_iommu_context *ctx = NULL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int i;

	for (i = 0; i < ARRAY_SIZE(kgsl_iommu_cbs); i++) {
		if (!strcmp(node->name, kgsl_iommu_cbs[i].name)) {
			int id = kgsl_iommu_cbs[i].id;

			if (ADRENO_QUIRK(adreno_dev,
				ADRENO_QUIRK_MMU_SECURE_CB_ALT)) {
				if (!strcmp(node->name, "gfx3d_secure"))
					continue;
			} else if (!strcmp(node->name, "gfx3d_secure_alt"))
				continue;

			ctx = &iommu->ctx[id];
			ctx->id = id;
			ctx->cb_num = -1;
			ctx->name = kgsl_iommu_cbs[i].name;

			break;
		}
	}

	if (ctx == NULL) {
		KGSL_DRV_INFO(device, "dt: Unused context label %s\n",
			node->name);
		return 0;
	}

	if (ctx->id == KGSL_IOMMU_CONTEXT_SECURE)
		device->mmu.secured = true;

	/* this property won't be found for all context banks */
	if (of_property_read_u32(node, "qcom,gpu-offset", &ctx->gpu_offset))
		ctx->gpu_offset = UINT_MAX;

	ctx->kgsldev = device;

	/* arm-smmu driver we'll have the right device pointer here. */
	if (of_find_property(node, "iommus", NULL)) {
		ctx->dev = &pdev->dev;
	} else {
		ctx->dev = kgsl_mmu_get_ctx(ctx->name);

		if (IS_ERR(ctx->dev))
			return PTR_ERR(ctx->dev);
	}

	of_dma_configure(ctx->dev, node);
	return 0;
}

static const struct {
	char *feature;
	unsigned long bit;
} kgsl_iommu_features[] = {
	{ "qcom,retention", KGSL_MMU_RETENTION },
	{ "qcom,global_pt", KGSL_MMU_GLOBAL_PAGETABLE },
	{ "qcom,hyp_secure_alloc", KGSL_MMU_HYP_SECURE_ALLOC },
	{ "qcom,force-32bit", KGSL_MMU_FORCE_32BIT },
};

static int _kgsl_iommu_probe(struct kgsl_device *device,
		struct device_node *node)
{
	const char *cname;
	struct property *prop;
	u32 reg_val[2];
	int i = 0;
	struct kgsl_iommu *iommu = KGSL_IOMMU_PRIV(device);
	struct kgsl_mmu *mmu = &device->mmu;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct device_node *child;
	struct platform_device *pdev = of_find_device_by_node(node);

	memset(iommu, 0, sizeof(*iommu));

	if (of_device_is_compatible(node, "qcom,kgsl-smmu-v1"))
		iommu->version = 1;
	else
		iommu->version = 2;

	if (of_property_read_u32_array(node, "reg", reg_val, 2)) {
		KGSL_CORE_ERR("dt: Unable to read KGSL IOMMU register range\n");
		return -EINVAL;
	}
	iommu->regstart = reg_val[0];
	iommu->regsize = reg_val[1];

	/* Protecting the SMMU registers is mandatory */
	if (of_property_read_u32_array(node, "qcom,protect", reg_val, 2)) {
		KGSL_CORE_ERR("dt: no iommu protection range specified\n");
		return -EINVAL;
	}
	iommu->protect.base = reg_val[0] / sizeof(u32);
	iommu->protect.range = reg_val[1] / sizeof(u32);

	of_property_for_each_string(node, "clock-names", prop, cname) {
		struct clk *c = devm_clk_get(&pdev->dev, cname);

		if (IS_ERR(c)) {
			KGSL_CORE_ERR("dt: Couldn't get clock: %s\n", cname);
			return -ENODEV;
		}
		if (i >= KGSL_IOMMU_MAX_CLKS) {
			KGSL_CORE_ERR("dt: too many clocks defined.\n");
			return -EINVAL;
		}

		iommu->clks[i] = c;
		++i;
	}

	for (i = 0; i < ARRAY_SIZE(kgsl_iommu_features); i++) {
		if (of_property_read_bool(node, kgsl_iommu_features[i].feature))
			mmu->features |= kgsl_iommu_features[i].bit;
	}

	/*
	 * Try to preserve the SMMU regulator if HW can support
	 * unmap fast path.
	 */
	if (of_property_read_bool(node, "qcom,unmap_fast")) {
		for (i = 0; i < KGSL_MAX_REGULATORS; i++) {
			if (!strcmp(pwr->regulators[i].name, "vddcx")) {
				iommu->vddcx_regulator =
					pwr->regulators[i].reg;
			}
		}
	}

	if (of_property_read_u32(node, "qcom,micro-mmu-control",
		&iommu->micro_mmu_ctrl))
		iommu->micro_mmu_ctrl = UINT_MAX;

	if (of_property_read_u32(node, "qcom,secure_align_mask",
		&mmu->secure_align_mask))
		mmu->secure_align_mask = 0xfff;

	if (of_property_read_u32(node, "qcom,secure-size", &mmu->secure_size))
		mmu->secure_size = KGSL_IOMMU_SECURE_SIZE;
	else if (mmu->secure_size >
			(KGSL_IOMMU_SECURE_END(mmu) - mmu->svm_base32))
		mmu->secure_size = KGSL_IOMMU_SECURE_SIZE;

	mmu->secure_base = KGSL_IOMMU_SECURE_END(mmu) - mmu->secure_size;

	/* Fill out the rest of the devices in the node */
	of_platform_populate(node, NULL, NULL, &pdev->dev);

	for_each_child_of_node(node, child) {
		int ret;

		if (!of_device_is_compatible(child, "qcom,smmu-kgsl-cb"))
			continue;

		ret = _kgsl_iommu_cb_probe(device, iommu, child);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct {
	char *compat;
	int (*probe)(struct kgsl_device *device, struct device_node *node);
} kgsl_dt_devices[] = {
	{ "qcom,kgsl-smmu-v1", _kgsl_iommu_probe },
	{ "qcom,kgsl-smmu-v2", _kgsl_iommu_probe },
};

static int kgsl_iommu_probe(struct kgsl_device *device)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kgsl_dt_devices); i++) {
		struct device_node *node;

		node = of_find_compatible_node(device->pdev->dev.of_node,
			NULL, kgsl_dt_devices[i].compat);

		if (node != NULL)
			return kgsl_dt_devices[i].probe(device, node);
	}

	return -ENODEV;
}

struct kgsl_mmu_ops kgsl_iommu_ops = {
	.mmu_init = kgsl_iommu_init,
	.mmu_close = kgsl_iommu_close,
	.mmu_start = kgsl_iommu_start,
	.mmu_stop = kgsl_iommu_stop,
	.mmu_set_pt = kgsl_iommu_set_pt,
	.mmu_clear_fsr = kgsl_iommu_clear_fsr,
	.mmu_get_current_ttbr0 = kgsl_iommu_get_current_ttbr0,
	.mmu_enable_clk = kgsl_iommu_enable_clk,
	.mmu_disable_clk = kgsl_iommu_disable_clk,
	.mmu_get_reg_ahbaddr = kgsl_iommu_get_reg_ahbaddr,
	.mmu_pt_equal = kgsl_iommu_pt_equal,
	.mmu_set_pf_policy = kgsl_iommu_set_pf_policy,
	.mmu_pagefault_resume = kgsl_iommu_pagefault_resume,
	.mmu_get_prot_regs = kgsl_iommu_get_prot_regs,
	.mmu_init_pt = kgsl_iommu_init_pt,
	.mmu_add_global = kgsl_iommu_add_global,
	.mmu_remove_global = kgsl_iommu_remove_global,
	.mmu_getpagetable = kgsl_iommu_getpagetable,
	.mmu_get_qdss_global_entry = kgsl_iommu_get_qdss_global_entry,
	.mmu_get_qtimer_global_entry = kgsl_iommu_get_qtimer_global_entry,
	.probe = kgsl_iommu_probe,
};

static struct kgsl_mmu_pt_ops iommu_pt_ops = {
	.mmu_map = kgsl_iommu_map,
	.mmu_unmap = kgsl_iommu_unmap,
	.mmu_destroy_pagetable = kgsl_iommu_destroy_pagetable,
	.get_ttbr0 = kgsl_iommu_get_ttbr0,
	.get_contextidr = kgsl_iommu_get_contextidr,
	.get_gpuaddr = kgsl_iommu_get_gpuaddr,
	.put_gpuaddr = kgsl_iommu_put_gpuaddr,
	.set_svm_region = kgsl_iommu_set_svm_region,
	.find_svm_region = kgsl_iommu_find_svm_region,
	.svm_range = kgsl_iommu_svm_range,
	.addr_in_range = kgsl_iommu_addr_in_range,
	.mmu_map_offset = kgsl_iommu_map_offset,
	.mmu_unmap_offset = kgsl_iommu_unmap_offset,
	.mmu_sparse_dummy_map = kgsl_iommu_sparse_dummy_map,
};
