/*
 *  linux/arch/arm/mm/dma-mapping.c
 *
 *  Copyright (C) 2000-2004 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  DMA uncached mapping support.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/vmalloc.h>

#include <asm/memory.h>
#include <asm/highmem.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/sizes.h>
#include <asm/dma-iommu.h>

/*
 * The DMA API is built upon the notion of "buffer ownership".  A buffer
 * is either exclusively owned by the CPU (and therefore may be accessed
 * by it) or exclusively owned by the DMA device.  These helper functions
 * represent the transitions between these two ownership states.
 *
 * Note, however, that on later ARMs, this notion does not work due to
 * speculative prefetches.  We model our approach on the assumption that
 * the CPU does do speculative prefetches, which means we clean caches
 * before transfers and delay cache invalidation until transfer completion.
 *
 */
static void __dma_page_cpu_to_dev(struct page *, unsigned long,
		size_t, enum dma_data_direction);
static void __dma_page_dev_to_cpu(struct page *, unsigned long,
		size_t, enum dma_data_direction);

/**
 * arm_dma_map_page - map a portion of a page for streaming DMA
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @page: page that buffer resides in
 * @offset: offset into page for start of buffer
 * @size: size of buffer to map
 * @dir: DMA transfer direction
 *
 * Ensure that any data held in the cache is appropriately discarded
 * or written back.
 *
 * The device owns this memory once this call has completed.  The CPU
 * can regain ownership by calling dma_unmap_page().
 */
static dma_addr_t arm_dma_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size, enum dma_data_direction dir,
	     struct dma_attrs *attrs)
{
	if (!arch_is_coherent())
		__dma_page_cpu_to_dev(page, offset, size, dir);
	return pfn_to_dma(dev, page_to_pfn(page)) + offset;
}

/**
 * arm_dma_unmap_page - unmap a buffer previously mapped through dma_map_page()
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @handle: DMA address of buffer
 * @size: size of buffer (same as passed to dma_map_page)
 * @dir: DMA transfer direction (same as passed to dma_map_page)
 *
 * Unmap a page streaming mode DMA translation.  The handle and size
 * must match what was provided in the previous dma_map_page() call.
 * All other usages are undefined.
 *
 * After this call, reads by the CPU to the buffer are guaranteed to see
 * whatever the device wrote there.
 */

static void arm_dma_unmap_page(struct device *dev, dma_addr_t handle,
		size_t size, enum dma_data_direction dir,
		struct dma_attrs *attrs)
{
	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(pfn_to_page(dma_to_pfn(dev, handle)),
				      handle & ~PAGE_MASK, size, dir);
}

static void arm_dma_sync_single_for_cpu(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	unsigned int offset = handle & (PAGE_SIZE - 1);
	struct page *page = pfn_to_page(dma_to_pfn(dev, handle-offset));
	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(page, offset, size, dir);
}

static void arm_dma_sync_single_for_device(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	unsigned int offset = handle & (PAGE_SIZE - 1);
	struct page *page = pfn_to_page(dma_to_pfn(dev, handle-offset));
	if (!arch_is_coherent())
		__dma_page_cpu_to_dev(page, offset, size, dir);
}

static int arm_dma_set_mask(struct device *dev, u64 dma_mask)
{
	if (!dev->dma_mask || !dma_supported(dev, dma_mask))
		return -EIO;

	*dev->dma_mask = dma_mask;

	return 0;
}

struct dma_map_ops arm_dma_ops = {
	.alloc			= arm_dma_alloc,
	.free			= arm_dma_free,
	.mmap			= arm_dma_mmap,
	.map_page		= arm_dma_map_page,
	.unmap_page		= arm_dma_unmap_page,
	.map_sg			= arm_dma_map_sg,
	.unmap_sg		= arm_dma_unmap_sg,
	.sync_single_for_cpu	= arm_dma_sync_single_for_cpu,
	.sync_single_for_device	= arm_dma_sync_single_for_device,
	.sync_sg_for_cpu	= arm_dma_sync_sg_for_cpu,
	.sync_sg_for_device	= arm_dma_sync_sg_for_device,
	.set_dma_mask		= arm_dma_set_mask,
};
EXPORT_SYMBOL(arm_dma_ops);

static u64 get_coherent_dma_mask(struct device *dev)
{
	u64 mask = ISA_DMA_THRESHOLD;

	if (dev) {
		mask = dev->coherent_dma_mask;

		/*
		 * Sanity check the DMA mask - it must be non-zero, and
		 * must be able to be satisfied by a DMA allocation.
		 */
		if (mask == 0) {
			dev_warn(dev, "coherent DMA mask is unset\n");
			return 0;
		}

		if ((~mask) & ISA_DMA_THRESHOLD) {
			dev_warn(dev, "coherent DMA mask %#llx is smaller "
				 "than system GFP_DMA mask %#llx\n",
				 mask, (unsigned long long)ISA_DMA_THRESHOLD);
			return 0;
		}
	}

	return mask;
}

