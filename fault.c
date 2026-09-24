/* fault.c */
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
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 * See fault.h and TESTING.md.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fault.h"

void
emuxfs_fault_point([[maybe_unused]] const char *name)
{
#ifdef EMUXFS_FAULT_INJECTION
	const char *point, *action;

	point = getenv(EMUXFS_FAULT_ENV_POINT);
	if ((point == nullptr) || (strcmp(point, name) != 0))
		return;

	action = getenv(EMUXFS_FAULT_ENV_ACTION);
	if ((action == nullptr) || (strcmp(action, "exit") == 0))
		_exit(EMUXFS_FAULT_EXIT_STATUS);
	if (strcmp(action, "abort") == 0)
		abort();
#endif
}
