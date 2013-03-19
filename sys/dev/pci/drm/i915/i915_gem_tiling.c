/*	$OpenBSD: i915_gem_tiling.c,v 1.1 2013/03/18 12:36:52 jsg Exp $	*/
/*
 * Copyright (c) 2008-2009 Owain G. Ainsworth <oga@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <dev/pci/drm/drmP.h>
#include <dev/pci/drm/drm.h>
#include <dev/pci/drm/i915_drm.h>
#include "i915_drv.h"

#include <machine/pmap.h>

#include <sys/queue.h>
#include <sys/workq.h>

/** @file i915_gem_tiling.c
 *
 * Support for managing tiling state of buffer objects.
 *
 * The idea behind tiling is to increase cache hit rates by rearranging
 * pixel data so that a group of pixel accesses are in the same cacheline.
 * Performance improvement from doing this on the back/depth buffer are on
 * the order of 30%.
 *
 * Intel architectures make this somewhat more complicated, though, by
 * adjustments made to addressing of data when the memory is in interleaved
 * mode (matched pairs of DIMMS) to improve memory bandwidth.
 * For interleaved memory, the CPU sends every sequential 64 bytes
 * to an alternate memory channel so it can get the bandwidth from both.
 *
 * The GPU also rearranges its accesses for increased bandwidth to interleaved
 * memory, and it matches what the CPU does for non-tiled.  However, when tiled
 * it does it a little differently, since one walks addresses not just in the
 * X direction but also Y.  So, along with alternating channels when bit
 * 6 of the address flips, it also alternates when other bits flip --  Bits 9
 * (every 512 bytes, an X tile scanline) and 10 (every two X tile scanlines)
 * are common to both the 915 and 965-class hardware.
 *
 * The CPU also sometimes XORs in higher bits as well, to improve
 * bandwidth doing strided access like we do so frequently in graphics.  This
 * is called "Channel XOR Randomization" in the MCH documentation.  The result
 * is that the CPU is XORing in either bit 11 or bit 17 to bit 6 of its address
 * decode.
 *
 * All of this bit 6 XORing has an effect on our memory management,
 * as we need to make sure that the 3d driver can correctly address object
 * contents.
 *
 * If we don't have interleaved memory, all tiling is safe and no swizzling is
 * required.
 *
 * When bit 17 is XORed in, we simply refuse to tile at all.  Bit
 * 17 is not just a page offset, so as we page an objet out and back in,
 * individual pages in it will have different bit 17 addresses, resulting in
 * each 64 bytes being swapped with its neighbor!
 *
 * Otherwise, if interleaved, we have to tell the 3d driver what the address
 * swizzling it needs to do is, since it's writing with the CPU to the pages
 * (bit 6 and potentially bit 11 XORed in), and the GPU is reading from the
 * pages (bit 6, 9, and 10 XORed in), resulting in a cumulative bit swizzling
 * required by the CPU of XORing in bit 6, 9, 10, and potentially 11, in order
 * to match what the GPU expects.
 */

/**
 * Detects bit 6 swizzling of address lookup between IGD access and CPU
 * access through main memory.
 */
void
i915_gem_detect_bit_6_swizzle(struct inteldrm_softc *dev_priv,
    struct pci_attach_args *bpa)
{
	struct drm_device	*dev = (struct drm_device *)dev_priv->drmdev;
	uint32_t		 swizzle_x = I915_BIT_6_SWIZZLE_UNKNOWN;
	uint32_t		 swizzle_y = I915_BIT_6_SWIZZLE_UNKNOWN;
	int			 need_disable;