static inline void __dma_clear_buffer(struct page *page, size_t size)
{
	void *ptr;
	/*
	 * Ensure that the allocated pages are zeroed, and that any data
	 * lurking in the kernel direct-mapped region is invalidated.
	 */
	ptr = page_address(page);
	if (ptr) {
		memset(ptr, 0, size);
		dmac_flush_range(ptr, ptr + size);
		outer_flush_range(__pa(ptr), __pa(ptr) + size);
	}
}

/*
 * Allocate a DMA buffer for 'dev' of size 'size' using the
 * specified gfp mask.  Note that 'size' must be page aligned.
 */
static struct page *__dma_alloc_buffer(struct device *dev, size_t size, gfp_t gfp)
{
	unsigned long order = get_order(size);
	struct page *page, *p, *e;
	u64 mask = get_coherent_dma_mask(dev);


	if (!mask)
		return NULL;

	if (mask < 0xffffffffULL)
		gfp |= GFP_DMA;

	page = alloc_pages(gfp, order);
	if (!page)
		return NULL;

	/*
	 * Now split the huge page and free the excess pages
	 */
	split_page(page, order);
	for (p = page + (size >> PAGE_SHIFT), e = page + (1 << order); p < e; p++)
		__free_page(p);

	__dma_clear_buffer(page, size);

	return page;
}

/*
 * Free a DMA buffer.  'size' must be page aligned.
 */
static void __dma_free_buffer(struct page *page, size_t size)
{
	struct page *e = page + (size >> PAGE_SHIFT);

	while (page < e) {
		__free_page(page);
		page++;
	}
}

#ifdef CONFIG_MMU
/* Sanity check size */
#if (CONSISTENT_DMA_SIZE % SZ_2M)
#error "CONSISTENT_DMA_SIZE must be multiple of 2MiB"
#endif

#define CONSISTENT_OFFSET(x)	(((unsigned long)(x) - CONSISTENT_BASE) >> PAGE_SHIFT)
#define CONSISTENT_PTE_INDEX(x) (((unsigned long)(x) - CONSISTENT_BASE) >> PGDIR_SHIFT)
#define NUM_CONSISTENT_PTES (CONSISTENT_DMA_SIZE >> PGDIR_SHIFT)

/*
 * These are the page tables (2MB each) covering uncached, DMA consistent allocations
 */
static pte_t *consistent_pte[NUM_CONSISTENT_PTES];

#include "vmregion.h"

static struct arm_vmregion_head consistent_head = {
	.vm_lock	= __SPIN_LOCK_UNLOCKED(&consistent_head.vm_lock),
	.vm_list	= LIST_HEAD_INIT(consistent_head.vm_list),
	.vm_start	= CONSISTENT_BASE,
	.vm_end		= CONSISTENT_END,
};

#ifdef CONFIG_HUGETLB_PAGE
#error ARM Coherent DMA allocator does not (yet) support huge TLB
#endif

/*
 * Initialise the consistent memory allocation.
 */
static int __init consistent_init(void)
{
	int ret = 0;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int i = 0;
	u32 base = CONSISTENT_BASE;

	do {
		pgd = pgd_offset(&init_mm, base);

		pud = pud_alloc(&init_mm, pgd, base);
		if (!pud) {
			pr_err("%s: no pud tables\n", __func__);
			ret = -ENOMEM;
			break;
		}

		pmd = pmd_alloc(&init_mm, pud, base);
		if (!pmd) {
			pr_err("%s: no pmd tables\n", __func__);
			ret = -ENOMEM;
			break;
		}
		WARN_ON(!pmd_none(*pmd));

		pte = pte_alloc_kernel(pmd, base);
		if (!pte) {
			pr_err("%s: no pte tables\n", __func__);
			ret = -ENOMEM;
			break;
		}

		consistent_pte[i++] = pte;
		base += PMD_SIZE;
	} while (base < CONSISTENT_END);

	return ret;
}

core_initcall(consistent_init);

static void *
__dma_alloc_remap(struct page *page, size_t size, gfp_t gfp, pgprot_t prot)
{
	struct arm_vmregion *c;
	size_t align;
	int bit;

	if (!consistent_pte[0]) {
		pr_err("%s: not initialised\n", __func__);
		dump_stack();
		return NULL;
	}

	/*
	 * Align the virtual region allocation - maximum alignment is
	 * a section size, minimum is a page size.  This helps reduce
	 * fragmentation of the DMA space, and also prevents allocations
	 * smaller than a section from crossing a section boundary.
	 */
	bit = fls(size - 1);
	if (bit > SECTION_SHIFT)
		bit = SECTION_SHIFT;
	align = 1 << bit;

	/*
	 * Allocate a virtual address in the consistent mapping region.
	 */
	c = arm_vmregion_alloc(&consistent_head, align, size,
			    gfp & ~(__GFP_DMA | __GFP_HIGHMEM));
	if (c) {
		pte_t *pte;
		int idx = CONSISTENT_PTE_INDEX(c->vm_start);
		u32 off = CONSISTENT_OFFSET(c->vm_start) & (PTRS_PER_PTE-1);

		pte = consistent_pte[idx] + off;
		c->priv = page;

		do {
			BUG_ON(!pte_none(*pte));

			set_pte_ext(pte, mk_pte(page, prot), 0);
			page++;
			pte++;
			off++;
			if (off >= PTRS_PER_PTE) {
				off = 0;
				pte = consistent_pte[++idx];
			}
		} while (size -= PAGE_SIZE);

		dsb();

		return (void *)c->vm_start;
	}
	return NULL;
}

