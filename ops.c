/* ops.c */
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

#include <sys/mman.h>
#include <sys/syslimits.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chk.h"
#include "ds.h"
#include "fault.h"
#include "emuxfs.h"
#include "ops.h"

static void emuxfs_wrbuf_flush(void);

static void
emuxfs_eids_set(void)
{
	EMUXFS_TRACE("enter");
	struct fuse_context *fc;

	fc = fuse_get_context();
	if (setegid(fc->gid))
		exit(-1);
	if (seteuid(fc->uid))
		exit(-1);
}

static void
emuxfs_eids_wrctx_set(const struct emuxfs_wrctx *wc)
{
	EMUXFS_TRACE("enter");
	if (setegid(wc->group))
		exit(-1);
	if (seteuid(wc->user))
		exit(-1);
}

static void
emuxfs_eids_reset(void)
{
	EMUXFS_TRACE("enter");
	if (seteuid(getuid()))
		exit(-1);
	if (setegid(getgid()))
		exit(-1);
}

static int
emuxfs_statfs(const char *path, struct statvfs *stvfs)
{
	EMUXFS_TRACE("enter");
	dind dev_count, i;
	struct emuxfs_dev *dev;
	int fd, err, subrc;
	struct statvfs st, st_agg;
	int has_st;
	size_t frsize, sz;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	has_st = 0;
	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;
	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		emuxfs_eids_set();
		fd = openat(dev->root_fd, path, O_RDONLY);
		err = errno;
		emuxfs_eids_reset();
		if (fd == -1)
			return -err;
		emuxfs_eids_set();
		subrc = fstatvfs(fd, &st);
		err = errno;
		emuxfs_eids_reset();
		if (subrc) {
			if (close(fd))
				exit(-1);
			return -err;
		}
		if (close(fd))
			exit(-1);
		frsize = st.f_frsize ? st.f_frsize : st.f_bsize;
		if (has_st) {
			/*
			 * A mirror must hold every node on every device, so
			 * the logical filesystem is limited by the most
			 * restrictive device.  Capacities and free counts
			 * therefore use the minimum; they are not summed, as
			 * they would be for striping.  f_bsize/f_frsize are
			 * fixed to the logical block size, and f_namemax must
			 * fit on every device, so it is also the minimum.
			 */
			sz = (frsize * st.f_blocks) / EMUXFS_BLOCK_SIZE;
			if (st_agg.f_blocks > sz)
				st_agg.f_blocks = sz;
			sz = (frsize * st.f_bfree) / EMUXFS_BLOCK_SIZE;
			if (st_agg.f_bfree > sz)
				st_agg.f_bfree = sz;
			sz = (frsize * st.f_bavail) / EMUXFS_BLOCK_SIZE;
			if (st_agg.f_bavail > sz)
				st_agg.f_bavail = sz;
			if (st_agg.f_files > st.f_files)
				st_agg.f_files = st.f_files;
			if (st_agg.f_ffree > st.f_ffree)
				st_agg.f_ffree = st.f_ffree;
			if (st_agg.f_favail > st.f_favail)
				st_agg.f_favail = st.f_favail;
			if (st_agg.f_namemax > st.f_namemax)
				st_agg.f_namemax = st.f_namemax;
		} else {
			st_agg = st;
			st_agg.f_bsize = EMUXFS_BLOCK_SIZE;
			st_agg.f_frsize = EMUXFS_BLOCK_SIZE;
			sz = (frsize * st.f_blocks) / EMUXFS_BLOCK_SIZE;
			st_agg.f_blocks = sz;
			sz = (frsize * st.f_bfree) / EMUXFS_BLOCK_SIZE;
			st_agg.f_bfree = sz;
			sz = (frsize * st.f_bavail) / EMUXFS_BLOCK_SIZE;
			st_agg.f_bavail = sz;
			st_agg.f_fsid = 0;
			st_agg.f_flag = 0;
			has_st = 1;
		}
	}
	if (has_st) {
		*stvfs = st_agg;
		return 0;
	}
	return -EIO;
}

static void *
emuxfs_fuse_init(struct fuse_conn_info *fci)
{
	EMUXFS_TRACE("enter");
	(void)fci;

	emuxfs_info("Mounted");
	return NULL;
}

static void
emuxfs_fuse_destroy(void *data)
{
	EMUXFS_TRACE("enter");
	(void)data;

	emuxfs_wrbuf_flush();
	emuxfs_info("Unmounting");
	if (emuxfs_final())
		exit(-1);
}

static int
emuxfs_fsync(const char *path, int datasync, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	/*
	 * A call to fsync(2) is made as part of the create, update, and delete
	 * operations.
	 */
	(void)path;
	(void)datasync;
	(void)ffi;

	emuxfs_wrbuf_flush();
	return 0;
}

static int
emuxfs_open(const char *path, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	dind dev_count, i;
	struct emuxfs_dev *dev;
	int fd, err, oflags;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	/*
	 * Creation is done by mknod(2), so open must not create.  Passing
	 * O_CREAT through would create a node that emuxfs does not track (no
	 * metadata) and would also be undefined behaviour, because openat(2)
	 * then requires a mode argument that FUSE does not supply.
	 */
	oflags = ffi->flags & ~(O_CREAT | O_EXCL);

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;
	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		emuxfs_eids_set();
		fd = openat(dev->root_fd, path, oflags);
		err = errno;
		emuxfs_eids_reset();
		if (fd == -1)
			return -err;
		if (close(fd))
			exit(-1);
		return 0;
	}
	return -EIO;
}

static int
emuxfs_opendir(const char *path, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	dind dev_count, i;
	struct emuxfs_dev *dev;
	int fd, err, oflags;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	/* Directories are created by mkdir(2); see emuxfs_open(). */
	oflags = (ffi->flags & ~(O_CREAT | O_EXCL)) | O_DIRECTORY;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;
	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		emuxfs_eids_set();
		fd = openat(dev->root_fd, path, oflags);
		err = errno;
		emuxfs_eids_reset();
		if (fd == -1)
			return -err;
		if (close(fd))
			exit(-1);
		return 0;
	}
	return -EIO;
}

static int
emuxfs_flush(const char *path, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	(void)path;
	(void)ffi;

	emuxfs_wrbuf_flush();
	return 0;
}

static int
emuxfs_release(const char *path, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	(void)path;
	(void)ffi;

	emuxfs_wrbuf_flush();
	return 0;
}

static int
emuxfs_releasedir(const char *path, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	(void)path;
	(void)ffi;

	return 0;
}

static int
emuxfs_lock(const char *path, struct fuse_file_info *ffi, int op,
    struct flock *flk)
{
	EMUXFS_TRACE("enter");
	(void)path;
	(void)ffi;
	(void)op;
	(void)flk;

	return -EOPNOTSUPP;
}

enum emuxfs_op_create_type {
	EMUXFS_CT_MKNOD,
	EMUXFS_CT_MKDIR,
	EMUXFS_CT_SYMLINK
};
struct emuxfs_op_create_args {
	enum emuxfs_op_create_type	 type;
	const char			*path;
	mode_t				 mode;
	dev_t				 sys_dev;
	const char			*link_content;
};

