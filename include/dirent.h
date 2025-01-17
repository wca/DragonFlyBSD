/*-
 * Copyright (c) 1989, 1993
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
 *	@(#)dirent.h	8.2 (Berkeley) 7/28/94
 * $FreeBSD: src/include/dirent.h,v 1.7 1999/12/29 05:01:20 peter Exp $
 */

#ifndef _DIRENT_H_
#define	_DIRENT_H_

/*
 * The kernel defines the format of directory entries returned by
 * the getdirentries(2) system call.
 */
#include <sys/dirent.h>

#if __BSD_VISIBLE

/* definitions for library routines operating on directories. */
#define	DIRBLKSIZ	1024

struct _dirdesc;
typedef struct _dirdesc	DIR;

/* flags for opendir2 */
#define	DTF_HIDEW	0x0001	/* hide whiteout entries */
#define	DTF_NODUP	0x0002	/* don't return duplicate names */
#define	DTF_REWIND	0x0004	/* rewind after reading union stack */
#define	__DTF_READALL	0x0008	/* everything has been read */
#define	__DTF_SKIPME	0x0010	/* next entry to read not current entry */

#include <sys/_null.h>

#else /* !__BSD_VISIBLE */

typedef void *	DIR;

#endif /* __BSD_VISIBLE */

#ifndef _KERNEL

#include <sys/cdefs.h>

__BEGIN_DECLS
DIR	*opendir(const char *);
struct dirent *
	 readdir(DIR *);
void	 rewinddir(DIR *);
int	 closedir(DIR *);
#if __POSIX_VISIBLE >= 199506
int	 readdir_r(DIR *, struct dirent *, struct dirent **);
#endif
#if __POSIX_VISIBLE >= 200809
int	 alphasort(const struct dirent **, const struct dirent **);
int	 dirfd(DIR *);
DIR	*fdopendir(int);
int	 scandir(const char *, struct dirent ***,
	    int (*)(const struct dirent *),
	    int (*)(const struct dirent **, const struct dirent **));
#endif
#if __XSI_VISIBLE
void	 seekdir(DIR *, long);
long	 telldir(DIR *);
#endif
#if __BSD_VISIBLE
DIR	*__opendir2(const char *, int);
DIR	*__fdopendir2(int, int);
struct dirent *
	 _readdir_unlocked(DIR *, int);
void	 _reclaim_telldir(DIR *);
void	 _seekdir(DIR *, long);
int	 getdents(int, char *, int);
int	 getdirentries(int, char *, int, long *);
#endif /* __BSD_VISIBLE */
__END_DECLS

#endif /* !_KERNEL */

#endif /* !_DIRENT_H_ */