static void __dma_free_remap(void *cpu_addr, size_t size)
{
	struct arm_vmregion *c;
	unsigned long addr;
	pte_t *ptep;
	int idx;
	u32 off;

	c = arm_vmregion_find_remove(&consistent_head, (unsigned long)cpu_addr);
	if (!c) {
		pr_err("%s: trying to free invalid coherent area: %p\n",
		       __func__, cpu_addr);
		dump_stack();
		return;
	}

	if ((c->vm_end - c->vm_start) != size) {
		pr_err("%s: freeing wrong coherent size (%ld != %d)\n",
		       __func__, c->vm_end - c->vm_start, size);
		dump_stack();
		size = c->vm_end - c->vm_start;
	}

	idx = CONSISTENT_PTE_INDEX(c->vm_start);
	off = CONSISTENT_OFFSET(c->vm_start) & (PTRS_PER_PTE-1);
	ptep = consistent_pte[idx] + off;
	addr = c->vm_start;
	do {
		pte_t pte = ptep_get_and_clear(&init_mm, addr, ptep);

		ptep++;
		addr += PAGE_SIZE;
		off++;
		if (off >= PTRS_PER_PTE) {
			off = 0;
			ptep = consistent_pte[++idx];
		}

		if (pte_none(pte) || !pte_present(pte))
			pr_crit("%s: bad page in kernel page table\n",
				__func__);
	} while (size -= PAGE_SIZE);

	flush_tlb_kernel_range(c->vm_start, c->vm_end);

	arm_vmregion_free(&consistent_head, c);
}

#else	/* !CONFIG_MMU */

#define __dma_alloc_remap(page, size, gfp, prot)	page_address(page)
#define __dma_free_remap(addr, size)			do { } while (0)

#endif	/* CONFIG_MMU */

static void *__dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
			 gfp_t gfp, pgprot_t prot)
{
	struct page *page;
	void *addr;

	/*
	 * Following is a work-around (a.k.a. hack) to prevent pages
	 * with __GFP_COMP being passed to split_page() which cannot
	 * handle them.  The real problem is that this flag probably
	 * should be 0 on ARM as it is not supported on this
	 * platform; see CONFIG_HUGETLBFS.
	 */
	gfp &= ~(__GFP_COMP);

	*handle = ARM_DMA_ERROR;
	size = PAGE_ALIGN(size);

	page = __dma_alloc_buffer(dev, size, gfp);
	if (!page)
		return NULL;

	if (!arch_is_coherent())
		addr = __dma_alloc_remap(page, size, gfp, prot);
	else
		addr = page_address(page);

	if (addr)
		*handle = pfn_to_dma(dev, page_to_pfn(page));
	else
		__dma_free_buffer(page, size);

	return addr;
}

static inline pgprot_t __get_dma_pgprot(struct dma_attrs *attrs, pgprot_t prot)
{
	prot = dma_get_attr(DMA_ATTR_WRITE_COMBINE, attrs) ?
			    pgprot_writecombine(prot) :
			    pgprot_dmacoherent(prot);
	return prot;
}

/*
 * Allocate DMA-coherent memory space and return both the kernel remapped
 * virtual and bus address for that space.
 */
void *arm_dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
		    gfp_t gfp, struct dma_attrs *attrs)
{
	pgprot_t prot = __get_dma_pgprot(attrs, pgprot_kernel);
	void *memory;

	if (dma_alloc_from_coherent(dev, size, handle, &memory))
		return memory;

	return __dma_alloc(dev, size, handle, gfp, prot);
}

/*
 * Create userspace mapping for the DMA-coherent memory.
 */
int arm_dma_mmap(struct device *dev, struct vm_area_struct *vma,
		 void *cpu_addr, dma_addr_t dma_addr, size_t size,
		 struct dma_attrs *attrs)
{
	int ret = -ENXIO;
#ifdef CONFIG_MMU
	unsigned long pfn = dma_to_pfn(dev, dma_addr);
	/* can not verify correctable lvcha */
	vma->vm_page_prot = dma_get_attr(DMA_ATTR_WRITE_COMBINE, attrs) ?
			    pgprot_writecombine(vma->vm_page_prot) :
			    pgprot_dmacoherent(vma->vm_page_prot);
	ret = remap_pfn_range(vma, vma->vm_start,
			      pfn + vma->vm_pgoff,
			      vma->vm_end - vma->vm_start,
			      vma->vm_page_prot);
#endif	/* CONFIG_MMU */

	return ret;
}

