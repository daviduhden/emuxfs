/* desc.c */

/*
 * This file belongs to emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
 *
 * A description captures the checksum-relevant attributes of a node: its
 * external inode (eno), type, ownership, mode, size and content digest.  The
 * metadata checksum is computed from this description, while the content
 * checksum is computed from the node's bytes, symlink target, or directory
 * entry list.  Descriptions are transient; only their checksums are stored in
 * meta.db.
 */

// clang-format off
#include <sys/stat.h>
#include <sys/syslimits.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chk.h"
#include "ds.h"
#include "emuxfs.h"
// clang-format on

/*
 * Compute the content checksum of a regular file.  Files up to one block are
 * read into a single buffer; larger files use their on-disk checksum tree so
 * a full verification does not need to stream the entire file.  The function
 * fails closed on stat, open, seek or short-read errors.
 */
EMUXFS int
emuxfs_desc_chk_reg_content(
    struct emuxfs_desc *desc, dind dev_index, const char *path)
{
	EMUXFS_TRACE("enter");
	int			 rc;
	struct emuxfs_dev	*dev;
	struct stat		 st;
	size_t			 fsz;
	int			 fd;
	struct emuxfs_chk	 chk;
	uint8_t			 readbuf[EMUXFS_BLOCK_SIZE];
	enum emuxfs_chk_alg_type alg;

	rc = 1;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;
	alg = dev->conf.chk_alg_type;

	if (fstatat(dev->root_fd, path, &st, AT_SYMLINK_NOFOLLOW))
		return 1;
	if (st.st_size < 0)
		return 1;
	fsz = (size_t)st.st_size;

	if (fsz <= EMUXFS_BLOCK_SIZE) {
		if ((fd = openat(dev->root_fd, path, O_RDONLY | O_NOFOLLOW)) ==
		    -1)
			goto out;
		emuxfs_chk_init(&chk, alg);
		if (lseek(fd, 0, SEEK_SET) != 0)
			goto out2;
		if (read(fd, readbuf, fsz) != (ssize_t)fsz)
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
	    (size_t)st.st_size, nullptr);
}

/*
 * Compute a directory's content checksum from the sorted names and the
 * metadata checksum of each child, as assembled by emuxfs_dir_content_chk().
 * The directory is opened without following a final symlink, then handed to
 * the shared directory walker.
 */
static int
emuxfs_desc_chk_dir_content(
    struct emuxfs_desc *desc, dind dev_index, const char *path)
{
	EMUXFS_TRACE("enter");
	int		   rc, fd;
	struct emuxfs_dev *dev;
	struct emuxfs_dir  dir;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	if ((fd = openat(dev->root_fd, path, O_RDONLY | O_NOFOLLOW)) == -1)
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

/*
 * Hash a symlink's target text.  The target is not followed; readlinkat(2)
 * returns the stored bytes.  The buffer is zeroed before the call so the
 * target is NUL-terminated and no stale bytes remain beyond the returned
 * length.
 */
EMUXFS int
emuxfs_desc_chk_symlink_content(
    struct emuxfs_desc *desc, dind dev_index, const char *path)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev	*dev;
	int			 fd;
	enum emuxfs_chk_alg_type alg;
	ssize_t			 lnksz;
	char			 lnkbuf[PATH_MAX];

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;
	fd = dev->root_fd;
	alg = dev->conf.chk_alg_type;

	memset(lnkbuf, 0, PATH_MAX);
	if ((lnksz = readlinkat(fd, path, lnkbuf, PATH_MAX - 1)) == -1)
		return 1;

	emuxfs_desc_chk_provided_content(
	    desc, (uint8_t *)lnkbuf, (size_t)lnksz, alg);
	return 0;
}

/*
 * Hash caller-provided bytes into a description's content checksum field.
 * This is used for symlink targets and for in-memory content during create,
 * where no backing file exists yet.
 */
EMUXFS void
emuxfs_desc_chk_provided_content(struct emuxfs_desc *desc,
    const uint8_t *content, size_t contentsz, enum emuxfs_chk_alg_type alg_type)
{
	EMUXFS_TRACE("enter");
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
emuxfs_desc_chk_node_content(
    struct emuxfs_desc *desc, dind dev_index, const char *path)
{
	EMUXFS_TRACE("enter");
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

/*
 * Compute the metadata checksum from the description fields.  The fields are
 * hashed in their little-endian on-disk encoding, including the content
 * checksum, so the result is independent of host byte order.  Size is
 * included only for regular files; directory and symlink sizes are not
 * stable across filesystems.
 */
EMUXFS void
emuxfs_desc_chk_meta(uint8_t *sum_out, const struct emuxfs_desc *desc,
    enum emuxfs_chk_alg_type alg_type)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_chk chk;
	size_t		  chksz;
	uint64_t	  u64h, u64le;

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

/*
 * Initialize a description from stat(2) data.  The caller supplies the eno
 * because it is an emuxfs identity, not a field of the backing filesystem.
 * The content checksum is left zeroed for the caller to fill in.
 */
EMUXFS int
emuxfs_desc_init_from_stat(
    struct emuxfs_desc *desc_out, struct stat *st, uint64_t eno)
{
	EMUXFS_TRACE("enter");
	emuxfs_desc_type desc_type;

	if (emuxfs_desc_type_from_mode(&desc_type, st->st_mode))
		return 1;
	if (st->st_size < 0)
		return 1;

	*desc_out = (struct emuxfs_desc){
	    .eno = eno,
	    .type = desc_type,
	    .owner = st->st_uid,
	    .group = st->st_gid,
	    .mode = st->st_mode,
	    .size = (uint64_t)st->st_size,
	};
	return 0;
}

/*
 * Translate a POSIX file type into the three node types emuxfs supports.
 * Devices, sockets, FIFOs and other special nodes are rejected with 1 rather
 * than represented by a type the on-disk format cannot express.
 */
EMUXFS int
emuxfs_desc_type_from_mode(emuxfs_desc_type *dt_out, mode_t mode)
{
	EMUXFS_TRACE("enter");
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