	if (IS_VALLEYVIEW(dev)) {
		swizzle_x = I915_BIT_6_SWIZZLE_NONE;
		swizzle_y = I915_BIT_6_SWIZZLE_NONE;
	} else if (INTEL_INFO(dev)->gen >= 6) {
		uint32_t dimm_c0, dimm_c1;
		dimm_c0 = I915_READ(MAD_DIMM_C0);
		dimm_c1 = I915_READ(MAD_DIMM_C1);
		dimm_c0 &= MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK;
		dimm_c1 &= MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK;
		/* Enable swizzling when the channels are populated with
		 * identically sized dimms. We don't need to check the 3rd
		 * channel because no cpu with gpu attached ships in that
		 * configuration. Also, swizzling only makes sense for 2
		 * channels anyway. */
		if (dimm_c0 == dimm_c1) {
			swizzle_x = I915_BIT_6_SWIZZLE_9_10;
			swizzle_y = I915_BIT_6_SWIZZLE_9;
		} else {
			swizzle_x = I915_BIT_6_SWIZZLE_NONE;
			swizzle_y = I915_BIT_6_SWIZZLE_NONE;
		}
	} else if (IS_GEN5(dev)) {
		/*
		 * On Ironlake whatever DRAM config, GPU always do
		 * same swizzling setup.
		 */
		swizzle_x = I915_BIT_6_SWIZZLE_9_10;
		swizzle_y = I915_BIT_6_SWIZZLE_9;
	} else if (IS_GEN2(dev)) {
		/* As far as we know, the 865 doesn't have these bit 6
		 * swizzling issues.
		 */
		swizzle_x = I915_BIT_6_SWIZZLE_NONE;
		swizzle_y = I915_BIT_6_SWIZZLE_NONE;
	} else if (IS_MOBILE(dev) || (IS_GEN3(dev) && !IS_G33(dev))) {
		uint32_t dcc;

		/* try to enable MCHBAR, a lot of biosen disable it */
		need_disable = intel_setup_mchbar(dev_priv, bpa);

		/* On 915-945 and GM965, channel interleave by the CPU is
		 * determined by DCC.  The CPU will alternate based on bit 6
		 * in interleaved mode, and the GPU will then also alternate
		 * on bit 6, 9, and 10 for X, but the CPU may also optionally
		 * alternate based on bit 17 (XOR not disabled and XOR
		 * bit == 17).
		 */
		dcc = I915_READ(DCC);
		switch (dcc & DCC_ADDRESSING_MODE_MASK) {
		case DCC_ADDRESSING_MODE_SINGLE_CHANNEL:
		case DCC_ADDRESSING_MODE_DUAL_CHANNEL_ASYMMETRIC:
			swizzle_x = I915_BIT_6_SWIZZLE_NONE;
			swizzle_y = I915_BIT_6_SWIZZLE_NONE;
			break;
		case DCC_ADDRESSING_MODE_DUAL_CHANNEL_INTERLEAVED:
			if (dcc & DCC_CHANNEL_XOR_DISABLE) {
				/* This is the base swizzling by the GPU for
				 * tiled buffers.
				 */
				swizzle_x = I915_BIT_6_SWIZZLE_9_10;
				swizzle_y = I915_BIT_6_SWIZZLE_9;
			} else if ((dcc & DCC_CHANNEL_XOR_BIT_17) == 0) {
				/* Bit 11 swizzling by the CPU in addition. */
				swizzle_x = I915_BIT_6_SWIZZLE_9_10_11;
				swizzle_y = I915_BIT_6_SWIZZLE_9_11;
			} else {
				/* Bit 17 swizzling by the CPU in addition. */
				swizzle_x = I915_BIT_6_SWIZZLE_9_10_17;
				swizzle_y = I915_BIT_6_SWIZZLE_9_17;
			}
			break;
		}
		if (dcc == 0xffffffff) {
			DRM_ERROR("Couldn't read from MCHBAR.  "
				  "Disabling tiling.\n");
			swizzle_x = I915_BIT_6_SWIZZLE_UNKNOWN;
			swizzle_y = I915_BIT_6_SWIZZLE_UNKNOWN;
		}

		intel_teardown_mchbar(dev_priv, bpa, need_disable);
	} else {
		/* The 965, G33, and newer, have a very flexible memory
		 * configuration. It will enable dual-channel mode
		 * (interleaving) on as much memory as it can, and the GPU
		 * will additionally sometimes enable different bit 6
		 * swizzling for tiled objects from the CPU.
		 *
		 * Here's what I found on G965:
		 *
		 *    slot fill			memory size	swizzling
		 * 0A   0B	1A	1B	1-ch	2-ch
		 * 512	0	0	0	512	0	O
		 * 512	0	512	0	16	1008	X
		 * 512	0	0	512	16	1008	X
		 * 0	512	0	512	16	1008	X
		 * 1024	1024	1024	0	2048	1024	O
		 *
		 * We could probably detect this based on either the DRB
		 * matching, which was the case for the swizzling required in
		 * the table above, or from the 1-ch value being less than
		 * the minimum size of a rank.
		 */
		if (I915_READ16(C0DRB3) != I915_READ16(C1DRB3)) {
			swizzle_x = I915_BIT_6_SWIZZLE_NONE;
			swizzle_y = I915_BIT_6_SWIZZLE_NONE;
		} else {
			swizzle_x = I915_BIT_6_SWIZZLE_9_10;
			swizzle_y = I915_BIT_6_SWIZZLE_9;
		}
	}