static int
emuxfs_op_create(struct emuxfs_op_create_args *args)
{
	EMUXFS_TRACE("enter");
	struct fuse_context *fc;

	dind dev_count, i;
	struct emuxfs_dev *dev;
	int fd;
	enum emuxfs_chk_alg_type alg;
	size_t chksz;

	gid_t parent_gid;

	int			 rc, err, subrc, subfd;
	int			 has_write;
	uint64_t		 eno;
	struct emuxfs_desc	 desc;
	struct emuxfs_chk	 content_chk;
	struct stat		 st;
	ino_t			 ino;
	struct emuxfs_meta	 meta;
	struct emuxfs_assign	 assign;

	struct emuxfs_cud	 cud;

	time_t			 now;

	mode_t			 old_umask;

	fc = fuse_get_context();

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = 0;
	if (emuxfs_state_eno_next_acquire(&eno))
		return -EIO;

	if (emuxfs_parent_gid(&parent_gid, args->path))
		return -EIO;
	EMUXFS_TRACE("op_create: parent_gid ok\n");

	now = time(NULL);

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		if (emuxfs_working_push(i))
			exit(-1);

		fd = dev->root_fd;
		alg = dev->conf.chk_alg_type;
		chksz = emuxfs_chk_size(alg);

		desc = (struct emuxfs_desc){
		    .eno = eno,
		    .owner = fc->uid,
		    .group = parent_gid,
		    .mode = args->mode & ~(fc->umask),
		    .size = 0,
		};
		if (emuxfs_desc_type_from_mode(&desc.type, args->mode)) {
			rc = -EOPNOTSUPP;
			goto early;
		}
		emuxfs_chk_init(&content_chk, alg);
		if (args->type == EMUXFS_CT_SYMLINK) {
			emuxfs_chk_update(&content_chk,
			    (uint8_t *)args->link_content,
			    strlen(args->link_content));
		}
		emuxfs_chk_final(desc.content_checksum, &content_chk);

		memcpy(&meta.checksums[chksz], desc.content_checksum, chksz);
		emuxfs_desc_chk_meta(&meta.checksums[0], &desc, alg);
		meta.header.eno = eno;
		meta.header.flags = MF_ASSIGNED;

		switch (args->type) {
		case EMUXFS_CT_MKNOD:
			if (S_ISREG(args->mode)) {
				emuxfs_eids_set();
				old_umask = umask(fc->umask);
				subfd = openat(fd, args->path,
				    O_RDWR|O_CREAT|O_EXCL, args->mode);
				err = errno;
				umask(old_umask);
				emuxfs_eids_reset();
				if (subfd != -1) {
					if (close(subfd))
						exit(-1);
					subrc = 0;
				} else
					subrc = -1;
			} else {
				rc = -EOPNOTSUPP;
				goto early;
			}
			break;
		case EMUXFS_CT_MKDIR:
			emuxfs_eids_set();
			old_umask = umask(fc->umask);
			subrc = mkdirat(fd, args->path, args->mode);
			err = errno;
			umask(old_umask);
			emuxfs_eids_reset();
			break;
		case EMUXFS_CT_SYMLINK:
			emuxfs_eids_set();
			old_umask = umask(fc->umask);
			subrc = symlinkat(args->link_content, fd, args->path);
			err = errno;
			umask(old_umask);
			emuxfs_eids_reset();
			break;
		default:
			exit(-1); /* Programming error. */
		}
		if (subrc) {
			if (!has_write) {
				rc = -err;
				goto early;
			}
			goto fail;
		}

		if (args->type != EMUXFS_CT_SYMLINK) {
			if ((subfd = openat(fd, args->path,
			    O_RDONLY|O_NOFOLLOW|O_CLOEXEC)) == -1)
				goto fail;
			if (fsync(subfd))
				exit(-1);
			if (close(subfd))
				exit(-1);
		}

		if (fstatat(fd, args->path, &st, AT_SYMLINK_NOFOLLOW))
			goto fail;
		ino = st.st_ino;
		assign = (struct emuxfs_assign){
		    .flags = AF_ASSIGNED,
		    .ino = ino
		};

		emuxfs_fault_point("create/before_meta");
		EMUXFS_TRACE("op_create: before meta\n");
		if (emuxfs_meta_write(&meta, i, ino))
			goto fail;
		emuxfs_fault_point("create/after_meta");
		EMUXFS_TRACE("op_create: after meta\n");
		if (emuxfs_assign_write(&assign, i, eno))
			goto fail;
		EMUXFS_TRACE("op_create: after assign\n");

		if (fsync(dev->meta_fd))
			exit(-1);
		if (fsync(dev->assign_fd))
			exit(-1);
		if (emuxfs_fsync_parent(fd, args->path))
			goto fail;

		if (emuxfs_readback(i, args->path, 0, &meta))
			goto fail;
		EMUXFS_TRACE("op_create: readback ok\n");

		cud.type = EMUXFS_CUD_CREATE;
		cud.path = args->path;
		/*
		 * The pre-image metadata is not used to recompute ancestors for
		 * a CREATE (the patch removes the entry by name), but keep it
		 * initialized so that no indeterminate value is ever observed.
		 */
		cud.pre_meta = meta;
		if (emuxfs_ancestors_meta_recompute(i, &cud))
			goto fail;

		has_write = 1;
		emuxfs_working_pop(i, now);
		continue;
fail:
		EMUXFS_TRACE("op_create: fail dev=%lu type=%d path=%s\n",
		    (unsigned long)i, (int)args->type, args->path);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
early:
		emuxfs_working_pop(i, now);
		return rc;
	}
	return has_write ? 0 : -EIO;
}

static int
emuxfs_mknod(const char *path, mode_t mode, dev_t sys_dev)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_create_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_CT_MKNOD;
	args.path = path;
	args.mode = mode;
	args.sys_dev = sys_dev;

	return emuxfs_op_create(&args);
}

static int
emuxfs_mkdir(const char *path, mode_t mode)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_create_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_CT_MKDIR;
	args.path = path;
	args.mode = mode;

	return emuxfs_op_create(&args);
}

static int
emuxfs_symlink(const char *link_content, const char *path)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_create_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_CT_SYMLINK;
	args.path = path;
	args.link_content = link_content;
	args.mode = S_IFLNK|0777;

	return emuxfs_op_create(&args);
}

enum emuxfs_op_delete_type {
	EMUXFS_DT_UNLINK,
	EMUXFS_DT_RMDIR
};

static int
emuxfs_op_delete(const char *path, enum emuxfs_op_delete_type type)
{
	EMUXFS_TRACE("enter");
	dind			 dev_count;
	dind			 i;
	struct emuxfs_dev	*dev;
	int			 fd;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;

	uint64_t return_eno;

	int			 rc, err, subrc;
	int			 has_write;
	const char		*stage;
	struct stat		 prewr_st;
	ino_t			 prewr_ino;
	struct emuxfs_meta	 prewr_meta;
	uint64_t		 prewr_eno;
	struct emuxfs_desc	 prewr_desc;
	uint8_t			 prewr_meta_chk_buf[EMUXFS_CHKSZ_MAX];
	struct emuxfs_meta	 wr_meta;
	struct emuxfs_assign	 wr_assign;
	char			 postwr_ppath[PATH_MAX];
	int			 postwr_pfd;
	struct stat		 postwr_st;
	struct emuxfs_cud	 cud;

	time_t			 now;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = 0;
	return_eno = UINT64_MAX;
	stage = "start";

	now = time(NULL);

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		if (emuxfs_working_push(i))
			exit(-1);

		fd = dev->root_fd;
		alg = dev->conf.chk_alg_type;
		chksz = emuxfs_chk_size(alg);

		stage = "fstatat";
		emuxfs_eids_set();
		subrc = fstatat(fd, path, &prewr_st, AT_SYMLINK_NOFOLLOW);
		err = errno;
		emuxfs_eids_reset();
		if (subrc) {
			if (has_write)
				goto fail;
			rc = -err;
			goto early;
		}
		prewr_ino = prewr_st.st_ino;

		/*
		 * emuxfs does not support hard links.  Deleting one name of a
		 * hard-linked file would clear the metadata shared by every
		 * other name, so refuse instead.  This is checked before any
		 * device is modified; if an earlier device was already
		 * modified the operation fails and is degraded.
		 */
		if ((type == EMUXFS_DT_UNLINK) &&
		    emuxfs_is_hardlink(&prewr_st)) {
			if (has_write)
				goto fail;
			emuxfs_warn("Unsupported hard link: %lu:/%s\n",
			    (dind)i, path);
			rc = -EOPNOTSUPP;
			goto early;
		}

		stage = "meta_read";
		if (emuxfs_meta_read(&prewr_meta, i, prewr_ino))
			goto fail;
		prewr_eno = prewr_meta.header.eno;
		stage = "desc_init";
		if (emuxfs_desc_init_from_stat(&prewr_desc, &prewr_st,
		    prewr_eno))
			goto fail;
		stage = "chk_node_content";
		if (emuxfs_desc_chk_node_content(&prewr_desc, i, path))
			goto fail;
		stage = "chk_meta";
		emuxfs_desc_chk_meta(prewr_meta_chk_buf, &prewr_desc, alg);
		if (bcmp(prewr_meta_chk_buf, &prewr_meta.checksums[0],
		    chksz) != 0)
			goto fail;

		switch (type) {
		case EMUXFS_DT_UNLINK:
			stage = "unlinkat";
			emuxfs_eids_set();
			subrc = unlinkat(fd, path, 0);
			err = errno;
			emuxfs_eids_reset();
			if (subrc) {
				if (has_write)
					goto fail;
				rc = -err;
				goto early;
			}
			/*
			 * Only after the name is gone may the large-file
			 * checksum tree be dropped.  Removing it for a file
			 * that unlink(2) refused to delete would destroy the
			 * checksums of a still-live file.
			 */
			if (prewr_st.st_size > EMUXFS_BLOCK_SIZE) {
				stage = "lfile_delete";
				if (emuxfs_lfile_delete(dev->lfile_fd,
				    prewr_ino))
					goto fail;
			}
			break;
		case EMUXFS_DT_RMDIR:
			stage = "rmdir_unlinkat";
			emuxfs_eids_set();
			subrc = unlinkat(fd, path, AT_REMOVEDIR);
			err = errno;
			emuxfs_eids_reset();
			break;
		default:
			exit(-1); /* Programming error. */
		}
		if (subrc) {
			if (has_write)
				goto fail;
			rc = -err;
			goto early;
		}

		memset(postwr_ppath, 0, PATH_MAX);
		strlcpy(postwr_ppath, path, PATH_MAX);
		if (emuxfs_path_pop(NULL, postwr_ppath, NULL)) {
			memset(postwr_ppath, 0, PATH_MAX);
			strlcpy(postwr_ppath, ".", PATH_MAX);
		}
		if ((postwr_pfd = openat(dev->root_fd, postwr_ppath,
		    O_RDONLY|O_NOFOLLOW|O_CLOEXEC)) == -1)
			goto fail;
		if (fsync(postwr_pfd))
			exit(-1);
		if (close(postwr_pfd))
			exit(-1);

		if (fstatat(fd, path, &postwr_st, AT_SYMLINK_NOFOLLOW) != -1)
			goto fail;
		if (errno != ENOENT)
			goto fail;

		emuxfs_fault_point("delete/after_unlink");
		memset(&wr_meta, 0, sizeof(wr_meta));
		memset(&wr_assign, 0, sizeof(wr_assign));
		if (emuxfs_meta_write(&wr_meta, i, prewr_ino))
			goto fail;
		emuxfs_fault_point("delete/after_meta");
		if (emuxfs_assign_write(&wr_assign, i, prewr_eno))
			goto fail;

		/*
		 * Make the cleared metadata and assignment durable before the
		 * working/seq commit below.  Otherwise a crash could leave the
		 * assign entry stale while the device looks clean.
		 */
		if (fsync(dev->meta_fd))
			exit(-1);
		if (fsync(dev->assign_fd))
			exit(-1);

		cud.type = EMUXFS_CUD_DELETE;
		cud.path = path;
		cud.pre_meta = prewr_meta;
		if (emuxfs_ancestors_meta_recompute(i, &cud))
			goto fail;

		if (!has_write) {
			has_write = 1;
			return_eno = prewr_eno;
		}

		emuxfs_working_pop(i, now);
		continue;
fail:
		EMUXFS_TRACE("op_delete: fail dev=%lu type=%d stage=%s path=%s\n",
		    (unsigned long)i, (int)type,
		    (stage != NULL) ? stage : "?", path);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
early:
		emuxfs_working_pop(i, now);
		return rc;
	}

	if (has_write) {
		if (emuxfs_state_eno_next_return(return_eno))
			exit(-1);
		return 0;
	}
	return -EIO;
}

