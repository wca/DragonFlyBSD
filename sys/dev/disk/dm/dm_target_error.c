/*        $NetBSD: dm_target_error.c,v 1.10 2010/01/04 00:12:22 haad Exp $      */

/*
 * Copyright (c) 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Adam Hamsik.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file implements initial version of device-mapper error target.
 */
#include <dev/disk/dm/dm.h>

/* Init function called from dm_table_load_ioctl. */
static int
dm_target_error_init(dm_table_entry_t *table_en, int argc, char **argv)
{

	kprintf("Error target init function called!!\n");

	dm_table_init_target(table_en, DM_ERROR_DEV, NULL);

	return 0;
}

/* Strategy routine called from dm_strategy. */
static int
dm_target_error_strategy(dm_table_entry_t *table_en, struct buf *bp)
{

	/* kprintf("Error target read function called!!\n"); */

	bp->b_error = EIO;
	bp->b_resid = 0;

	biodone(&bp->b_bio1);

	return 0;
}

/* Doesn't do anything here. */
static int
dm_target_error_destroy(dm_table_entry_t *table_en)
{
	return 0;
}

static int
dmte_mod_handler(module_t mod, int type, void *unused)
{
	dm_target_t *dmt = NULL;
	int err = 0;

	switch(type) {
	case MOD_LOAD:
		if ((dmt = dm_target_lookup("error")) != NULL) {
			dm_target_unbusy(dmt);
			return EEXIST;
		}
		dmt = dm_target_alloc("error");
		dmt->version[0] = 1;
		dmt->version[1] = 0;
		dmt->version[2] = 0;
		dmt->init = &dm_target_error_init;
		dmt->destroy = &dm_target_error_destroy;
		dmt->strategy = &dm_target_error_strategy;

		err = dm_target_insert(dmt);
		if (err == 0)
			kprintf("dm_target_error: Successfully initialized\n");
		break;

	case MOD_UNLOAD:
		err = dm_target_remove("error");
		if (err == 0)
			kprintf("dm_target_error: unloaded\n");
		break;

	default:
		break;
	}

	return err;
}

DM_TARGET_BUILTIN(dm_target_error, dmte_mod_handler);
