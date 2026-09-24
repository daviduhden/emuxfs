/* fuzz_conf.c */
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
 * Optional libFuzzer entry point for the muxfs.conf parser.  It is not built
 * by the normal build; 'make fuzz-smoke' runs a bounded session as part of
 * 'make stability' and CI.  Build and run interactively with:
 *
 *   make fuzz-conf
 *   ./tests/fuzz/fuzz_conf
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "emuxfs.h"

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