/*
 * free a page as defined by the above mapping.
 * Must not be called with IRQs disabled.
 */
void arm_dma_free(struct device *dev, size_t size, void *cpu_addr,
		  dma_addr_t handle, struct dma_attrs *attrs)
{
	WARN_ON(irqs_disabled());

	if (dma_release_from_coherent(dev, get_order(size), cpu_addr))
		return;

	size = PAGE_ALIGN(size);

	if (!arch_is_coherent())
		__dma_free_remap(cpu_addr, size);

	__dma_free_buffer(pfn_to_page(dma_to_pfn(dev, handle)), size);
}

static void dma_cache_maint_page(struct page *page, unsigned long offset,
	size_t size, enum dma_data_direction dir,
	void (*op)(const void *, size_t, int))
{
	/*
	 * A single sg entry may refer to multiple physically contiguous
	 * pages.  But we still need to process highmem pages individually.
	 * If highmem is not configured then the bulk of this loop gets
	 * optimized out.
	 */
	size_t left = size;
	do {
		size_t len = left;
		void *vaddr;

		if (PageHighMem(page)) {
			if (len + offset > PAGE_SIZE) {
				if (offset >= PAGE_SIZE) {
					page += offset / PAGE_SIZE;
					offset %= PAGE_SIZE;
				}
				len = min_t(size_t, PAGE_SIZE - offset, len);
			}
			vaddr = kmap_high_get(page);
			if (vaddr) {
				vaddr += offset;
				op(vaddr, len, dir);
				kunmap_high(page);
			} else if (cache_is_vipt()) {
				/* unmapped pages might still be cached */
				vaddr = kmap_atomic(page);
				op(vaddr + offset, len, dir);
				kunmap_atomic(vaddr);
			}
		} else {
			vaddr = page_address(page) + offset;
			op(vaddr, len, dir);
		}
		offset = 0;
		page++;
		left -= len;
	} while (left);
}

/*
 * Make an area consistent for devices.
 * Note: Drivers should NOT use this function directly, as it will break
 * platforms with CONFIG_DMABOUNCE.
 * Use the driver DMA support - see dma-mapping.h (dma_sync_*)
 */
static void __dma_page_cpu_to_dev(struct page *page, unsigned long off,
	size_t size, enum dma_data_direction dir)
{
	unsigned long paddr;

	dma_cache_maint_page(page, off, size, dir, dmac_map_area);

	paddr = page_to_phys(page) + off;
	if (dir == DMA_FROM_DEVICE) {
		outer_inv_range(paddr, paddr + size);
	} else {
		outer_clean_range(paddr, paddr + size);
	}
	/* FIXME: non-speculating: flush on bidirectional mappings? */
}

static void __dma_page_dev_to_cpu(struct page *page, unsigned long off,
	size_t size, enum dma_data_direction dir)
{
	unsigned long paddr = page_to_phys(page) + off;

	/* FIXME: non-speculating: not required */
	/* don't bother invalidating if DMA to device */
	if (dir != DMA_TO_DEVICE)
		outer_inv_range(paddr, paddr + size);

	dma_cache_maint_page(page, off, size, dir, dmac_unmap_area);

	/*
	 * Mark the D-cache clean for this page to avoid extra flushing.
	 */
	if (dir != DMA_TO_DEVICE && off == 0 && size >= PAGE_SIZE)
		set_bit(PG_dcache_clean, &page->flags);
}

/**
 * arm_dma_map_sg - map a set of SG buffers for streaming mode DMA
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @sg: list of buffers
 * @nents: number of buffers to map
 * @dir: DMA transfer direction
 *
 * Map a set of buffers described by scatterlist in streaming mode for DMA.
 * This is the scatter-gather version of the dma_map_single interface.
 * Here the scatter gather list elements are each tagged with the
 * appropriate dma address and length.  They are obtained via
 * sg_dma_{address,length}.
 *
 * Device ownership issues as mentioned for dma_map_single are the same
 * here.
 */
int arm_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;
	int i, j;

	for_each_sg(sg, s, nents, i) {
#ifdef CONFIG_NEED_SG_DMA_LENGTH
		s->dma_length = s->length;
#endif
		s->dma_address = ops->map_page(dev, sg_page(s), s->offset,
						s->length, dir, attrs);
		if (dma_mapping_error(dev, s->dma_address))
			goto bad_mapping;
	}
	return nents;

 bad_mapping:
	for_each_sg(sg, s, i, j)
		ops->unmap_page(dev, sg_dma_address(s), sg_dma_len(s), dir, attrs);
	return 0;
}

/**
 * arm_dma_unmap_sg - unmap a set of SG buffers mapped by dma_map_sg
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @sg: list of buffers
 * @nents: number of buffers to unmap (same as was passed to dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 *
 * Unmap a set of streaming mode DMA translations.  Again, CPU access
 * rules concerning calls here are the same as for dma_unmap_single().
 */
void arm_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;

	int i;

	for_each_sg(sg, s, nents, i)
		ops->unmap_page(dev, sg_dma_address(s), sg_dma_len(s), dir, attrs);
}

