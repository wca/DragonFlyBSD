/**************************************************************************
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
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

#define pr_fmt(fmt) "[TTM] " fmt

#include <drm/ttm/ttm_module.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_placement.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/wait.h>

#define TTM_ASSERT_LOCKED(param)
#define TTM_DEBUG(fmt, arg...)
#define TTM_BO_HASH_ORDER 13

static int ttm_bo_setup_vm(struct ttm_buffer_object *bo);
static int ttm_bo_swapout(struct ttm_mem_shrink *shrink);
static void ttm_bo_global_kobj_release(struct ttm_bo_global *glob);

static inline int ttm_mem_type_from_place(const struct ttm_place *place,
					  uint32_t *mem_type)
{
	int i;

	for (i = 0; i <= TTM_PL_PRIV5; i++)
		if (place->flags & (1 << i)) {
			*mem_type = i;
			return 0;
		}
	return -EINVAL;
}

static void ttm_mem_type_debug(struct ttm_bo_device *bdev, int mem_type)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];

	pr_err("    has_type: %d\n", man->has_type);
	pr_err("    use_type: %d\n", man->use_type);
	pr_err("    flags: 0x%08X\n", man->flags);
	pr_err("    gpu_offset: 0x%08lX\n", man->gpu_offset);
	pr_err("    size: %ju\n", (uintmax_t)man->size);
	pr_err("    available_caching: 0x%08X\n", man->available_caching);
	pr_err("    default_caching: 0x%08X\n", man->default_caching);
	if (mem_type != TTM_PL_SYSTEM)
		(*man->func->debug)(man, TTM_PFX);
}

static void ttm_bo_mem_space_debug(struct ttm_buffer_object *bo,
					struct ttm_placement *placement)
{
	int i, ret, mem_type;

	pr_err("No space for %p (%lu pages, %luK, %luM)\n",
	       bo, bo->mem.num_pages, bo->mem.size >> 10,
	       bo->mem.size >> 20);
	for (i = 0; i < placement->num_placement; i++) {
		ret = ttm_mem_type_from_place(&placement->placement[i],
						&mem_type);
		if (ret)
			return;
		pr_err("  placement[%d]=0x%08X (%d)\n",
		       i, placement->placement[i].flags, mem_type);
		ttm_mem_type_debug(bo->bdev, mem_type);
	}
}

#if 0
static ssize_t ttm_bo_global_show(struct ttm_bo_global *glob,
    char *buffer)
{

	return snprintf(buffer, PAGE_SIZE, "%lu\n",
			(unsigned long) atomic_read(&glob->bo_count));
}
#endif

static inline uint32_t ttm_bo_type_flags(unsigned type)
{
	return 1 << (type);
}

static void ttm_bo_release_list(struct kref *list_kref)
{
	struct ttm_buffer_object *bo =
	    container_of(list_kref, struct ttm_buffer_object, list_kref);
	struct ttm_bo_device *bdev = bo->bdev;
	size_t acc_size = bo->acc_size;

	BUG_ON(atomic_read(&bo->list_kref.refcount));
	BUG_ON(atomic_read(&bo->kref.refcount));
	BUG_ON(atomic_read(&bo->cpu_writers));
	BUG_ON(bo->sync_obj != NULL);
	BUG_ON(bo->mem.mm_node != NULL);
	BUG_ON(!list_empty(&bo->lru));
	BUG_ON(!list_empty(&bo->ddestroy));

	if (bo->ttm)
		ttm_tt_destroy(bo->ttm);
	atomic_dec(&bo->glob->bo_count);
	if (bo->destroy)
		bo->destroy(bo);
	else {
		kfree(bo);
	}
	ttm_mem_global_free(bdev->glob->mem_glob, acc_size);
}

static int ttm_bo_wait_unreserved(struct ttm_buffer_object *bo,
				  bool interruptible)
{
	if (interruptible) {
		return wait_event_interruptible(bo->event_queue,
					       !ttm_bo_is_reserved(bo));
	} else {
		wait_event(bo->event_queue, !ttm_bo_is_reserved(bo));
		return 0;
	}
}

void ttm_bo_add_to_lru(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man;

	BUG_ON(!ttm_bo_is_reserved(bo));

	if (!(bo->mem.placement & TTM_PL_FLAG_NO_EVICT)) {

		BUG_ON(!list_empty(&bo->lru));

		man = &bdev->man[bo->mem.mem_type];
		list_add_tail(&bo->lru, &man->lru);
		kref_get(&bo->list_kref);

		if (bo->ttm != NULL) {
			list_add_tail(&bo->swap, &bo->glob->swap_lru);
			kref_get(&bo->list_kref);
		}
	}
}

int ttm_bo_del_from_lru(struct ttm_buffer_object *bo)
{
	int put_count = 0;

	if (!list_empty(&bo->swap)) {
		list_del_init(&bo->swap);
		++put_count;
	}
	if (!list_empty(&bo->lru)) {
		list_del_init(&bo->lru);
		++put_count;
	}

	/*
	 * TODO: Add a driver hook to delete from
	 * driver-specific LRU's here.
	 */

	return put_count;
}

int ttm_bo_reserve_nolru(struct ttm_buffer_object *bo,
			  bool interruptible,
			  bool no_wait, bool use_ticket,
			  struct ww_acquire_ctx *ticket)
{
	int ret;

	while (unlikely(atomic_xchg(&bo->reserved, 1) != 0)) {
		/**
		 * Deadlock avoidance for multi-bo reserving.
		 */
		if (use_ticket && bo->seq_valid) {
			/**
			 * We've already reserved this one.
			 */
			if (unlikely(ticket->stamp == bo->val_seq))
				return -EDEADLK;
			/**
			 * Already reserved by a thread that will not back
			 * off for us. We need to back off.
			 */
			if (unlikely(ticket->stamp - bo->val_seq <= LONG_MAX))
				return -EAGAIN;
		}

		if (no_wait)
			return -EBUSY;

		ret = ttm_bo_wait_unreserved(bo, interruptible);

		if (unlikely(ret))
			return ret;
	}

	if (use_ticket) {
		bool wake_up = false;

		/**
		 * Wake up waiters that may need to recheck for deadlock,
		 * if we decreased the sequence number.
		 */
		if (unlikely((bo->val_seq - ticket->stamp <= LONG_MAX)
			     || !bo->seq_valid))
			wake_up = true;

		/*
		 * In the worst case with memory ordering these values can be
		 * seen in the wrong order. However since we call wake_up_all
		 * in that case, this will hopefully not pose a problem,
		 * and the worst case would only cause someone to accidentally
		 * hit -EAGAIN in ttm_bo_reserve when they see old value of
		 * val_seq. However this would only happen if seq_valid was
		 * written before val_seq was, and just means some slightly
		 * increased cpu usage
		 */
		bo->val_seq = ticket->stamp;
		bo->seq_valid = true;
		if (wake_up)
			wake_up_all(&bo->event_queue);
	} else {
		bo->seq_valid = false;
	}

	return 0;
}
EXPORT_SYMBOL(ttm_bo_reserve);

static void ttm_bo_ref_bug(struct kref *list_kref)
{
	BUG();
}

void ttm_bo_list_ref_sub(struct ttm_buffer_object *bo, int count,
			 bool never_free)
{
	kref_sub(&bo->list_kref, count,
		 (never_free) ? ttm_bo_ref_bug : ttm_bo_release_list);
}

int ttm_bo_reserve(struct ttm_buffer_object *bo,
		   bool interruptible,
		   bool no_wait, bool use_ticket,
		   struct ww_acquire_ctx *ticket)
{
	struct ttm_bo_global *glob = bo->glob;
	int put_count = 0;
	int ret;

