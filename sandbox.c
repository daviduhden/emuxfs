/* sandbox.c */
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
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 *
 * This module is the only place that calls pledge(2) and unveil(2).  Keeping
 * the policy in one file makes it auditable and keeps the rest of the program
 * free of OpenBSD-specific sandboxing code.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emuxfs.h"
#include "sandbox.h"

static int
emuxfs_sandbox_unveil_path_perm(const char *path, const char *permissions)
{
	if (unveil(path, permissions) == -1) {
		dprintf(2, "emuxfs: unveil(\"%s\", \"%s\"): %s\n", path,
		    permissions, strerror(errno));
		return 1;
	}
	return 0;
}

EMUXFS int
emuxfs_sandbox_unveil_path(const char *path)
{
	return emuxfs_sandbox_unveil_path_perm(path, "rwc");
}

EMUXFS int
emuxfs_sandbox_unveil_mirrors(const struct emuxfs_args *args)
{
	size_t i;

	for (i = 0; i < args->dev_count; ++i) {
		if (emuxfs_sandbox_unveil_path(args->dev_paths[i]))
			return 1;
	}

	return 0;
}

EMUXFS int
emuxfs_sandbox_unveil_lock(void)
{
	if (unveil(NULL, NULL) == -1) {
		dprintf(2, "emuxfs: unveil lock: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}

EMUXFS int
emuxfs_sandbox_pledge(const char *promises)
{
	if (pledge(promises, NULL) == -1) {
		dprintf(2, "emuxfs: pledge(\"%s\"): %s\n", promises,
		    strerror(errno));
		return 1;
	}
	return 0;
}
