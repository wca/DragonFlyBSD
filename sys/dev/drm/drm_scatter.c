/*-
 * Copyright (c) 2009 Robert C. Noland III <rnoland@FreeBSD.org>
 * All Rights Reserved.
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
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * $FreeBSD: src/sys/dev/drm2/drm_scatter.c,v 1.1 2012/05/22 11:07:44 kib Exp $
 */

/** @file drm_scatter.c
 * Allocation of memory for scatter-gather mappings by the graphics chip.
 * The memory allocated here is then made into an aperture in the card
 * by mapping the pages into the GART.
 */

#include <drm/drmP.h>
#include "drm_legacy.h"

int drm_legacy_sg_alloc(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_scatter_gather *request = data;
	struct drm_sg_mem *entry;
	vm_size_t size;
	vm_pindex_t pindex;

	if (dev->sg)
		return -EINVAL;

	DRM_DEBUG("request size=%ld\n", request->size);

	entry = kmalloc(sizeof(*entry), M_DRM, M_WAITOK | M_ZERO);

	size = round_page(request->size);
	entry->pages = OFF_TO_IDX(size);
	entry->busaddr = kmalloc(entry->pages * sizeof(*entry->busaddr),
	    M_DRM, M_WAITOK | M_ZERO);

	entry->vaddr = kmem_alloc_attr(&kernel_map, size, M_WAITOK | M_ZERO,
	    0, BUS_SPACE_MAXADDR_32BIT, VM_MEMATTR_WRITE_COMBINING);
	if (entry->vaddr == 0) {
		drm_legacy_sg_cleanup(entry);
		return (-ENOMEM);
	}

	for(pindex = 0; pindex < entry->pages; pindex++) {
		entry->busaddr[pindex] =
		    vtophys(entry->vaddr + IDX_TO_OFF(pindex));
	}

	DRM_LOCK(dev);
	if (dev->sg) {
		DRM_UNLOCK(dev);
		drm_legacy_sg_cleanup(entry);
		return (-EINVAL);
	}
	dev->sg = entry;
	DRM_UNLOCK(dev);

	request->handle = entry->vaddr;

	DRM_DEBUG("allocated %ju pages @ 0x%08jx, contents=%08lx\n",
	    entry->pages, (uintmax_t)entry->vaddr,
	    *(unsigned long *)entry->vaddr);

	return (0);
}

void drm_legacy_sg_cleanup(struct drm_sg_mem *entry)
{
	if (entry == NULL)
		return;

	if (entry->vaddr != 0)
		kmem_free(&kernel_map, entry->vaddr, IDX_TO_OFF(entry->pages));

	drm_free(entry->busaddr, M_DRM);
	drm_free(entry, M_DRM);

	return;
}

int drm_legacy_sg_free(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_scatter_gather *request = data;
	struct drm_sg_mem *entry;

	DRM_LOCK(dev);
	entry = dev->sg;
	dev->sg = NULL;
	DRM_UNLOCK(dev);

	if (!entry || entry->vaddr != request->handle)
		return (-EINVAL);

	DRM_DEBUG("free 0x%jx\n", (uintmax_t)entry->vaddr);

	drm_legacy_sg_cleanup(entry);

	return (0);
}