	ret = ttm_bo_reserve_nolru(bo, interruptible, no_wait, use_ticket,
				    ticket);
	if (likely(ret == 0)) {
		lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
		put_count = ttm_bo_del_from_lru(bo);
		lockmgr(&glob->lru_lock, LK_RELEASE);
		ttm_bo_list_ref_sub(bo, put_count, true);
	}

	return ret;
}

int ttm_bo_reserve_slowpath_nolru(struct ttm_buffer_object *bo,
				  bool interruptible,
				  struct ww_acquire_ctx *ticket)
{
	bool wake_up = false;
	int ret;

	while (unlikely(atomic_xchg(&bo->reserved, 1) != 0)) {
		WARN_ON(bo->seq_valid && ticket->stamp == bo->val_seq);

		ret = ttm_bo_wait_unreserved(bo, interruptible);

		if (unlikely(ret))
			return ret;
	}

	if (bo->val_seq - ticket->stamp < LONG_MAX || !bo->seq_valid)
		wake_up = true;

	/**
	 * Wake up waiters that may need to recheck for deadlock,
	 * if we decreased the sequence number.
	 */
	bo->val_seq = ticket->stamp;
	bo->seq_valid = true;
	if (wake_up)
		wake_up_all(&bo->event_queue);

	return 0;
}

int ttm_bo_reserve_slowpath(struct ttm_buffer_object *bo,
			    bool interruptible, struct ww_acquire_ctx *ticket)
{
	struct ttm_bo_global *glob = bo->glob;
	int put_count, ret;

	ret = ttm_bo_reserve_slowpath_nolru(bo, interruptible, ticket);
	if (likely(!ret)) {
		lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
		put_count = ttm_bo_del_from_lru(bo);
		lockmgr(&glob->lru_lock, LK_RELEASE);
		ttm_bo_list_ref_sub(bo, put_count, true);
	}
	return ret;
}
EXPORT_SYMBOL(ttm_bo_reserve_slowpath);

/*
 * Must interlock with event_queue to avoid race against
 * wait_event_common() which can cause wait_event_common()
 * to become stuck.
 */
static void
ttm_bo_unreserve_core(struct ttm_buffer_object *bo)
{
	lockmgr(&bo->event_queue.lock, LK_EXCLUSIVE);
	atomic_set(&bo->reserved, 0);
	lockmgr(&bo->event_queue.lock, LK_RELEASE);
	wake_up_all(&bo->event_queue);
}

void ttm_bo_unreserve_ticket_locked(struct ttm_buffer_object *bo, struct ww_acquire_ctx *ticket)
{
	ttm_bo_add_to_lru(bo);
	ttm_bo_unreserve_core(bo);
}

void ttm_bo_unreserve(struct ttm_buffer_object *bo)
{
	struct ttm_bo_global *glob = bo->glob;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	ttm_bo_unreserve_ticket_locked(bo, NULL);
	lockmgr(&glob->lru_lock, LK_RELEASE);
}
EXPORT_SYMBOL(ttm_bo_unreserve);

void ttm_bo_unreserve_ticket(struct ttm_buffer_object *bo, struct ww_acquire_ctx *ticket)
{
	struct ttm_bo_global *glob = bo->glob;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	ttm_bo_unreserve_ticket_locked(bo, ticket);
	lockmgr(&glob->lru_lock, LK_RELEASE);
}
EXPORT_SYMBOL(ttm_bo_unreserve_ticket);

/*
 * Call bo->mutex locked.
 */
