/*
 * Copyright 2013 Garrett D'Amore <garrett@damore.org>
 * Copyright 2010 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2002-2004 Tim J. Robbins. All rights reserved.
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Paul Borman at Krystal Technologies.
 *
 * Copyright (c) 2011 The FreeBSD Foundation
 * All rights reserved.
 * Portions of this software were developed by David Chisnall
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 * @(#)none.c	8.1 (Berkeley) 6/4/93
 */

#include <errno.h>
#include <limits.h>
#include <runetype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "mblocal.h"

static size_t	_none_mbrtowc(wchar_t * __restrict, const char * __restrict,
		    size_t, mbstate_t * __restrict);
static int	_none_mbsinit(const mbstate_t *);
static size_t	_none_mbsnrtowcs(wchar_t * __restrict dst,
		    const char ** __restrict src, size_t nms, size_t len,
		    mbstate_t * __restrict ps __unused);
static size_t	_none_wcrtomb(char * __restrict, wchar_t,
		    mbstate_t * __restrict);
static size_t	_none_wcsnrtombs(char * __restrict, const wchar_t ** __restrict,
		    size_t, size_t, mbstate_t * __restrict);

/* setup defaults */

int __mb_cur_max = 1;
int __mb_sb_limit = 256; /* Expected to be <= _CACHED_RUNES */

int
_none_init(struct xlocale_ctype *l, _RuneLocale *rl)
{

	l->__mbrtowc = _none_mbrtowc;
	l->__mbsinit = _none_mbsinit;
	l->__mbsnrtowcs = _none_mbsnrtowcs;
	l->__wcrtomb = _none_wcrtomb;
	l->__wcsnrtombs = _none_wcsnrtombs;
	l->runes = rl;
	l->__mb_cur_max = 1;
	l->__mb_sb_limit = 256;
	return(0);
}

static int
_none_mbsinit(const mbstate_t *ps __unused)
{

	/*
	 * Encoding is not state dependent - we are always in the
	 * initial state.
	 */
	return (1);
}

static size_t
_none_mbrtowc(wchar_t * __restrict pwc, const char * __restrict s, size_t n,
    mbstate_t * __restrict ps __unused)
{

	if (s == NULL)
		/* Reset to initial shift state (no-op) */
		return (0);
	if (n == 0)
		/* Incomplete multibyte sequence */
		return ((size_t)-2);
	if (pwc != NULL)
		*pwc = (unsigned char)*s;
	return (*s == '\0' ? 0 : 1);
}

static size_t
_none_wcrtomb(char * __restrict s, wchar_t wc,
    mbstate_t * __restrict ps __unused)
{

	if (s == NULL)
		/* Reset to initial shift state (no-op) */
		return (1);
	if (wc < 0 || wc > UCHAR_MAX) {
		errno = EILSEQ;
		return ((size_t)-1);
	}
	*s = (unsigned char)wc;
	return (1);
}

static size_t
_none_mbsnrtowcs(wchar_t * __restrict dst, const char ** __restrict src,
    size_t nms, size_t len, mbstate_t * __restrict ps __unused)
{
	const char *s;
	size_t nchr;

	if (dst == NULL) {
		s = memchr(*src, '\0', nms);
		return (s != NULL ? s - *src : nms);
	}

	s = *src;
	nchr = 0;
	while (len-- > 0 && nms-- > 0) {
		if ((*dst++ = (unsigned char)*s++) == L'\0') {
			*src = NULL;
			return (nchr);
		}
		nchr++;
	}
	*src = s;
	return (nchr);
}

static size_t
_none_wcsnrtombs(char * __restrict dst, const wchar_t ** __restrict src,
    size_t nwc, size_t len, mbstate_t * __restrict ps __unused)
{
	const wchar_t *s;
	size_t nchr;

	if (dst == NULL) {
		for (s = *src; nwc > 0 && *s != L'\0'; s++, nwc--) {
			if (*s < 0 || *s > UCHAR_MAX) {
				errno = EILSEQ;
				return ((size_t)-1);
			}
		}
		return (s - *src);
	}

	s = *src;
	nchr = 0;
	while (len-- > 0 && nwc-- > 0) {
		if (*s < 0 || *s > UCHAR_MAX) {
			errno = EILSEQ;
			return ((size_t)-1);
		}
		if ((*dst++ = *s++) == '\0') {
			*src = NULL;
			return (nchr);
		}
		nchr++;
	}
	*src = s;
	return (nchr);
}

