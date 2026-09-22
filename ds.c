/* ds.c */
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
 * This implemenation of the dynamic stack uses a singly-linked list of heap
 * allocations, for which the size of each node is quantized to an integer
 * multiple of the system's native page size.  To minimize the number of calls
 * to malloc(3) and free(3) this implemenation tracks the maximum total number
 * of bytes allocated since initialization, and upon allocation it requests
 * enough memory to bring the total reserved memory back up to the current
 * maximum total.  It is expected that the reserved size of this dynamic stack
 * will settle quickly to a fixed size, at which point no further malloc(3) or
 * free(3) calls will be necessary until finalization.  It is also expected
 * that most if not all allocations will settle to being provided from a single
 * contiguous region of memory, with address order corresponding to temporal
 * order of allocation, which should be relatively cache coherent.
 */

#include <sys/queue.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"
#include "emuxfs.h"

static const size_t emuxfs_ds_memalign = sizeof(uint64_t);

static size_t emuxfs_ds_offset;
static size_t emuxfs_ds_pagesz;
static size_t emuxfs_ds_entcount;
static size_t emuxfs_ds_total_pagecount;
static size_t emuxfs_ds_total_allocated;
static size_t emuxfs_ds_max_pagecount;

static SLIST_HEAD(dshead, ds) emuxfs_ds_head;

struct ds {
	SLIST_ENTRY(ds)	 ent;
	size_t		 pagecount;
	uint8_t		*begin;
	uint8_t		*end;
	uint8_t		*allocend;
	uint8_t		 data[];
};

static int emuxfs_ds_add_pages(size_t);

EMUXFS int
emuxfs_dspush(void **p, size_t sz)
{
	struct ds *n;
	size_t s;

	sz = emuxfs_align_up(sz, emuxfs_ds_memalign);

	n = SLIST_FIRST(&emuxfs_ds_head);
	if (n->allocend + sz >= n->end) {
		s = emuxfs_align_up(sz + emuxfs_ds_offset, emuxfs_ds_pagesz) /
		    emuxfs_ds_pagesz;
		if (s < (emuxfs_ds_max_pagecount - emuxfs_ds_total_pagecount))
			s = (emuxfs_ds_max_pagecount -
			    emuxfs_ds_total_pagecount);
		if (emuxfs_ds_add_pages(s))
			return 1;
		return emuxfs_dspush(p, sz);
	}

	*p = n->allocend;
	n->allocend += sz;
	emuxfs_ds_total_allocated += sz;

	return 0;
}

static void
emuxfs_ds_free_head(struct ds *n)
{
	SLIST_REMOVE_HEAD(&emuxfs_ds_head, ent);
	--emuxfs_ds_entcount;
	emuxfs_ds_total_pagecount -= n->pagecount;
	emuxfs_ds_total_allocated -= (size_t)(n->allocend - n->begin);
	free(n);
}

EMUXFS int
emuxfs_dspop(void *p)
{
	struct ds *n;

	while (!SLIST_EMPTY(&emuxfs_ds_head)) {
		n = SLIST_FIRST(&emuxfs_ds_head);
		if ((p < (void *)n->begin) || (p >= (void *)n->end))
			emuxfs_ds_free_head(n);
		else {
			emuxfs_ds_total_allocated -= (size_t)(n->allocend -
			    (uint8_t *)p);
			n->allocend = p;
			if ((emuxfs_ds_total_allocated == 0) &&
			    (emuxfs_ds_total_pagecount <
			     emuxfs_ds_max_pagecount)) {
				emuxfs_ds_free_head(n);
				if (emuxfs_ds_add_pages(
				    emuxfs_ds_max_pagecount))
					return 1;
			}
			return 0;
		}
	}
	return 1;
}

EMUXFS int
emuxfs_dsgrow(void **p_inout, size_t sz)
{
	struct ds *n;
	uint8_t *sp, *dp;
	size_t ssz, dsz;

	sp = (uint8_t *)*p_inout;
	sz = emuxfs_align_up(sz, emuxfs_ds_memalign);

	n = SLIST_FIRST(&emuxfs_ds_head);
	if ((n->begin < sp) || (sp >= n->allocend))
		return 1;

	if (n->allocend + sz >= n->end) {
		ssz = (size_t)(n->allocend - sp);
		dsz = ssz + sz;
		if (emuxfs_dspush((void **)&dp, dsz))
			return 1;
		memcpy(dp, sp, ssz);
		n->allocend -= ssz;
		emuxfs_ds_total_allocated -= ssz;
		*p_inout = dp;
	} else {
		n->allocend += sz;
		emuxfs_ds_total_allocated += sz;
	}
	return 0;
}

static int
emuxfs_ds_add_pages(size_t pagecount)
{
	uint8_t *d;
	struct ds *n;
	size_t sz;

	sz = pagecount * emuxfs_ds_pagesz;

	d = malloc(sz);
	n = (struct ds *)d;
	if (n == NULL)
		return 1;
	n->pagecount = pagecount;
	n->begin = n->allocend = (d + emuxfs_ds_offset);
	n->end = d + sz;

	SLIST_INSERT_HEAD(&emuxfs_ds_head, n, ent);
	++emuxfs_ds_entcount;
	emuxfs_ds_total_pagecount += pagecount;
	if (emuxfs_ds_total_pagecount > emuxfs_ds_max_pagecount)
		emuxfs_ds_total_pagecount = emuxfs_ds_max_pagecount;

	return 0;
}

EMUXFS int
emuxfs_dsinit(void)
{
	long pagesz;

	emuxfs_ds_offset = emuxfs_align_up(sizeof(struct ds),
	    emuxfs_ds_memalign);
	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz < 1)
		return 1;
	emuxfs_ds_pagesz = (size_t)pagesz;
	if (emuxfs_ds_offset >= emuxfs_ds_pagesz)
		return 1;
	emuxfs_ds_entcount = 0;
	emuxfs_ds_total_pagecount = 0;
	emuxfs_ds_total_allocated = 0;

	SLIST_INIT(&emuxfs_ds_head);

	if (emuxfs_ds_add_pages(1))
		return 1;

	return 0;
}

EMUXFS int
emuxfs_dsfinal(void)
{
	struct ds *n;

	while (!SLIST_EMPTY(&emuxfs_ds_head)) {
		n = SLIST_FIRST(&emuxfs_ds_head);
		SLIST_REMOVE_HEAD(&emuxfs_ds_head, ent);
		free(n);
	}

	return 0;
}