	dev_priv->mm.bit_6_swizzle_x = swizzle_x;
	dev_priv->mm.bit_6_swizzle_y = swizzle_y;
}

/* Check pitch constriants for all chips & tiling formats */
bool
i915_tiling_ok(struct drm_device *dev, int stride, int size, int tiling_mode)
{
	int tile_width;

	/* Linear is always fine */
	if (tiling_mode == I915_TILING_NONE)
		return true;

	if (IS_GEN2(dev) ||
	    (tiling_mode == I915_TILING_Y && HAS_128_BYTE_Y_TILING(dev)))
		tile_width = 128;
	else
		tile_width = 512;

	/* check maximum stride & object size */
	if (INTEL_INFO(dev)->gen >= 4) {
		/* i965 stores the end address of the gtt mapping in the fence
		 * reg, so dont bother to check the size */
		if (stride / 128 > I965_FENCE_MAX_PITCH_VAL)
			return false;
	} else {
		if (stride > 8192)
			return false;

		if (IS_GEN3(dev)) {
			if (size > I830_FENCE_MAX_SIZE_VAL << 20)
				return false;
		} else {
			if (size > I830_FENCE_MAX_SIZE_VAL << 19)
				return false;
		}
	}

	/* 965+ just needs multiples of tile width */
	if (INTEL_INFO(dev)->gen >= 4) {
		if (stride & (tile_width - 1))
			return false;
		return true;
	}

	/* Pre-965 needs power of two tile widths */
	if (stride < tile_width)
		return false;

	if (stride & (stride - 1))
		return false;

	return true;
}

bool
i915_gem_object_fence_ok(struct drm_i915_gem_object *obj, int tiling_mode)
{
	u32 size;

	if (tiling_mode == I915_TILING_NONE)
		return true;

	if (INTEL_INFO(obj->base.dev)->gen >= 4)
		return true;

	if (INTEL_INFO(obj->base.dev)->gen == 3) {
		if (obj->gtt_offset & ~I915_FENCE_START_MASK)
			return false;
	} else {
		if (obj->gtt_offset & ~I830_FENCE_START_MASK)
			return false;
	}

	/*
	 * Previous chips need to be aligned to the size of the smallest
	 * fence register that can contain the object.
	 */
	if (INTEL_INFO(obj->base.dev)->gen == 3)
		size = 1024*1024;
	else
		size = 512*1024;

	while (size < obj->base.size)
		size <<= 1;

#if 0
	if (obj->gtt_space->size != size)
		return false;
#endif

	if (obj->gtt_offset & (size - 1))
		return false;

	return true;
}

/**
 * Sets the tiling mode of an object, returning the required swizzling of
 * bit 6 of addresses in the object.
 */
