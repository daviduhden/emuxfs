/* ds.h */
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
 * The Dynamic Stack
 *
 * This allocator uses a stack of allocations with runtime-defined size.
 * Allocations are ordered by the time of allocation.  When an older allocation
 * is popped, it is freed and invalidates any newer allocations up to that
 * point.  In this implementation the caller is expected to pop all pushed
 * allocations in reverse order; failure to do this may result in memory
 * leakage.  It is important to note that it is intended that the lifetimes of
 * these allocations align with the lifetimes of the other local variables
 * declared in the same scope, and that usage beyond this scope is undefined.
 * This implementation is not thread-safe and should be called by only one
 * thread.  This implementation should also not be called from a signal
 * handler.  The allocator may preemptively reserve memory based on statistics
 * relating to its usage.
 *
 * An allocation may 'grow', increasing its allocated size by a number of
 * bytes, provided that it is the most recently 'pushed' allocation.  Calling
 * emuxfs_dsgrow() on any other pointer is undefined.
 */

#ifndef _DS_H_
#define _DS_H_

#include <stddef.h>

EMUXFS int emuxfs_dsinit(void);
EMUXFS int emuxfs_dsfinal(void);
EMUXFS int emuxfs_dspush(void **, size_t);
EMUXFS int emuxfs_dspop(void *);
EMUXFS int emuxfs_dsgrow(void **, size_t);

#endif /* _DS_H_ */
