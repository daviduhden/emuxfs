/* mount.c */
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
 * This file belongs to emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 *
 * emuxfs uses the FUSE implementation shipped with OpenBSD, through its
 * FUSE 2.6 high-level API.  fuse_setup(3) mounts and daemonizes;
 * fuse_loop(3) runs the single-threaded event loop; fuse_destroy(3) invokes
 * the destroy callback.  The FUSE device is opened and the filesystem is
 * mounted before any pledge(2)/unveil(2) policy is applied, and unmount(2) is
 * not called afterwards because it is not permitted under pledge(2); the
 * kernel detaches the filesystem when the process closes the FUSE device on
 * exit.  See SECURITY.md and RECOVERY.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"
#include "emuxfs.h"
#include "ops.h"
#include "sandbox.h"

static void
emuxfs_mount_usage(void)
{
	EMUXFS_TRACE("enter");
	fprintf(stderr, "usage: emuxfs mount [-f] mount_point directory ...\n");
}

/*
 * Return 1 if 'child' is 'parent' itself or lies beneath it.  Both paths must
 * be canonical absolute paths without a trailing slash (except for "/").  The
 * comparison is component-wise so that "/a/bc" is not treated as being under
 * "/a/b".
 */
static int
emuxfs_path_within(const char *parent, const char *child)
{
	size_t plen;

	plen = strlen(parent);
	if (plen == 0)
		return 0;
	if (strncmp(parent, child, plen) != 0)
		return 0;
	if (child[plen] == '\0')
		return 1; /* Equal. */
	if (parent[plen - 1] == '/')
		return 1; /* Parent is the root directory. */
	return child[plen] == '/';
}

/*
 * Replace the mount point and each array directory with its canonical
 * absolute path, then refuse a topology in which the mount point and a mirror
 * directory contain one another.  The native libfuse daemonizes and changes
 * the working directory to "/", after which a relative path would resolve
 * incorrectly, so the paths must be absolute before the sandbox is
 * configured.  A self-referential topology (the mounted filesystem inside its
 * own backing storage or vice versa) would make the serving process traverse
 * the mount it is serving and deadlock, so it is rejected here.
 *
 * Returns 0 on success, 2 for an unsafe topology, 1 otherwise.
 */
static int
emuxfs_mount_absolutize(struct emuxfs_args *args)
{
	EMUXFS_TRACE("enter");
	size_t i;
	char *rp;

	rp = realpath(args->mp_path, nullptr);
	if (rp == nullptr)
		return 1;
	if (strlcpy(args->mp_path, rp, PATH_MAX) >= PATH_MAX) {
		free(rp);
		return 1;
	}
	free(rp);

	for (i = 0; i < args->dev_count; ++i) {
		rp = realpath(args->dev_paths[i], nullptr);
		if (rp == nullptr)
			return 1;
		if (strlcpy(args->dev_paths[i], rp, PATH_MAX) >= PATH_MAX) {
			free(rp);
			return 1;
		}
		free(rp);
	}

	for (i = 0; i < args->dev_count; ++i) {
		if (emuxfs_path_within(args->mp_path, args->dev_paths[i]) ||
		    emuxfs_path_within(args->dev_paths[i], args->mp_path))
			return 2;
	}

	return 0;
}

static void
emuxfs_mount_teardown(struct fuse *fuse, char *mp)
{
	EMUXFS_TRACE("enter");
	/*
	 * fuse_destroy(3) invokes the destroy callback, which flushes any
	 * buffered write and detaches the array.  It does not unmount:
	 * unmount(2) is not covered by any pledge(2) promise, and the kernel
	 * detaches the filesystem when the FUSE device is closed as the
	 * process exits.  emuxfs_final() is idempotent and acts as a fallback.
	 */
	fuse_destroy(fuse);
	emuxfs_final();
	free(mp);
}

EMUXFS int
emuxfs_mount_main(int argc, char *argv[])
{
	EMUXFS_TRACE("enter");
	int n, rc;
	char *fuse_argv[8];
	char *mp;
	struct fuse *fuse;

	if (emuxfs_parse_args(argc, argv, 0)) {
		emuxfs_mount_usage();
		exit(1);
	}
	switch (emuxfs_mount_absolutize(&emuxfs_cmdline)) {
	case 0:
		break;
	case 2:
		fprintf(stderr, "Error: the mount point and a mirror directory "
		    "must not contain one another.\n");
		exit(1);
	default:
		fprintf(stderr,
		    "Error: Unable to resolve array directories.\n");
		exit(1);
	}

	if (emuxfs_init(0, 0))
		exit(-1);

	switch (emuxfs_dev_seq_check()) {
	case 0:
		break; /* Match. */
	case 1:
		exit(-1); /* Error. */
	case 2:
		exit(1); /* Mismatch.  Error message already printed. */
	default:
		exit(-1); /* Programming error. */
	}

	n = 0;
	fuse_argv[n++] = argv[0];
	if (emuxfs_cmdline.f)
		fuse_argv[n++] = "-f";
	fuse_argv[n++] = "-ouse_ino";
	fuse_argv[n++] = "-oallow_other";
	fuse_argv[n++] = emuxfs_cmdline.mp_path;

	mp = nullptr;
	fuse = fuse_setup(n, fuse_argv, &emuxfs_fuse_ops,
	    sizeof(emuxfs_fuse_ops), &mp, nullptr, nullptr);
	EMUXFS_TRACE("fuse_setup %s", fuse == nullptr ? "nullptr" : "ok");
	if (fuse == nullptr) {
		fprintf(stderr, "Error: Unable to mount %s.\n",
		    emuxfs_cmdline.mp_path);
		emuxfs_final();
		exit(1);
	}

	/*
	 * The FUSE device is open and the filesystem is mounted, so nothing
	 * outside the array directories needs to remain visible and no
	 * privileged syscalls need to remain available.
	 */
	if (emuxfs_sandbox_unveil_mirrors(&emuxfs_cmdline)) {
		EMUXFS_TRACE("unveil mirrors failed");
		fprintf(stderr, "Error: Unable to restrict filesystem "
		    "visibility.\n");
		emuxfs_mount_teardown(fuse, mp);
		exit(1);
	}
	if (emuxfs_sandbox_unveil_lock()) {
		EMUXFS_TRACE("unveil lock failed");
		emuxfs_mount_teardown(fuse, mp);
		exit(1);
	}
	if (emuxfs_sandbox_pledge(EMUXFS_PLEDGE_MOUNT)) {
		EMUXFS_TRACE("pledge failed");
		fprintf(stderr, "Error: Unable to restrict system calls.\n");
		emuxfs_mount_teardown(fuse, mp);
		exit(1);
	}

	EMUXFS_TRACE("entering fuse_loop");
	rc = fuse_loop(fuse);
	EMUXFS_TRACE("fuse_loop returned %d", rc);
	emuxfs_mount_teardown(fuse, mp);

	return (rc == -1) ? 1 : 0;
}