static int ttm_bo_add_ttm(struct ttm_buffer_object *bo, bool zero_alloc)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_global *glob = bo->glob;
	int ret = 0;
	uint32_t page_flags = 0;

	TTM_ASSERT_LOCKED(&bo->mutex);
	bo->ttm = NULL;

	if (bdev->need_dma32)
		page_flags |= TTM_PAGE_FLAG_DMA32;

	switch (bo->type) {
	case ttm_bo_type_device:
		if (zero_alloc)
			page_flags |= TTM_PAGE_FLAG_ZERO_ALLOC;
	case ttm_bo_type_kernel:
		bo->ttm = bdev->driver->ttm_tt_create(bdev, bo->num_pages << PAGE_SHIFT,
						      page_flags, glob->dummy_read_page);
		if (unlikely(bo->ttm == NULL))
			ret = -ENOMEM;
		break;
	case ttm_bo_type_sg:
		bo->ttm = bdev->driver->ttm_tt_create(bdev, bo->num_pages << PAGE_SHIFT,
						      page_flags | TTM_PAGE_FLAG_SG,
						      glob->dummy_read_page);
		if (unlikely(bo->ttm == NULL)) {
			ret = -ENOMEM;
			break;
		}
		bo->ttm->sg = bo->sg;
		break;
	default:
		pr_err("Illegal buffer object type\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ttm_bo_handle_move_mem(struct ttm_buffer_object *bo,
				  struct ttm_mem_reg *mem,
				  bool evict, bool interruptible,
				  bool no_wait_gpu)
{
	struct ttm_bo_device *bdev = bo->bdev;
	bool old_is_pci = ttm_mem_reg_is_pci(bdev, &bo->mem);
	bool new_is_pci = ttm_mem_reg_is_pci(bdev, mem);
	struct ttm_mem_type_manager *old_man = &bdev->man[bo->mem.mem_type];
	struct ttm_mem_type_manager *new_man = &bdev->man[mem->mem_type];
	int ret = 0;

	if (old_is_pci || new_is_pci ||
	    ((mem->placement & bo->mem.placement & TTM_PL_MASK_CACHING) == 0)) {
		ret = ttm_mem_io_lock(old_man, true);
		if (unlikely(ret != 0))
			goto out_err;
		ttm_bo_unmap_virtual_locked(bo);
		ttm_mem_io_unlock(old_man);
	}

	/*
	 * Create and bind a ttm if required.
	 */

	if (!(new_man->flags & TTM_MEMTYPE_FLAG_FIXED)) {
		if (bo->ttm == NULL) {
			bool zero = !(old_man->flags & TTM_MEMTYPE_FLAG_FIXED);
			ret = ttm_bo_add_ttm(bo, zero);
			if (ret)
				goto out_err;
		}

		ret = ttm_tt_set_placement_caching(bo->ttm, mem->placement);
		if (ret)
			goto out_err;

		if (mem->mem_type != TTM_PL_SYSTEM) {
			ret = ttm_tt_bind(bo->ttm, mem);
			if (ret)
				goto out_err;
		}

		if (bo->mem.mem_type == TTM_PL_SYSTEM) {
			if (bdev->driver->move_notify)
				bdev->driver->move_notify(bo, mem);
			bo->mem = *mem;
			mem->mm_node = NULL;
			goto moved;
		}
	}

	if (bdev->driver->move_notify)
		bdev->driver->move_notify(bo, mem);

	if (!(old_man->flags & TTM_MEMTYPE_FLAG_FIXED) &&
	    !(new_man->flags & TTM_MEMTYPE_FLAG_FIXED))
		ret = ttm_bo_move_ttm(bo, evict, no_wait_gpu, mem);
	else if (bdev->driver->move)
		ret = bdev->driver->move(bo, evict, interruptible,
					 no_wait_gpu, mem);
	else
		ret = ttm_bo_move_memcpy(bo, evict, no_wait_gpu, mem);

	if (ret) {
		if (bdev->driver->move_notify) {
			struct ttm_mem_reg tmp_mem = *mem;
			*mem = bo->mem;
			bo->mem = tmp_mem;
			bdev->driver->move_notify(bo, mem);
			bo->mem = *mem;
			*mem = tmp_mem;
		}

		goto out_err;
	}

moved:
	if (bo->evicted) {
		ret = bdev->driver->invalidate_caches(bdev, bo->mem.placement);
		if (ret)
			pr_err("Can not flush read caches\n");
		bo->evicted = false;
	}

	if (bo->mem.mm_node) {
		bo->offset = (bo->mem.start << PAGE_SHIFT) +
		    bdev->man[bo->mem.mem_type].gpu_offset;
		bo->cur_placement = bo->mem.placement;
	} else
		bo->offset = 0;

	return 0;

out_err:
	new_man = &bdev->man[bo->mem.mem_type];
	if ((new_man->flags & TTM_MEMTYPE_FLAG_FIXED) && bo->ttm) {
		ttm_tt_unbind(bo->ttm);
		ttm_tt_destroy(bo->ttm);
		bo->ttm = NULL;
	}

	return ret;
}

/**
 * Call bo::reserved.
 * Will release GPU memory type usage on destruction.
 * This is the place to put in driver specific hooks to release
 * driver private resources.
 * Will release the bo::reserved lock.
 */

static void ttm_bo_cleanup_memtype_use(struct ttm_buffer_object *bo)
{
	if (bo->bdev->driver->move_notify)
		bo->bdev->driver->move_notify(bo, NULL);

	if (bo->ttm) {
		ttm_tt_unbind(bo->ttm);
		ttm_tt_destroy(bo->ttm);
		bo->ttm = NULL;
	}
	ttm_bo_mem_put(bo, &bo->mem);
	ttm_bo_unreserve_core(bo);

	/*
	 * Since the final reference to this bo may not be dropped by
	 * the current task we have to put a memory barrier here to make
	 * sure the changes done in this function are always visible.
	 *
	 * This function only needs protection against the final kref_put.
	 */
	cpu_mfence();
}

static void ttm_bo_cleanup_refs_or_queue(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_global *glob = bo->glob;
	struct ttm_bo_driver *driver = bdev->driver;
	void *sync_obj = NULL;
	int put_count;
	int ret;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	ret = ttm_bo_reserve_nolru(bo, false, true, false, 0);

	lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
	(void) ttm_bo_wait(bo, false, false, true);
	if (!ret && !bo->sync_obj) {
		lockmgr(&bdev->fence_lock, LK_RELEASE);
		put_count = ttm_bo_del_from_lru(bo);

		lockmgr(&glob->lru_lock, LK_RELEASE);
		ttm_bo_cleanup_memtype_use(bo);

		ttm_bo_list_ref_sub(bo, put_count, true);

		return;
	}
	if (bo->sync_obj)
		sync_obj = driver->sync_obj_ref(bo->sync_obj);
	lockmgr(&bdev->fence_lock, LK_RELEASE);

	if (!ret) {

		/*
		 * Make NO_EVICT bos immediately available to
		 * shrinkers, now that they are queued for
		 * destruction.
		 */
		if (bo->mem.placement & TTM_PL_FLAG_NO_EVICT) {
			bo->mem.placement &= ~TTM_PL_FLAG_NO_EVICT;
			ttm_bo_add_to_lru(bo);
		}

		ttm_bo_unreserve_core(bo);
	}

	kref_get(&bo->list_kref);
	list_add_tail(&bo->ddestroy, &bdev->ddestroy);
	lockmgr(&glob->lru_lock, LK_RELEASE);

	if (sync_obj) {
		driver->sync_obj_flush(sync_obj);
		driver->sync_obj_unref(&sync_obj);
	}
	schedule_delayed_work(&bdev->wq,
			      ((hz / 100) < 1) ? 1 : hz / 100);
}

/**
 * function ttm_bo_cleanup_refs_and_unlock
 * If bo idle, remove from delayed- and lru lists, and unref.
 * If not idle, do nothing.
 *
 * Must be called with lru_lock and reservation held, this function
 * will drop both before returning.
 *
 * @interruptible         Any sleeps should occur interruptibly.
 * @no_wait_gpu           Never wait for gpu. Return -EBUSY instead.
 */

static int ttm_bo_cleanup_refs_and_unlock(struct ttm_buffer_object *bo,
					  bool interruptible,
					  bool no_wait_gpu)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_bo_driver *driver = bdev->driver;
	struct ttm_bo_global *glob = bo->glob;
	int put_count;
	int ret;

	lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
	ret = ttm_bo_wait(bo, false, false, true);

	if (ret && !no_wait_gpu) {
		void *sync_obj;

		/*
		 * Take a reference to the fence and unreserve,
		 * at this point the buffer should be dead, so
		 * no new sync objects can be attached.
		 */
		sync_obj = driver->sync_obj_ref(bo->sync_obj);
		lockmgr(&bdev->fence_lock, LK_RELEASE);

		ttm_bo_unreserve_core(bo);
		lockmgr(&glob->lru_lock, LK_RELEASE);

		ret = driver->sync_obj_wait(sync_obj, false, interruptible);
		driver->sync_obj_unref(&sync_obj);
		if (ret)
			return ret;

		/*
		 * remove sync_obj with ttm_bo_wait, the wait should be
		 * finished, and no new wait object should have been added.
		 */
		lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
		ret = ttm_bo_wait(bo, false, false, true);
		WARN_ON(ret);
		lockmgr(&bdev->fence_lock, LK_RELEASE);
		if (ret)
			return ret;

		lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
		ret = ttm_bo_reserve_nolru(bo, false, true, false, 0);

		/*
		 * We raced, and lost, someone else holds the reservation now,
		 * and is probably busy in ttm_bo_cleanup_memtype_use.
		 *
		 * Even if it's not the case, because we finished waiting any
		 * delayed destruction would succeed, so just return success
		 * here.
		 */
		if (ret) {
			lockmgr(&glob->lru_lock, LK_RELEASE);
			return 0;
		}
	} else
		lockmgr(&bdev->fence_lock, LK_RELEASE);

	if (ret || unlikely(list_empty(&bo->ddestroy))) {
		ttm_bo_unreserve_core(bo);
		lockmgr(&glob->lru_lock, LK_RELEASE);
		return ret;
	}

	put_count = ttm_bo_del_from_lru(bo);
	list_del_init(&bo->ddestroy);
	++put_count;

	lockmgr(&glob->lru_lock, LK_RELEASE);
	ttm_bo_cleanup_memtype_use(bo);

	ttm_bo_list_ref_sub(bo, put_count, true);

	return 0;
}

/**
 * Traverse the delayed list, and call ttm_bo_cleanup_refs on all
 * encountered buffers.
 */

static int ttm_bo_delayed_delete(struct ttm_bo_device *bdev, bool remove_all)
{
	struct ttm_bo_global *glob = bdev->glob;
	struct ttm_buffer_object *entry = NULL;
	int ret = 0;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	if (list_empty(&bdev->ddestroy))
		goto out_unlock;

	entry = list_first_entry(&bdev->ddestroy,
		struct ttm_buffer_object, ddestroy);
	kref_get(&entry->list_kref);

	for (;;) {
		struct ttm_buffer_object *nentry = NULL;

		if (entry->ddestroy.next != &bdev->ddestroy) {
			nentry = list_first_entry(&entry->ddestroy,
				struct ttm_buffer_object, ddestroy);
			kref_get(&nentry->list_kref);
		}

		ret = ttm_bo_reserve_nolru(entry, false, true, false, 0);
		if (remove_all && ret) {
			lockmgr(&glob->lru_lock, LK_RELEASE);
			ret = ttm_bo_reserve_nolru(entry, false, false,
						   false, 0);
			lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
		}

		if (!ret)
			ret = ttm_bo_cleanup_refs_and_unlock(entry, false,
							     !remove_all);
		else
			lockmgr(&glob->lru_lock, LK_RELEASE);

		kref_put(&entry->list_kref, ttm_bo_release_list);
		entry = nentry;

		if (ret || !entry)
			goto out;

		lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
		if (list_empty(&entry->ddestroy))
			break;
	}

out_unlock:
	lockmgr(&glob->lru_lock, LK_RELEASE);
out:
	if (entry)
		kref_put(&entry->list_kref, ttm_bo_release_list);
	return ret;
}

