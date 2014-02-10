/*	$OpenBSD: ttm_bo_util.c,v 1.4 2014/02/10 02:15:25 jsg Exp $	*/
/**************************************************************************
 *
 * Copyright (c) 2007-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 */

#include <dev/pci/drm/ttm/ttm_bo_driver.h>
#include <dev/pci/drm/ttm/ttm_placement.h>
#include <dev/pci/drm/refcount.h>

int	 ttm_mem_reg_ioremap(struct ttm_bo_device *, struct ttm_mem_reg *,
	     void **);
void	 ttm_mem_reg_iounmap(struct ttm_bo_device *, struct ttm_mem_reg *,
	     void *);

void	*kmap(struct vm_page *);
void	 kunmap(void *addr);
void	*vmap(struct vm_page **, unsigned int, unsigned long, pgprot_t);
void	 vunmap(void *, size_t);

void ttm_bo_free_old_node(struct ttm_buffer_object *bo)
{
	ttm_bo_mem_put(bo, &bo->mem);
}

int ttm_bo_move_ttm(struct ttm_buffer_object *bo,
		    bool evict,
		    bool no_wait_gpu, struct ttm_mem_reg *new_mem)
{
	struct ttm_tt *ttm = bo->ttm;
	struct ttm_mem_reg *old_mem = &bo->mem;
	int ret;

	if (old_mem->mem_type != TTM_PL_SYSTEM) {
		ttm_tt_unbind(ttm);
		ttm_bo_free_old_node(bo);
		ttm_flag_masked(&old_mem->placement, TTM_PL_FLAG_SYSTEM,
				TTM_PL_MASK_MEM);
		old_mem->mem_type = TTM_PL_SYSTEM;
	}

	ret = ttm_tt_set_placement_caching(ttm, new_mem->placement);
	if (unlikely(ret != 0))
		return ret;

	if (new_mem->mem_type != TTM_PL_SYSTEM) {
		ret = ttm_tt_bind(ttm, new_mem);
		if (unlikely(ret != 0))
			return ret;
	}

	*old_mem = *new_mem;
	new_mem->mm_node = NULL;

	return 0;
}
EXPORT_SYMBOL(ttm_bo_move_ttm);

int ttm_mem_io_lock(struct ttm_mem_type_manager *man, bool interruptible)
{
	if (likely(man->io_reserve_fastpath))
		return 0;

	if (interruptible)
		return rw_enter(&man->io_reserve_rwlock, RW_WRITE | RW_INTR);

	rw_enter_write(&man->io_reserve_rwlock);
	return 0;
}

void ttm_mem_io_unlock(struct ttm_mem_type_manager *man)
{
	if (likely(man->io_reserve_fastpath))
		return;

	rw_exit_write(&man->io_reserve_rwlock);
}

static int ttm_mem_io_evict(struct ttm_mem_type_manager *man)
{
	struct ttm_buffer_object *bo;

	if (!man->use_io_reserve_lru || list_empty(&man->io_reserve_lru))
		return -EAGAIN;

	bo = list_first_entry(&man->io_reserve_lru,
			      struct ttm_buffer_object,
			      io_reserve_lru);
	list_del_init(&bo->io_reserve_lru);
	ttm_bo_unmap_virtual_locked(bo);

	return 0;
}

static int ttm_mem_io_reserve(struct ttm_bo_device *bdev,
			      struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];
	int ret = 0;

	if (!bdev->driver->io_mem_reserve)
		return 0;
	if (likely(man->io_reserve_fastpath))
		return bdev->driver->io_mem_reserve(bdev, mem);

	if (bdev->driver->io_mem_reserve &&
	    mem->bus.io_reserved_count++ == 0) {
retry:
		ret = bdev->driver->io_mem_reserve(bdev, mem);
		if (ret == -EAGAIN) {
			ret = ttm_mem_io_evict(man);
			if (ret == 0)
				goto retry;
		}
	}
	return ret;
}