static int
emuxfs_unlink(const char *path)
{
	EMUXFS_TRACE("enter");
	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	return emuxfs_op_delete(path, EMUXFS_DT_UNLINK);
}

static int
emuxfs_rmdir(const char *path)
{
	EMUXFS_TRACE("enter");
	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	return emuxfs_op_delete(path, EMUXFS_DT_RMDIR);
}

enum emuxfs_op_read_type {
	EMUXFS_RT_GETATTR,
	EMUXFS_RT_READ,
	EMUXFS_RT_READLINK,
	EMUXFS_RT_READDIR
};

struct emuxfs_op_read_args {
	enum emuxfs_op_read_type type;
	const char		*path;
	struct stat		*st_out;
	char			*buf;
	size_t			 bufsz;
	off_t			 offset;
	struct fuse_file_info	*ffi;
	char			*buf_out;
	size_t			 size;
	struct statvfs		*stvfs;
	void			*fill_data;
	fuse_fill_dir_t		 fill;
};

static int
emuxfs_getattr_inner(struct stat *st_out, struct stat *st, uint64_t eno,
    int *err)
{
	EMUXFS_TRACE("enter");
	size_t sz;

	(void)err;

	st->st_ino = eno;
	sz = (size_t)st->st_size;
	st->st_blocks = (sz / EMUXFS_BLOCK_SIZE) +
	    ((sz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	st->st_blksize = EMUXFS_BLOCK_SIZE;
	*st_out = *st;

	return 0;
}

static int
emuxfs_read_inner(int root_fd, struct emuxfs_op_read_args *args,
    struct stat *st,
    enum emuxfs_chk_alg_type alg, size_t chksz, struct emuxfs_meta *meta,
    ssize_t *rdsz_out, int *err, int lfile_fd)
{
	EMUXFS_TRACE("enter");
	int fd, rc;
	size_t fsz;
	uint8_t buf[EMUXFS_BLOCK_SIZE];
	struct emuxfs_chk content_chk;
	uint8_t content_sum[EMUXFS_CHKSZ_MAX];
	size_t rdsz;
	struct emuxfs_range r;
	size_t i_offset, out_offset, out_size, buf_offset;
	uint64_t i;
	int lfd;
	uint8_t *lfile;

	rc = EMUXFS_EINT;

	emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDONLY);
	*err = errno;
	emuxfs_eids_reset();
	if (fd == -1) {
		rc = EMUXFS_EFS;
		goto out;
	}

	fsz = (size_t)st->st_size;
	if ((size_t)args->offset >= fsz) {
		*rdsz_out = 0;
		rc = 0;
		goto out2;
	}

	if (fsz <= EMUXFS_BLOCK_SIZE) {
		if (read(fd, buf, fsz) != (ssize_t)fsz) {
			rc = EMUXFS_EFS;
			goto out2;
		}
		emuxfs_chk_init(&content_chk, alg);
		emuxfs_chk_update(&content_chk, buf, fsz);
		emuxfs_chk_final(content_sum, &content_chk);
		if (bcmp(content_sum, &meta->checksums[chksz], chksz) != 0) {
			rc = EMUXFS_ECHK;
			goto out2;
		}
		rdsz = fsz - (size_t)args->offset;
		if (rdsz > args->size)
			rdsz = args->size;
		memcpy(args->buf_out, &buf[args->offset], rdsz);

		*rdsz_out = (ssize_t)rdsz;
		rc = 0;
		goto out2;
	}

	r.byte_begin = (size_t)args->offset;
	r.byte_end = (size_t)args->offset + args->size;
	if (r.byte_end > fsz)
		r.byte_end = fsz;
	emuxfs_range_compute(&r, chksz);
	out_offset = 0;

	if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDONLY))
		goto out2;
	if ((lfile = mmap(NULL, r.lfilesz, PROT_READ, MAP_SHARED, lfd,
	    (off_t)r.lfileoff)) == MAP_FAILED)
		goto out3;

	for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
		i_offset = i * EMUXFS_BLOCK_SIZE;
		rdsz = EMUXFS_BLOCK_SIZE;
		if (i_offset + rdsz > fsz)
			rdsz = fsz - i_offset;
		if (pread(fd, buf, rdsz, (off_t)i_offset) != (ssize_t)rdsz) {
			rc = EMUXFS_EFS;
			goto out4;
		}
		emuxfs_chk_init(&content_chk, alg);
		emuxfs_chk_update(&content_chk, buf, rdsz);
		emuxfs_chk_final(content_sum, &content_chk);
		if (bcmp(content_sum, &lfile[chksz * (i - r.blk_index_begin)],
		    chksz) != 0) {
			rc = EMUXFS_ECHK;
			goto out4;
		}
		buf_offset = 0;
		out_size = rdsz;
		if (i_offset < r.byte_begin) {
			buf_offset = (r.byte_begin - i_offset);
			out_size -= buf_offset;
		}
		memcpy(&args->buf_out[out_offset], &buf[buf_offset], out_size);
		out_offset += out_size;
	}

	*rdsz_out = (ssize_t)out_offset;
	rc = 0;
out4:
	if (r.lfilesz == 0)
		exit(-1); /* Programming error. */
	if (munmap(lfile, r.lfilesz))
		exit(-1);
out3:
	if (close(lfd))
		exit(-1);
out2:
	if (close(fd))
		exit(-1);
out:
	return rc;
}

static int
emuxfs_readlink_inner(int root_fd, struct emuxfs_op_read_args *args,
    enum emuxfs_chk_alg_type alg, size_t chksz, const struct emuxfs_desc *desc,
    const struct emuxfs_meta *meta, int *err)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_desc lnk_desc;
	ssize_t lnksz, rdsz;
	char lnkbuf[PATH_MAX];
	uint8_t lnk_meta_sum[EMUXFS_CHKSZ_MAX];

	memset(lnkbuf, 0, PATH_MAX);
	emuxfs_eids_set();
	lnksz = readlinkat(root_fd, args->path, lnkbuf, PATH_MAX - 1);
	*err = errno;
	emuxfs_eids_reset();
	if (lnksz == -1)
		return EMUXFS_EFS;
	if (lnksz >= PATH_MAX)
		return EMUXFS_EFS;

	lnk_desc = *desc;
	memset(lnk_desc.content_checksum, 0, EMUXFS_CHKSZ_MAX);
	emuxfs_desc_chk_provided_content(&lnk_desc, (uint8_t *)lnkbuf,
	    (size_t)lnksz, alg);
	emuxfs_desc_chk_meta(lnk_meta_sum, &lnk_desc, alg);
	if (bcmp(lnk_meta_sum, &meta->checksums[0], chksz) != 0)
		return EMUXFS_ECHK;

	if (args->size == 0)
		return EMUXFS_EFS;
	rdsz = ((size_t)lnksz < (args->size - 1)) ? lnksz :
	    (ssize_t)(args->size - 1);
	memcpy(args->buf_out, lnkbuf, (size_t)rdsz);
	args->buf_out[rdsz] = '\0';

	return 0;
}

