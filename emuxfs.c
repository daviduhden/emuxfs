#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds.h"
#include "emuxfs.h"
#include "sandbox.h"

static void
usage(void)
{
	EMUXFS_TRACE("enter");
	const char *prog;

	prog = getprogname();

	dprintf(2, "usage: %s format [-a checksum_algorithm] directory ...\n",
	    prog);
	dprintf(2, "       %s mount [-f] mount_point directory ...\n", prog);
	dprintf(2, "       %s audit directory ...\n", prog);
	dprintf(2, "       %s heal directory ...\n", prog);
	dprintf(2, "       %s sync destination source ...\n", prog);
	dprintf(2, "       %s version\n", prog);
}

int
main(int argc, char *argv[])
{
	EMUXFS_TRACE("enter");
	const char *cmd;

	if (emuxfs_state_syslog_init())
		exit(-1);
	if (emuxfs_dsinit())
		exit(-1);

	if (argc < 2) {
		usage();
		exit(1);
	}
	cmd = argv[1];

	if (strcmp(cmd, "format") == 0)
		return emuxfs_format_main(argc, argv);
	else if (strcmp(cmd, "mount") == 0)
		return emuxfs_mount_main(argc, argv);
	else if (strcmp(cmd, "audit") == 0)
		return emuxfs_scan_main(EMUXFS_SCAN_AUDIT, argc, argv);
	else if (strcmp(cmd, "heal") == 0)
		return emuxfs_scan_main(EMUXFS_SCAN_HEAL, argc, argv);
	else if (strcmp(cmd, "sync") == 0)
		return emuxfs_sync_main(argc, argv);
	else if (strcmp(cmd, "version") == 0) {
		if (emuxfs_sandbox_pledge(EMUXFS_PLEDGE_VERSION))
			exit(-1);
		emuxfs_version_print();
		exit(0);
	}

	usage();
	exit(1);
}
