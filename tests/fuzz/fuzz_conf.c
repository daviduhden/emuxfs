/* fuzz_conf.c */
/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
 *
 * Optional libFuzzer entry point for the muxfs.conf parser.  It is not built
 * by the normal build; 'make fuzz-smoke' runs a bounded session as part of
 * 'make stability' and CI.  Build and run interactively with:
 *
 *   make fuzz-conf
 *   ./tests/fuzz/fuzz_conf
 */

// clang-format off
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "emuxfs.h"
// clang-format on

/*
 * One libFuzzer iteration.  The input is written to a private temporary file
 * because the configuration parser expects a descriptor; the file is removed
 * on every return path.  A parser rejection is a normal outcome and is not
 * distinguished from success here.
 */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct emuxfs_dev_conf conf;
	char		       path[] = "/tmp/emuxfs-fuzz-XXXXXX";
	int		       fd;

	if (size == 0)
		return 0;

	if ((fd = mkstemp(path)) == -1)
		return 0;
	if (write(fd, data, size) != (ssize_t)size) {
		close(fd);
		unlink(path);
		return 0;
	}
	if (lseek(fd, 0, SEEK_SET) == -1) {
		close(fd);
		unlink(path);
		return 0;
	}
	(void)emuxfs_conf_parse(&conf, fd);
	close(fd);
	unlink(path);

	return 0;
}