static void ttm_bo_delayed_workqueue(struct work_struct *work)
{
	struct ttm_bo_device *bdev =
	    container_of(work, struct ttm_bo_device, wq.work);

	if (ttm_bo_delayed_delete(bdev, false)) {
		schedule_delayed_work(&bdev->wq,
				      ((hz / 100) < 1) ? 1 : hz / 100);
	}
}

/*
 * NOTE: bdev->vm_lock already held on call, this function release it.
 */
static void ttm_bo_release(struct kref *kref)
{
	struct ttm_buffer_object *bo =
	    container_of(kref, struct ttm_buffer_object, kref);
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = &bdev->man[bo->mem.mem_type];
	int release_active;

	if (atomic_read(&bo->kref.refcount) > 0) {
		lockmgr(&bdev->vm_lock, LK_RELEASE);
		return;
	}
	if (likely(bo->vm_node != NULL)) {
		RB_REMOVE(ttm_bo_device_buffer_objects,
				&bdev->addr_space_rb, bo);
		drm_mm_put_block(bo->vm_node);
		bo->vm_node = NULL;
	}

	/*
	 * Should we clean up our implied list_kref?  Because ttm_bo_release()
	 * can be called reentrantly due to races (this may not be true any
	 * more with the lock management changes in the deref), it is possible
	 * to get here twice, but there's only one list_kref ref to drop and
	 * in the other path 'bo' can be kfree()d by another thread the
	 * instant we release our lock.
	 */
	release_active = test_bit(TTM_BO_PRIV_FLAG_ACTIVE, &bo->priv_flags);
	if (release_active) {
		clear_bit(TTM_BO_PRIV_FLAG_ACTIVE, &bo->priv_flags);
		lockmgr(&bdev->vm_lock, LK_RELEASE);
		ttm_mem_io_lock(man, false);
		ttm_mem_io_free_vm(bo);
		ttm_mem_io_unlock(man);
		ttm_bo_cleanup_refs_or_queue(bo);
		kref_put(&bo->list_kref, ttm_bo_release_list);
	} else {
		lockmgr(&bdev->vm_lock, LK_RELEASE);
	}
}

void ttm_bo_unref(struct ttm_buffer_object **p_bo)
{
	struct ttm_buffer_object *bo = *p_bo;
	struct ttm_bo_device *bdev = bo->bdev;

	*p_bo = NULL;
	lockmgr(&bdev->vm_lock, LK_EXCLUSIVE);
	if (kref_put(&bo->kref, ttm_bo_release) == 0)
		lockmgr(&bdev->vm_lock, LK_RELEASE);
}
EXPORT_SYMBOL(ttm_bo_unref);

int ttm_bo_lock_delayed_workqueue(struct ttm_bo_device *bdev)
{
	return cancel_delayed_work_sync(&bdev->wq);
}
EXPORT_SYMBOL(ttm_bo_lock_delayed_workqueue);

void ttm_bo_unlock_delayed_workqueue(struct ttm_bo_device *bdev, int resched)
{
	if (resched)
		schedule_delayed_work(&bdev->wq,
				      ((hz / 100) < 1) ? 1 : hz / 100);
}
EXPORT_SYMBOL(ttm_bo_unlock_delayed_workqueue);

static int ttm_bo_evict(struct ttm_buffer_object *bo, bool interruptible,
			bool no_wait_gpu)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_reg evict_mem;
	struct ttm_placement placement;
	int ret = 0;

	lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
	ret = ttm_bo_wait(bo, false, interruptible, no_wait_gpu);
	lockmgr(&bdev->fence_lock, LK_RELEASE);

	if (unlikely(ret != 0)) {
		if (ret != -ERESTARTSYS) {
			pr_err("Failed to expire sync object before buffer eviction\n");
		}
		goto out;
	}

	BUG_ON(!ttm_bo_is_reserved(bo));

	evict_mem = bo->mem;
	evict_mem.mm_node = NULL;
	evict_mem.bus.io_reserved_vm = false;
	evict_mem.bus.io_reserved_count = 0;

	placement.num_placement = 0;
	placement.num_busy_placement = 0;
	bdev->driver->evict_flags(bo, &placement);
	ret = ttm_bo_mem_space(bo, &placement, &evict_mem, interruptible,
				no_wait_gpu);
	if (ret) {
		if (ret != -ERESTARTSYS) {
			pr_err("Failed to find memory space for buffer 0x%p eviction\n",
			       bo);
			ttm_bo_mem_space_debug(bo, &placement);
		}
		goto out;
	}

	ret = ttm_bo_handle_move_mem(bo, &evict_mem, true, interruptible,
				     no_wait_gpu);
	if (ret) {
		if (ret != -ERESTARTSYS)
			pr_err("Buffer eviction failed\n");
		ttm_bo_mem_put(bo, &evict_mem);
		goto out;
	}
	bo->evicted = true;
out:
	return ret;
}

static int ttm_mem_evict_first(struct ttm_bo_device *bdev,
				uint32_t mem_type,
				bool interruptible,
				bool no_wait_gpu)
{
	struct ttm_bo_global *glob = bdev->glob;
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];
	struct ttm_buffer_object *bo;
	int ret = -EBUSY, put_count;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	list_for_each_entry(bo, &man->lru, lru) {
		ret = ttm_bo_reserve_nolru(bo, false, true, false, 0);
		if (!ret)
			break;
	}

	if (ret) {
		lockmgr(&glob->lru_lock, LK_RELEASE);
		return ret;
	}

	kref_get(&bo->list_kref);

	if (!list_empty(&bo->ddestroy)) {
		ret = ttm_bo_cleanup_refs_and_unlock(bo, interruptible,
						     no_wait_gpu);
		kref_put(&bo->list_kref, ttm_bo_release_list);
		return ret;
	}

	put_count = ttm_bo_del_from_lru(bo);
	lockmgr(&glob->lru_lock, LK_RELEASE);

	BUG_ON(ret != 0);

	ttm_bo_list_ref_sub(bo, put_count, true);

	ret = ttm_bo_evict(bo, interruptible, no_wait_gpu);
	ttm_bo_unreserve(bo);

	kref_put(&bo->list_kref, ttm_bo_release_list);
	return ret;
}

void ttm_bo_mem_put(struct ttm_buffer_object *bo, struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bo->bdev->man[mem->mem_type];

	if (mem->mm_node)
		(*man->func->put_node)(man, mem);
}
EXPORT_SYMBOL(ttm_bo_mem_put);

/**
 * Repeatedly evict memory from the LRU for @mem_type until we create enough
 * space, or we've evicted everything and there isn't enough space.
 */
static int ttm_bo_mem_force_space(struct ttm_buffer_object *bo,
					uint32_t mem_type,
					const struct ttm_place *place,
					struct ttm_mem_reg *mem,
					bool interruptible,
					bool no_wait_gpu)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];
	int ret;

	do {
		ret = (*man->func->get_node)(man, bo, place, mem);
		if (unlikely(ret != 0))
			return ret;
		if (mem->mm_node)
			break;
		ret = ttm_mem_evict_first(bdev, mem_type,
					  interruptible, no_wait_gpu);
		if (unlikely(ret != 0))
			return ret;
	} while (1);
	if (mem->mm_node == NULL)
		return -ENOMEM;
	mem->mem_type = mem_type;
	return 0;
}

