/*-
 * Copyright (c) 1998 Michael Smith
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/lib/libstand/sbrk.c,v 1.2.2.1 2000/05/18 08:00:57 ps Exp $
 * $DragonFly: src/lib/libstand/sbrk.c,v 1.2 2003/06/17 04:26:51 dillon Exp $
 */

/*
 * Minimal sbrk() emulation required for malloc support.
 */

#include <string.h>
#include "stand.h"

static size_t	maxheap, heapsize = 0;
static void	*sbrkbase;

void
setheap(void *base, void *top)
{
    /* Align start address to 16 bytes for the malloc code. Sigh. */
    sbrkbase = (void *)(((uintptr_t)base + 15) & ~15);
    maxheap = top - sbrkbase;
}

char *
getheap(size_t *sizep)
{
    *sizep = maxheap;
    return sbrkbase;
}

char *
sbrk(int incr)
{
    char	*ret;
    
    if ((heapsize + incr) <= maxheap) {
	ret = sbrkbase + heapsize;
	bzero(ret, incr);
	heapsize += incr;
	return(ret);
    }
    errno = ENOMEM;
    return((char *)-1);
}