static int
emuxfs_readdir_inner(dind dev_index, int root_fd,
    struct emuxfs_op_read_args *args, enum emuxfs_chk_alg_type alg,
    size_t chksz,
    struct stat *st, struct emuxfs_desc *desc, struct emuxfs_meta *meta,
    int *err)
{
	EMUXFS_TRACE("enter");
	int rc, fd;
	uint8_t content_sum[EMUXFS_CHKSZ_MAX];
	struct emuxfs_dir dir;
	struct dirent *dirent;
	size_t i;
	const char *dname;
	size_t dnamelen;

	(void)alg;
	(void)st;
	(void)desc;

	rc = EMUXFS_EINT;

	emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
	*err = errno;
	emuxfs_eids_reset();
	if (fd == -1) {
		rc = EMUXFS_EFS;
		goto out;
	}
	if (close(fd))
		exit(-1);

	if (emuxfs_pushdir(&dir, root_fd, args->path))
		exit(-1);

	if (emuxfs_dir_content_chk(content_sum, dev_index, &dir)) {
		rc = EMUXFS_ECHK;
		goto out2;
	}
	if (bcmp(content_sum, &meta->checksums[chksz], chksz) != 0) {
		rc = EMUXFS_ECHK;
		goto out2;
	}
	for (i = 0; i < dir.ent_count; ++i) {
		dirent = dir.ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) == 0))
			continue;
		args->fill(args->fill_data, dname, NULL, 0);
	}

	rc = 0;
out2:
	if (emuxfs_popdir(&dir))
		exit(-1);
out:
	return rc;
}

static int
emuxfs_op_read(struct emuxfs_op_read_args *args)
{
	EMUXFS_TRACE("enter");
	dind dev_count, i;
	struct emuxfs_dev *dev;
	int fd;
	enum emuxfs_chk_alg_type alg;
	size_t chksz;

	int err, rc, subrc;
	struct stat st;
	ino_t ino;
	struct emuxfs_meta meta;
	uint64_t eno;
	struct emuxfs_desc desc;

	uint8_t chk_buf[EMUXFS_CHKSZ_MAX];
	ssize_t rdsz;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		fd = dev->root_fd;
		alg = dev->conf.chk_alg_type;
		chksz = emuxfs_chk_size(alg);
		rdsz = -1;

		emuxfs_eids_set();
		subrc = fstatat(fd, args->path, &st, AT_SYMLINK_NOFOLLOW);
		err = errno;
		emuxfs_eids_reset();
		if (subrc) {
			if ((err == ENOENT) && emuxfs_parent_readback(i,
			    args->path))
				goto fail;
			rc = -err;
			goto early;
		}
		ino = st.st_ino;
		if (emuxfs_meta_read(&meta, i, ino))
			goto fail;
		eno = meta.header.eno;
		if (emuxfs_desc_init_from_stat(&desc, &st, eno)) {
			rc = -EOPNOTSUPP;
			goto early;
		}
		memcpy(desc.content_checksum, &meta.checksums[chksz], chksz);
		emuxfs_desc_chk_meta(chk_buf, &desc, alg);
		if (bcmp(chk_buf, &meta.checksums[0], chksz) != 0)
			goto fail;

		switch (args->type) {
		case EMUXFS_RT_GETATTR:
			subrc = emuxfs_getattr_inner(args->st_out, &st, eno,
			    &err);
			break;
		case EMUXFS_RT_READ:
			subrc = emuxfs_read_inner(fd, args, &st, alg, chksz,
			    &meta, &rdsz, &err, dev->lfile_fd);
			break;
		case EMUXFS_RT_READLINK:
			subrc = emuxfs_readlink_inner(fd, args, alg, chksz,
			    &desc, &meta, &err);
			break;
		case EMUXFS_RT_READDIR:
			subrc = emuxfs_readdir_inner(i, fd, args, alg, chksz,
			    &st, &desc, &meta, &err);
			break;
		default:
			exit(-1); /* Programming error. */
		}
		switch (subrc) {
		case 0:
			break;
		case EMUXFS_EINT:
			exit(-1); /* Unrecoverable runtime error. */
		case EMUXFS_EFS:
			rc = -err;
			goto early;
		case EMUXFS_ECHK:
			goto fail;
		default:
			exit(-1); /* Programming error. */
		}

		emuxfs_restore_now();
		if (rdsz > -1)
			return (int)rdsz;
		return 0;
fail:
		if (emuxfs_state_restore_push_back(i, args->path))
			exit(-1);
		continue;
early:
		emuxfs_restore_now();
		return rc;
	}
	return -EIO;
}

static int
emuxfs_getattr(const char *path, struct stat *st_out)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_read_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_RT_GETATTR;
	args.path = path;
	args.st_out = st_out;

	return emuxfs_op_read(&args);
}

static int
emuxfs_read(const char *path, char *buf_out, size_t size, off_t offset,
    struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_read_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_RT_READ;
	args.path = path;
	args.buf_out = buf_out;
	args.size = size;
	args.offset = offset;
	args.ffi = ffi;

	return emuxfs_op_read(&args);
}

static int
emuxfs_readlink(const char *path, char *buf_out, size_t size)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_read_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_RT_READLINK;
	args.path = path;
	args.buf_out = buf_out;
	args.size = size;

	return emuxfs_op_read(&args);
}

static int
emuxfs_readdir(const char *path, void *fill_data, fuse_fill_dir_t fill,
    off_t offset, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_read_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_RT_READDIR;
	args.path = path;
	args.fill_data = fill_data;
	args.fill = fill;
	args.offset = offset;
	args.ffi = ffi;

	return emuxfs_op_read(&args);
}

enum emuxfs_op_update_type {
	EMUXFS_UT_CHMOD,
	EMUXFS_UT_CHOWN,
	EMUXFS_UT_UTIMENS,
	EMUXFS_UT_TRUNCATE,
	EMUXFS_UT_WRITE
};

struct emuxfs_op_update_args {
	enum emuxfs_op_update_type	 type;
	const char			*path;
	mode_t				 mode;
	uid_t				 uid;
	gid_t				 gid;
	const struct timespec		*ts;
	off_t				 offset;
	const char			*buf;
	size_t				 bufsz;
	const struct emuxfs_wrctx	*wc;
};

static int
emuxfs_truncate_inner(int root_fd, struct emuxfs_op_update_args *args,
    enum emuxfs_chk_alg_type alg, size_t chksz, struct stat *st,
    struct emuxfs_meta *prewr_meta, struct emuxfs_meta *wr_meta,
    struct emuxfs_desc *wr_desc, int *err, int lfile_fd)
{
	EMUXFS_TRACE("enter");
	int rc;
	int fd;
	uint8_t content_buf[EMUXFS_BLOCK_SIZE];
	size_t prewr_sz, smaller_sz, larger_sz;

	struct emuxfs_chk	prewr_content_chk;
	uint8_t			prewr_content_sum[EMUXFS_CHKSZ_MAX];
	struct emuxfs_chk	wr_content_chk;

	struct emuxfs_range r;
	size_t rdsz, i_offset, off, beginsz, padsz;
	size_t newoff;
	uint64_t i;
	int lfd;
	uint8_t *lfile;
	int szcase;

	const size_t blksz = EMUXFS_BLOCK_SIZE;

	rc = EMUXFS_EINT;
	fd = -1;
	lfd = -1;
	lfile = MAP_FAILED;
	r.lfilesz = 0;

	emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDWR|O_NOFOLLOW);
	*err = errno;
	emuxfs_eids_reset();
	if (fd == -1) {
		rc = EMUXFS_EFS;
		goto out;
	}
	if (args->offset < 0) {
		rc = EMUXFS_EFS;
		goto out;
	}
	newoff = (size_t)args->offset;

	if (st->st_size < 0) {
		rc = EMUXFS_EFS;
		goto out;
	}
	larger_sz = smaller_sz = prewr_sz = (size_t)st->st_size;
	if (newoff > larger_sz)
		larger_sz = newoff;
	if (newoff < smaller_sz)
		smaller_sz = newoff;

	if (newoff == prewr_sz) {
		rc = 0;
		goto out;
	}

	if (prewr_sz <= blksz) {
		memset(content_buf, 0, blksz);
		if (pread(fd, content_buf, prewr_sz, 0) != (ssize_t)prewr_sz) {
			rc = EMUXFS_EFS;
			goto out;
		}
		emuxfs_chk_init(&prewr_content_chk, alg);
		emuxfs_chk_update(&prewr_content_chk, content_buf, prewr_sz);
		emuxfs_chk_final(prewr_content_sum, &prewr_content_chk);
		if (bcmp(prewr_content_sum, &prewr_meta->checksums[chksz],
		    chksz) != 0) {
			rc = EMUXFS_ECHK;
			goto out;
		}
	} else {
		if (newoff <= prewr_sz) {
			r.byte_begin = newoff;
			r.byte_end = prewr_sz;
		} else {
			r.byte_begin = r.byte_end = prewr_sz;
			if (r.byte_begin > 1)
				--r.byte_begin;
		}
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDONLY))
			goto out;
		if ((lfile = mmap(NULL, r.lfilesz, PROT_READ, MAP_SHARED, lfd,
		    (off_t)r.lfileoff)) == MAP_FAILED)
			goto out;

		for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
			i_offset = i * blksz;
			rdsz = blksz;
			if (i_offset + rdsz > prewr_sz)
				rdsz = prewr_sz - i_offset;
			if (pread(fd, content_buf, rdsz, (off_t)i_offset) !=
			    (ssize_t)rdsz) {
				rc = EMUXFS_EFS;
				goto out;
			}
			emuxfs_chk_init(&prewr_content_chk, alg);
			emuxfs_chk_update(&prewr_content_chk, content_buf,
			    rdsz);
			emuxfs_chk_final(prewr_content_sum, &prewr_content_chk);
			if (bcmp(prewr_content_sum, &lfile[chksz * (i -
			    r.blk_index_begin)], chksz) != 0) {
				rc = EMUXFS_ECHK;
				goto out;
			}
		}

		if (munmap(lfile, r.lfilesz))
			exit(-1);
		lfile = MAP_FAILED;
		r.lfilesz = 0;
		if (close(lfd))
			exit(-1);
		lfd = -1;
	}

	szcase = (prewr_sz > blksz) ? 1 : 0;
	szcase += (newoff > blksz) ? 2 : 0;
	switch (szcase) {
	case 1:
		if (emuxfs_lfile_delete(lfile_fd, st->st_ino))
			goto out;
		memset(content_buf, 0, blksz);
		if (pread(fd, content_buf, newoff, 0) != (ssize_t)newoff) {
			rc = EMUXFS_EFS;
			goto out;
		}
		/* FALLTHROUGH */
	case 0:
		emuxfs_chk_init(&wr_content_chk, alg);
		emuxfs_chk_update(&wr_content_chk, content_buf, newoff);
		emuxfs_chk_final(wr_desc->content_checksum, &wr_content_chk);
		memcpy(&wr_meta->checksums[chksz], wr_desc->content_checksum,
		    chksz);
		break;
	case 2:
		if (emuxfs_lfile_create(lfile_fd, chksz, st->st_ino,
		    newoff))
			goto out;
		r.byte_begin = 0;
		r.byte_end = newoff;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_WRONLY))
			goto out;
		if ((lfile = mmap(NULL, r.lfilesz, PROT_WRITE, MAP_SHARED, lfd,
		    (off_t)r.lfileoff)) == MAP_FAILED)
			goto out;

		for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
			i_offset = i * blksz;
			rdsz = blksz;
			if (i_offset + rdsz > newoff)
				rdsz = newoff - i_offset;
			if (pread(fd, content_buf, rdsz, (off_t)i_offset) !=
			    (ssize_t)rdsz) {
				rc = EMUXFS_EFS;
				goto out;
			}
			emuxfs_chk_init(&wr_content_chk, alg);
			emuxfs_chk_update(&wr_content_chk, content_buf, rdsz);
			emuxfs_chk_final(&lfile[chksz * i], &wr_content_chk);
		}

		if (munmap(lfile, r.lfilesz))
			exit(-1);
		lfile = MAP_FAILED;
		r.lfilesz = 0;
		if (close(lfd))
			exit(-1);
		lfd = -1;

		if (emuxfs_lfile_ancestors_recompute(wr_desc->content_checksum,
		    lfile_fd, alg, st->st_ino, newoff, r.blk_index_begin,
		    r.blk_index_end))
			goto out;
		break;
	case 3:
		if (emuxfs_lfile_resize(lfile_fd, chksz, st->st_ino,
		    prewr_sz, newoff))
			goto out;
		if (newoff < prewr_sz) {
			r.byte_begin = r.byte_end = newoff;
			if (r.byte_begin > 0)
				--r.byte_begin;
			emuxfs_range_compute(&r, chksz);

			if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino,
			    O_WRONLY))
				goto out;
			if ((lfile = mmap(NULL, r.lfilesz, PROT_WRITE,
			    MAP_SHARED, lfd, (off_t)r.lfileoff)) == MAP_FAILED)
				goto out;

			rdsz = newoff - r.blk_begin;
			if (pread(fd, content_buf, rdsz, (off_t)r.blk_begin) !=
			    (ssize_t)rdsz) {
				rc = EMUXFS_EFS;
				goto out;
			}

			emuxfs_chk_init(&wr_content_chk, alg);
			emuxfs_chk_update(&wr_content_chk, content_buf, rdsz);
			emuxfs_chk_final(&lfile[0], &wr_content_chk);

			if (munmap(lfile, r.lfilesz))
				exit(-1);
			lfile = MAP_FAILED;
			r.lfilesz = 0;
			if (close(lfd))
				exit(-1);
			lfd = -1;

			if (emuxfs_lfile_ancestors_recompute(wr_desc
			    ->content_checksum, lfile_fd, alg, st->st_ino,
			    newoff, r.blk_index_begin, r.blk_index_end))
				goto out;
			break;
		}
		r.byte_begin = prewr_sz;
		r.byte_end = newoff;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_WRONLY))
			goto out;
		if ((lfile = mmap(NULL, r.lfilesz, PROT_WRITE, MAP_SHARED, lfd,
		    (off_t)r.lfileoff)) == MAP_FAILED)
			goto out;

		for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
			i_offset = i * blksz;
			off = 0;
			if (i_offset < prewr_sz) {
				beginsz = prewr_sz - i_offset;
				if (beginsz > blksz)
					beginsz = blksz;
				if (pread(fd, content_buf, beginsz,
				    (off_t)i_offset) != (ssize_t)beginsz) {
					rc = EMUXFS_EFS;
					goto out;
				}
				off += beginsz;
			}
			if (off < blksz) {
				padsz = newoff - (i_offset + off);
				if (off + padsz > blksz)
					padsz = blksz - off;
				memset(&content_buf[off], 0, padsz);
				off += padsz;
			}
			emuxfs_chk_init(&wr_content_chk, alg);
			emuxfs_chk_update(&wr_content_chk, content_buf, off);
			emuxfs_chk_final(&lfile[chksz * (i -
			    r.blk_index_begin)], &wr_content_chk);
		}

		if (munmap(lfile, r.lfilesz))
			exit(-1);
		lfile = MAP_FAILED;
		r.lfilesz = 0;
		if (close(lfd))
			exit(-1);
		lfd = -1;

		if (emuxfs_lfile_ancestors_recompute(wr_desc->content_checksum,
		    lfile_fd, alg, st->st_ino, newoff, r.blk_index_begin,
		    r.blk_index_end))
			goto out;
		break;
	default:
		exit(-1); /* Unreachable. */
	}

	if (ftruncate(fd, (off_t)newoff)) {
		rc = EMUXFS_EFS;
		goto out;
	}

	wr_desc->size = newoff;

	rc = 0;
out:
	if (fd != -1) {
		if (close(fd))
			exit(-1);
	}
	return rc;
}