/**
 * arm_dma_sync_sg_for_cpu
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @sg: list of buffers
 * @nents: number of buffers to map (returned from dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 */
void arm_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		ops->sync_single_for_cpu(dev, sg_dma_address(s), s->length,
					 dir);
}

/**
 * arm_dma_sync_sg_for_device
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @sg: list of buffers
 * @nents: number of buffers to map (returned from dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 */
void arm_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct dma_map_ops *ops = get_dma_ops(dev);
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		ops->sync_single_for_device(dev, sg_dma_address(s), s->length,
					    dir);
}

#define PREALLOC_DMA_DEBUG_ENTRIES	4096

static int __init dma_debug_do_init(void)
{
	dma_debug_init(PREALLOC_DMA_DEBUG_ENTRIES);
	return 0;
}
fs_initcall(dma_debug_do_init);

#ifdef CONFIG_ARM_DMA_USE_IOMMU

/* IOMMU */

static inline dma_addr_t __alloc_iova(struct dma_iommu_mapping *mapping,
				      size_t size)
{
	unsigned int order = get_order(size);
	unsigned int align = 0;
	unsigned int count, start;
	unsigned long flags;

	count = ((PAGE_ALIGN(size) >> PAGE_SHIFT) +
		 (1 << mapping->order) - 1) >> mapping->order;

	if (order > mapping->order)
		align = (1 << (order - mapping->order)) - 1;

	spin_lock_irqsave(&mapping->lock, flags);
	start = bitmap_find_next_zero_area(mapping->bitmap, mapping->bits, 0,
					   count, align);
	if (start > mapping->bits) {
		spin_unlock_irqrestore(&mapping->lock, flags);
		return ARM_DMA_ERROR;
	}

	bitmap_set(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);

	return mapping->base + (start << (mapping->order + PAGE_SHIFT));
}

static inline void __free_iova(struct dma_iommu_mapping *mapping,
			       dma_addr_t addr, size_t size)
{
	unsigned int start = (addr - mapping->base) >>
			     (mapping->order + PAGE_SHIFT);
	unsigned int count = ((size >> PAGE_SHIFT) +
			      (1 << mapping->order) - 1) >> mapping->order;
	unsigned long flags;

	spin_lock_irqsave(&mapping->lock, flags);
	bitmap_clear(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);
}

static struct page **__iommu_alloc_buffer(struct device *dev, size_t size, gfp_t gfp)
{
	struct page **pages;
	int count = size >> PAGE_SHIFT;
	int array_size = count * sizeof(struct page *);
	int i=0;

	if (array_size <= PAGE_SIZE)
		pages = kzalloc(array_size, gfp);
	else
		pages = vzalloc(array_size);
	if (!pages)
		return NULL;

	while (count) {
		int j, order = __ffs(count);

		pages[i] = alloc_pages(gfp | __GFP_NOWARN, order);
		while (!pages[i] && order)
			pages[i] = alloc_pages(gfp | __GFP_NOWARN, --order);
		if (!pages[i])
			goto error;

		if (order)
			split_page(pages[i], order);
		j = 1 << order;
		while (--j)
			pages[i + j] = pages[i] + j;

		__dma_clear_buffer(pages[i], PAGE_SIZE << order);
		i += 1 << order;
		count -= 1 << order;
	}

	return pages;
error:
	while (--i)
		if (pages[i])
			__free_pages(pages[i], 0);
	if (array_size < PAGE_SIZE)
		kfree(pages);
	else
		vfree(pages);
	return NULL;
}

static int __iommu_free_buffer(struct device *dev, struct page **pages, size_t size)
{
	int count = size >> PAGE_SHIFT;
	int array_size = count * sizeof(struct page *);
	int i;
	for (i=0; i < count; i++)
		if (pages[i])
			__free_pages(pages[i], 0);
	if (array_size < PAGE_SIZE)
		kfree(pages);
	else
		vfree(pages);
	return 0;
}

/*
 * Create a CPU mapping for a specified pages
 */
static void *
__iommu_alloc_remap(struct page **pages, size_t size, gfp_t gfp, pgprot_t prot)
{
	struct arm_vmregion *c;
	size_t align;
	size_t count = size >> PAGE_SHIFT;
	int bit;

	if (!consistent_pte[0]) {
		pr_err("%s: not initialised\n", __func__);
		dump_stack();
		return NULL;
	}

	/*
	 * Align the virtual region allocation - maximum alignment is
	 * a section size, minimum is a page size.  This helps reduce
	 * fragmentation of the DMA space, and also prevents allocations
	 * smaller than a section from crossing a section boundary.
	 */
	bit = fls(size - 1);
	if (bit > SECTION_SHIFT)
		bit = SECTION_SHIFT;
	align = 1 << bit;

	/*
	 * Allocate a virtual address in the consistent mapping region.
	 */
	c = arm_vmregion_alloc(&consistent_head, align, size,
			    gfp & ~(__GFP_DMA | __GFP_HIGHMEM));
	if (c) {
		pte_t *pte;
		int idx = CONSISTENT_PTE_INDEX(c->vm_start);
		int i = 0;
		u32 off = CONSISTENT_OFFSET(c->vm_start) & (PTRS_PER_PTE-1);

		pte = consistent_pte[idx] + off;
		c->priv = pages;

		do {
			BUG_ON(!pte_none(*pte));

			set_pte_ext(pte, mk_pte(pages[i], prot), 0);
			pte++;
			off++;
			i++;
			if (off >= PTRS_PER_PTE) {
				off = 0;
				pte = consistent_pte[++idx];
			}
		} while (i < count);

		dsb();

		return (void *)c->vm_start;
	}
	return NULL;
}

