/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1991 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
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
 *	from: @(#)cons.h	7.2 (Berkeley) 5/9/91
 * $FreeBSD: src/sys/sys/cons.h,v 1.24 2000/01/11 14:54:01 yokota Exp $
 * $DragonFly: src/sys/sys/cons.h,v 1.7 2007/05/07 05:21:42 dillon Exp $
 */

#ifndef _MACHINE_CONS_H_
#define	_MACHINE_CONS_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif

struct consdev;

typedef	void	cn_probe_t (struct consdev *);
typedef	void	cn_init_t (struct consdev *);
typedef	void	cn_init_fini_t (struct consdev *);
typedef	void	cn_term_t (struct consdev *);
typedef	int	cn_getc_t (void *);
typedef	int	cn_checkc_t (void *);
typedef	void	cn_putc_t (void *, int);
typedef	void	cn_dbctl_t (void *, int);
typedef void	cn_poll_t (void *, int);

struct consdev {
	cn_probe_t	*cn_probe;	/* probe hardware */
	cn_init_t	*cn_init;	/* initialize for use */
	cn_init_fini_t	*cn_init_fini;	/* tie in cdev_t */
	cn_term_t	*cn_term;	/* terminate use as console */
	cn_getc_t	*cn_getc;	/* kernel getchar interface */
	cn_checkc_t	*cn_checkc;	/* kernel test char ready */
	cn_putc_t	*cn_putc;	/* kernel putchar interface */
	cn_dbctl_t	*cn_dbctl;	/* debugger control interface */
	cn_poll_t	*cn_poll;	/* polling mode control */
	struct	tty *cn_tp;	/* tty structure for console device */
	cdev_t	cn_dev;		/* device after cn_init_fini tie-in */
	short	cn_pri;		/* pecking order; the higher the better */
	short	cn_probegood;	/* probe routine must set to non-zero */
	void	*cn_private;	/* private data for get/check/put/dbctl */
	void	*cn_gdbprivate;	/* private data for gdb */
	int		cn_unit;	/* some drivers prefer this */
	int		cn_flags;	/* capabilities of this console */
	//char	cn_name[SPECNAMELEN + 1];	/* console (device) name */

};

/* Values for cn_flags. */
#define	CN_FLAG_NODEBUG	0x00000001	/* Not supported with debugger. */
#define	CN_FLAG_NOAVAIL	0x00000002	/* Temporarily not available. */


/* values for cn_pri - reflect our policy for console selection */
#define	CN_DEAD		0	/* device doesn't exist */
#define CN_NORMAL	1	/* device exists but is nothing special */
#define CN_INTERNAL	2	/* "internal" bit-mapped display */
#define CN_REMOTE	3	/* serial interface with remote bit set */

#ifdef _KERNEL
extern	int cons_unavail;
extern	int sysbeep_enable;		/* enable audio system beep */
extern	struct consdev *cn_tab;	/* console device */
extern  struct consdev *gdb_tab;/* gdb debugger device */

#define CONS_DRIVER(name, probe, init, initfini, term, getc, checkc, putc, dbctl, poller)	\
	static struct consdev name##_consdev = {			\
		probe, init, initfini, term, getc, checkc, putc, dbctl,	\
		poller							\
	};								\
	DATA_SET(cons_set, name##_consdev)

/* Other kernel entry points. */
int	cncheckc (void);
int	cngetc (void);
void	cninit (void);
void	cninit_finish (void);
void	cndbctl (int);
void	cnputc (int);
void	cnpoll (int);

#endif /* _KERNEL */

#endif /* !_MACHINE_CONS_H_ */
