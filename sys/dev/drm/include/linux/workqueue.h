/*-
 * Copyright (c) 2010 Isilon Systems, Inc.
 * Copyright (c) 2010 iX Systems, Inc.
 * Copyright (c) 2010 Panasas, Inc.
 * Copyright (c) 2013, 2014 Mellanox Technologies, Ltd.
 * Copyright (c) 2014-2015 François Tigeot
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef	_LINUX_WORKQUEUE_H_
#define	_LINUX_WORKQUEUE_H_

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/lockdep.h>

#include <sys/taskqueue.h>

struct workqueue_struct {
	struct taskqueue	*taskqueue;
};

struct work_struct {
	struct	task 		work_task;
	struct	taskqueue	*taskqueue;
	void			(*fn)(struct work_struct *);
};

struct delayed_work {
	struct work_struct	work;
	struct callout		timer;
	struct lwkt_token	token;
};

static inline struct delayed_work *
to_delayed_work(struct work_struct *work)
{

	return container_of(work, struct delayed_work, work);
}


static inline void
_work_fn(void *context, int pending)
{
	struct work_struct *work;

	work = context;
	work->fn(work);
}

#define	INIT_WORK(work, func) 	 					\
do {									\
	(work)->fn = (func);						\
	(work)->taskqueue = NULL;					\
	TASK_INIT(&(work)->work_task, 0, _work_fn, (work));		\
} while (0)

#define	INIT_DELAYED_WORK(_work, func)					\
do {									\
	INIT_WORK(&(_work)->work, func);				\
	lwkt_token_init(&(_work)->token, "workqueue token");		\
	callout_init_mp(&(_work)->timer);				\
} while (0)

#define	INIT_DEFERRABLE_WORK	INIT_DELAYED_WORK

#define	schedule_work(work)						\
do {									\
	(work)->taskqueue = taskqueue_thread[mycpuid];				\
	taskqueue_enqueue(taskqueue_thread[mycpuid], &(work)->work_task);	\
} while (0)

#define	flush_scheduled_work()	flush_taskqueue(taskqueue_thread[mycpuid])

static inline int queue_work(struct workqueue_struct *q, struct work_struct *work)
{
	(work)->taskqueue = (q)->taskqueue;
	/* Return opposite val to align with Linux logic */
	return !taskqueue_enqueue((q)->taskqueue, &(work)->work_task);
}

static inline void
_delayed_work_fn(void *arg)
{
	struct delayed_work *work;

	work = arg;
	taskqueue_enqueue(work->work.taskqueue, &work->work.work_task);
}

static inline int
queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *work,
    unsigned long delay)
{
	int pending;

	pending = work->work.work_task.ta_pending;
	work->work.taskqueue = wq->taskqueue;
	if (delay != 0) {
		lwkt_gettoken(&work->token);
		callout_reset(&work->timer, delay, _delayed_work_fn, work);
		lwkt_reltoken(&work->token);
	} else {
		_delayed_work_fn((void *)work);
	}

	return (!pending);
}

static inline bool schedule_delayed_work(struct delayed_work *dwork,
                                         unsigned long delay)
{
        struct workqueue_struct wq;
        wq.taskqueue = taskqueue_thread[mycpuid];
        return queue_delayed_work(&wq, dwork, delay);
}

static inline struct workqueue_struct *
_create_workqueue_common(char *name, int cpus)
{
	struct workqueue_struct *wq;

	wq = kmalloc(sizeof(*wq), M_DRM, M_WAITOK);
	wq->taskqueue = taskqueue_create((name), M_WAITOK,
	    taskqueue_thread_enqueue,  &wq->taskqueue);
	taskqueue_start_threads(&wq->taskqueue, cpus, 0, -1, "%s", name);

	return (wq);
}


#define	create_singlethread_workqueue(name)				\
	_create_workqueue_common(name, 1)

#define	create_workqueue(name)						\
	_create_workqueue_common(name, MAXCPU)

#define alloc_ordered_workqueue(name, flags)				\
	_create_workqueue_common(name, 1)

#define alloc_workqueue(name, flags, max_active)			\
	_create_workqueue_common(name, max_active)

static inline void
destroy_workqueue(struct workqueue_struct *wq)
{
	taskqueue_free(wq->taskqueue);
	kfree(wq);
}

#define	flush_workqueue(wq)	flush_taskqueue((wq)->taskqueue)

static inline void
_flush_fn(void *context, int pending)
{
}

static inline void
flush_taskqueue(struct taskqueue *tq)
{
	struct task flushtask;

	PHOLD(curproc);
	TASK_INIT(&flushtask, 0, _flush_fn, NULL);
	taskqueue_enqueue(tq, &flushtask);
	taskqueue_drain(tq, &flushtask);
	PRELE(curproc);
}

static inline int
cancel_work_sync(struct work_struct *work)
{
	if (work->taskqueue &&
	    taskqueue_cancel(work->taskqueue, &work->work_task, NULL))
		taskqueue_drain(work->taskqueue, &work->work_task);
	return 0;
}

/*
 * This may leave work running on another CPU as it does on Linux.
 */
static inline int
cancel_delayed_work(struct delayed_work *work)
{

	lwkt_gettoken(&work->token);
	callout_stop(&work->timer);
	lwkt_reltoken(&work->token);
	if (work->work.taskqueue)
		return (taskqueue_cancel(work->work.taskqueue,
		    &work->work.work_task, NULL) == 0);
	return 0;
}

static inline int
cancel_delayed_work_sync(struct delayed_work *work)
{

	lwkt_gettoken(&work->token);
	callout_drain(&work->timer);
	lwkt_reltoken(&work->token);
	if (work->work.taskqueue &&
	    taskqueue_cancel(work->work.taskqueue, &work->work.work_task, NULL))
		taskqueue_drain(work->work.taskqueue, &work->work.work_task);
	return 0;
}

static inline bool
mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
		                      unsigned long delay)
{
	cancel_delayed_work(dwork);
	queue_delayed_work(wq, dwork, delay);
	return false;
}

/* System-wide workqueues */
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_power_efficient_wq;

#endif	/* _LINUX_WORKQUEUE_H_ */
