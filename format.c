/* format.c */
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
#include <sys/syslimits.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid.h>

#include "chk.h"
#include "ds.h"
#include "emuxfs.h"
#include "sandbox.h"

static const char *sepdotemuxfs = "/.muxfs";
static const char *sepemuxfsdotconf = "/muxfs.conf";
static const char *sepstatedotdb = "/state.db";
static const char *sepmetadotdb = "/meta.db";
static const char *sepassigndotdb = "/assign.db";
static const char *seplfile = "/lfile";

static void
emuxfs_format_usage(void)
{
	fprintf(stderr, "usage: emuxfs format [-a checksum_algorithm] "
	    "directory ...\n");
}

/* Returns 0 on success, 2 if dev_root is not empty, 1 otherwise. */
EMUXFS int
emuxfs_dev_format(const char *dev_root, enum emuxfs_chk_alg_type alg,
    size_t chksz, size_t metasz, time_t now, const uint8_t *array_uuid)
{
	int rc;
	struct emuxfs_dev_conf conf;
	int empty;
	char path_buf[PATH_MAX];
	uuid_t uuid;
	uint32_t uuid_status;
	int fd;
	struct emuxfs_dev_state dstate;
	struct stat st;
	ino_t ino;
	struct emuxfs_desc desc;
	struct emuxfs_chk chk;
	struct emuxfs_meta meta;
	struct emuxfs_assign assign;

	static const uint64_t eno = 0;

	rc = 1;

	empty = 0;
	if (emuxfs_dir_is_empty(&empty, dev_root))
		goto out;
	if (!empty) {
		rc = 2;
		goto out;
	}

	if (chown(dev_root, 0, 0))
		goto out;
	if (chmod(dev_root, 0755))
		goto out;

	if (strlen(dev_root) + strlen(sepdotemuxfs) >= PATH_MAX)
		goto out;
	memset(path_buf, 0, PATH_MAX);
	strlcat(path_buf, dev_root, PATH_MAX);
	strlcat(path_buf, sepdotemuxfs, PATH_MAX);
	if (mkdir(path_buf, 0700))
		goto out;
	if (emuxfs_fsync_parent(AT_FDCWD, path_buf))
		goto out;

	conf = (struct emuxfs_dev_conf){
	    .version = emuxfs_program_version,
	    .chk_alg_type = alg,
	    .seq_zero_time = now,
	};
	memcpy(conf.array_uuid, array_uuid, EMUXFS_UUID_SIZE);
	uuid_create(&uuid, &uuid_status);
	if (uuid_status != uuid_s_ok)
		goto out;
	uuid_enc_le(conf.dev_uuid, &uuid);

	if (strlen(dev_root) + strlen(sepdotemuxfs) +
	    strlen(sepemuxfsdotconf) >= PATH_MAX)
		goto out;
	memset(path_buf, 0, PATH_MAX);
	strlcat(path_buf, dev_root, PATH_MAX);
	strlcat(path_buf, sepdotemuxfs, PATH_MAX);
	strlcat(path_buf, sepemuxfsdotconf, PATH_MAX);
	if ((fd = open(path_buf, O_RDWR|O_CREAT|O_EXCL, 0700)) == -1)
		goto out;
	if (emuxfs_conf_write(&conf, fd)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (close(fd))
		exit(-1);

	dstate = (struct emuxfs_dev_state){
	    .seq = 0,
	    .mounted = 0,
	    .working = 0,
	    .restoring = 0,
	    .degraded = 0,
	};
	if (strlen(dev_root) + strlen(sepdotemuxfs) +
	    strlen(sepstatedotdb) >= PATH_MAX)
		goto out;
	memset(path_buf, 0, PATH_MAX);
	strlcat(path_buf, dev_root, PATH_MAX);
	strlcat(path_buf, sepdotemuxfs, PATH_MAX);
	strlcat(path_buf, sepstatedotdb, PATH_MAX);
	if ((fd = open(path_buf, O_RDWR|O_CREAT|O_EXCL, 0700)) == -1)
		goto out;
	if (emuxfs_dev_state_write_fd(fd, &dstate)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (fsync(fd)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (close(fd))
		exit(-1);

	if (stat(dev_root, &st))
		goto out;
	ino = st.st_ino;
	if (emuxfs_desc_init_from_stat(&desc, &st, eno))
		goto out;
	emuxfs_chk_init(&chk, alg);
	emuxfs_chk_final(desc.content_checksum, &chk);
	meta = (struct emuxfs_meta){
	    .header = (struct emuxfs_meta_header){
		.flags = MF_ASSIGNED,
		.eno = eno,
	    },
	};
	memcpy(&meta.checksums[chksz], desc.content_checksum, chksz);
	emuxfs_desc_chk_meta(&meta.checksums[0], &desc, alg);
	if (strlen(dev_root) + strlen(sepdotemuxfs) +
	    strlen(sepmetadotdb) >= PATH_MAX)
		goto out;
	memset(path_buf, 0, PATH_MAX);
	strlcat(path_buf, dev_root, PATH_MAX);
	strlcat(path_buf, sepdotemuxfs, PATH_MAX);
	strlcat(path_buf, sepmetadotdb, PATH_MAX);
	if ((fd = open(path_buf, O_RDWR|O_CREAT|O_EXCL, 0700)) == -1)
		goto out;
	if (emuxfs_meta_write_fd(fd, &meta, ino, metasz)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (fsync(fd)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (close(fd))
		exit(-1);

	assign = (struct emuxfs_assign){
	    .flags = AF_ASSIGNED,
	    .ino = ino,
	};
	if (strlen(dev_root) + strlen(sepdotemuxfs) +
	    strlen(sepassigndotdb) >= PATH_MAX)
		goto out;
	memset(path_buf, 0, PATH_MAX);
	strlcat(path_buf, dev_root, PATH_MAX);
	strlcat(path_buf, sepdotemuxfs, PATH_MAX);
	strlcat(path_buf, sepassigndotdb, PATH_MAX);
	if ((fd = open(path_buf, O_RDWR|O_CREAT|O_EXCL, 0700)) == -1)
		goto out;
	if (emuxfs_assign_write_fd(fd, &assign, eno)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (fsync(fd)) {
		if (close(fd))
			exit(-1);
		goto out;
	}
	if (close(fd))
		exit(-1);

	if (strlen(dev_root) + strlen(sepdotemuxfs) +
	    strlen(seplfile) >= PATH_MAX)
		goto out;
	memset(path_buf, 0, PATH_MAX);
	strlcat(path_buf, dev_root, PATH_MAX);
	strlcat(path_buf, sepdotemuxfs, PATH_MAX);
	strlcat(path_buf, seplfile, PATH_MAX);
	if (mkdir(path_buf, 0700))
		goto out;
	if (emuxfs_fsync_parent(AT_FDCWD, path_buf))
		goto out;

	rc = 0;
out:
	return rc;
}

EMUXFS int
emuxfs_format_main(int argc, char *argv[])
{
	int rc, c, subrc;
	enum emuxfs_chk_alg_type alg;
	char dev_roots[EMUXFS_DEV_COUNT_MAX][PATH_MAX];
	size_t dev_root_count, len, i;
	time_t now;
	uuid_t uuid;
	uint32_t uuid_status;
	uint8_t array_uuid_buf[EMUXFS_UUID_SIZE];
	size_t chksz, metasz;

	/*
	 * emuxfs_dsinit() has already been called by main().  Calling it again
	 * would reinitialize (and leak) the dynamic stack.
	 */

	rc = 1;
	alg = CAT_MD5;
	memset(dev_roots, 0, EMUXFS_DEV_COUNT_MAX * PATH_MAX);
	dev_root_count = 0;

	/* Shift one argument to account for the sub-command. */
	++argv;
	--argc;

	while ((c = getopt(argc, argv, "a:")) != -1) {
		switch (c) {
		case 'a':
			if (emuxfs_chk_str_to_type(&alg, optarg,
			    strlen(optarg)))
				goto out;
			break;
		default:
			emuxfs_format_usage();
			exit(1);
		}
	}
	argc -= optind;
	argv += optind;
	while (argc > 0) {
		len = strlen(argv[0]);
		if (len == 0 || len >= PATH_MAX)
			goto out;
		if (dev_root_count == EMUXFS_DEV_COUNT_MAX)
			goto out;
		memcpy(dev_roots[dev_root_count++], argv[0], len);
		--argc;
		++argv;
	}
	if (dev_root_count == 0)
		goto out;

	/*
	 * Confine the process to the directories being formatted and drop
	 * unnecessary system call privileges before touching the disk.
	 */
	for (i = 0; i < dev_root_count; ++i) {
		if (emuxfs_sandbox_unveil_path(dev_roots[i]))
			goto out;
	}
	if (emuxfs_sandbox_unveil_lock())
		goto out;
	if (emuxfs_sandbox_pledge(EMUXFS_PLEDGE_FORMAT))
		goto out;

	now = time(NULL);
	uuid_create(&uuid, &uuid_status);
	if (uuid_status != uuid_s_ok)
		goto out;
	uuid_enc_le(array_uuid_buf, &uuid);
	chksz = emuxfs_chk_size(alg);
	if (emuxfs_meta_size_raw(&metasz, alg))
		goto out;

	for (i = 0; i < dev_root_count; ++i) {
		subrc = emuxfs_dev_format(dev_roots[i], alg, chksz, metasz, now,
		    array_uuid_buf);
		if (subrc == 2) {
			fprintf(stderr, "Directory \"%s\" is not empty.\n",
			    dev_roots[i]);
			goto out;
		}
		if (subrc != 0)
			goto out;
	}

	rc = 0;
out:
	if (emuxfs_dsfinal())
		exit(-1);
	return rc;
}