static uint32_t ttm_bo_select_caching(struct ttm_mem_type_manager *man,
				      uint32_t cur_placement,
				      uint32_t proposed_placement)
{
	uint32_t caching = proposed_placement & TTM_PL_MASK_CACHING;
	uint32_t result = proposed_placement & ~TTM_PL_MASK_CACHING;

	/**
	 * Keep current caching if possible.
	 */

	if ((cur_placement & caching) != 0)
		result |= (cur_placement & caching);
	else if ((man->default_caching & caching) != 0)
		result |= man->default_caching;
	else if ((TTM_PL_FLAG_CACHED & caching) != 0)
		result |= TTM_PL_FLAG_CACHED;
	else if ((TTM_PL_FLAG_WC & caching) != 0)
		result |= TTM_PL_FLAG_WC;
	else if ((TTM_PL_FLAG_UNCACHED & caching) != 0)
		result |= TTM_PL_FLAG_UNCACHED;

	return result;
}

static bool ttm_bo_mt_compatible(struct ttm_mem_type_manager *man,
				 uint32_t mem_type,
				 const struct ttm_place *place,
				 uint32_t *masked_placement)
{
	uint32_t cur_flags = ttm_bo_type_flags(mem_type);

	if ((cur_flags & place->flags & TTM_PL_MASK_MEM) == 0)
		return false;

	if ((place->flags & man->available_caching) == 0)
		return false;

	cur_flags |= (place->flags & man->available_caching);

	*masked_placement = cur_flags;
	return true;
}

/**
 * Creates space for memory region @mem according to its type.
 *
 * This function first searches for free space in compatible memory types in
 * the priority order defined by the driver.  If free space isn't found, then
 * ttm_bo_mem_force_space is attempted in priority order to evict and find
 * space.
 */
int ttm_bo_mem_space(struct ttm_buffer_object *bo,
			struct ttm_placement *placement,
			struct ttm_mem_reg *mem,
			bool interruptible,
			bool no_wait_gpu)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man;
	uint32_t mem_type = TTM_PL_SYSTEM;
	uint32_t cur_flags = 0;
	bool type_found = false;
	bool type_ok = false;
	bool has_erestartsys = false;
	int i, ret;

	mem->mm_node = NULL;
	for (i = 0; i < placement->num_placement; ++i) {
		const struct ttm_place *place = &placement->placement[i];

		ret = ttm_mem_type_from_place(place, &mem_type);
		if (ret)
			return ret;
		man = &bdev->man[mem_type];

		type_ok = ttm_bo_mt_compatible(man, mem_type, place,
						&cur_flags);

		if (!type_ok)
			continue;

		cur_flags = ttm_bo_select_caching(man, bo->mem.placement,
						  cur_flags);
		/*
		 * Use the access and other non-mapping-related flag bits from
		 * the memory placement flags to the current flags
		 */
		ttm_flag_masked(&cur_flags, place->flags,
				~TTM_PL_MASK_MEMTYPE);

		if (mem_type == TTM_PL_SYSTEM)
			break;

		if (man->has_type && man->use_type) {
			type_found = true;
			ret = (*man->func->get_node)(man, bo, place, mem);
			if (unlikely(ret))
				return ret;
		}
		if (mem->mm_node)
			break;
	}

	if ((type_ok && (mem_type == TTM_PL_SYSTEM)) || mem->mm_node) {
		mem->mem_type = mem_type;
		mem->placement = cur_flags;
		return 0;
	}

	if (!type_found)
		return -EINVAL;

	for (i = 0; i < placement->num_busy_placement; ++i) {
		const struct ttm_place *place = &placement->busy_placement[i];

		ret = ttm_mem_type_from_place(place, &mem_type);
		if (ret)
			return ret;
		man = &bdev->man[mem_type];
		if (!man->has_type)
			continue;
		if (!ttm_bo_mt_compatible(man, mem_type, place, &cur_flags))
			continue;

		cur_flags = ttm_bo_select_caching(man, bo->mem.placement,
						  cur_flags);
		/*
		 * Use the access and other non-mapping-related flag bits from
		 * the memory placement flags to the current flags
		 */
		ttm_flag_masked(&cur_flags, place->flags,
				~TTM_PL_MASK_MEMTYPE);

		if (mem_type == TTM_PL_SYSTEM) {
			mem->mem_type = mem_type;
			mem->placement = cur_flags;
			mem->mm_node = NULL;
			return 0;
		}

		ret = ttm_bo_mem_force_space(bo, mem_type, place, mem,
						interruptible, no_wait_gpu);
		if (ret == 0 && mem->mm_node) {
			mem->placement = cur_flags;
			return 0;
		}
		if (ret == -ERESTARTSYS)
			has_erestartsys = true;
	}
	ret = (has_erestartsys) ? -ERESTARTSYS : -ENOMEM;
	return ret;
}
EXPORT_SYMBOL(ttm_bo_mem_space);

static int ttm_bo_move_buffer(struct ttm_buffer_object *bo,
			struct ttm_placement *placement,
			bool interruptible,
			bool no_wait_gpu)
{
	int ret = 0;
	struct ttm_mem_reg mem;
	struct ttm_bo_device *bdev = bo->bdev;

	BUG_ON(!ttm_bo_is_reserved(bo));

	/*
	 * FIXME: It's possible to pipeline buffer moves.
	 * Have the driver move function wait for idle when necessary,
	 * instead of doing it here.
	 */
	lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
	ret = ttm_bo_wait(bo, false, interruptible, no_wait_gpu);
	lockmgr(&bdev->fence_lock, LK_RELEASE);
	if (ret)
		return ret;
	mem.num_pages = bo->num_pages;
	mem.size = mem.num_pages << PAGE_SHIFT;
	mem.page_alignment = bo->mem.page_alignment;
	mem.bus.io_reserved_vm = false;
	mem.bus.io_reserved_count = 0;
	/*
	 * Determine where to move the buffer.
	 */
	ret = ttm_bo_mem_space(bo, placement, &mem,
			       interruptible, no_wait_gpu);
	if (ret)
		goto out_unlock;
	ret = ttm_bo_handle_move_mem(bo, &mem, false,
				     interruptible, no_wait_gpu);
out_unlock:
	if (ret && mem.mm_node)
		ttm_bo_mem_put(bo, &mem);
	return ret;
}

static bool ttm_bo_mem_compat(struct ttm_placement *placement,
			      struct ttm_mem_reg *mem,
			      uint32_t *new_flags)
{
	int i;

	for (i = 0; i < placement->num_placement; i++) {
		const struct ttm_place *heap = &placement->placement[i];
		if (mem->mm_node &&
		    (mem->start < heap->fpfn ||
		     (heap->lpfn != 0 && (mem->start + mem->num_pages) > heap->lpfn)))
			continue;

		*new_flags = heap->flags;
		if ((*new_flags & mem->placement & TTM_PL_MASK_CACHING) &&
		    (*new_flags & mem->placement & TTM_PL_MASK_MEM))
			return true;
	}

	for (i = 0; i < placement->num_busy_placement; i++) {
		const struct ttm_place *heap = &placement->busy_placement[i];
		if (mem->mm_node &&
		    (mem->start < heap->fpfn ||
		     (heap->lpfn != 0 && (mem->start + mem->num_pages) > heap->lpfn)))
			continue;

		*new_flags = heap->flags;
		if ((*new_flags & mem->placement & TTM_PL_MASK_CACHING) &&
		    (*new_flags & mem->placement & TTM_PL_MASK_MEM))
			return true;
	}

	return false;
}

