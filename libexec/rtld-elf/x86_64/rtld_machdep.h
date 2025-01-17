/*-
 * Copyright (c) 1999, 2000 John D. Polstra.
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
 * $FreeBSD$
 */

#ifndef RTLD_MACHDEP_H
#define RTLD_MACHDEP_H	1

#include <sys/types.h>
#include <machine/atomic.h>

#define	CACHE_LINE_SIZE		64

struct Struct_Obj_Entry;

/* Return the address of the .dynamic section in the dynamic linker. */
#define rtld_dynamic(obj) \
    ((const Elf_Dyn *)((obj)->relocbase + (Elf_Addr)&_DYNAMIC))

/* Fixup the jump slot at "where" to transfer control to "target". */
static inline Elf_Addr
reloc_jmpslot(Elf_Addr *where, Elf_Addr target,
	      const struct Struct_Obj_Entry *obj,
	      const struct Struct_Obj_Entry *refobj, const Elf_Rel *rel)
{
#ifdef dbg
    dbg("reloc_jmpslot: *%p = %p", (void *)(where),
	(void *)(target));
#endif
    (*(Elf_Addr *)(where) = (Elf_Addr)(target));
    return target;
}

#define make_function_pointer(def, defobj) \
	((defobj)->relocbase + (def)->st_value)

#define call_initfini_pointer(obj, target) \
	(((InitFunc)(target))())

#define call_init_pointer(obj, target) \
	(((InitArrFunc)(target))(main_argc, main_argv, environ))
#define calculate_first_tls_offset(size, align) \
	roundup2(size, align)
#define calculate_tls_offset(prev_offset, prev_size, size, align) \
	roundup2((prev_offset) + (size), align)
#define calculate_tls_end(off, size) 	(off)

typedef struct {
    unsigned long ti_module;
    unsigned long ti_offset;
} tls_index;

struct tls_tcb;

extern void *__tls_get_addr(tls_index *ti);
extern void *__tls_get_addr_tcb(struct tls_tcb *tcb, tls_index *ti);

#define	RTLD_DEFAULT_STACK_PF_EXEC	PF_X
#define	RTLD_DEFAULT_STACK_EXEC		PROT_EXEC

#endif
