/* ds_malloc.c */
/*
 * Copyright (c) 2022 Stephen D. Adams <stephen@sdadams.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This is a fallback implementation of the dynamic stack that simply delegates
 * to malloc(3), free(3), and realloc(3).  It must honour the same contract as
 * ds.c: emuxfs_dsgrow() *adds* the requested number of bytes to the allocation
 * (see ds.h), which realloc(3) cannot express on its own, so each allocation
 * is prefixed with its current size.  The header keeps the pointer returned to
 * the caller aligned for any type the callers use (they store both uint8_t
 * data and struct dirent pointers).
 */

#include <stdint.h>
#include <stdlib.h>

#include "ds.h"

struct emuxfs_ds_hdr {
	size_t size;
};

#define EMUXFS_DS_HDR_SIZE (sizeof(struct emuxfs_ds_hdr))

EMUXFS int
emuxfs_dsinit(void)
{
	return 0;
}

EMUXFS int
emuxfs_dsfinal(void)
{
	return 0;
}

EMUXFS int
emuxfs_dspush(void **p_out, size_t s)
{
	struct emuxfs_ds_hdr *h;

	if (s > SIZE_MAX - EMUXFS_DS_HDR_SIZE)
		exit(-1);
	h = malloc(EMUXFS_DS_HDR_SIZE + s);
	if (h == nullptr)
		exit(-1);

	h->size = s;
	*p_out = (uint8_t *)h + EMUXFS_DS_HDR_SIZE;
	return 0;
}

EMUXFS int
emuxfs_dspop(void *p)
{
	struct emuxfs_ds_hdr *h;

	if (p == nullptr)
		exit(-1); /* Programming error. */
	h = (struct emuxfs_ds_hdr *)((uint8_t *)p - EMUXFS_DS_HDR_SIZE);
	free(h);
	return 0;
}

EMUXFS int
emuxfs_dsgrow(void **p_inout, size_t s)
{
	struct emuxfs_ds_hdr *h;
	size_t newsz;

	if (*p_inout == nullptr)
		exit(-1); /* Programming error. */
	h = (struct emuxfs_ds_hdr *)
	    ((uint8_t *)*p_inout - EMUXFS_DS_HDR_SIZE);
	if (s > SIZE_MAX - h->size - EMUXFS_DS_HDR_SIZE)
		exit(-1);
	newsz = h->size + s;

	h = realloc(h, EMUXFS_DS_HDR_SIZE + newsz);
	if (h == nullptr)
		exit(-1);

	h->size = newsz;
	*p_inout = (uint8_t *)h + EMUXFS_DS_HDR_SIZE;
	return 0;
}