/*
 * Create a mapping in device IO address space for specified pages
 */
static dma_addr_t
__iommu_create_mapping(struct device *dev, struct page **pages, size_t size)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	dma_addr_t dma_addr, iova;
	int i, ret = ARM_DMA_ERROR;

	dma_addr = __alloc_iova(mapping, size);
	if (dma_addr == ARM_DMA_ERROR)
		return dma_addr;

	iova = dma_addr;
	for (i=0; i<count; ) {
		unsigned int next_pfn = page_to_pfn(pages[i]) + 1;
		phys_addr_t phys = page_to_phys(pages[i]);
		unsigned int len, j;

		for (j = i + 1; j < count; j++, next_pfn++)
			if (page_to_pfn(pages[j]) != next_pfn)
				break;

		len = (j - i) << PAGE_SHIFT;
		ret = iommu_map(mapping->domain, iova, phys, len, 0);
		if (ret < 0)
			goto fail;
		iova += len;
		i = j;
	}
	return dma_addr;
fail:
	iommu_unmap(mapping->domain, dma_addr, iova-dma_addr);
	__free_iova(mapping, dma_addr, size);
	return ARM_DMA_ERROR;
}

static int __iommu_remove_mapping(struct device *dev, dma_addr_t iova, size_t size)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;

	/*
	 * add optional in-page offset from iova to size and align
	 * result to page size
	 */
	size = PAGE_ALIGN((iova & ~PAGE_MASK) + size);
	iova &= PAGE_MASK;

	iommu_unmap(mapping->domain, iova, size);
	__free_iova(mapping, iova, size);
	return 0;
}

static void *arm_iommu_alloc_attrs(struct device *dev, size_t size,
	    dma_addr_t *handle, gfp_t gfp, struct dma_attrs *attrs)
{
	pgprot_t prot = __get_dma_pgprot(attrs, pgprot_kernel);
	struct page **pages;
	void *addr = NULL;

	*handle = ARM_DMA_ERROR;
	size = PAGE_ALIGN(size);

	pages = __iommu_alloc_buffer(dev, size, gfp);
	if (!pages)
		return NULL;

	*handle = __iommu_create_mapping(dev, pages, size);
	if (*handle == ARM_DMA_ERROR)
		goto err_buffer;

	addr = __iommu_alloc_remap(pages, size, gfp, prot);
	if (!addr)
		goto err_mapping;

	return addr;

err_mapping:
	__iommu_remove_mapping(dev, *handle, size);
err_buffer:
	__iommu_free_buffer(dev, pages, size);
	return NULL;
}

static int arm_iommu_mmap_attrs(struct device *dev, struct vm_area_struct *vma,
		    void *cpu_addr, dma_addr_t dma_addr, size_t size,
		    struct dma_attrs *attrs)
{
	struct arm_vmregion *c;

	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot);
	c = arm_vmregion_find(&consistent_head, (unsigned long)cpu_addr);

	if (c) {
		struct page **pages = c->priv;

		unsigned long uaddr = vma->vm_start;
		unsigned long usize = vma->vm_end - vma->vm_start;
		int i = 0;

		do {
			int ret;

			ret = vm_insert_page(vma, uaddr, pages[i++]);
			if (ret) {
				pr_err("Remapping memory, error: %d\n", ret);
				return ret;
			}

			uaddr += PAGE_SIZE;
			usize -= PAGE_SIZE;
		} while (usize > 0);
	}
	return 0;
}

/*
 * free a page as defined by the above mapping.
 * Must not be called with IRQs disabled.
 */
void arm_iommu_free_attrs(struct device *dev, size_t size, void *cpu_addr,
			  dma_addr_t handle, struct dma_attrs *attrs)
{
	struct arm_vmregion *c;
	size = PAGE_ALIGN(size);

	c = arm_vmregion_find(&consistent_head, (unsigned long)cpu_addr);
	if (c) {
		struct page **pages = c->priv;
		__dma_free_remap(cpu_addr, size);
		__iommu_remove_mapping(dev, handle, size);
		__iommu_free_buffer(dev, pages, size);
	}
}

/*
 * Map a part of the scatter-gather list into contiguous io address space
 */
