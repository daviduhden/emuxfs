/* fault.c */

/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md). See fault.h and TESTING.md.
 */

// clang-format off
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fault.h"
// clang-format on

/*
 * Optional crash point.  In a normal build this is an empty function.  In the
 * fault-injection build, EMUXFS_FAULT_POINT selects one named boundary; when
 * it matches, EMUXFS_FAULT_ACTION chooses _exit(70) (the default) or abort().
 * Other points return immediately so a test can target a single transition.
 */
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
