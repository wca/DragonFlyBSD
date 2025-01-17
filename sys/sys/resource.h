/*
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)resource.h	8.4 (Berkeley) 1/9/95
 * $FreeBSD: src/sys/sys/resource.h,v 1.12.2.2 2002/08/20 18:42:20 dillon Exp $
 */

#ifndef _SYS_RESOURCE_H_
#define	_SYS_RESOURCE_H_

#include <sys/cdefs.h>
#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#include <sys/_timeval.h>

/*
 * Process priority specifications to get/setpriority.
 */
#define	PRIO_MIN	-20
#define	PRIO_MAX	20

#if __BSD_VISIBLE
#define	IOPRIO_MIN	1
#define	IOPRIO_MAX	10
#endif

#define	PRIO_PROCESS	0
#define	PRIO_PGRP	1
#define	PRIO_USER	2

/*
 * Resource utilization information.
 */

#define	RUSAGE_SELF	0
#define	RUSAGE_CHILDREN	-1

struct	rusage {
	struct timeval ru_utime;	/* user time used */
	struct timeval ru_stime;	/* system time used */
	long	ru_maxrss;		/* max resident set size */
#define	ru_first	ru_ixrss
	long	ru_ixrss;		/* integral shared memory size */
	long	ru_idrss;		/* integral unshared data " */
	long	ru_isrss;		/* integral unshared stack " */
	long	ru_minflt;		/* page reclaims */
	long	ru_majflt;		/* page faults */
	long	ru_nswap;		/* swaps */
	long	ru_inblock;		/* block input operations */
	long	ru_oublock;		/* block output operations */
	long	ru_msgsnd;		/* messages sent */
	long	ru_msgrcv;		/* messages received */
	long	ru_nsignals;		/* signals received */
	long	ru_nvcsw;		/* voluntary context switches */
	long	ru_nivcsw;		/* involuntary " */
#define	ru_last		ru_nivcsw
};

/*
 * Resource limits
 */
#define	RLIMIT_CPU	0		/* cpu time in milliseconds */
#define	RLIMIT_FSIZE	1		/* maximum file size */
#define	RLIMIT_DATA	2		/* data size */
#define	RLIMIT_STACK	3		/* stack size */
#define	RLIMIT_CORE	4		/* core file size */
#define	RLIMIT_RSS	5		/* resident set size */
#define	RLIMIT_MEMLOCK	6		/* locked-in-memory address space */
#define	RLIMIT_NPROC	7		/* number of processes */
#define	RLIMIT_NOFILE	8		/* number of open files */
#define	RLIMIT_SBSIZE	9		/* maximum size of all socket buffers */
#define	RLIMIT_VMEM	10		/* virtual process size (inclusive of mmap) */
#define	RLIMIT_AS       RLIMIT_VMEM     /* standard name for address space size */
#define	RLIMIT_POSIXLOCKS 11		/* maximum number of POSIX locks per user */

#define	RLIM_NLIMITS	12		/* number of resource limits */

#define	RLIM_INFINITY	((rlim_t)(((u_quad_t)1 << 63) - 1))	/* no limit */
#define	RLIM_SAVED_MAX	RLIM_INFINITY	/* unrepresentable hard limit */
#define	RLIM_SAVED_CUR	RLIM_INFINITY	/* unrepresentable soft limit */

/*
 * Resource limit string identifiers
 */
#ifdef _RLIMIT_IDENT
static char *rlimit_ident[] = {
	"cpu",
	"fsize",
	"data",
	"stack",
	"core",
	"rss",
	"memlock",
	"nproc",
	"nofile",
	"sbsize",
	"vmem",
	"posixlock",
};
#endif

struct rlimit {
	rlim_t	rlim_cur;		/* current (soft) limit */
	rlim_t	rlim_max;		/* maximum value for rlim_cur */
};

#if __BSD_VISIBLE
struct orlimit {
	int32_t	rlim_cur;		/* current (soft) limit */
	int32_t	rlim_max;		/* maximum value for rlim_cur */
};

struct loadavg {
	fixpt_t	ldavg[3];
	long	fscale;
};

/*
 * CPU state fields as reported by the sysctl kern.cp_time
 */
#define	CP_USER         0
#define	CP_NICE         1
#define	CP_SYS          2
#define	CP_INTR         3
#define	CP_IDLE         4
#define	CPUSTATES       5
#endif /* __BSD_VISIBLE */

#ifdef _KERNEL
extern struct loadavg averunnable;

int	dosetrlimit(u_int which, struct rlimit *limp);
#else
__BEGIN_DECLS
int	getpriority(int, int);
int	getrlimit(int, struct rlimit *);
int	getrusage(int, struct rusage *);
int	setpriority(int, int, int);
int	setrlimit(int, const struct rlimit *);
#if __BSD_VISIBLE
int	ioprio_get(int, int);
int	ioprio_set(int, int, int);
#endif
__END_DECLS
#endif	/* _KERNEL */

#endif	/* !_SYS_RESOURCE_H_ */