static int __map_sg_chunk(struct device *dev, struct scatterlist *sg,
			  size_t size, dma_addr_t *handle,
			  enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova, iova_base;
	int ret = 0;
	unsigned int count;
	struct scatterlist *s;

	size = PAGE_ALIGN(size);
	*handle = ARM_DMA_ERROR;

	iova_base = iova = __alloc_iova(mapping, size);
	if (iova == ARM_DMA_ERROR)
		return -ENOMEM;

	for (count = 0, s = sg; count < (size >> PAGE_SHIFT); s = sg_next(s)) {
		phys_addr_t phys = page_to_phys(sg_page(s));
		unsigned int len = PAGE_ALIGN(s->offset + s->length);

		if (!arch_is_coherent())
			__dma_page_cpu_to_dev(sg_page(s), s->offset, s->length, dir);

		ret = iommu_map(mapping->domain, iova, phys, len, 0);
		if (ret < 0)
			goto fail;
		count += len >> PAGE_SHIFT;
		iova += len;
	}
	*handle = iova_base;

	return 0;
fail:
	iommu_unmap(mapping->domain, iova_base, count * PAGE_SIZE);
	__free_iova(mapping, iova_base, size);
	return ret;
}

/**
 * arm_iommu_map_sg - map a set of SG buffers for streaming mode DMA
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to map
 * @dir: DMA transfer direction
 *
 * Map a set of buffers described by scatterlist in streaming mode for DMA.
 * The scatter gather list elements are merged together (if possible) and
 * tagged with the appropriate dma address and length. They are obtained via
 * sg_dma_{address,length}.
 */
int arm_iommu_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		     enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct scatterlist *s = sg, *dma = sg, *start = sg;
	int i, count = 0;
	unsigned int offset = s->offset;
	unsigned int size = s->offset + s->length;
	unsigned int max = dma_get_max_seg_size(dev);

	for (i = 1; i < nents; i++) {
		s->dma_address = ARM_DMA_ERROR;
		s->dma_length = 0;

		s = sg_next(s);

		if (s->offset || (size & ~PAGE_MASK) || size + s->length > max) {
			if (__map_sg_chunk(dev, start, size, &dma->dma_address,
			    dir) < 0)
				goto bad_mapping;

			dma->dma_address += offset;
			dma->dma_length = size - offset;

			size = offset = s->offset;
			start = s;
			dma = sg_next(dma);
			count += 1;
		}
		size += s->length;
	}
	if (__map_sg_chunk(dev, start, size, &dma->dma_address, dir) < 0)
		goto bad_mapping;

	dma->dma_address += offset;
	dma->dma_length = size - offset;

	return count+1;

bad_mapping:
	for_each_sg(sg, s, count, i)
		__iommu_remove_mapping(dev, sg_dma_address(s), sg_dma_len(s));
	return 0;
}

/**
 * arm_iommu_unmap_sg - unmap a set of SG buffers mapped by dma_map_sg
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to unmap (same as was passed to dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 *
 * Unmap a set of streaming mode DMA translations.  Again, CPU access
 * rules concerning calls here are the same as for dma_unmap_single().
 */
void arm_iommu_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		if (sg_dma_len(s))
			__iommu_remove_mapping(dev, sg_dma_address(s),
					       sg_dma_len(s));
		if (!arch_is_coherent())
			__dma_page_dev_to_cpu(sg_page(s), s->offset,
					      s->length, dir);
	}
}

/**
 * arm_iommu_sync_sg_for_cpu
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to map (returned from dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 */
void arm_iommu_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		if (!arch_is_coherent())
			__dma_page_dev_to_cpu(sg_page(s), s->offset, s->length, dir);

}

/**
 * arm_iommu_sync_sg_for_device
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to map (returned from dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 */
void arm_iommu_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		if (!arch_is_coherent())
			__dma_page_cpu_to_dev(sg_page(s), s->offset, s->length, dir);
}


/**
 * arm_iommu_map_page
 * @dev: valid struct device pointer
 * @page: page that buffer resides in
 * @offset: offset into page for start of buffer
 * @size: size of buffer to map
 * @dir: DMA transfer direction
 *
 * IOMMU aware version of arm_dma_map_page()
 */
static dma_addr_t arm_iommu_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size, enum dma_data_direction dir,
	     struct dma_attrs *attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t dma_addr;
	int ret, len = PAGE_ALIGN(size + offset);

	if (!arch_is_coherent())
		__dma_page_cpu_to_dev(page, offset, size, dir);

	dma_addr = __alloc_iova(mapping, len);
	if (dma_addr == ARM_DMA_ERROR)
		return dma_addr;

	ret = iommu_map(mapping->domain, dma_addr, page_to_phys(page), len, 0);
	if (ret < 0)
		goto fail;

	return dma_addr + offset;
fail:
	__free_iova(mapping, dma_addr, len);
	return ARM_DMA_ERROR;
}

/**
 * arm_iommu_unmap_page
 * @dev: valid struct device pointer
 * @handle: DMA address of buffer
 * @size: size of buffer (same as passed to dma_map_page)
 * @dir: DMA transfer direction (same as passed to dma_map_page)
 *
 * IOMMU aware version of arm_dma_unmap_page()
 */