int ttm_bo_validate(struct ttm_buffer_object *bo,
			struct ttm_placement *placement,
			bool interruptible,
			bool no_wait_gpu)
{
	int ret;
	uint32_t new_flags;

	BUG_ON(!ttm_bo_is_reserved(bo));
	/*
	 * Check whether we need to move buffer.
	 */
	if (!ttm_bo_mem_compat(placement, &bo->mem, &new_flags)) {
		ret = ttm_bo_move_buffer(bo, placement, interruptible,
					 no_wait_gpu);
		if (ret)
			return ret;
	} else {
		/*
		 * Use the access and other non-mapping-related flag bits from
		 * the compatible memory placement flags to the active flags
		 */
		ttm_flag_masked(&bo->mem.placement, new_flags,
				~TTM_PL_MASK_MEMTYPE);
	}
	/*
	 * We might need to add a TTM.
	 */
	if (bo->mem.mem_type == TTM_PL_SYSTEM && bo->ttm == NULL) {
		ret = ttm_bo_add_ttm(bo, true);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL(ttm_bo_validate);

int ttm_bo_init(struct ttm_bo_device *bdev,
		struct ttm_buffer_object *bo,
		unsigned long size,
		enum ttm_bo_type type,
		struct ttm_placement *placement,
		uint32_t page_alignment,
		bool interruptible,
		struct vm_object *persistent_swap_storage,
		size_t acc_size,
		struct sg_table *sg,
		void (*destroy) (struct ttm_buffer_object *))
{
	int ret = 0;
	unsigned long num_pages;
	struct ttm_mem_global *mem_glob = bdev->glob->mem_glob;

	ret = ttm_mem_global_alloc(mem_glob, acc_size, false, false);
	if (ret) {
		pr_err("Out of kernel memory\n");
		if (destroy)
			(*destroy)(bo);
		else
			kfree(bo);
		return -ENOMEM;
	}

	num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (num_pages == 0) {
		pr_err("Illegal buffer object size\n");
		if (destroy)
			(*destroy)(bo);
		else
			kfree(bo);
		ttm_mem_global_free(mem_glob, acc_size);
		return -EINVAL;
	}
	bo->destroy = destroy;

	kref_init(&bo->kref);
	kref_init(&bo->list_kref);
	atomic_set(&bo->cpu_writers, 0);
	atomic_set(&bo->reserved, 1);
	init_waitqueue_head(&bo->event_queue);
	INIT_LIST_HEAD(&bo->lru);
	INIT_LIST_HEAD(&bo->ddestroy);
	INIT_LIST_HEAD(&bo->swap);
	INIT_LIST_HEAD(&bo->io_reserve_lru);
	/*bzero(&bo->vm_rb, sizeof(bo->vm_rb));*/
	bo->bdev = bdev;
	bo->glob = bdev->glob;
	bo->type = type;
	bo->num_pages = num_pages;
	bo->mem.size = num_pages << PAGE_SHIFT;
	bo->mem.mem_type = TTM_PL_SYSTEM;
	bo->mem.num_pages = bo->num_pages;
	bo->mem.mm_node = NULL;
	bo->mem.page_alignment = page_alignment;
	bo->mem.bus.io_reserved_vm = false;
	bo->mem.bus.io_reserved_count = 0;
	bo->priv_flags = 0;
	bo->mem.placement = (TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED);
	bo->seq_valid = false;
	bo->persistent_swap_storage = persistent_swap_storage;
	bo->acc_size = acc_size;
	bo->sg = sg;
	atomic_inc(&bo->glob->bo_count);

	/*
	 * Mirror ref from kref_init() for list_kref.
	 */
	set_bit(TTM_BO_PRIV_FLAG_ACTIVE, &bo->priv_flags);

	/*
	 * For ttm_bo_type_device buffers, allocate
	 * address space from the device.
	 */
	if (bo->type == ttm_bo_type_device ||
	    bo->type == ttm_bo_type_sg) {
		ret = ttm_bo_setup_vm(bo);
		if (ret)
			goto out_err;
	}

	ret = ttm_bo_validate(bo, placement, interruptible, false);
	if (ret)
		goto out_err;

	ttm_bo_unreserve(bo);
	return 0;

out_err:
	ttm_bo_unreserve(bo);
	ttm_bo_unref(&bo);

	return ret;
}
EXPORT_SYMBOL(ttm_bo_init);

size_t ttm_bo_acc_size(struct ttm_bo_device *bdev,
		       unsigned long bo_size,
		       unsigned struct_size)
{
	unsigned npages = (PAGE_ALIGN(bo_size)) >> PAGE_SHIFT;
	size_t size = 0;

	size += ttm_round_pot(struct_size);
	size += PAGE_ALIGN(npages * sizeof(void *));
	size += ttm_round_pot(sizeof(struct ttm_tt));
	return size;
}
EXPORT_SYMBOL(ttm_bo_acc_size);

size_t ttm_bo_dma_acc_size(struct ttm_bo_device *bdev,
			   unsigned long bo_size,
			   unsigned struct_size)
{
	unsigned npages = (PAGE_ALIGN(bo_size)) >> PAGE_SHIFT;
	size_t size = 0;

	size += ttm_round_pot(struct_size);
	size += PAGE_ALIGN(npages * sizeof(void *));
	size += PAGE_ALIGN(npages * sizeof(dma_addr_t));
	size += ttm_round_pot(sizeof(struct ttm_dma_tt));
	return size;
}
EXPORT_SYMBOL(ttm_bo_dma_acc_size);

int ttm_bo_create(struct ttm_bo_device *bdev,
			unsigned long size,
			enum ttm_bo_type type,
			struct ttm_placement *placement,
			uint32_t page_alignment,
			bool interruptible,
			struct vm_object *persistent_swap_storage,
			struct ttm_buffer_object **p_bo)
{
	struct ttm_buffer_object *bo;
	size_t acc_size;
	int ret;

	*p_bo = NULL;
	bo = kmalloc(sizeof(*bo), M_DRM, M_WAITOK | M_ZERO);
	if (unlikely(bo == NULL))
		return -ENOMEM;

	acc_size = ttm_bo_acc_size(bdev, size, sizeof(struct ttm_buffer_object));
	ret = ttm_bo_init(bdev, bo, size, type, placement, page_alignment,
			  interruptible, persistent_swap_storage, acc_size,
			  NULL, NULL);
	if (likely(ret == 0))
		*p_bo = bo;

	return ret;
}
EXPORT_SYMBOL(ttm_bo_create);

static int ttm_bo_force_list_clean(struct ttm_bo_device *bdev,
					unsigned mem_type, bool allow_errors)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];
	struct ttm_bo_global *glob = bdev->glob;
	int ret;

	/*
	 * Can't use standard list traversal since we're unlocking.
	 */

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	while (!list_empty(&man->lru)) {
		lockmgr(&glob->lru_lock, LK_RELEASE);
		ret = ttm_mem_evict_first(bdev, mem_type, false, false);
		if (ret) {
			if (allow_errors) {
				return ret;
			} else {
				pr_err("Cleanup eviction failed\n");
			}
		}
		lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	}
	lockmgr(&glob->lru_lock, LK_RELEASE);
	return 0;
}

int ttm_bo_clean_mm(struct ttm_bo_device *bdev, unsigned mem_type)
{
	struct ttm_mem_type_manager *man;
	int ret = -EINVAL;

	if (mem_type >= TTM_NUM_MEM_TYPES) {
		pr_err("Illegal memory type %d\n", mem_type);
		return ret;
	}
	man = &bdev->man[mem_type];

	if (!man->has_type) {
		pr_err("Trying to take down uninitialized memory manager type %u\n",
		       mem_type);
		return ret;
	}

	man->use_type = false;
	man->has_type = false;

	ret = 0;
	if (mem_type > 0) {
		ttm_bo_force_list_clean(bdev, mem_type, false);

		ret = (*man->func->takedown)(man);
	}

	return ret;
}
EXPORT_SYMBOL(ttm_bo_clean_mm);

