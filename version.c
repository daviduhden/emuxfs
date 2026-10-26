/* version.c */

// clang-format off
#include <stdio.h>

#include "emuxfs.h"
// clang-format on

/*
 * Program version written into muxfs.conf.  The number and revision are
 * provenance only; EMUXFS_FORMAT_VERSION controls on-disk compatibility.
 */
struct emuxfs_version emuxfs_program_version = {
    .number = 1, .revision = 0, .flavor = VF_CURRENT};

EMUXFS void
emuxfs_version_print(void)
{
	EMUXFS_TRACE("enter");
	static const char *flavors[] = {"current", "release", "stable"};
	printf("emuxfs %u.%u-%s (The Enhanced Multiplexed File System)\n",
	    emuxfs_program_version.number, emuxfs_program_version.revision,
	    flavors[emuxfs_program_version.flavor]);
}
