/* fault.h */
/*
 * Copyright (c) 2022 Stephen D. Adams <stephen@sdadams.org>
 * Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
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
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
 *
 * Optional, compile-time-gated fault injection for crash-consistency tests.
 * It is inert unless EMUXFS_FAULT_INJECTION is defined, and is never enabled
 * by the normal build.  Call sites are unconditional; the disabled form is an
 * empty function so no #ifdef clutter is needed at each boundary.
 */

#ifndef _FAULT_H_
#define _FAULT_H_

#define EMUXFS_FAULT_ENV_POINT "EMUXFS_FAULT_POINT"
#define EMUXFS_FAULT_ENV_ACTION "EMUXFS_FAULT_ACTION"
#define EMUXFS_FAULT_EXIT_STATUS 70

/*
 * If the environment names 'name' as the active fault point, terminate the
 * process at that point (simulating a crash) or abort.  Otherwise return.
 */
void emuxfs_fault_point(const char *);

#endif /* _FAULT_H_ */