int
i915_gem_set_tiling(struct drm_device *dev, void *data,
		   struct drm_file *file_priv)
{
	struct drm_i915_gem_set_tiling	*args = data;
	struct inteldrm_softc		*dev_priv = dev->dev_private;
	struct drm_obj			*obj;
	struct drm_i915_gem_object	*obj_priv;
	int				 ret = 0;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (obj == NULL)
		return (EBADF);
	obj_priv = to_intel_bo(obj);
	drm_hold_object(obj);

	if (obj_priv->pin_count != 0) {
		ret = EBUSY;
		goto out;
	}
	if (i915_tiling_ok(dev, args->stride, obj->size,
	    args->tiling_mode) == 0) {
		ret = EINVAL;
		goto out;
	}

	if (args->tiling_mode == I915_TILING_NONE) {
		args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		args->stride = 0;
	} else {
		if (args->tiling_mode == I915_TILING_X)
			args->swizzle_mode = dev_priv->mm.bit_6_swizzle_x;
		else
			args->swizzle_mode = dev_priv->mm.bit_6_swizzle_y;
		/* If we can't handle the swizzling, make it untiled. */
		if (args->swizzle_mode == I915_BIT_6_SWIZZLE_UNKNOWN) {
			args->tiling_mode = I915_TILING_NONE;
			args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
			args->stride = 0;
		}
	}

	if (args->tiling_mode != obj_priv->tiling_mode ||
	    args->stride != obj_priv->stride) {
		/*
		 * We need to rebind the object if its current allocation no
		 * longer meets the alignment restrictions for its new tiling
		 * mode. Otherwise we can leave it alone, but must clear any
		 * fence register.
		 */
		/* fence may no longer be correct, wipe it */

		obj_priv->map_and_fenceable = 
		    obj_priv->dmamap == NULL ||
#ifdef notyet
		    (obj_priv->gtt_offset + obj->size <=
		    dev_priv->mm.gtt_mappable_end &&
#else
		    (
#endif
		    i915_gem_object_fence_ok(obj_priv, args->tiling_mode));

		/* Rebind if we need a change of alignment */
		if (!obj_priv->map_and_fenceable) {
			u32 unfenced_alignment =
			    i915_gem_get_unfenced_gtt_alignment(dev,
								obj_priv->base.size,
								args->tiling_mode);
			if (obj_priv->gtt_offset & (unfenced_alignment - 1))
				ret = i915_gem_object_unbind(obj_priv);
		}

		if (ret == 0) {
			obj_priv->fence_dirty =
				obj_priv->fenced_gpu_access ||
				obj_priv->fence_reg != I915_FENCE_REG_NONE;

			obj_priv->tiling_mode = args->tiling_mode;
			obj_priv->stride = args->stride;
			
			/* Force the fence to be reacquired for GTT access */
			i915_gem_release_mmap(obj_priv);
		}

	}
	/* we have to maintain this existing ABI... */
	args->stride = obj_priv->stride;
	args->tiling_mode = obj_priv->tiling_mode;
out:
	drm_unhold_and_unref(obj);

	return (ret);
}

/**
 * Returns the current tiling mode and required bit 6 swizzling for the object.
 */
int
i915_gem_get_tiling(struct drm_device *dev, void *data,
		   struct drm_file *file_priv)
{
	struct drm_i915_gem_get_tiling	*args = data;
	struct inteldrm_softc		*dev_priv = dev->dev_private;
	struct drm_obj			*obj;
	struct drm_i915_gem_object	*obj_priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (obj == NULL)
		return (EBADF);
	drm_hold_object(obj);
	obj_priv = to_intel_bo(obj);

	args->tiling_mode = obj_priv->tiling_mode;
	switch (obj_priv->tiling_mode) {
	case I915_TILING_X:
		args->swizzle_mode = dev_priv->mm.bit_6_swizzle_x;
		break;
	case I915_TILING_Y:
		args->swizzle_mode = dev_priv->mm.bit_6_swizzle_y;
		break;
	case I915_TILING_NONE:
		args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		break;
	default:
		DRM_ERROR("unknown tiling mode\n");
	}