static int
emuxfs_write_inner(int root_fd, struct emuxfs_op_update_args *args,
    enum emuxfs_chk_alg_type alg, size_t chksz, struct stat *st,
    struct emuxfs_meta *prewr_meta, struct emuxfs_meta *wr_meta,
    struct emuxfs_desc *wr_desc, int *err, int lfile_fd,
    const struct emuxfs_wrctx *wc)
{
	EMUXFS_TRACE("enter");
	int rc;
	int fd;
	uint8_t content_buf[EMUXFS_BLOCK_SIZE];
	size_t prewr_sz, wrub, largest_sz;

	struct emuxfs_chk	prewr_content_chk;
	uint8_t			prewr_content_sum[EMUXFS_CHKSZ_MAX];
	struct emuxfs_chk	wr_content_chk;

	struct emuxfs_range r;
	size_t rdsz, i_offset, wroff, off, beginsz, padsz, wrsz, endsz;
	size_t newoff;
	uint64_t i;
	int lfd;
	uint8_t *lfile;

	const size_t blksz = EMUXFS_BLOCK_SIZE;

	rc = EMUXFS_EINT;
	fd = -1;
	lfd = -1;
	lfile = MAP_FAILED;
	r.lfilesz = 0;

	if (wc != NULL)
		emuxfs_eids_wrctx_set(wc);
	else
		emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDWR|O_NOFOLLOW);
	*err = errno;
	emuxfs_eids_reset();
	if (fd == -1) {
		rc = EMUXFS_EFS;
		goto out;
	}
	if (args->offset < 0) {
		rc = EMUXFS_EFS;
		goto out;
	}
	newoff = (size_t)args->offset;

	if (st->st_size < 0) {
		rc = EMUXFS_EFS;
		goto out;
	}
	largest_sz = prewr_sz = (size_t)st->st_size;
	wrub = (newoff + args->bufsz);
	if (wrub > largest_sz)
		largest_sz = wrub;

	if (prewr_sz <= blksz) {
		/*
		 * This memset is necessary since 'content_buf' is used at the
		 * write stage and the write may begin beyond the end of the
		 * current file size, in which case the file content will be
		 * padded with zeroes and this must be accounted for when
		 * computing the checksum.
		 */
		memset(content_buf, 0, blksz);

		if (read(fd, content_buf, prewr_sz) != (ssize_t)prewr_sz) {
			rc = EMUXFS_EFS;
			goto out;
		}
		emuxfs_chk_init(&prewr_content_chk, alg);
		emuxfs_chk_update(&prewr_content_chk, content_buf, prewr_sz);
		emuxfs_chk_final(prewr_content_sum, &prewr_content_chk);
		if (bcmp(prewr_content_sum, &prewr_meta->checksums[chksz],
		    chksz) != 0) {
			rc = EMUXFS_ECHK;
			goto out;
		}
	} else if (newoff < prewr_sz) {
		r.byte_begin = newoff;
		r.byte_end = newoff + args->bufsz;
		if (r.byte_end > prewr_sz)
			r.byte_end = prewr_sz;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDONLY))
			goto out;
		if ((lfile = mmap(NULL, r.lfilesz, PROT_READ, MAP_SHARED, lfd,
		    (off_t)r.lfileoff)) == MAP_FAILED)
			goto out;

		for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
			i_offset = i * blksz;
			rdsz = blksz;
			if (i_offset + rdsz > prewr_sz)
				rdsz = prewr_sz - i_offset;
			if (pread(fd, content_buf, rdsz, (off_t)i_offset) !=
			    (ssize_t)rdsz) {
				rc = EMUXFS_EFS;
				goto out;
			}
			emuxfs_chk_init(&prewr_content_chk, alg);
			emuxfs_chk_update(&prewr_content_chk, content_buf,
			    rdsz);
			emuxfs_chk_final(prewr_content_sum, &prewr_content_chk);
			if (bcmp(prewr_content_sum, &lfile[chksz * (i -
			    r.blk_index_begin)], chksz) != 0) {
				rc = EMUXFS_ECHK;
				goto out;
			}
		}

		if (munmap(lfile, r.lfilesz))
			exit(-1);
		lfile = MAP_FAILED;
		r.lfilesz = 0;
		if (close(lfd))
			exit(-1);
		lfd = -1;
	}

	if ((prewr_sz <= blksz) && (wrub > blksz)) {
		if (emuxfs_lfile_create(lfile_fd, chksz, st->st_ino, wrub))
			goto out;
	}

	if (largest_sz <= blksz) {
		memcpy(&content_buf[newoff], args->buf, args->bufsz);
		if (pwrite(fd, args->buf, args->bufsz, (off_t)newoff) !=
		    (ssize_t)args->bufsz) {
			rc = EMUXFS_EFS;
			goto out;
		}

		emuxfs_chk_init(&wr_content_chk, alg);
		emuxfs_chk_update(&wr_content_chk, content_buf, largest_sz);
		emuxfs_chk_final(wr_desc->content_checksum, &wr_content_chk);
		memcpy(&wr_meta->checksums[chksz], wr_desc->content_checksum,
		    chksz);
	} else {
		if ((prewr_sz > blksz) && (wrub > prewr_sz)) {
			if (emuxfs_lfile_resize(lfile_fd, chksz, st->st_ino,
			    prewr_sz, wrub))
				goto out;
		}

		r.byte_begin = newoff;
		if (prewr_sz < r.byte_begin)
			r.byte_begin = prewr_sz;
		r.byte_end = newoff + args->bufsz;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_WRONLY))
			goto out;
		if ((lfile = mmap(NULL, r.lfilesz, PROT_WRITE, MAP_SHARED, lfd,
		    (off_t)r.lfileoff)) == MAP_FAILED)
			goto out;

		wroff = 0;

		for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
			i_offset = i * blksz;
			off = 0;
			if (i_offset < newoff) {
				if (i_offset < prewr_sz) {
					/*
					 * Copy only the bytes that precede
					 * the write offset: everything from
					 * newoff on is supplied by args->buf
					 * below.  Reading to prewr_sz here
					 * would leave the old bytes in the
					 * block that contains newoff and
					 * would also make the copy below
					 * underflow when the write ends
					 * inside that block.
					 */
					beginsz = (prewr_sz < newoff) ?
					    prewr_sz : newoff;
					beginsz -= i_offset;
					if (beginsz > blksz)
						beginsz = blksz;
					if (pread(fd, content_buf, beginsz,
					    (off_t)i_offset) !=
					    (ssize_t)beginsz) {
						rc = EMUXFS_EFS;
						goto out;
					}
					off += beginsz;
				}
				if ((off < blksz) && (prewr_sz <
				    newoff)) {
					padsz = newoff - (i_offset + off);
					if (off + padsz > blksz)
						padsz = blksz - off;
					memset(&content_buf[off], 0, padsz);
					off += padsz;
				}
			}
			if (off < blksz) {
				wrsz = wrub - (i_offset + off);
				if (off + wrsz > blksz)
					wrsz = blksz - off;
				memcpy(&content_buf[off], &args->buf[wroff],
				    wrsz);
				wroff += wrsz;
				off += wrsz;
			}
			if (off < blksz) {
				endsz = largest_sz - (i_offset + off);
				if (off + endsz > blksz)
					endsz = blksz - off;
				if (pread(fd, &content_buf[off], endsz,
				    (off_t)(i_offset + off)) !=
				    (ssize_t)endsz) {
					rc = EMUXFS_EFS;
					goto out;
				}
				off += endsz;
			}

			if (pwrite(fd, content_buf, off, (off_t)i_offset) !=
			    (ssize_t)off) {
				rc = EMUXFS_EFS;
				goto out;
			}

			emuxfs_chk_init(&wr_content_chk, alg);
			emuxfs_chk_update(&wr_content_chk, content_buf, off);
			emuxfs_chk_final(&lfile[chksz * (i -
			    r.blk_index_begin)], &wr_content_chk);
		}

		if (munmap(lfile, r.lfilesz))
			exit(-1);
		lfile = MAP_FAILED;
		r.lfilesz = 0;
		if (fsync(lfd))
			exit(-1);
		if (close(lfd))
			exit(-1);
		lfd = -1;

		if (emuxfs_lfile_ancestors_recompute(wr_desc->content_checksum,
		    lfile_fd, alg, st->st_ino, largest_sz, r.blk_index_begin,
		    r.blk_index_end))
			goto out;
		memcpy(&wr_meta->checksums[chksz], wr_desc->content_checksum,
		    chksz);
	}

	wr_desc->size = largest_sz;

	rc = 0;
out:
	if (lfile != MAP_FAILED) {
		if (r.lfilesz == 0)
			exit(-1); /* Programming error. */
		if (munmap(lfile, r.lfilesz))
			exit(-1);
	} else if (r.lfilesz != 0)
		exit(-1); /* Programming error. */
	if (lfd != -1) {
		if (close(lfd))
			exit(-1);
	}
	if (fd != -1) {
		if (close(fd))
			exit(-1);
	}
	return rc;
}