/*
 * Multibyte binary data to escaped wchar.
 * Round-trip match guaranteed, including 0x00 bytes.
 *
 * Cannot return an error.  *slen bytes is converted to the destination
 * buffer until one or the other is exhausted.  Destination elements returned
 * and *slen modified with source elements processed.
 *
 * Incomplete sequences or partial re-encodings that would overflow the
 * destination buffer are not processed and will also leave excess *slen.
 *
 * Never returns an error.  Instead, incomplete sequences are not processed
 * and *slen will index to the beginning of the incomplete sequence.  It is
 * possible for 0 to be returned and for *slen to be set to 0 due to an
 * incomplete whole-buffer sequence, unless termination is specified.
 *
 * If termination is specified any trailing incomplete sequences are escaped
 * and *slen will index to the end of the source buffer, unless insufficient
 * room exists in the destination.  If there is insufficient room, *slen may
 * not be able to index to the end of the source buffer.
 *
 * Does not support a NULL dst on purpose - caller is expected to loop
 * in parts.
 */
static size_t
_none_mbintowcr(wchar_t * __restrict dst, const char * __restrict src,
		size_t dlen, size_t *slen, int flags)
{
	size_t i;
	size_t j;
	size_t n = *slen;

	for (i = j = 0; i < n; ++i) {
		if (j == dlen)
			break;
		if (dst)
			dst[j] = (unsigned char)src[i];
		++j;
	}
	/* no partial sequences so we can ignore flags */
	*slen = i;

	return j;
}

/*
 * Escaped wchar to multibyte binary data.
 * Round-trip match guaranteed, including 0x00 bytes.
 *
 * *slen bytes is converted to the destination buffer until one or the other
 * is exhausted.  Destination elements returned and *slen modified with
 * source elements processed.
 *
 * Can return an error only if the first wchar src[] element is illegal,
 * otherwise will process up to the illegal wchar and return an error on
 * the next call (if called with the remainder).
 *
 * Never returns -2.  Instead, incomplete sequences are not processed and
 * *slen will index to the beginning of the incomplete sequence.  If
 * termination is specified, incomplete sequences are discarded and *slen
 * indexes to the end of the input array.
 *
 * Does not support a NULL dst on purpose - caller is expected to loop
 * in parts.
 */
static size_t
_none_wcrtombin(char * __restrict dst, const wchar_t * __restrict src,
		size_t dlen, size_t *slen, int flags)
{
	size_t i;
	size_t j;
	size_t n = *slen;

	for (i = j = 0; i < n; ++i) {
		if (j == dlen)
			break;
		if (src[i] >= 0x100) {
			if (i == 0) {
				errno = EILSEQ;
				return(-1);
			}
			break;
		}
		if (dst)
			dst[j] = (unsigned char)src[i];
		++j;
	}
	/* no partial sequences so we can ignore flags */
	*slen = i;

	return j;
}

/* setup defaults */

struct xlocale_ctype __xlocale_global_ctype = {
	{{0}, "C"},
	(_RuneLocale*)&_DefaultRuneLocale,
	_none_mbrtowc,
	_none_mbsinit,
	_none_mbsnrtowcs,
	_none_wcrtomb,
	_none_wcsnrtombs,
	_none_mbintowcr,
	_none_wcrtombin,
	1, /* __mb_cur_max, */
	256 /* __mb_sb_limit */
};

struct xlocale_ctype __xlocale_C_ctype = {
	{{0}, "C"},
	(_RuneLocale*)&_DefaultRuneLocale,
	_none_mbrtowc,
	_none_mbsinit,
	_none_mbsnrtowcs,
	_none_wcrtomb,
	_none_wcsnrtombs,
	_none_mbintowcr,
	_none_wcrtombin,
	1, /* __mb_cur_max, */
	256 /* __mb_sb_limit */
};