	drm_unhold_and_unref(obj);

	return 0;
}

int
i915_gem_swizzle_page(struct vm_page *pg)
{
	vaddr_t	 va;
	int	 i;
	u_int8_t temp[64], *vaddr;

#if defined (__HAVE_PMAP_DIRECT)
	va = pmap_map_direct(pg);
#else
	va = uvm_km_valloc(kernel_map, PAGE_SIZE);
	if (va == 0)
		return (ENOMEM);
	pmap_kenter_pa(va, VM_PAGE_TO_PHYS(pg), UVM_PROT_RW);
	pmap_update(pmap_kernel());
#endif
	vaddr = (u_int8_t *)va;

	for (i = 0; i < PAGE_SIZE; i += 128) {
		memcpy(temp, &vaddr[i], 64);
		memcpy(&vaddr[i], &vaddr[i + 64], 64);
		memcpy(&vaddr[i + 64], temp, 64);
	}

#if defined (__HAVE_PMAP_DIRECT)
	pmap_unmap_direct(va);
#else
	pmap_kremove(va, PAGE_SIZE);
	pmap_update(pmap_kernel());
	uvm_km_free(kernel_map, va, PAGE_SIZE);
#endif
	return (0);
}

void
i915_gem_object_do_bit_17_swizzle(struct drm_i915_gem_object *obj)
{
	struct drm_device	*dev = obj->base.dev;
	struct inteldrm_softc	*dev_priv = dev->dev_private;
	struct vm_page		*pg;
	bus_dma_segment_t	*segp;
	int			 page_count = obj->base.size >> PAGE_SHIFT;
	int                      i, n, ret;

	if (dev_priv->mm.bit_6_swizzle_x != I915_BIT_6_SWIZZLE_9_10_17 ||
	    obj->bit_17 == NULL)
		return;

	segp = &obj->dma_segs[0];
	n = 0;
	for (i = 0; i < page_count; i++) {
		/* compare bit 17 with previous one (in case we swapped).
		 * if they don't match we'll have to swizzle the page
		 */
		if ((((segp->ds_addr + n) >> 17) & 0x1) !=
		    test_bit(i, obj->bit_17)) {
			/* XXX move this to somewhere where we already have pg */
			pg = PHYS_TO_VM_PAGE(segp->ds_addr + n);
			KASSERT(pg != NULL);
			ret = i915_gem_swizzle_page(pg);
			if (ret)
				return;
			atomic_clearbits_int(&pg->pg_flags, PG_CLEAN);
		}

		n += PAGE_SIZE;
		if (n >= segp->ds_len) {
			n = 0;
			segp++;
		}
	}

}

void
i915_gem_object_save_bit_17_swizzle(struct drm_i915_gem_object *obj)
{
	struct drm_device	*dev = obj->base.dev;
	struct inteldrm_softc	*dev_priv = dev->dev_private;
	bus_dma_segment_t	*segp;
	int			 page_count = obj->base.size >> PAGE_SHIFT;
	int			 i, n;

	if (dev_priv->mm.bit_6_swizzle_x != I915_BIT_6_SWIZZLE_9_10_17)
		return;

	if (obj->bit_17 == NULL) {
		/* round up number of pages to a multiple of 32 so we know what
		 * size to make the bitmask. XXX this is wasteful with malloc
		 * and a better way should be done
		 */
		size_t nb17 = ((page_count + 31) & ~31)/32;
		obj->bit_17 = drm_alloc(nb17 * sizeof(u_int32_t));
		if (obj-> bit_17 == NULL) {
			return;
		}

	}

	segp = &obj->dma_segs[0];
	n = 0;
	for (i = 0; i < page_count; i++) {
		if ((segp->ds_addr + n) & (1 << 17))
			set_bit(i, obj->bit_17);
		else
			clear_bit(i, obj->bit_17);

		n += PAGE_SIZE;
		if (n >= segp->ds_len) {
			n = 0;
			segp++;
		}
	}
}