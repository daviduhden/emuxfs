/* version.c */
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

#include <stdio.h>

#include "emuxfs.h"

struct emuxfs_version emuxfs_program_version = {
	.number   = 1,
	.revision = 0,
	.flavor   = VF_CURRENT
};

EMUXFS void
emuxfs_version_print(void)
{
	EMUXFS_TRACE("enter");
	static const char *flavors[] = {
		"current",
		"release",
		"stable"
	};
	printf("emuxfs %u.%u-%s (The Enhanced Multiplexed File System)\n",
	    emuxfs_program_version.number, emuxfs_program_version.revision,
	    flavors[emuxfs_program_version.flavor]);
}
