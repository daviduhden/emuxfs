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

/*
 * Wrapper that reports the failing path and permission string.  unveil(2)
 * takes an absolute path that must already exist; callers are responsible for
 * supplying one.  Returns 0 on success and 1 after printing a diagnostic.
 */
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

/*
 * Make the device directory visible for read, write and create operations.
 * This is the permission set needed by both normal operations and recovery;
 * nested .muxfs descriptors are opened after this call.
 */
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

/*
 * Remove all remaining filesystem visibility.  After this call the process
 * cannot unveil additional paths, so it must be the last sandbox setup step
 * before the command begins its work.
 */
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

/*
 * Apply a pledge(2) promise set.  The strings live in sandbox.h and are
 * derived from the syscalls reachable by each sub-command.  A failure is
 * reported and returned to the caller, which must abort rather than continue
 * unsandboxed.
 */
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