static void ttm_mem_io_free(struct ttm_bo_device *bdev,
			    struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];

	if (likely(man->io_reserve_fastpath))
		return;

	if (bdev->driver->io_mem_reserve &&
	    --mem->bus.io_reserved_count == 0 &&
	    bdev->driver->io_mem_free)
		bdev->driver->io_mem_free(bdev, mem);

}

int ttm_mem_io_reserve_vm(struct ttm_buffer_object *bo)
{
	struct ttm_mem_reg *mem = &bo->mem;
	int ret;

	if (!mem->bus.io_reserved_vm) {
		struct ttm_mem_type_manager *man =
			&bo->bdev->man[mem->mem_type];

		ret = ttm_mem_io_reserve(bo->bdev, mem);
		if (unlikely(ret != 0))
			return ret;
		mem->bus.io_reserved_vm = true;
		if (man->use_io_reserve_lru)
			list_add_tail(&bo->io_reserve_lru,
				      &man->io_reserve_lru);
	}
	return 0;
}

void ttm_mem_io_free_vm(struct ttm_buffer_object *bo)
{
	struct ttm_mem_reg *mem = &bo->mem;

	if (mem->bus.io_reserved_vm) {
		mem->bus.io_reserved_vm = false;
		list_del_init(&bo->io_reserve_lru);
		ttm_mem_io_free(bo->bdev, mem);
	}
}

int ttm_mem_reg_ioremap(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem,
			void **virtual)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];
	int ret;
	void *addr;
	int flags;

	*virtual = NULL;
	(void) ttm_mem_io_lock(man, false);
	ret = ttm_mem_io_reserve(bdev, mem);
	ttm_mem_io_unlock(man);
	if (ret || !mem->bus.is_iomem)
		return ret;

	if (mem->bus.addr) {
		addr = mem->bus.addr;
	} else {
		if (mem->placement & TTM_PL_FLAG_WC)
			flags = BUS_SPACE_MAP_PREFETCHABLE;
		else
			flags = 0;

		if (bus_space_map(bdev->memt, mem->bus.base + mem->bus.offset,
		    mem->bus.size, BUS_SPACE_MAP_LINEAR | flags, &mem->bus.bsh)) {
			printf("%s bus_space_map failed\n", __func__);
			return -ENOMEM;
		}

		addr = bus_space_vaddr(bdev->memt, mem->bus.bsh);

		if (!addr) {
			(void) ttm_mem_io_lock(man, false);
			ttm_mem_io_free(bdev, mem);
			ttm_mem_io_unlock(man);
			return -ENOMEM;
		}
	}
	*virtual = addr;
	return 0;
}

void ttm_mem_reg_iounmap(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem,
			 void *virtual)
{
	struct ttm_mem_type_manager *man;

	man = &bdev->man[mem->mem_type];

	if (virtual && mem->bus.addr == NULL)
		bus_space_unmap(bdev->memt, mem->bus.bsh, mem->bus.size);
	(void) ttm_mem_io_lock(man, false);
	ttm_mem_io_free(bdev, mem);
	ttm_mem_io_unlock(man);
}

static int ttm_copy_io_page(void *dst, void *src, unsigned long page)
{
	uint32_t *dstP =
	    (uint32_t *) ((unsigned long)dst + (page << PAGE_SHIFT));
	uint32_t *srcP =
	    (uint32_t *) ((unsigned long)src + (page << PAGE_SHIFT));

	int i;
	for (i = 0; i < PAGE_SIZE / sizeof(uint32_t); ++i)
#ifdef notyet
		iowrite32(ioread32(srcP++), dstP++);
#else
		*dstP++ = *srcP++;
#endif
	return 0;
}

static int ttm_copy_io_ttm_page(struct ttm_tt *ttm, void *src,
				unsigned long page,
				pgprot_t prot)
{
	struct vm_page *d = ttm->pages[page];
	void *dst;

	if (!d)
		return -ENOMEM;

	src = (void *)((unsigned long)src + (page << PAGE_SHIFT));

	if (pgprot_val(prot) != pgprot_val(PAGE_KERNEL))
		dst = vmap(&d, 1, 0, prot);
	else
		dst = kmap(d);
	if (!dst)
		return -ENOMEM;

	memcpy(dst, src, PAGE_SIZE);

	if (pgprot_val(prot) != pgprot_val(PAGE_KERNEL))
		vunmap(dst, PAGE_SIZE);
	else
		kunmap(d);

	return 0;
}

