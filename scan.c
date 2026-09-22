/* scan.c */
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

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds.h"
#include "emuxfs.h"
#include "sandbox.h"

/*
 * 'path' is required to be null-terminated and pointing to a buffer of
 * capacity PATH_MAX, 'len' is provided so that 'path' may be mutated, then
 * returned to its original state.
 */
static int
emuxfs_scan_impl(enum emuxfs_scan_mode mode, dind dev_index, char *path,
    size_t len)
{
	EMUXFS_TRACE("enter");
	int			 rc;
	struct emuxfs_dev	*dev;
	struct stat		 st;
	struct emuxfs_dir	 dir;
	struct dirent		*dirent;
	size_t			 i, sublen, dnamelen;
	const char		*dname;
	const char		*epath; /* Effective path. */

	epath = (len > 0) ? path : ".";

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	if (fstatat(dev->root_fd, epath, &st, AT_SYMLINK_NOFOLLOW)) {
		fprintf(stderr, "Cannot stat %s/%s: %s\n", dev->root_path,
		    epath, strerror(errno));
		return 1;
	}

	/*
	 * emuxfs does not support hard links.  Two names for one inode share a
	 * metadata slot and an eno, so the array cannot be validated or healed
	 * without risking the other names.  Report and fail rather than
	 * pretend it is an ordinary node.
	 */
	if (emuxfs_is_hardlink(&st)) {
		fprintf(stderr, "Unsupported hard link: %s/%s\n",
		    dev->root_path, epath);
		return 1;
	}

	if (S_ISDIR(st.st_mode)) {
		rc = 1;
		if (emuxfs_pushdir(&dir, dev->root_fd, epath)) {
			fprintf(stderr, "Cannot read directory %s/%s\n",
			    dev->root_path, epath);
			goto dirout;
		}
		for (i = 0; i < dir.ent_count; ++i) {
			dirent = dir.ent_array[i];
			dname = dirent->d_name;
			dnamelen = dirent->d_namlen;
			if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
				continue;
			if ((dnamelen == 2) && (strncmp("..", dname, 2) == 0))
				continue;
			if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) ==
			    0))
				continue;
			sublen = dnamelen;
			if (len > 0)
				sublen += len + 1;
			if (sublen >= PATH_MAX) {
				fprintf(stderr, "Path too long: %s/%s\n",
				    dev->root_path, epath);
				goto dirout2;
			}
			if (len > 0)
				strlcat(path, "/", PATH_MAX);
			strlcat(path, dname, PATH_MAX);
			if (emuxfs_scan_impl(mode, dev_index, path, sublen))
				goto dirout2;
			path[len] = '\0';
		}
		if (emuxfs_readback(dev_index, epath, 0, NULL)) {
			printf("%s/%s\n", dev->root_path, epath);
			if ((mode == EMUXFS_SCAN_HEAL) &&
			    emuxfs_state_restore_push_back(dev_index, epath))
				exit(-1);
		}
		rc = 0;
dirout2:
		if (emuxfs_popdir(&dir)) {
			EMUXFS_TRACE("scan_impl: popdir failed for %s\n",
			    epath);
			exit(-1);
		}
dirout:
		return rc;
	}
	if (!(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) {
		fprintf(stderr, "Unsupported node type: %s/%s (mode %o)\n",
		    dev->root_path, epath, (unsigned)st.st_mode);
		return 1;
	}
	if (emuxfs_readback(dev_index, epath, 0, NULL)) {
		printf("%s/%s\n", dev->root_path, epath);
		if ((mode == EMUXFS_SCAN_HEAL) &&
		    emuxfs_state_restore_push_back(dev_index, epath))
			exit(-1);
	}
	return 0;
}