static void arm_iommu_unmap_page(struct device *dev, dma_addr_t handle,
		size_t size, enum dma_data_direction dir,
		struct dma_attrs *attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(mapping->domain, iova));
	int offset = handle & ~PAGE_MASK;
	int len = PAGE_ALIGN(size + offset);

	if (!iova)
		return;

	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(page, offset, size, dir);

	iommu_unmap(mapping->domain, iova, len);
	__free_iova(mapping, iova, len);
}

static void arm_iommu_sync_single_for_cpu(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(mapping->domain, iova));
	unsigned int offset = handle & ~PAGE_MASK;

	if (!iova)
		return;

	if (!arch_is_coherent())
		__dma_page_dev_to_cpu(page, offset, size, dir);
}

static void arm_iommu_sync_single_for_device(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(mapping->domain, iova));
	unsigned int offset = handle & ~PAGE_MASK;

	if (!iova)
		return;

	__dma_page_cpu_to_dev(page, offset, size, dir);
}

static struct dma_map_ops arm_iommu_ops = {
	.alloc		= arm_iommu_alloc_attrs,
	.free		= arm_iommu_free_attrs,
	.mmap		= arm_iommu_mmap_attrs,

	.map_page		= arm_iommu_map_page,
	.unmap_page		= arm_iommu_unmap_page,
	.sync_single_for_cpu	= arm_iommu_sync_single_for_cpu,
	.sync_single_for_device	= arm_iommu_sync_single_for_device,

	.map_sg			= arm_iommu_map_sg,
	.unmap_sg		= arm_iommu_unmap_sg,
	.sync_sg_for_cpu	= arm_iommu_sync_sg_for_cpu,
	.sync_sg_for_device	= arm_iommu_sync_sg_for_device,
};

/**
 * arm_iommu_create_mapping
 * @bus: pointer to the bus holding the client device (for IOMMU calls)
 * @base: start address of the valid IO address space
 * @size: size of the valid IO address space
 * @order: accuracy of the IO addresses allocations
 *
 * Creates a mapping structure which holds information about used/unused
 * IO address ranges, which is required to perform memory allocation and
 * mapping with IOMMU aware functions.
 *
 * The client device need to be attached to the mapping with
 * arm_iommu_attach_device function.
 */
struct dma_iommu_mapping *
arm_iommu_create_mapping(struct bus_type *bus, dma_addr_t base, size_t size,
			 int order)
{
	unsigned int count = (size >> PAGE_SHIFT) + order;
	unsigned int bitmap_size = BITS_TO_LONGS(count) * sizeof(long);
	struct dma_iommu_mapping *mapping;
	int err = -ENOMEM;

	if (!count)
		return ERR_PTR(-EINVAL);
 
	mapping = kzalloc(sizeof(struct dma_iommu_mapping), GFP_KERNEL);
	if (!mapping)
		goto err;

	mapping->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!mapping->bitmap)
		goto err2;

	mapping->base = base;
	mapping->bits = BITS_PER_BYTE * bitmap_size;
	mapping->order = order;
	spin_lock_init(&mapping->lock);

	mapping->domain = iommu_domain_alloc(bus);
	if (!mapping->domain)
		goto err3;

	kref_init(&mapping->kref);
	return mapping;
err3:
	kfree(mapping->bitmap);
err2:
	kfree(mapping);
err:
	return ERR_PTR(err);
}
EXPORT_SYMBOL(arm_iommu_create_mapping);

static void release_iommu_mapping(struct kref *kref)
{
	struct dma_iommu_mapping *mapping =
		container_of(kref, struct dma_iommu_mapping, kref);

	iommu_domain_free(mapping->domain);
	kfree(mapping->bitmap);
	kfree(mapping);
}

void arm_iommu_release_mapping(struct dma_iommu_mapping *mapping)
{
	if (mapping)
		kref_put(&mapping->kref, release_iommu_mapping);
}
EXPORT_SYMBOL(arm_iommu_release_mapping);

/**
 * arm_iommu_attach_device
 * @dev: valid struct device pointer
 * @mapping: io address space mapping structure (returned from
 *	arm_iommu_create_mapping)
 *
 * Attaches specified io address space mapping to the provided device,
 * this replaces the dma operations (dma_map_ops pointer) with the
 * IOMMU aware version. More than one client might be attached to
 * the same io address space mapping.
 */
int arm_iommu_attach_device(struct device *dev,
			    struct dma_iommu_mapping *mapping)
{
	int err;

	err = iommu_attach_device(mapping->domain, dev);
	if (err)
		return err;

	kref_get(&mapping->kref);
	dev->archdata.mapping = mapping;
	set_dma_ops(dev, &arm_iommu_ops);

	pr_info("Attached IOMMU controller to %s device. dev = 0x%p\n", dev_name(dev), dev);
	return 0;
}
EXPORT_SYMBOL(arm_iommu_attach_device);

#endif