static int ttm_copy_ttm_io_page(struct ttm_tt *ttm, void *dst,
				unsigned long page,
				vm_prot_t prot)
{
	struct vm_page *s = ttm->pages[page];
	void *src;

	if (!s)
		return -ENOMEM;

	dst = (void *)((unsigned long)dst + (page << PAGE_SHIFT));
	if (pgprot_val(prot) != pgprot_val(PAGE_KERNEL))
		src = vmap(&s, 1, 0, prot);
	else
		src = kmap(s);
	if (!src)
		return -ENOMEM;

#define memcpy_toio(d, s, n) memcpy(d, s, n)
	memcpy_toio(dst, src, PAGE_SIZE);

	if (pgprot_val(prot) != pgprot_val(PAGE_KERNEL))
		vunmap(src, PAGE_SIZE);
	else
		kunmap(s);

	return 0;
}

int ttm_bo_move_memcpy(struct ttm_buffer_object *bo,
		       bool evict, bool no_wait_gpu,
		       struct ttm_mem_reg *new_mem)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = &bdev->man[new_mem->mem_type];
	struct ttm_tt *ttm = bo->ttm;
	struct ttm_mem_reg *old_mem = &bo->mem;
	struct ttm_mem_reg old_copy = *old_mem;
	void *old_iomap;
	void *new_iomap;
	int ret;
	unsigned long i;
	unsigned long page;
	unsigned long add = 0;
	int dir;

	ret = ttm_mem_reg_ioremap(bdev, old_mem, &old_iomap);
	if (ret)
		return ret;
	ret = ttm_mem_reg_ioremap(bdev, new_mem, &new_iomap);
	if (ret)
		goto out;

	if (old_iomap == NULL && new_iomap == NULL)
		goto out2;
	if (old_iomap == NULL && ttm == NULL)
		goto out2;

	/* TTM might be null for moves within the same region.
	 */
	if (ttm && ttm->state == tt_unpopulated) {
		ret = ttm->bdev->driver->ttm_tt_populate(ttm);
		if (ret) {
			/* if we fail here don't nuke the mm node
			 * as the bo still owns it */
			old_copy.mm_node = NULL;
			goto out1;
		}
	}

	add = 0;
	dir = 1;

	if ((old_mem->mem_type == new_mem->mem_type) &&
	    (new_mem->start < old_mem->start + old_mem->size)) {
		dir = -1;
		add = new_mem->num_pages - 1;
	}

	for (i = 0; i < new_mem->num_pages; ++i) {
		page = i * dir + add;
		if (old_iomap == NULL) {
			pgprot_t prot = ttm_io_prot(old_mem->placement,
						    PAGE_KERNEL);
			ret = ttm_copy_ttm_io_page(ttm, new_iomap, page,
						   prot);
		} else if (new_iomap == NULL) {
			pgprot_t prot = ttm_io_prot(new_mem->placement,
						    PAGE_KERNEL);
			ret = ttm_copy_io_ttm_page(ttm, old_iomap, page,
						   prot);
		} else
			ret = ttm_copy_io_page(new_iomap, old_iomap, page);
		if (ret) {
			/* failing here, means keep old copy as-is */
			old_copy.mm_node = NULL;
			goto out1;
		}
	}
	DRM_MEMORYBARRIER();
out2:
	old_copy = *old_mem;
	*old_mem = *new_mem;
	new_mem->mm_node = NULL;

	if ((man->flags & TTM_MEMTYPE_FLAG_FIXED) && (ttm != NULL)) {
		ttm_tt_unbind(ttm);
		ttm_tt_destroy(ttm);
		bo->ttm = NULL;
	}

out1:
	ttm_mem_reg_iounmap(bdev, old_mem, new_iomap);