static int
emuxfs_op_update(struct emuxfs_op_update_args *args)
{
	EMUXFS_TRACE("enter");
	dind			 dev_count;
	dind			 i;
	struct emuxfs_dev	*dev;
	int			 fd;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;

	int			 rc, err, subrc, subfd;
	int			 has_write;
	struct stat		 prewr_st;
	ino_t			 prewr_ino;
	struct emuxfs_meta	 prewr_meta;
	uint64_t		 prewr_eno;
	struct emuxfs_desc	 prewr_desc;
	uint8_t			 prewr_meta_chk_buf[EMUXFS_CHKSZ_MAX];
	struct emuxfs_desc	 wr_desc;
	struct emuxfs_meta	 wr_meta;

	size_t			 mod_begin, mod_end, mod_size;

	struct emuxfs_cud	 cud;

	time_t			 now;

	const struct emuxfs_wrctx	*wc;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = 0;

	now = time(NULL);

	wc = args->wc;

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		if (emuxfs_working_push(i))
			exit(-1);

		fd = dev->root_fd;
		alg = dev->conf.chk_alg_type;
		chksz = emuxfs_chk_size(alg);

		if (wc != NULL)
			emuxfs_eids_wrctx_set(wc);
		else
			emuxfs_eids_set();
		subrc = fstatat(fd, args->path, &prewr_st, AT_SYMLINK_NOFOLLOW);
		err = errno;
		emuxfs_eids_reset();
		if (subrc) {
			if (has_write)
				goto fail;
			rc = -err;
			goto early;
		}
		prewr_ino = prewr_st.st_ino;
		if (emuxfs_meta_read(&prewr_meta, i, prewr_ino))
			goto fail;
		prewr_eno = prewr_meta.header.eno;
		if (emuxfs_desc_init_from_stat(&prewr_desc, &prewr_st,
		    prewr_eno))
			goto fail;
		memcpy(prewr_desc.content_checksum,
		    &prewr_meta.checksums[chksz], chksz);
		emuxfs_desc_chk_meta(prewr_meta_chk_buf, &prewr_desc, alg);
		if (bcmp(prewr_meta_chk_buf, &prewr_meta.checksums[0],
		    chksz) != 0)
			goto fail;

		wr_desc = prewr_desc;
		wr_meta = prewr_meta;

		switch (args->type) {
		case EMUXFS_UT_CHMOD:
			emuxfs_eids_set();
			subrc = fchmodat(fd, args->path, args->mode,
			    AT_SYMLINK_NOFOLLOW);
			err = errno;
			emuxfs_eids_reset();
			if (subrc)
				subrc = EMUXFS_EFS;
			wr_desc.mode = (prewr_desc.mode & (uint64_t)S_IFMT) |
			    ((~(uint64_t)S_IFMT) & args->mode);
			break;
		case EMUXFS_UT_CHOWN:
			emuxfs_eids_set();
			subrc = fchownat(fd, args->path, args->uid, args->gid,
			    AT_SYMLINK_NOFOLLOW);
			err = errno;
			emuxfs_eids_reset();
			if (subrc)
				subrc = EMUXFS_EFS;
			if (args->uid != (uid_t)-1)
				wr_desc.owner = args->uid;
			if (args->gid != (gid_t)-1)
				wr_desc.group = args->gid;
			/* Possibly not POSIX compliant. */
			wr_desc.mode &= (~(uint64_t)(S_ISUID|S_ISGID));
			break;
		case EMUXFS_UT_UTIMENS:
			emuxfs_eids_set();
			subrc = utimensat(fd, args->path, args->ts,
			    AT_SYMLINK_NOFOLLOW);
			err = errno;
			emuxfs_eids_reset();
			if (subrc)
				subrc = EMUXFS_EFS;
			break;
		case EMUXFS_UT_TRUNCATE:
			subrc = emuxfs_truncate_inner(fd, args, alg, chksz,
			    &prewr_st, &prewr_meta, &wr_meta, &wr_desc,
			    &err, dev->lfile_fd);
			break;
		case EMUXFS_UT_WRITE:
			subrc = emuxfs_write_inner(fd, args, alg, chksz,
			    &prewr_st, &prewr_meta, &wr_meta, &wr_desc, &err,
			    dev->lfile_fd, wc);
			break;
		default:
			exit(-1); /* Programming error. */
		}
		switch (subrc) {
		case 0:
			break;
		case EMUXFS_EINT:
			exit(-1); /* Unrecoverable runtime error. */
		case EMUXFS_EFS:
			if (has_write)
				goto fail;
			rc = -err;
			goto early;
		case EMUXFS_ECHK:
			goto fail;
		default:
			exit(-1); /* Programming error. */
		}
		emuxfs_desc_chk_meta(&wr_meta.checksums[0], &wr_desc, alg);
		emuxfs_fault_point("update/before_meta");
		if (emuxfs_meta_write(&wr_meta, i, prewr_ino))
			goto fail;
		emuxfs_fault_point("update/after_meta");

		if (prewr_desc.type != EMUXFS_DT_LNK) {
			if ((subfd = openat(fd, args->path,
			    O_RDONLY|O_NOFOLLOW)) == -1)
				goto fail;
			if (fsync(subfd))
				exit(-1);
			if (close(subfd))
				exit(-1);
		}
		if (fsync(dev->meta_fd))
			exit(-1);

		switch (args->type) {
		case EMUXFS_UT_TRUNCATE:
			mod_end = (size_t)args->offset;
			if ((size_t)args->offset <= (size_t)prewr_st.st_size) {
				mod_begin = (size_t)args->offset;
				if (mod_begin > 1)
					--mod_begin;
			} else
				mod_begin = (size_t)prewr_st.st_size;
			mod_size = mod_end;
			if (mod_size > EMUXFS_BLOCK_SIZE) {
				if (emuxfs_lfile_readback(NULL, i, args->path,
				    mod_begin, mod_end,
				    &wr_meta.checksums[chksz]))
					goto fail;
				if (emuxfs_readback(i, args->path, 1, &wr_meta))
					goto fail;
			} else {
				if (emuxfs_readback(i, args->path, 0, &wr_meta))
					goto fail;
			}
			break;
		case EMUXFS_UT_WRITE:
			mod_begin = (size_t)args->offset;
			if (mod_begin > (size_t)prewr_st.st_size)
				mod_begin = (size_t)prewr_st.st_size;
			mod_end = (size_t)args->offset + args->bufsz;
			mod_size = mod_end;
			if (mod_size < (size_t)prewr_st.st_size)
				mod_size = (size_t)prewr_st.st_size;
			if (mod_size > EMUXFS_BLOCK_SIZE) {
				if (emuxfs_lfile_readback(NULL, i, args->path,
				    mod_begin, mod_end,
				    &wr_meta.checksums[chksz]))
					goto fail;
				if (emuxfs_readback(i, args->path, 1, &wr_meta))
					goto fail;
			} else {
				if (emuxfs_readback(i, args->path, 0, &wr_meta))
					goto fail;
			}
			break;
		default:
			if (emuxfs_readback(i, args->path, 1, &wr_meta))
				goto fail;
		}

		cud.type = EMUXFS_CUD_UPDATE;
		cud.path = args->path;
		cud.pre_meta = prewr_meta;
		if (emuxfs_ancestors_meta_recompute(i, &cud))
			goto fail;

		has_write = 1;
		emuxfs_working_pop(i, now);
		continue;
fail:
		EMUXFS_TRACE("op_update: fail dev=%lu type=%d path=%s\n",
		    (unsigned long)i, (int)args->type, args->path);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
early:
		emuxfs_working_pop(i, now);
		return rc;
	}
	if (has_write) {
		if (args->type == EMUXFS_UT_WRITE)
			return (int)args->bufsz;
		return 0;
	}
	return -EIO;
}

static int
emuxfs_rename(const char *from, const char *to)
{
	EMUXFS_TRACE("enter");
	dind			 dev_count;
	dind			 i;
	struct emuxfs_dev	*dev;
	int			 fd;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;

	int			 rc, err, subrc;
	int			 has_write;
	struct stat		 prewr_st;
	ino_t			 prewr_ino;
	struct emuxfs_meta	 prewr_meta;
	uint64_t		 prewr_eno;
	struct emuxfs_desc	 prewr_desc;
	uint8_t			 prewr_meta_chk_buf[EMUXFS_CHKSZ_MAX];
	struct stat		 postwr_st;

	struct emuxfs_cud	 cud;

	time_t			 now;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&from))
		return -EIO;
	if (emuxfs_path_sanitize(&to))
		return -EIO;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = 0;

	now = time(NULL);

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		if (emuxfs_working_push(i))
			exit(-1);

		fd = dev->root_fd;
		alg = dev->conf.chk_alg_type;
		chksz = emuxfs_chk_size(alg);

		emuxfs_eids_set();
		subrc = fstatat(fd, from, &prewr_st, AT_SYMLINK_NOFOLLOW);
		err = errno;
		emuxfs_eids_reset();
		if (subrc) {
			if (has_write)
				goto fail;
			rc = -err;
			goto early;
		}
		prewr_ino = prewr_st.st_ino;
		if (emuxfs_meta_read(&prewr_meta, i, prewr_ino))
			goto fail;
		prewr_eno = prewr_meta.header.eno;
		if (emuxfs_desc_init_from_stat(&prewr_desc, &prewr_st,
		    prewr_eno))
			goto fail;
		memcpy(prewr_desc.content_checksum,
		    &prewr_meta.checksums[chksz], chksz);
		emuxfs_desc_chk_meta(prewr_meta_chk_buf, &prewr_desc, alg);
		if (bcmp(prewr_meta_chk_buf, &prewr_meta.checksums[0],
		    chksz) != 0)
			goto fail;

		/*
		 * The renameing process juggles the file to achieve 3 separate
		 * objectives.
		 *
		 * The first rename tests if the requested operation is
		 * possible.
		 */
		emuxfs_eids_set();
		subrc = renameat(fd, from, fd, to);
		err = errno;
		emuxfs_eids_reset();
		if (subrc) {
			if (has_write)
				goto fail;
			rc = -err;
			goto early;
		}

		/*
		 * The second rename moves the file out of the tree so that the
		 * meta entries for the ancestors of 'from' can be recomputed
		 * without colliding with 'to', its ancestors, and their
		 * meta entries.
		 */
		if (renameat(fd, to, fd, ".muxfs/rename.tmp"))
			goto fail;
		if (fstatat(fd, from, &postwr_st, AT_SYMLINK_NOFOLLOW) != -1)
			goto fail;
		if (errno != ENOENT)
			goto fail;
		cud.type = EMUXFS_CUD_DELETE;
		cud.path = from;
		cud.pre_meta = prewr_meta;
		if (emuxfs_ancestors_meta_recompute(i, &cud))
			goto fail;

		/*
		 * The third rename moves the file back to its destination; at
		 * this point the ancestors of 'to' can be recomputed.
		 */
		if (renameat(fd, ".muxfs/rename.tmp", fd, to))
			goto fail;
		if (emuxfs_fsync_parent(fd, from))
			goto fail;
		if (emuxfs_fsync_parent(fd, to))
			goto fail;
		if (emuxfs_readback(i, to, 0, &prewr_meta))
			goto fail;
		cud.type = EMUXFS_CUD_CREATE;
		cud.path = to;
		cud.pre_meta = prewr_meta;
		if (emuxfs_ancestors_meta_recompute(i, &cud))
			goto fail;

		has_write = 1;
		emuxfs_working_pop(i, now);
		continue;