int ttm_bo_evict_mm(struct ttm_bo_device *bdev, unsigned mem_type)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem_type];

	if (mem_type == 0 || mem_type >= TTM_NUM_MEM_TYPES) {
		pr_err("Illegal memory manager memory type %u\n", mem_type);
		return -EINVAL;
	}

	if (!man->has_type) {
		pr_err("Memory type %u has not been initialized\n", mem_type);
		return 0;
	}

	return ttm_bo_force_list_clean(bdev, mem_type, true);
}
EXPORT_SYMBOL(ttm_bo_evict_mm);

int ttm_bo_init_mm(struct ttm_bo_device *bdev, unsigned type,
			unsigned long p_size)
{
	int ret = -EINVAL;
	struct ttm_mem_type_manager *man;

	BUG_ON(type >= TTM_NUM_MEM_TYPES);
	man = &bdev->man[type];
	BUG_ON(man->has_type);
	man->io_reserve_fastpath = true;
	man->use_io_reserve_lru = false;
	lockinit(&man->io_reserve_mutex, "ttmman", 0, LK_CANRECURSE);
	INIT_LIST_HEAD(&man->io_reserve_lru);

	ret = bdev->driver->init_mem_type(bdev, type, man);
	if (ret)
		return ret;
	man->bdev = bdev;

	ret = 0;
	if (type != TTM_PL_SYSTEM) {
		ret = (*man->func->init)(man, p_size);
		if (ret)
			return ret;
	}
	man->has_type = true;
	man->use_type = true;
	man->size = p_size;

	INIT_LIST_HEAD(&man->lru);

	return 0;
}
EXPORT_SYMBOL(ttm_bo_init_mm);

static void ttm_bo_global_kobj_release(struct ttm_bo_global *glob)
{
	ttm_mem_unregister_shrink(glob->mem_glob, &glob->shrink);
	vm_page_free_contig(glob->dummy_read_page, PAGE_SIZE);
	glob->dummy_read_page = NULL;
	/*
	vm_page_free(glob->dummy_read_page);
	*/
}

void ttm_bo_global_release(struct drm_global_reference *ref)
{
	struct ttm_bo_global *glob = ref->object;

	if (refcount_release(&glob->kobj_ref))
		ttm_bo_global_kobj_release(glob);
}
EXPORT_SYMBOL(ttm_bo_global_release);

int ttm_bo_global_init(struct drm_global_reference *ref)
{
	struct ttm_bo_global_ref *bo_ref =
		container_of(ref, struct ttm_bo_global_ref, ref);
	struct ttm_bo_global *glob = ref->object;
	int ret;

	lockinit(&glob->device_list_mutex, "ttmdlm", 0, LK_CANRECURSE);
	lockinit(&glob->lru_lock, "ttmlru", 0, LK_CANRECURSE);
	glob->mem_glob = bo_ref->mem_glob;
	glob->dummy_read_page = vm_page_alloc_contig(
	    0, VM_MAX_ADDRESS, PAGE_SIZE, 0, 1*PAGE_SIZE, VM_MEMATTR_UNCACHEABLE);

	if (unlikely(glob->dummy_read_page == NULL)) {
		ret = -ENOMEM;
		goto out_no_drp;
	}

	INIT_LIST_HEAD(&glob->swap_lru);
	INIT_LIST_HEAD(&glob->device_list);

	ttm_mem_init_shrink(&glob->shrink, ttm_bo_swapout);
	ret = ttm_mem_register_shrink(glob->mem_glob, &glob->shrink);
	if (unlikely(ret != 0)) {
		pr_err("Could not register buffer object swapout\n");
		goto out_no_shrink;
	}

	atomic_set(&glob->bo_count, 0);

	refcount_init(&glob->kobj_ref, 1);
	return (0);

out_no_shrink:
	vm_page_free_contig(glob->dummy_read_page, PAGE_SIZE);
	glob->dummy_read_page = NULL;
	/*
	vm_page_free(glob->dummy_read_page);
	*/
out_no_drp:
	kfree(glob);
	return ret;
}
EXPORT_SYMBOL(ttm_bo_global_init);


int ttm_bo_device_release(struct ttm_bo_device *bdev)
{
	int ret = 0;
	unsigned i = TTM_NUM_MEM_TYPES;
	struct ttm_mem_type_manager *man;
	struct ttm_bo_global *glob = bdev->glob;

	while (i--) {
		man = &bdev->man[i];
		if (man->has_type) {
			man->use_type = false;
			if ((i != TTM_PL_SYSTEM) && ttm_bo_clean_mm(bdev, i)) {
				ret = -EBUSY;
				pr_err("DRM memory manager type %d is not clean\n",
				       i);
			}
			man->has_type = false;
		}
	}

	lockmgr(&glob->device_list_mutex, LK_EXCLUSIVE);
	list_del(&bdev->device_list);
	lockmgr(&glob->device_list_mutex, LK_RELEASE);

	cancel_delayed_work_sync(&bdev->wq);

	while (ttm_bo_delayed_delete(bdev, true))
		;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	if (list_empty(&bdev->ddestroy))
		TTM_DEBUG("Delayed destroy list was clean\n");

	if (list_empty(&bdev->man[0].lru))
		TTM_DEBUG("Swap list was clean\n");
	lockmgr(&glob->lru_lock, LK_RELEASE);

	BUG_ON(!drm_mm_clean(&bdev->addr_space_mm));
	lockmgr(&bdev->vm_lock, LK_EXCLUSIVE);
	drm_mm_takedown(&bdev->addr_space_mm);
	lockmgr(&bdev->vm_lock, LK_RELEASE);

	return ret;
}
EXPORT_SYMBOL(ttm_bo_device_release);

int ttm_bo_device_init(struct ttm_bo_device *bdev,
		       struct ttm_bo_global *glob,
		       struct ttm_bo_driver *driver,
		       uint64_t file_page_offset,
		       bool need_dma32)
{
	int ret = -EINVAL;

	lockinit(&bdev->vm_lock, "ttmvml", 0, LK_CANRECURSE);
	bdev->driver = driver;

	memset(bdev->man, 0, sizeof(bdev->man));

	/*
	 * Initialize the system memory buffer type.
	 * Other types need to be driver / IOCTL initialized.
	 */
	ret = ttm_bo_init_mm(bdev, TTM_PL_SYSTEM, 0);
	if (unlikely(ret != 0))
		goto out_no_sys;

	RB_INIT(&bdev->addr_space_rb);
	drm_mm_init(&bdev->addr_space_mm, file_page_offset, 0x10000000);

	INIT_DELAYED_WORK(&bdev->wq, ttm_bo_delayed_workqueue);
	INIT_LIST_HEAD(&bdev->ddestroy);
	bdev->dev_mapping = NULL;
	bdev->glob = glob;
	bdev->need_dma32 = need_dma32;
	bdev->val_seq = 0;
	lockinit(&bdev->fence_lock, "ttmfence", 0, LK_CANRECURSE);
	lockmgr(&glob->device_list_mutex, LK_EXCLUSIVE);
	list_add_tail(&bdev->device_list, &glob->device_list);
	lockmgr(&glob->device_list_mutex, LK_RELEASE);

	return 0;
out_no_sys:
	return ret;
}
EXPORT_SYMBOL(ttm_bo_device_init);

/*
 * buffer object vm functions.
 */

bool ttm_mem_reg_is_pci(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];

	if (!(man->flags & TTM_MEMTYPE_FLAG_FIXED)) {
		if (mem->mem_type == TTM_PL_SYSTEM)
			return false;

		if (man->flags & TTM_MEMTYPE_FLAG_CMA)
			return false;

		if (mem->placement & TTM_PL_FLAG_CACHED)
			return false;
	}
	return true;
}

void ttm_bo_unmap_virtual_locked(struct ttm_buffer_object *bo)
{

	ttm_bo_release_mmap(bo);
	ttm_mem_io_free_vm(bo);
}