out:
	ttm_mem_reg_iounmap(bdev, &old_copy, old_iomap);
	ttm_bo_mem_put(bo, &old_copy);
	return ret;
}
EXPORT_SYMBOL(ttm_bo_move_memcpy);

static void ttm_transfered_destroy(struct ttm_buffer_object *bo)
{
	kfree(bo);
}

/**
 * ttm_buffer_object_transfer
 *
 * @bo: A pointer to a struct ttm_buffer_object.
 * @new_obj: A pointer to a pointer to a newly created ttm_buffer_object,
 * holding the data of @bo with the old placement.
 *
 * This is a utility function that may be called after an accelerated move
 * has been scheduled. A new buffer object is created as a placeholder for
 * the old data while it's being copied. When that buffer object is idle,
 * it can be destroyed, releasing the space of the old placement.
 * Returns:
 * !0: Failure.
 */

static int ttm_buffer_object_transfer(struct ttm_buffer_object *bo,
				      struct ttm_buffer_object **new_obj)
{
	struct ttm_buffer_object *fbo;
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_driver *driver = bdev->driver;

	fbo = kmalloc(sizeof(*fbo), GFP_KERNEL);
	if (!fbo)
		return -ENOMEM;

	*fbo = *bo;

	/**
	 * Fix up members that we shouldn't copy directly:
	 * TODO: Explicit member copy would probably be better here.
	 */

#ifdef notyet
	init_waitqueue_head(&fbo->event_queue);
#endif
	INIT_LIST_HEAD(&fbo->ddestroy);
	INIT_LIST_HEAD(&fbo->lru);
	INIT_LIST_HEAD(&fbo->swap);
	INIT_LIST_HEAD(&fbo->io_reserve_lru);
	fbo->vm_node = NULL;
	atomic_set(&fbo->cpu_writers, 0);

	mtx_enter(&bdev->fence_lock);
	if (bo->sync_obj)
		fbo->sync_obj = driver->sync_obj_ref(bo->sync_obj);
	else
		fbo->sync_obj = NULL;
	mtx_leave(&bdev->fence_lock);
	refcount_init(&fbo->list_kref, 1);
	refcount_init(&fbo->kref, 1);
	fbo->destroy = &ttm_transfered_destroy;
	fbo->acc_size = 0;

	*new_obj = fbo;
	return 0;
}

pgprot_t ttm_io_prot(uint32_t caching_flags, pgprot_t tmp)
{
#ifdef PMAP_WC
	if (caching_flags & TTM_PL_FLAG_WC)
		return PMAP_WC;
	else
#endif
		return PMAP_NOCACHE;
}
EXPORT_SYMBOL(ttm_io_prot);

static int ttm_bo_ioremap(struct ttm_buffer_object *bo,
			  unsigned long offset,
			  unsigned long size,
			  struct ttm_bo_kmap_obj *map)
{
	struct ttm_mem_reg *mem = &bo->mem;
	int flags;

	if (bo->mem.bus.addr) {
		map->bo_kmap_type = ttm_bo_map_premapped;
		map->virtual = (void *)(((u8 *)bo->mem.bus.addr) + offset);
	} else {
		map->bo_kmap_type = ttm_bo_map_iomap;
		if (mem->placement & TTM_PL_FLAG_WC)
			flags = BUS_SPACE_MAP_PREFETCHABLE;
		else
			flags = 0;

		if (bus_space_map(bo->bdev->memt,
		    mem->bus.base + bo->mem.bus.offset + offset,
		    size, BUS_SPACE_MAP_LINEAR | flags,
		    &bo->mem.bus.bsh)) {
			printf("%s bus_space_map failed\n", __func__);
			map->virtual = 0;
		} else
			map->virtual = bus_space_vaddr(bo->bdev->memt,
			    bo->mem.bus.bsh);
	}
	return (!map->virtual) ? -ENOMEM : 0;
}

