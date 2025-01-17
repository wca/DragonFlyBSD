/*-
 *Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * Copyright (c) 2011 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by Konstantin Belousov
 * under sponsorship from the FreeBSD Foundation.
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
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Rickard E. (Rik) Faith <faith@valinux.com>
 *    Gareth Hughes <gareth@valinux.com>
 *
 */

/** @file drm_memory.c
 * Wrappers for kernel memory allocation routines, and MTRR management support.
 *
 * This file previously implemented a memory consumption tracking system using
 * the "area" argument for various different types of allocations, but that
 * has been stripped out for now.
 */

#include <drm/drmP.h>
#include "drm_legacy.h"

#ifdef HAVE_PAGE_AGP
# include <asm/agp.h>
#else
# ifdef __powerpc__
#  define PAGE_AGP	__pgprot(_PAGE_KERNEL | _PAGE_NO_CACHE)
# else
#  define PAGE_AGP	PAGE_KERNEL
# endif
#endif

MALLOC_DEFINE(M_DRM, "m_drm", "DRM memory allocations");

void drm_mem_init(void)
{
}

void drm_mem_uninit(void)
{
}

void *drm_ioremap_wc(struct drm_device *dev, drm_local_map_t *map)
{
	return pmap_mapdev_attr(map->offset, map->size, VM_MEMATTR_WRITE_COMBINING);
}

void *drm_ioremap(struct drm_device *dev, drm_local_map_t *map)
{
	return pmap_mapdev(map->offset, map->size);
}

void drm_ioremapfree(drm_local_map_t *map)
{
	pmap_unmapdev((vm_offset_t) map->handle, map->size);
}

int
drm_mtrr_add(unsigned long offset, size_t size, int flags)
{
	int act;
	struct mem_range_desc mrdesc;

	mrdesc.mr_base = offset;
	mrdesc.mr_len = size;
	mrdesc.mr_flags = flags;
	act = MEMRANGE_SET_UPDATE;
	strlcpy(mrdesc.mr_owner, "drm", sizeof(mrdesc.mr_owner));
	return mem_range_attr_set(&mrdesc, &act);
}

int
drm_mtrr_del(int __unused handle, unsigned long offset, size_t size, int flags)
{
	int act;
	struct mem_range_desc mrdesc;

	mrdesc.mr_base = offset;
	mrdesc.mr_len = size;
	mrdesc.mr_flags = flags;
	act = MEMRANGE_SET_REMOVE;
	strlcpy(mrdesc.mr_owner, "drm", sizeof(mrdesc.mr_owner));
	return mem_range_attr_set(&mrdesc, &act);
}

void
drm_clflush_pages(vm_page_t *pages, unsigned long num_pages)
{

	pmap_invalidate_cache_pages(pages, num_pages);
}