fail:
		EMUXFS_TRACE("rename: fail dev=%lu from=%s to=%s\n",
		    (unsigned long)i, from, to);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
early:
		emuxfs_working_pop(i, now);
		return rc;
	}
	return has_write ? 0 : -EIO;
}

static int
emuxfs_link(const char *from, const char *to)
{
	EMUXFS_TRACE("enter");
	(void)from;
	(void)to;

	return -EOPNOTSUPP;
}

static int
emuxfs_chmod(const char *path, mode_t mode)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_UT_CHMOD;
	args.path = path;
	args.mode = mode;
	args.wc = NULL;

	return emuxfs_op_update(&args);
}

static int
emuxfs_chown(const char *path, uid_t uid, gid_t gid)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_UT_CHOWN;
	args.path = path;
	args.uid = uid;
	args.gid = gid;
	args.wc = NULL;

	return emuxfs_op_update(&args);
}

static int
emuxfs_utimens(const char *path, const struct timespec *ts)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_UT_UTIMENS;
	args.path = path;
	args.ts = ts;
	args.wc = NULL;

	return emuxfs_op_update(&args);
}

static int
emuxfs_truncate(const char *path, off_t offset)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	args.type = EMUXFS_UT_TRUNCATE;
	args.path = path;
	args.offset = offset;
	args.wc = NULL;

	return emuxfs_op_update(&args);
}

/*
 * FUSE access(2) callback.  FUSE callbacks return a negated errno, so on
 * failure the caller's errno is preserved and negated rather than discarded.
 * The effective credentials are switched to the requesting user for the
 * underlying check, mirroring the other read-only callbacks.
 */
static int
emuxfs_access(const char *path, int amode)
{
	EMUXFS_TRACE("enter");
	dind			 dev_count;
	dind			 i;
	struct emuxfs_dev	*dev;
	int			 err, subrc;

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		emuxfs_eids_set();
		subrc = faccessat(dev->root_fd, path, amode, AT_EACCESS);
		err = errno;
		emuxfs_eids_reset();
		return subrc ? -err : 0;
	}

	return -EIO;
}

static void
emuxfs_wrbuf_flush(void)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;
	const struct emuxfs_wrbuf *wr;
	int subrc;

	if (!emuxfs_state_wrbuf_is_set())
		return;

	if (emuxfs_state_wrbuf_get(&wr))
		exit(-1);

	args.type = EMUXFS_UT_WRITE;
	args.path = wr->path;
	args.buf = (const char *)wr->buf;
	args.bufsz = wr->sz;
	args.offset = (off_t)wr->off;
	args.wc = &wr->wc;

	subrc = emuxfs_op_update(&args);
	if (subrc < 0 || (size_t)subrc != wr->sz)
		exit(-1);

	if (emuxfs_state_wrbuf_reset())
		exit(-1);
}

static int
emuxfs_buffered_write_begin(const char *path, const char *buf, size_t bufsz,
    off_t offset, struct fuse_context *fc)
{
	EMUXFS_TRACE("enter");
	int rc;

	rc = emuxfs_access(path, W_OK);
	if (rc != 0)
		return rc; /* Negated errno, as returned by the FUSE layer. */

	if (emuxfs_state_wrbuf_set(path, fc->uid, fc->gid, bufsz,
	    (size_t)offset, (const uint8_t *)buf))
		exit(-1);
	return (int)bufsz;
}

static int
emuxfs_buffered_write(const char *path, const char *buf, size_t bufsz,
    off_t offset)
{
	EMUXFS_TRACE("enter");
	struct fuse_context *fc;
	size_t wrsz;

	if (bufsz > EMUXFS_WRBUF_SIZE)
		exit(-1); /* Programming error. */

	fc = fuse_get_context();

	if (!emuxfs_state_wrbuf_is_set()) {
		return emuxfs_buffered_write_begin(path, buf, bufsz, offset,
		    fc);
	}

	if (!emuxfs_state_wrbuf_append(&wrsz, path, fc->uid, fc->gid,
	    bufsz, (size_t)offset, (const uint8_t *)buf)) {
		if (wrsz < bufsz) {
			offset += wrsz;
			bufsz -= wrsz;
			emuxfs_wrbuf_flush();
			if (emuxfs_state_wrbuf_set(path, fc->uid, fc->gid,
			    bufsz, (size_t)offset, (const uint8_t *)buf))
				exit(-1);
		}
		return (int)bufsz;
	}

	emuxfs_wrbuf_flush();
	return emuxfs_buffered_write_begin(path, buf, bufsz, offset, fc);
}

static int
emuxfs_write(const char *path, const char *buf, size_t bufsz, off_t offset,
    struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;

	(void)ffi;

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	if (bufsz <= EMUXFS_WRBUF_SIZE)
		return emuxfs_buffered_write(path, buf, bufsz, offset);

	emuxfs_wrbuf_flush();

	args.type = EMUXFS_UT_WRITE;
	args.path = path;
	args.buf = buf;
	args.bufsz = bufsz;
	args.offset = offset;
	args.wc = NULL;

	return emuxfs_op_update(&args);
}

const struct fuse_operations emuxfs_fuse_ops = {
	/* Pass-through */
	.statfs      = emuxfs_statfs,
	/* Stateful */
	.init        = emuxfs_fuse_init,
	.destroy     = emuxfs_fuse_destroy,
	/* No-ops */
	.releasedir  = emuxfs_releasedir,
	/* Create */
	.mknod       = emuxfs_mknod,
	.mkdir       = emuxfs_mkdir,
	.symlink     = emuxfs_symlink,
	/* Read */
	.open        = emuxfs_open,
	.opendir     = emuxfs_opendir,
	.getattr     = emuxfs_getattr,
	.read        = emuxfs_read,
	.readlink    = emuxfs_readlink,
	.readdir     = emuxfs_readdir,
	.access      = emuxfs_access,
	/* Update */
	.fsync       = emuxfs_fsync,
	.flush       = emuxfs_flush,
	.release     = emuxfs_release,
	.rename      = emuxfs_rename,
	.chmod       = emuxfs_chmod,
	.chown       = emuxfs_chown,
	.utimens     = emuxfs_utimens,
	.truncate    = emuxfs_truncate,
	.write       = emuxfs_write,
	/* Delete */
	.unlink      = emuxfs_unlink,
	.rmdir       = emuxfs_rmdir,
	/* Explicitly not supported */
	.link        = emuxfs_link,
	.lock        = emuxfs_lock,

	/*
	 * The OpenBSD libfuse and kernel do not implement these operations,
	 * so they are deliberately absent.  File creation is mknod followed
	 * by open; there are no xattrs, no fsyncdir and no readdirplus.
	 */
	/*.create      = emuxfs_create      ,*/
	/*.fsyncdir    = emuxfs_fsyncdir    ,*/
	/*.setxattr    = emuxfs_setxattr    ,*/
	/*.getxattr    = emuxfs_getxattr    ,*/
	/*.listxattr   = emuxfs_listxattr   ,*/
	/*.removexattr = emuxfs_removexattr ,*/
	/*.bmap        = emuxfs_bmap        ,*/
};