void ttm_bo_unmap_virtual(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = &bdev->man[bo->mem.mem_type];

	ttm_mem_io_lock(man, false);
	ttm_bo_unmap_virtual_locked(bo);
	ttm_mem_io_unlock(man);
}


EXPORT_SYMBOL(ttm_bo_unmap_virtual);

static void ttm_bo_vm_insert_rb(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;

	/* The caller acquired bdev->vm_lock. */
	RB_INSERT(ttm_bo_device_buffer_objects, &bdev->addr_space_rb, bo);
}

/**
 * ttm_bo_setup_vm:
 *
 * @bo: the buffer to allocate address space for
 *
 * Allocate address space in the drm device so that applications
 * can mmap the buffer and access the contents. This only
 * applies to ttm_bo_type_device objects as others are not
 * placed in the drm device address space.
 */

static int ttm_bo_setup_vm(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int ret;

retry_pre_get:
	ret = drm_mm_pre_get(&bdev->addr_space_mm);
	if (unlikely(ret != 0))
		return ret;

	lockmgr(&bdev->vm_lock, LK_EXCLUSIVE);
	bo->vm_node = drm_mm_search_free(&bdev->addr_space_mm,
					 bo->mem.num_pages, 0, 0);

	if (unlikely(bo->vm_node == NULL)) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	bo->vm_node = drm_mm_get_block_atomic(bo->vm_node,
					      bo->mem.num_pages, 0);

	if (unlikely(bo->vm_node == NULL)) {
		lockmgr(&bdev->vm_lock, LK_RELEASE);
		goto retry_pre_get;
	}

	ttm_bo_vm_insert_rb(bo);
	lockmgr(&bdev->vm_lock, LK_RELEASE);
	bo->addr_space_offset = ((uint64_t) bo->vm_node->start) << PAGE_SHIFT;

	return 0;
out_unlock:
	lockmgr(&bdev->vm_lock, LK_RELEASE);
	return ret;
}

int ttm_bo_wait(struct ttm_buffer_object *bo,
		bool lazy, bool interruptible, bool no_wait)
{
	struct ttm_bo_driver *driver = bo->bdev->driver;
	struct ttm_bo_device *bdev = bo->bdev;
	void *sync_obj;
	int ret = 0;

	if (likely(bo->sync_obj == NULL))
		return 0;

	while (bo->sync_obj) {

		if (driver->sync_obj_signaled(bo->sync_obj)) {
			void *tmp_obj = bo->sync_obj;
			bo->sync_obj = NULL;
			clear_bit(TTM_BO_PRIV_FLAG_MOVING, &bo->priv_flags);
			lockmgr(&bdev->fence_lock, LK_RELEASE);
			driver->sync_obj_unref(&tmp_obj);
			lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
			continue;
		}

		if (no_wait)
			return -EBUSY;

		sync_obj = driver->sync_obj_ref(bo->sync_obj);
		lockmgr(&bdev->fence_lock, LK_RELEASE);
		ret = driver->sync_obj_wait(sync_obj,
					    lazy, interruptible);
		if (unlikely(ret != 0)) {
			driver->sync_obj_unref(&sync_obj);
			lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
			return ret;
		}
		lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
		if (likely(bo->sync_obj == sync_obj)) {
			void *tmp_obj = bo->sync_obj;
			bo->sync_obj = NULL;
			clear_bit(TTM_BO_PRIV_FLAG_MOVING,
				  &bo->priv_flags);
			lockmgr(&bdev->fence_lock, LK_RELEASE);
			driver->sync_obj_unref(&sync_obj);
			driver->sync_obj_unref(&tmp_obj);
			lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
		} else {
			lockmgr(&bdev->fence_lock, LK_RELEASE);
			driver->sync_obj_unref(&sync_obj);
			lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
		}
	}
	return 0;
}
EXPORT_SYMBOL(ttm_bo_wait);

int ttm_bo_synccpu_write_grab(struct ttm_buffer_object *bo, bool no_wait)
{
	struct ttm_bo_device *bdev = bo->bdev;
	int ret = 0;

	/*
	 * Using ttm_bo_reserve makes sure the lru lists are updated.
	 */

	ret = ttm_bo_reserve(bo, true, no_wait, false, 0);
	if (unlikely(ret != 0))
		return ret;
	lockmgr(&bdev->fence_lock, LK_EXCLUSIVE);
	ret = ttm_bo_wait(bo, false, true, no_wait);
	lockmgr(&bdev->fence_lock, LK_RELEASE);
	if (likely(ret == 0))
		atomic_inc(&bo->cpu_writers);
	ttm_bo_unreserve(bo);
	return ret;
}
EXPORT_SYMBOL(ttm_bo_synccpu_write_grab);

void ttm_bo_synccpu_write_release(struct ttm_buffer_object *bo)
{
	atomic_dec(&bo->cpu_writers);
}
EXPORT_SYMBOL(ttm_bo_synccpu_write_release);

/**
 * A buffer object shrink method that tries to swap out the first
 * buffer object on the bo_global::swap_lru list.
 */

static int ttm_bo_swapout(struct ttm_mem_shrink *shrink)
{
	struct ttm_bo_global *glob =
	    container_of(shrink, struct ttm_bo_global, shrink);
	struct ttm_buffer_object *bo;
	int ret = -EBUSY;
	int put_count;
	uint32_t swap_placement = (TTM_PL_FLAG_CACHED | TTM_PL_FLAG_SYSTEM);

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	list_for_each_entry(bo, &glob->swap_lru, swap) {
		ret = ttm_bo_reserve_nolru(bo, false, true, false, 0);
		if (!ret)
			break;
	}

	if (ret) {
		lockmgr(&glob->lru_lock, LK_RELEASE);
		return ret;
	}

	kref_get(&bo->list_kref);

	if (!list_empty(&bo->ddestroy)) {
		ret = ttm_bo_cleanup_refs_and_unlock(bo, false, false);
		kref_put(&bo->list_kref, ttm_bo_release_list);
		return ret;
	}

	put_count = ttm_bo_del_from_lru(bo);
	lockmgr(&glob->lru_lock, LK_RELEASE);

	ttm_bo_list_ref_sub(bo, put_count, true);

	/**
	 * Wait for GPU, then move to system cached.
	 */

	lockmgr(&bo->bdev->fence_lock, LK_EXCLUSIVE);
	ret = ttm_bo_wait(bo, false, false, false);
	lockmgr(&bo->bdev->fence_lock, LK_RELEASE);

	if (unlikely(ret != 0))
		goto out;

	if ((bo->mem.placement & swap_placement) != swap_placement) {
		struct ttm_mem_reg evict_mem;

		evict_mem = bo->mem;
		evict_mem.mm_node = NULL;
		evict_mem.placement = TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED;
		evict_mem.mem_type = TTM_PL_SYSTEM;

		ret = ttm_bo_handle_move_mem(bo, &evict_mem, true,
					     false, false);
		if (unlikely(ret != 0))
			goto out;
	}

	ttm_bo_unmap_virtual(bo);

	/**
	 * Swap out. Buffer will be swapped in again as soon as
	 * anyone tries to access a ttm page.
	 */

	if (bo->bdev->driver->swap_notify)
		bo->bdev->driver->swap_notify(bo);

	ret = ttm_tt_swapout(bo->ttm, bo->persistent_swap_storage);
out:

	/**
	 *
	 * Unreserve without putting on LRU to avoid swapping out an
	 * already swapped buffer.
	 */

	ttm_bo_unreserve_core(bo);
	kref_put(&bo->list_kref, ttm_bo_release_list);
	return ret;
}

void ttm_bo_swapout_all(struct ttm_bo_device *bdev)
{
	while (ttm_bo_swapout(&bdev->glob->shrink) == 0)
		;
}
EXPORT_SYMBOL(ttm_bo_swapout_all);
