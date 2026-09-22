/* desc.c */
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

#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chk.h"
#include "ds.h"
#include "emuxfs.h"

EMUXFS int
emuxfs_desc_chk_reg_content(struct emuxfs_desc *desc, dind dev_index,
    const char *path)
{
	int rc;
	struct emuxfs_dev *dev;
	struct stat st;
	size_t fsz;
	int fd;
	struct emuxfs_chk chk;
	uint8_t readbuf[EMUXFS_BLOCK_SIZE];
	enum emuxfs_chk_alg_type alg;

	rc = 1;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;
	alg = dev->conf.chk_alg_type;

	if (fstatat(dev->root_fd, path, &st, AT_SYMLINK_NOFOLLOW))
		return 1;
	fsz = st.st_size;

	if (fsz <= EMUXFS_BLOCK_SIZE) {
		if ((fd = openat(dev->root_fd, path, O_RDONLY|O_NOFOLLOW)) ==
		    -1)
			goto out;
		emuxfs_chk_init(&chk, alg);
		if (lseek(fd, 0, SEEK_SET) != 0)
			goto out2;
		if (read(fd, readbuf, fsz) != fsz)
			goto out2;
		emuxfs_chk_update(&chk, readbuf, fsz);
		emuxfs_chk_final(desc->content_checksum, &chk);

		rc = 0;
out2:
		if (close(fd))
			exit(-1);
out:
		return rc;
	}

	return emuxfs_lfile_readback(desc->content_checksum, dev_index, path, 0,
	    st.st_size, NULL);
}

static int
emuxfs_desc_chk_dir_content(struct emuxfs_desc *desc, dind dev_index,
    const char *path)
{
	int rc, fd;
	struct emuxfs_dev *dev;
	struct emuxfs_dir dir;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	if ((fd = openat(dev->root_fd, path, O_RDONLY|O_NOFOLLOW)) ==
	    -1)
		return 1;
	if (emuxfs_pushdir(&dir, fd, "."))
		exit(-1);
	rc = emuxfs_dir_content_chk(desc->content_checksum, dev_index, &dir);
	if (emuxfs_popdir(&dir))
		exit(-1);
	if (close(fd))
		exit(-1);
	return rc;
}

EMUXFS int
emuxfs_desc_chk_symlink_content(struct emuxfs_desc *desc, dind dev_index,
    const char *path)
{
	struct emuxfs_dev *dev;
	int fd;
	enum emuxfs_chk_alg_type alg;
	ssize_t lnksz;
	char lnkbuf[PATH_MAX];

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;
	fd = dev->root_fd;
	alg = dev->conf.chk_alg_type;

	memset(lnkbuf, 0, PATH_MAX);
	if ((lnksz = readlinkat(fd, path, lnkbuf, PATH_MAX - 1)) == -1)
		return 1;

	emuxfs_desc_chk_provided_content(desc, (uint8_t *)lnkbuf, lnksz, alg);
	return 0;
}

EMUXFS void
emuxfs_desc_chk_provided_content(struct emuxfs_desc *desc,
    const uint8_t *content,
    size_t contentsz, enum emuxfs_chk_alg_type alg_type)
{
	struct emuxfs_chk chk;

	emuxfs_chk_init(&chk, alg_type);
	emuxfs_chk_update(&chk, content, contentsz);
	emuxfs_chk_final(desc->content_checksum, &chk);
}

/*
 * It is assumed that 'desc' has been initialized via
 * emuxfs_desc_init_from_stat().
 */
EMUXFS int
emuxfs_desc_chk_node_content(struct emuxfs_desc *desc, dind dev_index,
    const char *path)
{
	switch (desc->type) {
	case EMUXFS_DT_REG:
		return emuxfs_desc_chk_reg_content(desc, dev_index, path);
	case EMUXFS_DT_DIR:
		return emuxfs_desc_chk_dir_content(desc, dev_index, path);
	case EMUXFS_DT_LNK:
		return emuxfs_desc_chk_symlink_content(desc, dev_index, path);
	default:
		return 1;
	}

	exit(-1); /* Unreachable. */
}

EMUXFS void
emuxfs_desc_chk_meta(uint8_t *sum_out, const struct emuxfs_desc *desc,
    enum emuxfs_chk_alg_type alg_type)
{
	struct emuxfs_chk chk;
	ssize_t chksz;
	uint64_t u64h, u64le;

	chksz = emuxfs_chk_size(alg_type);

	emuxfs_chk_init(&chk, alg_type);

	u64h = desc->eno;
	u64le = htole64(u64h);
	emuxfs_chk_update(&chk, (uint8_t *)&u64le, sizeof(uint64_t));
	u64h = desc->owner;
	u64le = htole64(u64h);
	emuxfs_chk_update(&chk, (uint8_t *)&u64le, sizeof(uint64_t));
	u64h = desc->group;
	u64le = htole64(u64h);
	emuxfs_chk_update(&chk, (uint8_t *)&u64le, sizeof(uint64_t));
	u64h = desc->mode;
	u64le = htole64(u64h);
	emuxfs_chk_update(&chk, (uint8_t *)&u64le, sizeof(uint64_t));
	if (desc->type == EMUXFS_DT_REG) {
		u64h = desc->size;
		u64le = htole64(u64h);
		emuxfs_chk_update(&chk, (uint8_t *)&u64le, sizeof(uint64_t));
	}

	emuxfs_chk_update(&chk, desc->content_checksum, chksz);
	emuxfs_chk_final(sum_out, &chk);
}

EMUXFS int
emuxfs_desc_init_from_stat(struct emuxfs_desc *desc_out, struct stat *st,
    uint64_t eno)
{
	emuxfs_desc_type desc_type;

	if (emuxfs_desc_type_from_mode(&desc_type, st->st_mode))
		return 1;

	*desc_out = (struct emuxfs_desc){
	    .eno = eno,
	    .type = desc_type,
	    .owner = st->st_uid,
	    .group = st->st_gid,
	    .mode = st->st_mode,
	    .size = st->st_size,
	};
	return 0;
}

EMUXFS int
emuxfs_desc_type_from_mode(emuxfs_desc_type *dt_out, mode_t mode)
{
	if (S_ISREG(mode))
		*dt_out = EMUXFS_DT_REG;
	else if (S_ISDIR(mode))
		*dt_out = EMUXFS_DT_DIR;
	else if (S_ISLNK(mode))
		*dt_out = EMUXFS_DT_LNK;
	else
		return 1;
	return 0;
}