void *
kmap(struct vm_page *pg)
{
	vaddr_t va;

#if defined (__HAVE_PMAP_DIRECT)
	va = pmap_map_direct(pg);
#else
	va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_any, &kp_none, &kd_waitok);
	if (va == 0)
		return (NULL);
	pmap_kenter_pa(va, VM_PAGE_TO_PHYS(pg), UVM_PROT_RW);
	pmap_update(pmap_kernel());
#endif
	return (void *)va;
}

void
kunmap(void *addr)
{
	vaddr_t va = (vaddr_t)addr;

#if defined (__HAVE_PMAP_DIRECT)
	pmap_unmap_direct(va);
#else
	pmap_kremove(va, PAGE_SIZE);
	pmap_update(pmap_kernel());
	km_free((void *)va, PAGE_SIZE, &kv_any, &kp_none);
#endif
}

void *
vmap(struct vm_page **pages, unsigned int npages, unsigned long flags,
     pgprot_t prot)
{
	vaddr_t va;
	paddr_t pa;
	int i;

	va = (vaddr_t)km_alloc(PAGE_SIZE * npages, &kv_any, &kp_none,
	    &kd_waitok);
	if (va == 0)
		return NULL;
	for (i = 0; i < npages; i++) {
		pa = VM_PAGE_TO_PHYS(pages[i]) | prot;
		pmap_enter(pmap_kernel(), va + (i * PAGE_SIZE), pa,
		    VM_PROT_READ | VM_PROT_WRITE,
		    VM_PROT_READ | VM_PROT_WRITE | PMAP_WIRED);
		pmap_update(pmap_kernel());
	}

	return (void *)va;
}

void
vunmap(void *addr, size_t size)
{
	vaddr_t va = (vaddr_t)addr;

	pmap_remove(pmap_kernel(), va, va + size);
	pmap_update(pmap_kernel());
	km_free((void *)va, size, &kv_any, &kp_none);
}

static int ttm_bo_kmap_ttm(struct ttm_buffer_object *bo,
			   unsigned long start_page,
			   unsigned long num_pages,
			   struct ttm_bo_kmap_obj *map)
{
	struct ttm_mem_reg *mem = &bo->mem; pgprot_t prot;
	struct ttm_tt *ttm = bo->ttm;
	int ret;

	BUG_ON(!ttm);

	if (ttm->state == tt_unpopulated) {
		ret = ttm->bdev->driver->ttm_tt_populate(ttm);
		if (ret)
			return ret;
	}

	if (num_pages == 1 && (mem->placement & TTM_PL_FLAG_CACHED)) {
		/*
		 * We're mapping a single page, and the desired
		 * page protection is consistent with the bo.
		 */

		map->bo_kmap_type = ttm_bo_map_kmap;
		map->page = ttm->pages[start_page];
		map->virtual = kmap(map->page);
	} else {
		/*
		 * We need to use vmap to get the desired page protection
		 * or to make the buffer object look contiguous.
		 */
		prot = (mem->placement & TTM_PL_FLAG_CACHED) ?
			PAGE_KERNEL :
			ttm_io_prot(mem->placement, PAGE_KERNEL);
		map->bo_kmap_type = ttm_bo_map_vmap;
		map->virtual = vmap(ttm->pages + start_page, num_pages,
				    0, prot);
	}
	return (!map->virtual) ? -ENOMEM : 0;
}

int ttm_bo_kmap(struct ttm_buffer_object *bo,
		unsigned long start_page, unsigned long num_pages,
		struct ttm_bo_kmap_obj *map)
{
	struct ttm_mem_type_manager *man =
		&bo->bdev->man[bo->mem.mem_type];
	unsigned long offset, size;
	int ret;

	BUG_ON(!list_empty(&bo->swap));
	map->virtual = NULL;
	map->bo = bo;
	if (num_pages > bo->num_pages)
		return -EINVAL;
	if (start_page > bo->num_pages)
		return -EINVAL;
#if 0
	if (num_pages > 1 && !DRM_SUSER(DRM_CURPROC))
		return -EPERM;
#endif
	(void) ttm_mem_io_lock(man, false);
	ret = ttm_mem_io_reserve(bo->bdev, &bo->mem);
	ttm_mem_io_unlock(man);
	if (ret)
		return ret;
	if (!bo->mem.bus.is_iomem) {
		return ttm_bo_kmap_ttm(bo, start_page, num_pages, map);
	} else {
		offset = start_page << PAGE_SHIFT;
		size = num_pages << PAGE_SHIFT;
		return ttm_bo_ioremap(bo, offset, size, map);
	}
}
EXPORT_SYMBOL(ttm_bo_kmap);