static int
emuxfs_scan(enum emuxfs_scan_mode mode, dind dev_index)
{
	EMUXFS_TRACE("enter");
	char path[PATH_MAX];
	struct emuxfs_dev *dev;
	size_t bad;

	/*
	 * Cross-check assign.db against meta.db before walking the tree.  A
	 * crash between the metadata and assign writes, or a mapping left by
	 * inode reuse, would otherwise make validation and repair decisions
	 * on inconsistent information.
	 */
	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;
	if (emuxfs_meta_assign_check(dev_index, &bad)) {
		dprintf(2, "Error: %s: cannot read the metadata/assign "
		    "mappings\n", dev->root_path);
		return 1;
	}
	if (bad != 0) {
		dprintf(2, "Error: %s: %lu metadata/assign mapping(s) are "
		    "inconsistent\n", dev->root_path, (unsigned long)bad);
		return 1;
	}

	memset(path, 0, PATH_MAX);
	if (emuxfs_scan_impl(mode, dev_index, path, 0))
		return 1;
	if (mode == EMUXFS_SCAN_HEAL)
		emuxfs_restore_now();
	return 0;
}

static void
emuxfs_audit_usage(void)
{
	EMUXFS_TRACE("enter");
	fprintf(stderr, "usage: emuxfs audit directory ...\n");
}

static void
emuxfs_heal_usage(void)
{
	EMUXFS_TRACE("enter");
	fprintf(stderr, "usage: emuxfs heal directory ...\n");
}

EMUXFS int
emuxfs_scan_main(enum emuxfs_scan_mode scan_mode, int argc, char *argv[])
{
	EMUXFS_TRACE("enter");
	dind i, dev_count;

	/*
	 * emuxfs_dsinit() and emuxfs_state_syslog_init() have already been
	 * called by main().  Calling them again would reinitialize (and leak)
	 * the dynamic stack and the syslog handle.
	 */
	if (emuxfs_parse_args(argc, argv, 1)) {
		if (scan_mode == EMUXFS_SCAN_AUDIT)
			emuxfs_audit_usage();
		else
			emuxfs_heal_usage();
		exit(1);
	}

	/*
	 * Confine the process to the array directories and drop unnecessary
	 * system call privileges before mounting the devices.
	 */
	if (emuxfs_sandbox_unveil_mirrors(&emuxfs_cmdline))
		exit(1);
	if (emuxfs_sandbox_unveil_lock())
		exit(1);
	if (emuxfs_sandbox_pledge(EMUXFS_PLEDGE_SCAN))
		exit(1);

	/*
	 * audit must not modify state.db: open the devices read-only.  heal
	 * may write and opens them read-write.
	 */
	emuxfs_cmdline.readonly = (scan_mode == EMUXFS_SCAN_AUDIT);

	EMUXFS_TRACE("scan_main: before init\n");
	if (emuxfs_init(0))
		exit(-1);
	EMUXFS_TRACE("scan_main: after init, dev_count=%lu\n",
	    (unsigned long)emuxfs_dev_count());

	if ((dev_count = emuxfs_dev_count()) == 0) {
		dprintf(2, "Error: The directory array is empty.\n");
		emuxfs_final();
		exit(1);
	}

	EMUXFS_TRACE("scan_main: before seq_check\n");
	switch (emuxfs_dev_seq_check()) {
	case 0:
		break; /* Match. */
	case 1:
		emuxfs_final();
		exit(-1); /* Error. */
	case 2:
		emuxfs_final();
		exit(1); /* Mismatch.  Error message already printed. */
	default:
		emuxfs_final();
		exit(-1); /* Programming error. */
	}
	EMUXFS_TRACE("scan_main: after seq_check\n");

	for (i = 0; i < dev_count; ++i) {
		EMUXFS_TRACE("scan_main: scanning dev %lu\n", (unsigned long)i);
		if (emuxfs_scan(scan_mode, i)) {
			EMUXFS_TRACE("scan_main: scan dev %lu failed\n",
			    (unsigned long)i);
			emuxfs_final();
			exit(-1);
		}
	}
	EMUXFS_TRACE("scan_main: scans done\n");

	if ((scan_mode == EMUXFS_SCAN_HEAL) &&
	    (emuxfs_state_ambiguity_count() > 0)) {
		dprintf(2, "Error: ambiguous copies were found; nothing was "
		    "overwritten. Resolve them explicitly with "
		    "'emuxfs sync destination source'.\n");
		emuxfs_final();
		exit(1);
	}

	if (emuxfs_final())
		exit(-1);

	return 0;
}
