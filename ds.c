/* ds.c */

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

// clang-format off
#include <sys/queue.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"
#include "emuxfs.h"
// clang-format on

static const size_t emuxfs_ds_memalign = sizeof(uint64_t);

static size_t emuxfs_ds_offset;
static size_t emuxfs_ds_pagesz;
static size_t emuxfs_ds_total_pagecount;
static size_t emuxfs_ds_total_allocated;
static size_t emuxfs_ds_max_pagecount;

static SLIST_HEAD(dshead, ds) emuxfs_ds_head;

struct ds {
	SLIST_ENTRY(ds) ent;
	size_t	 pagecount;
	uint8_t *begin;
	uint8_t *end;
	uint8_t *allocend;
	uint8_t	 data[];
};

static int emuxfs_ds_add_pages(size_t);

/*
 * Reserve 'sz' bytes from the current allocation block.  Sizes are rounded up
 * to the stack's alignment so every returned pointer is suitable for the
 * pointer arrays and structures stored by callers.  If the head block cannot
 * satisfy the request, enough pages are added to restore the historical peak
 * reservation before retrying.  Returns 0 and stores the pointer, or 1 on
 * allocation failure.
 */
EMUXFS int
emuxfs_dspush(void **p, size_t sz)
{
	EMUXFS_TRACE("enter");
	struct ds *n;
	size_t	   s;

	sz = emuxfs_align_up(sz, emuxfs_ds_memalign);
	EMUXFS_TRACE("dspush sz=%zu total=%zu max=%zu", sz,
	    emuxfs_ds_total_pagecount, emuxfs_ds_max_pagecount);

	n = SLIST_FIRST(&emuxfs_ds_head);
	if (sz >= (size_t)(n->end - n->allocend)) {
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
	EMUXFS_TRACE("enter");
	SLIST_REMOVE_HEAD(&emuxfs_ds_head, ent);
	emuxfs_ds_total_pagecount -= n->pagecount;
	emuxfs_ds_total_allocated -= (size_t)(n->allocend - n->begin);
	free(n);
}

/*
 * Pop all allocations newer than 'p', then make 'p' the new top.  Blocks that
 * lie entirely above the target are freed.  The caller must pop in reverse
 * allocation order; passing a pointer that is not on the stack is an error.
 */
EMUXFS int
emuxfs_dspop(void *p)
{
	EMUXFS_TRACE("enter");
	struct ds *n;

	while (!SLIST_EMPTY(&emuxfs_ds_head)) {
		n = SLIST_FIRST(&emuxfs_ds_head);
		if (((uintptr_t)p < (uintptr_t)n->begin) ||
		    ((uintptr_t)p >= (uintptr_t)n->end))
			emuxfs_ds_free_head(n);
		else {
			emuxfs_ds_total_allocated -=
			    (size_t)(n->allocend - (uint8_t *)p);
			n->allocend = p;
			if ((emuxfs_ds_total_allocated == 0) &&
			    (emuxfs_ds_total_pagecount <
				emuxfs_ds_max_pagecount)) {
				emuxfs_ds_free_head(n);
				if (emuxfs_ds_add_pages(
					emuxfs_ds_max_pagecount -
					emuxfs_ds_total_pagecount))
					return 1;
			}
			return 0;
		}
	}
	return 1;
}

/*
 * Grow the most recent allocation in place when it is the top allocation.
 * If the current block has no room, the live prefix is copied into a newly
 * pushed allocation and the old tail is released.  'p_inout' is updated when
 * the allocation moves; growing an older allocation is not supported.
 */
EMUXFS int
emuxfs_dsgrow(void **p_inout, size_t sz)
{
	EMUXFS_TRACE("enter");
	struct ds *n;
	uint8_t	  *sp, *dp;
	size_t	   ssz, dsz;

	sp = (uint8_t *)*p_inout;
	sz = emuxfs_align_up(sz, emuxfs_ds_memalign);
	EMUXFS_TRACE("dsgrow sp=%p sz=%zu", (void *)sp, sz);

	n = SLIST_FIRST(&emuxfs_ds_head);
	if (((uintptr_t)sp < (uintptr_t)n->begin) ||
	    ((uintptr_t)sp >= (uintptr_t)n->allocend)) {
		EMUXFS_TRACE("dsgrow: sp=%p begin=%p allocend=%p end=%p\n",
		    (void *)sp, (void *)n->begin, (void *)n->allocend,
		    (void *)n->end);
		return 1;
	}

	if (sz >= (size_t)(n->end - n->allocend)) {
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

/*
 * Allocate a page-quantized block and push it onto the stack.  The block
 * header and the first allocation share the same mapping, with the header
 * size rounded up to preserve alignment.  Updates the high-water mark used by
 * later push operations.
 */
static int
emuxfs_ds_add_pages(size_t pagecount)
{
	EMUXFS_TRACE("enter");
	uint8_t	  *d;
	struct ds *n;
	size_t	   sz;

	sz = pagecount * emuxfs_ds_pagesz;

	d = malloc(sz);
	n = (struct ds *)d;
	if (n == nullptr)
		return 1;
	n->pagecount = pagecount;
	n->begin = n->allocend = (d + emuxfs_ds_offset);
	n->end = d + sz;

	SLIST_INSERT_HEAD(&emuxfs_ds_head, n, ent);
	emuxfs_ds_total_pagecount += pagecount;
	if (emuxfs_ds_total_pagecount > emuxfs_ds_max_pagecount)
		emuxfs_ds_max_pagecount = emuxfs_ds_total_pagecount;

	return 0;
}

/*
 * Initialize the stack state and reserve the first page.  The header offset
 * must fit inside a page so that a page-sized allocation still has usable
 * payload.  A second initialization would leak the existing list, so callers
 * initialize exactly once per process.
 */
EMUXFS int
emuxfs_dsinit(void)
{
	EMUXFS_TRACE("enter");
	long pagesz;

	emuxfs_ds_offset =
	    emuxfs_align_up(sizeof(struct ds), emuxfs_ds_memalign);
	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz < 1)
		return 1;
	emuxfs_ds_pagesz = (size_t)pagesz;
	if (emuxfs_ds_offset >= emuxfs_ds_pagesz)
		return 1;
	emuxfs_ds_total_pagecount = 0;
	emuxfs_ds_total_allocated = 0;

	SLIST_INIT(&emuxfs_ds_head);

	if (emuxfs_ds_add_pages(1))
		return 1;

	return 0;
}

/*
 * Release every block without resetting the cached high-water mark.  The
 * process is exiting, so the reservation statistics are intentionally left
 * intact.
 */
EMUXFS int
emuxfs_dsfinal(void)
{
	EMUXFS_TRACE("enter");
	struct ds *n;

	while (!SLIST_EMPTY(&emuxfs_ds_head)) {
		n = SLIST_FIRST(&emuxfs_ds_head);
		SLIST_REMOVE_HEAD(&emuxfs_ds_head, ent);
		free(n);
	}

	return 0;
}