void ttm_bo_kunmap(struct ttm_bo_kmap_obj *map)
{
	struct ttm_buffer_object *bo = map->bo;
	struct ttm_mem_type_manager *man =
		&bo->bdev->man[bo->mem.mem_type];

	if (!map->virtual)
		return;
	switch (map->bo_kmap_type) {
	case ttm_bo_map_iomap:
		bus_space_unmap(bo->bdev->memt, bo->mem.bus.bsh,
		    bo->mem.bus.size);
		break;
	case ttm_bo_map_vmap:
		vunmap(map->virtual, bo->mem.bus.size);
		break;
	case ttm_bo_map_kmap:
		kunmap(map->virtual);
		break;
	case ttm_bo_map_premapped:
		break;
	default:
		BUG();
	}
	(void) ttm_mem_io_lock(man, false);
	ttm_mem_io_free(map->bo->bdev, &map->bo->mem);
	ttm_mem_io_unlock(man);
	map->virtual = NULL;
	map->page = NULL;
}
EXPORT_SYMBOL(ttm_bo_kunmap);

int ttm_bo_move_accel_cleanup(struct ttm_buffer_object *bo,
			      void *sync_obj,
			      bool evict,
			      bool no_wait_gpu,
			      struct ttm_mem_reg *new_mem)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_driver *driver = bdev->driver;
	struct ttm_mem_type_manager *man = &bdev->man[new_mem->mem_type];
	struct ttm_mem_reg *old_mem = &bo->mem;
	int ret;
	struct ttm_buffer_object *ghost_obj;
	void *tmp_obj = NULL;

	mtx_enter(&bdev->fence_lock);
	if (bo->sync_obj) {
		tmp_obj = bo->sync_obj;
		bo->sync_obj = NULL;
	}
	bo->sync_obj = driver->sync_obj_ref(sync_obj);
	if (evict) {
		ret = ttm_bo_wait(bo, false, false, false);
		mtx_leave(&bdev->fence_lock);
		if (tmp_obj)
			driver->sync_obj_unref(&tmp_obj);
		if (ret)
			return ret;

		if ((man->flags & TTM_MEMTYPE_FLAG_FIXED) &&
		    (bo->ttm != NULL)) {
			ttm_tt_unbind(bo->ttm);
			ttm_tt_destroy(bo->ttm);
			bo->ttm = NULL;
		}
		ttm_bo_free_old_node(bo);
	} else {
		/**
		 * This should help pipeline ordinary buffer moves.
		 *
		 * Hang old buffer memory on a new buffer object,
		 * and leave it to be released when the GPU
		 * operation has completed.
		 */

		set_bit(TTM_BO_PRIV_FLAG_MOVING, &bo->priv_flags);
		mtx_leave(&bdev->fence_lock);
		if (tmp_obj)
			driver->sync_obj_unref(&tmp_obj);

		ret = ttm_buffer_object_transfer(bo, &ghost_obj);
		if (ret)
			return ret;

		/**
		 * If we're not moving to fixed memory, the TTM object
		 * needs to stay alive. Otherwhise hang it on the ghost
		 * bo to be unbound and destroyed.
		 */

		if (!(man->flags & TTM_MEMTYPE_FLAG_FIXED))
			ghost_obj->ttm = NULL;
		else
			bo->ttm = NULL;

		ttm_bo_unreserve(ghost_obj);
		ttm_bo_unref(&ghost_obj);
	}

	*old_mem = *new_mem;
	new_mem->mm_node = NULL;

	return 0;
}
EXPORT_SYMBOL(ttm_bo_move_accel_cleanup);
