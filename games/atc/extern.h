/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Ed James.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
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
 *	@(#)extern.h	8.1 (Berkeley) 5/31/93
 */

/*
 * Copyright (c) 1987 by Ed James, UC Berkeley.  All rights reserved.
 *
 * Copy permission is hereby granted provided that this notice is
 * retained on all partial or complete copies.
 *
 * For more info on this and all of my stuff, mail edjames@berkeley.edu.
 */

extern char		GAMES[];
extern const char	*filename;

extern int		clck, safe_planes, start_time, test_mode;

extern FILE		*filein, *fileout;

extern C_SCREEN		screen, *sp;

extern LIST		air, ground;

extern struct termios	tty_start, tty_new;

extern DISPLACEMENT	displacement[MAXDIR];

/* graphics.c */
extern void	done_screen(void);
extern void	draw_all(void);
extern void	erase_all(void);
extern int	getAChar(void);
extern void	init_gr(void);
extern void	ioaddstr(int, const char *);
extern void	ioclrtobot(void);
extern void	ioclrtoeol(int);
extern void	ioerror(int, int, const char *);
extern void	iomove(int);
extern void	loser(const PLANE *, const char *);
extern void	planewin(void);
extern void	redraw(void);
extern void	setup_screen(const C_SCREEN *);
extern void	quit(int);
/* input.c */
extern int	dir_no(char);
extern int	getcommand(void);
/* list.c */
extern void	append(LIST *, PLANE *);
extern void	delete(LIST *, PLANE *);
extern PLANE	*newplane(void);
/* log.c */
extern int	log_score(int);
extern void	log_score_quit(int);
extern void	open_score_file(void);
/* update.c */
extern int	addplane(void);
extern const char	*command(const PLANE *);
extern PLANE	*findplane(int);
extern char	name(const PLANE *);
extern char	number(char);
extern void	update(int);

