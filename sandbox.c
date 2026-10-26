/* sandbox.c */

/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
 *
 * This module is the only place that calls pledge(2) and unveil(2).  Keeping
 * the policy in one file makes it auditable and keeps the rest of the program
 * free of OpenBSD-specific sandboxing code.
 */

// clang-format off
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emuxfs.h"
#include "sandbox.h"
// clang-format on

static int
emuxfs_sandbox_unveil_path_perm(const char *path, const char *permissions)
{
	EMUXFS_TRACE("enter");
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
	EMUXFS_TRACE("enter");
	return emuxfs_sandbox_unveil_path_perm(path, "rwc");
}

EMUXFS int
emuxfs_sandbox_unveil_mirrors(const struct emuxfs_args *args)
{
	EMUXFS_TRACE("enter");
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
	EMUXFS_TRACE("enter");
	if (unveil(nullptr, nullptr) == -1) {
		dprintf(2, "emuxfs: unveil lock: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}

EMUXFS int
emuxfs_sandbox_pledge(const char *promises)
{
	EMUXFS_TRACE("enter");
	if (pledge(promises, nullptr) == -1) {
		dprintf(2, "emuxfs: pledge(\"%s\"): %s\n", promises,
		    strerror(errno));
		return 1;
	}
	return 0;
}
