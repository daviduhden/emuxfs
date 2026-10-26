/* ops.c */

/*
 * This file belongs to emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
 *
 * FUSE operation callbacks and the multi-device transaction engine.  Every
 * mutating callback applies the same logical change to each usable device in
 * index order.  Each device advances its own durable sequence number only
 * after its tree, metadata, assignments and parent directory are durable, so
 * an interrupted operation is visible as a sequence mismatch rather than as a
 * silent partial commit.  Reads verify metadata, content and the eno/ino
 * mapping, try other devices on damage, and queue repairs for the next drain.
 */

// clang-format off
#include <sys/mman.h>
#include <sys/syslimits.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chk.h"
#include "ds.h"
#include "emuxfs.h"
#include "fault.h"
#include "ops.h"
// clang-format on

static void emuxfs_wrbuf_flush(void);
static int  emuxfs_truncate(const char *, off_t);

/*
 * Adopt the requesting FUSE user's effective credentials for one backing
 * operation.  Supplementary groups are dropped first; otherwise a request
 * attributed to an unprivileged user could still satisfy a group permission
 * check through root's groups.  The daemon keeps euid 0 for its own privileged
 * work, so these switches must always be paired with emuxfs_eids_reset().
 */
static void
emuxfs_eids_set(void)
{
	EMUXFS_TRACE("enter");
	auto fc = fuse_get_context();
	/*
	 * Drop the supplementary groups before dropping euid.  Otherwise an
	 * operation attributed to the requesting user would still carry
	 * root's groups and could pass a group check that the user's own
	 * credentials would fail.  The daemon needs no supplementary groups
	 * of its own: its privileged work runs with euid 0.
	 */
	if (setgroups(0, nullptr))
		exit(-1);
	if (setegid(fc->gid))
		exit(-1);
	if (seteuid(fc->uid))
		exit(-1);
}

/*
 * Impersonate the credentials captured with a deferred write.  This follows
 * the same ordering as emuxfs_eids_set(): drop supplementary groups, then set
 * egid and euid.  Used only by the legacy write-buffer flush path.
 */
static void
emuxfs_eids_wrctx_set(const struct emuxfs_wrctx *wc)
{
	EMUXFS_TRACE("enter");
	if (setgroups(0, nullptr))
		exit(-1);
	if (setegid(wc->group))
		exit(-1);
	if (seteuid(wc->user))
		exit(-1);
}

/*
 * Restore the daemon's own credentials after an impersonated operation.
 * Order matters: regain euid 0 before restoring egid, so the process can
 * change group IDs again.
 */
static void
emuxfs_eids_reset(void)
{
	EMUXFS_TRACE("enter");
	if (seteuid(getuid()))
		exit(-1);
	if (setegid(getgid()))
		exit(-1);
}

/*
 * Report aggregate filesystem statistics.  Mirrors are not striped: every
 * device holds a complete copy, so capacity, free space, inode counts and
 * maximum name length are the minimum across usable devices.  Block counts are
 * normalized to EMUXFS_BLOCK_SIZE, and all multiplications use checked
 * arithmetic so an overflowing backing filesystem is reported rather than
 * wrapped.
 */
static int
emuxfs_statfs(const char *path, struct statvfs *stvfs)
{
	EMUXFS_TRACE("enter");
	dind		   dev_count, i;
	struct emuxfs_dev *dev;
	int		   fd, err, subrc;
	struct statvfs	   st, st_agg;
	bool		   has_st;
	size_t		   frsize, sz;
	uint64_t	   bytes;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	has_st = false;
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
			if (ckd_mul(&bytes, (uint64_t)frsize,
				(uint64_t)st.f_blocks))
				return -EOVERFLOW;
			sz = (size_t)(bytes / EMUXFS_BLOCK_SIZE);
			if (st_agg.f_blocks > sz)
				st_agg.f_blocks = sz;
			if (ckd_mul(
				&bytes, (uint64_t)frsize, (uint64_t)st.f_bfree))
				return -EOVERFLOW;
			sz = (size_t)(bytes / EMUXFS_BLOCK_SIZE);
			if (st_agg.f_bfree > sz)
				st_agg.f_bfree = sz;
			if (ckd_mul(&bytes, (uint64_t)frsize,
				(uint64_t)st.f_bavail))
				return -EOVERFLOW;
			sz = (size_t)(bytes / EMUXFS_BLOCK_SIZE);
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
			if (ckd_mul(&bytes, (uint64_t)frsize,
				(uint64_t)st.f_blocks))
				return -EOVERFLOW;
			sz = (size_t)(bytes / EMUXFS_BLOCK_SIZE);
			st_agg.f_blocks = sz;
			if (ckd_mul(
				&bytes, (uint64_t)frsize, (uint64_t)st.f_bfree))
				return -EOVERFLOW;
			sz = (size_t)(bytes / EMUXFS_BLOCK_SIZE);
			st_agg.f_bfree = sz;
			if (ckd_mul(&bytes, (uint64_t)frsize,
				(uint64_t)st.f_bavail))
				return -EOVERFLOW;
			sz = (size_t)(bytes / EMUXFS_BLOCK_SIZE);
			st_agg.f_bavail = sz;
			st_agg.f_fsid = 0;
			st_agg.f_flag = st.f_flag;
			has_st = true;
		}
	}
	if (has_st) {
		*stvfs = st_agg;
		return 0;
	}
	return -EIO;
}

static void *
emuxfs_fuse_init(struct fuse_conn_info *)
{
	EMUXFS_TRACE("enter");

	emuxfs_info("Mounted");
	return nullptr;
}

static void
emuxfs_fuse_destroy(void *)
{
	EMUXFS_TRACE("enter");

	emuxfs_wrbuf_flush();
	emuxfs_info("Unmounting");
	if (emuxfs_final())
		exit(-1);
}

static int
emuxfs_fsync(const char *, int, struct fuse_file_info *)
{
	EMUXFS_TRACE("enter");
	/*
	 * A call to fsync(2) is made as part of the create, update, and delete
	 * operations.
	 */

	emuxfs_wrbuf_flush();
	return 0;
}

static int
emuxfs_open(const char *path, struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	dind		   dev_count, i;
	struct emuxfs_dev *dev;
	int		   fd, err, oflags;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	/*
	 * Creation is done by mknod(2), so open must not create.  Passing
	 * O_CREAT through would create a node that emuxfs does not track (no
	 * metadata) and would also be undefined behaviour, because openat(2)
	 * then requires a mode argument that FUSE does not supply.
	 *
	 * Truncation is performed by the update path so that metadata stays
	 * consistent; passing O_TRUNC to openat(2) would truncate the backing
	 * file behind emuxfs's back and leave meta.db describing the old
	 * contents.
	 */
	oflags = (ffi->flags & ~(O_CREAT | O_EXCL)) | O_NOFOLLOW | O_CLOEXEC;
	if (oflags & O_TRUNC) {
		int trc;

		trc = emuxfs_truncate(path, 0);
		if (trc)
			return trc;
		oflags &= ~O_TRUNC;
	}

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
emuxfs_opendir(const char *path, [[maybe_unused]] struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	dind		   dev_count, i;
	struct emuxfs_dev *dev;
	int		   fd, err, oflags;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	/*
	 * Directories are created by mkdir(2); see emuxfs_open().  Ignore the
	 * caller's open flags: a directory is always opened read-only, without
	 * following a symlink, and never with O_TRUNC.
	 */
	oflags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

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
emuxfs_flush(const char *, struct fuse_file_info *)
{
	EMUXFS_TRACE("enter");

	emuxfs_wrbuf_flush();
	return 0;
}

static int
emuxfs_release(const char *, struct fuse_file_info *)
{
	EMUXFS_TRACE("enter");

	emuxfs_wrbuf_flush();
	return 0;
}

static int
emuxfs_releasedir(const char *, struct fuse_file_info *)
{
	EMUXFS_TRACE("enter");

	return 0;
}

static int
emuxfs_lock(const char *, struct fuse_file_info *, int, struct flock *)
{
	EMUXFS_TRACE("enter");

	return -EOPNOTSUPP;
}

enum emuxfs_op_create_type {
	EMUXFS_CT_MKNOD,
	EMUXFS_CT_MKDIR,
	EMUXFS_CT_SYMLINK
};
struct emuxfs_op_create_args {
	enum emuxfs_op_create_type type;
	const char		  *path;
	mode_t			   mode;
	dev_t			   sys_dev;
	const char		  *link_content;
};

/*
 * Create one node on every usable device.  A single eno is allocated for the
 * logical node before the loop; each device receives its own metadata and
 * assignment record.  If a device fails before mutating anything, its working
 * marker is canceled without advancing seq.  If it fails after a mutation, it
 * is marked degraded and the operation continues on later devices, succeeding
 * as long as at least one device committed.  Only regular files, directories
 * and symlinks are supported; other node types are refused.
 */
static int
emuxfs_op_create(struct emuxfs_op_create_args *args)
{
	EMUXFS_TRACE("enter");
	struct fuse_context *fc;

	dind			 dev_count, i;
	struct emuxfs_dev	*dev;
	int			 fd;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;

	int		     rc, err, subrc, subfd;
	bool		     has_write;
	uint64_t	     eno;
	struct emuxfs_desc   desc;
	struct emuxfs_chk    content_chk;
	struct stat	     st;
	ino_t		     ino;
	struct emuxfs_meta   meta;
	struct emuxfs_assign assign;

	struct emuxfs_cud cud;

	time_t now;

	mode_t old_umask;

	fc = fuse_get_context();

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = false;
	if (emuxfs_state_eno_next_acquire(&eno))
		return -EIO;

	now = time(nullptr);

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
		    .group = 0,
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
		meta.header.eno = eno;
		meta.header.flags = MF_ASSIGNED;

		switch (args->type) {
		case EMUXFS_CT_MKNOD:
			if (S_ISREG(args->mode)) {
				emuxfs_eids_set();
				old_umask = umask(fc->umask);
				subfd = openat(fd, args->path,
				    O_RDWR | O_CREAT | O_EXCL, args->mode);
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
				 O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) == -1)
				goto fail;
			if (fsync(subfd))
				exit(-1);
			if (close(subfd))
				exit(-1);
		}

		if (fstatat(fd, args->path, &st, AT_SYMLINK_NOFOLLOW))
			goto fail;
		ino = st.st_ino;
		/*
		 * Take owner/group from the node that was actually created.
		 * This preserves the backing filesystem's POSIX result (the
		 * caller's egid, or the parent's gid for a setgid directory)
		 * and avoids a root chown/chmod that could re-add setuid bits.
		 */
		desc.owner = st.st_uid;
		desc.group = st.st_gid;
		desc.mode = (desc.mode & (uint64_t)S_IFMT) |
		    (uint64_t)(st.st_mode & 07777);
		emuxfs_desc_chk_meta(&meta.checksums[0], &desc, alg);
		assign =
		    (struct emuxfs_assign){.flags = AF_ASSIGNED, .ino = ino};

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

		has_write = true;
		emuxfs_working_pop(i, now);
		continue;
	fail:
		EMUXFS_TRACE("op_create: fail dev=%lu type=%d path=%s\n",
		    (unsigned long)i, (int)args->type, args->path);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
	early:
		/*
		 * No device mutation has happened on this device yet.  Clear
		 * the recovery marker without advancing 'seq': a failed
		 * operation must not look like an interrupted commit.
		 */
		emuxfs_working_cancel(i);
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
	args.mode = S_IFLNK | 0777;

	return emuxfs_op_create(&args);
}

enum emuxfs_op_delete_type { EMUXFS_DT_UNLINK, EMUXFS_DT_RMDIR };

/*
 * Remove one node from every usable device.  The current metadata is verified
 * before deletion, hard-linked files are refused, and the large-file checksum
 * tree is removed only after unlinkat(2) succeeds.  Metadata and assignment
 * records are cleared and fsynced before the working marker is cleared.  On
 * success, the deleted node's eno is returned to the allocator so the identity
 * can be reused.
 */
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

	int		     rc, err, subrc;
	bool		     has_write;
	const char	    *stage;
	struct stat	     prewr_st;
	ino_t		     prewr_ino;
	struct emuxfs_meta   prewr_meta;
	uint64_t	     prewr_eno;
	struct emuxfs_desc   prewr_desc;
	uint8_t		     prewr_meta_chk_buf[EMUXFS_CHKSZ_MAX];
	struct emuxfs_meta   wr_meta;
	struct emuxfs_assign wr_assign;
	char		     postwr_ppath[PATH_MAX];
	int		     postwr_pfd;
	struct stat	     postwr_st;
	struct emuxfs_cud    cud;

	time_t now;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = false;
	return_eno = UINT64_MAX;
	stage = "start";

	now = time(nullptr);

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
			emuxfs_warn(
			    "Unsupported hard link: %lu:/%s\n", (dind)i, path);
			rc = -EOPNOTSUPP;
			goto early;
		}

		stage = "meta_read";
		if (emuxfs_meta_read(&prewr_meta, i, prewr_ino))
			goto fail;
		if (prewr_meta.header.flags != MF_ASSIGNED)
			goto fail;
		prewr_eno = prewr_meta.header.eno;
		stage = "desc_init";
		if (emuxfs_desc_init_from_stat(
			&prewr_desc, &prewr_st, prewr_eno))
			goto fail;
		stage = "chk_node_content";
		if (emuxfs_desc_chk_node_content(&prewr_desc, i, path))
			goto fail;
		stage = "chk_meta";
		emuxfs_desc_chk_meta(prewr_meta_chk_buf, &prewr_desc, alg);
		if (bcmp(prewr_meta_chk_buf, &prewr_meta.checksums[0], chksz) !=
		    0)
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
			if ((size_t)prewr_st.st_size > EMUXFS_BLOCK_SIZE) {
				stage = "lfile_delete";
				if (emuxfs_lfile_delete(
					dev->lfile_fd, prewr_ino))
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
		if (emuxfs_path_pop(nullptr, postwr_ppath, nullptr)) {
			memset(postwr_ppath, 0, PATH_MAX);
			strlcpy(postwr_ppath, ".", PATH_MAX);
		}
		if ((postwr_pfd = openat(dev->root_fd, postwr_ppath,
			 O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) == -1)
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
			has_write = true;
			return_eno = prewr_eno;
		}

		emuxfs_working_pop(i, now);
		continue;
	fail:
		EMUXFS_TRACE(
		    "op_delete: fail dev=%lu type=%d stage=%s path=%s\n",
		    (unsigned long)i, (int)type,
		    (stage != nullptr) ? stage : "?", path);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
	early:
		/*
		 * No device mutation has happened on this device yet.  Clear
		 * the recovery marker without advancing 'seq': a failed
		 * operation must not look like an interrupted commit.
		 */
		emuxfs_working_cancel(i);
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
    [[maybe_unused]] int *err)
{
	EMUXFS_TRACE("enter");
	size_t sz;

	st->st_ino = eno;
	sz = (size_t)st->st_size;
	/*
	 * POSIX defines st_blocks in 512-byte units; st_blksize is the
	 * preferred I/O size.  Dividing by EMUXFS_BLOCK_SIZE here made stat(2)
	 * and du(1) under-report allocation by a factor of eight.
	 */
	st->st_blocks = (sz / 512) + ((sz % 512) ? 1 : 0);
	st->st_blksize = EMUXFS_BLOCK_SIZE;
	*st_out = *st;

	return 0;
}

/*
 * Read a byte range from one device and verify it.  Small files are checked
 * as a whole against the content checksum in meta.db.  Larger files map only
 * the checksum-tree leaves for the requested block range and copy the exact
 * requested bytes from the block-aligned reads, never a whole block beyond the
 * caller's buffer.  Returns EMUXFS_ECHK for a checksum mismatch so the caller
 * can try another mirror and queue a repair.
 */
static int
emuxfs_read_inner(int root_fd, struct emuxfs_op_read_args *args,
    struct stat *st, enum emuxfs_chk_alg_type alg, size_t chksz,
    struct emuxfs_meta *meta, ssize_t *rdsz_out, int *err, int lfile_fd)
{
	EMUXFS_TRACE("enter");
	int		    fd, rc;
	size_t		    fsz;
	uint8_t		    buf[EMUXFS_BLOCK_SIZE];
	struct emuxfs_chk   content_chk;
	uint8_t		    content_sum[EMUXFS_CHKSZ_MAX];
	size_t		    rdsz;
	struct emuxfs_range r;
	size_t		    i_offset, out_offset, copy_begin, copy_end;
	uint64_t	    i;
	int		    lfd;
	uint8_t		   *lfile;

	rc = EMUXFS_EINT;
	*err = 0;

	emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	*err = errno;
	emuxfs_eids_reset();
	if (fd == -1) {
		rc = EMUXFS_EFS;
		goto out;
	}
	/* Do not let a stale errno from a successful open leak into EFS paths.
	 */
	*err = 0;

	fsz = (size_t)st->st_size;
	if ((size_t)args->offset >= fsz) {
		*rdsz_out = 0;
		rc = 0;
		goto out2;
	}
	/*
	 * A zero-length read must not reach the mmap(2) below: the checksum
	 * tree has no entry to map and a zero-length mapping fails, which
	 * would be reported as an unrecoverable internal error.
	 */
	if (args->size == 0) {
		*rdsz_out = 0;
		rc = 0;
		goto out2;
	}

	if (fsz <= EMUXFS_BLOCK_SIZE) {
		if (read(fd, buf, fsz) != (ssize_t)fsz) {
			*err = EIO;
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
	if (ckd_add(&r.byte_end, (size_t)args->offset, args->size)) {
		*err = EOVERFLOW;
		rc = EMUXFS_EFS;
		goto out2;
	}
	if (r.byte_end > fsz)
		r.byte_end = fsz;
	emuxfs_range_compute(&r, chksz);
	out_offset = 0;

	if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDONLY)) {
		*err = EIO;
		goto out2;
	}
	{
		struct stat lst;

		if (fstat(lfd, &lst)) {
			*err = EIO;
			goto out3;
		}
		if ((lst.st_size < 0) ||
		    ((uint64_t)lst.st_size <
			(uint64_t)r.lfileoff + r.lfilesz)) {
			*err = EIO;
			goto out3;
		}
	}
	if ((lfile = mmap(nullptr, r.lfilesz, PROT_READ, MAP_SHARED, lfd,
		 (off_t)r.lfileoff)) == MAP_FAILED) {
		*err = EIO;
		goto out3;
	}

	for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
		i_offset = i * EMUXFS_BLOCK_SIZE;
		rdsz = EMUXFS_BLOCK_SIZE;
		if (i_offset + rdsz > fsz)
			rdsz = fsz - i_offset;
		if (pread(fd, buf, rdsz, (off_t)i_offset) != (ssize_t)rdsz) {
			*err = EIO;
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
		/*
		 * The checksum tree is addressed by whole blocks, so the
		 * first and last block of the requested range can extend
		 * beyond byte_begin/byte_end.  Copy only the requested
		 * bytes: copying a whole block here would write past the
		 * end of the caller's buffer (the block's tail bytes may
		 * also lie beyond its end).
		 */
		copy_begin = i_offset;
		if (copy_begin < r.byte_begin)
			copy_begin = r.byte_begin;
		copy_end = i_offset + rdsz;
		if (copy_end > r.byte_end)
			copy_end = r.byte_end;
		if (copy_end > copy_begin) {
			memcpy(&args->buf_out[out_offset],
			    &buf[copy_begin - i_offset], copy_end - copy_begin);
			out_offset += copy_end - copy_begin;
		}
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
	ssize_t		   lnksz, rdsz;
	char		   lnkbuf[PATH_MAX];
	uint8_t		   lnk_meta_sum[EMUXFS_CHKSZ_MAX];

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
	emuxfs_desc_chk_provided_content(
	    &lnk_desc, (uint8_t *)lnkbuf, (size_t)lnksz, alg);
	emuxfs_desc_chk_meta(lnk_meta_sum, &lnk_desc, alg);
	if (bcmp(lnk_meta_sum, &meta->checksums[0], chksz) != 0)
		return EMUXFS_ECHK;

	if (args->size == 0) {
		*err = EINVAL;
		return EMUXFS_EFS;
	}
	rdsz = ((size_t)lnksz < (args->size - 1)) ? lnksz :
						    (ssize_t)(args->size - 1);
	memcpy(args->buf_out, lnkbuf, (size_t)rdsz);
	args->buf_out[rdsz] = '\0';

	return 0;
}

static int
emuxfs_readdir_inner(dind dev_index, int root_fd,
    struct emuxfs_op_read_args		     *args,
    [[maybe_unused]] enum emuxfs_chk_alg_type alg, size_t chksz,
    [[maybe_unused]] struct stat *st, [[maybe_unused]] struct emuxfs_desc *desc,
    struct emuxfs_meta *meta, int *err)
{
	EMUXFS_TRACE("enter");
	int		  rc, fd;
	uint8_t		  content_sum[EMUXFS_CHKSZ_MAX];
	struct emuxfs_dir dir;
	struct dirent	 *dirent;
	size_t		  i;
	const char	 *dname;
	size_t		  dnamelen;

	rc = EMUXFS_EINT;

	emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
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
		/*
		 * The high-level API passes the previous call's cookie in
		 * 'offset'.  Use the sorted entry index as the cookie and stop
		 * when fill() reports that the reply buffer is full; ignoring
		 * both could silently truncate a directory or repeat entries.
		 */
		if ((off_t)i < args->offset)
			continue;
		if (args->fill(
			args->fill_data, dname, nullptr, (off_t)(i + 1)) != 0)
			break;
	}

	rc = 0;
out2:
	if (emuxfs_popdir(&dir))
		exit(-1);
out:
	return rc;
}

/*
 * Shared read-side dispatcher for getattr, read, readlink and readdir.  A
 * usable device is selected by validating metadata and the requested
 * operation.  Missing nodes are remembered rather than immediately returned:
 * another mirror may serve the node, in which case the missing copies are
 * queued for repair.  Permission errors are not treated as corruption.  A
 * genuinely absent node returns ENOENT without queuing repairs.  Any queued
 * corruption is drained after a successful read so the next access observes a
 * repaired tree.
 */
static int
emuxfs_op_read(struct emuxfs_op_read_args *args)
{
	EMUXFS_TRACE("enter");
	dind			 dev_count, i;
	struct emuxfs_dev	*dev;
	int			 fd;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;

	int		   err, rc, subrc;
	int		   last_err;
	dind		   j;
	struct stat	   st;
	ino_t		   ino;
	struct emuxfs_meta meta;
	uint64_t	   eno;
	struct emuxfs_desc desc;
	bool		   missing[EMUXFS_DEV_COUNT_MAX] = {false};

	uint8_t chk_buf[EMUXFS_CHKSZ_MAX];
	ssize_t rdsz;
	bool	hard_error;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	hard_error = false;
	last_err = EIO;
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
			if (err == ENOENT) {
				/*
				 * One mirror is missing the node.  It may be
				 * genuinely absent everywhere, or this copy may
				 * be the damaged one.  Try the other mirrors;
				 * if one serves the node, queue the missing
				 * copies for repair.
				 */
				missing[i] = true;
				if (!hard_error)
					last_err = ENOENT;
				continue;
			}
			last_err = err ? err : EIO;
			if ((last_err == EACCES) || (last_err == EPERM)) {
				hard_error = true;
				continue;
			}
			goto fail;
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
			subrc =
			    emuxfs_getattr_inner(args->st_out, &st, eno, &err);
			break;
		case EMUXFS_RT_READ:
			subrc = emuxfs_read_inner(fd, args, &st, alg, chksz,
			    &meta, &rdsz, &err, dev->lfile_fd);
			break;
		case EMUXFS_RT_READLINK:
			subrc = emuxfs_readlink_inner(
			    fd, args, alg, chksz, &desc, &meta, &err);
			break;
		case EMUXFS_RT_READDIR:
			subrc = emuxfs_readdir_inner(
			    i, fd, args, alg, chksz, &st, &desc, &meta, &err);
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
			last_err = err ? err : EIO;
			/*
			 * Permission and argument errors are not corruption;
			 * trying another mirror or queueing a repair would be
			 * wrong.  A node that vanished after the stat is
			 * treated like a missing mirror.  I/O and consistency
			 * errors are queued so the next mirror can serve the
			 * request and the damaged copy can be repaired.
			 */
			if (last_err == ENOENT) {
				missing[i] = true;
				if (!hard_error)
					last_err = ENOENT;
				continue;
			}
			if (last_err == EINVAL)
				return -last_err;
			if ((last_err == EACCES) || (last_err == EPERM)) {
				hard_error = true;
				continue;
			}
			goto fail;
		case EMUXFS_ECHK:
			goto fail;
		default:
			exit(-1); /* Programming error. */
		}

		/*
		 * A mirror that did not have the node was damaged (or stale).
		 * Queue those copies now that a valid source has been found;
		 * a genuinely absent node never reaches this point.
		 */
		for (j = 0; j < dev_count; ++j) {
			if (missing[j] &&
			    emuxfs_state_restore_push_back(j, args->path))
				exit(-1);
		}
		emuxfs_restore_now();
		if (rdsz > -1)
			return (int)rdsz;
		return 0;
	fail:
		hard_error = true;
		last_err = EIO;
		if (emuxfs_state_restore_push_back(i, args->path))
			exit(-1);
		continue;
	early:
		emuxfs_restore_now();
		return rc;
	}
	/*
	 * If every usable mirror reported ENOENT, the node is genuinely
	 * absent; do not queue repairs for it.  Otherwise drain the queue: a
	 * repair may have succeeded from a copy that was not readable through
	 * this callback, and the next access will then see consistent data.
	 */
	if (!hard_error && (last_err == ENOENT))
		return -ENOENT;
	if ((last_err == EACCES) || (last_err == EPERM))
		return -last_err;
	emuxfs_restore_now();
	return -last_err;
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
	enum emuxfs_op_update_type type;
	const char		  *path;
	mode_t			   mode;
	uid_t			   uid;
	gid_t			   gid;
	const struct timespec	  *ts;
	off_t			   offset;
	const char		  *buf;
	size_t			   bufsz;
	const struct emuxfs_wrctx *wc;
};

/*
 * Resize one device's file and update its checksum tree.  The switch on
 * old/new size class handles the four meaningful cases: large to small,
 * small to large, large to large, and small to small.  Before mutating, the
 * existing bytes in the affected range are verified against the old
 * checksum tree.  When growing, the new hole is treated as zeros for hashing,
 * and the tree shape is adjusted only when the resize crosses a level
 * boundary.  The caller writes the resulting metadata after this function
 * returns.
 */
static int
emuxfs_truncate_inner(int root_fd, struct emuxfs_op_update_args *args,
    enum emuxfs_chk_alg_type alg, size_t chksz, struct stat *st,
    struct emuxfs_meta *prewr_meta, struct emuxfs_meta *wr_meta,
    struct emuxfs_desc *wr_desc, int *err, int lfile_fd)
{
	EMUXFS_TRACE("enter");
	int	rc;
	int	fd;
	uint8_t content_buf[EMUXFS_BLOCK_SIZE];
	size_t	prewr_sz, smaller_sz, larger_sz;

	struct emuxfs_chk prewr_content_chk;
	uint8_t		  prewr_content_sum[EMUXFS_CHKSZ_MAX];
	struct emuxfs_chk wr_content_chk;

	struct emuxfs_range r;
	size_t		    rdsz, i_offset, off, beginsz, padsz;
	size_t		    newoff;
	uint64_t	    i;
	int		    lfd;
	uint8_t		   *lfile;
	int		    szcase;

	const size_t blksz = EMUXFS_BLOCK_SIZE;

	rc = EMUXFS_EINT;
	fd = -1;
	lfd = -1;
	lfile = MAP_FAILED;
	r.lfilesz = 0;

	emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDWR | O_NOFOLLOW);
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
		if ((lfile = mmap(nullptr, r.lfilesz, PROT_READ, MAP_SHARED,
			 lfd, (off_t)r.lfileoff)) == MAP_FAILED)
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
			emuxfs_chk_update(
			    &prewr_content_chk, content_buf, rdsz);
			emuxfs_chk_final(prewr_content_sum, &prewr_content_chk);
			if (bcmp(prewr_content_sum,
				&lfile[chksz * (i - r.blk_index_begin)],
				chksz) != 0) {
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
		[[fallthrough]];
	case 0:
		emuxfs_chk_init(&wr_content_chk, alg);
		emuxfs_chk_update(&wr_content_chk, content_buf, newoff);
		emuxfs_chk_final(wr_desc->content_checksum, &wr_content_chk);
		memcpy(&wr_meta->checksums[chksz], wr_desc->content_checksum,
		    chksz);
		break;
	case 2:
		if (emuxfs_lfile_create(lfile_fd, chksz, st->st_ino, newoff))
			goto out;
		r.byte_begin = 0;
		r.byte_end = newoff;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDWR))
			goto out;
		/*
		 * OpenBSD's mmap(2) documents a PROT_WRITE mapping without
		 * PROT_READ (and an O_WRONLY descriptor) as unusable, so map
		 * read/write exactly as the lfile tree helpers do.
		 */
		if ((lfile = mmap(nullptr, r.lfilesz, PROT_READ | PROT_WRITE,
			 MAP_SHARED, lfd, (off_t)r.lfileoff)) == MAP_FAILED)
			goto out;

		for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
			i_offset = i * blksz;
			rdsz = blksz;
			if (i_offset + rdsz > newoff)
				rdsz = newoff - i_offset;
			/*
			 * This case grows a file that fits in one block: only
			 * the first block holds data, and everything from
			 * prewr_sz on is a hole of zeroes.  pread(2) returns
			 * short at end of file, so read only the bytes that
			 * exist and zero-fill the rest of the block.
			 */
			off = 0;
			if (i_offset < prewr_sz) {
				beginsz = prewr_sz - i_offset;
				if (beginsz > rdsz)
					beginsz = rdsz;
				if (pread(fd, content_buf, beginsz,
					(off_t)i_offset) != (ssize_t)beginsz) {
					rc = EMUXFS_EFS;
					goto out;
				}
				off = beginsz;
			}
			memset(&content_buf[off], 0, rdsz - off);
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
		memcpy(&wr_meta->checksums[chksz], wr_desc->content_checksum,
		    chksz);
		break;
	case 3:
		if (emuxfs_lfile_resize(
			lfile_fd, chksz, st->st_ino, prewr_sz, newoff))
			goto out;
		if (newoff < prewr_sz) {
			r.byte_begin = r.byte_end = newoff;
			if (r.byte_begin > 0)
				--r.byte_begin;
			emuxfs_range_compute(&r, chksz);

			if (emuxfs_lfile_open(
				&lfd, lfile_fd, st->st_ino, O_RDWR))
				goto out;
			if ((lfile = mmap(nullptr, r.lfilesz,
				 PROT_READ | PROT_WRITE, MAP_SHARED, lfd,
				 (off_t)r.lfileoff)) == MAP_FAILED)
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

			if (emuxfs_lfile_ancestors_recompute(
				wr_desc->content_checksum, lfile_fd, alg,
				st->st_ino, newoff, r.blk_index_begin,
				r.blk_index_end))
				goto out;
			memcpy(&wr_meta->checksums[chksz],
			    wr_desc->content_checksum, chksz);
			break;
		}
		r.byte_begin = prewr_sz;
		r.byte_end = newoff;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDWR))
			goto out;
		/*
		 * OpenBSD's mmap(2) documents a PROT_WRITE mapping without
		 * PROT_READ (and an O_WRONLY descriptor) as unusable, so map
		 * read/write exactly as the lfile tree helpers do.
		 */
		if ((lfile = mmap(nullptr, r.lfilesz, PROT_READ | PROT_WRITE,
			 MAP_SHARED, lfd, (off_t)r.lfileoff)) == MAP_FAILED)
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
			emuxfs_chk_final(
			    &lfile[chksz * (i - r.blk_index_begin)],
			    &wr_content_chk);
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
		memcpy(&wr_meta->checksums[chksz], wr_desc->content_checksum,
		    chksz);
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
	/*
	 * Mirror emuxfs_write_inner(): every error path after the lfile is
	 * opened must release the mapping and the descriptor, or repeated
	 * failures (for example on a corrupt array) leak file descriptors and
	 * address space.
	 */
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

/*
 * Write one device's file and update its checksum tree.  The pre-image bytes
 * covered by the write are verified first.  Writes entirely below one block
 * use a single buffer; larger writes update each touched block, preserving
 * bytes before and after the written range, and recompute the affected tree
 * ancestors.  The buffer is zero-filled where a write starts past the current
 * end so the hole's zero bytes are included in the checksum.  The caller
 * writes metadata after this returns.
 */
static int
emuxfs_write_inner(int root_fd, struct emuxfs_op_update_args *args,
    enum emuxfs_chk_alg_type alg, size_t chksz, struct stat *st,
    struct emuxfs_meta *prewr_meta, struct emuxfs_meta *wr_meta,
    struct emuxfs_desc *wr_desc, int *err, int lfile_fd,
    const struct emuxfs_wrctx *wc)
{
	EMUXFS_TRACE("enter");
	int	rc;
	int	fd;
	uint8_t content_buf[EMUXFS_BLOCK_SIZE];
	size_t	prewr_sz, wrub, largest_sz;

	struct emuxfs_chk prewr_content_chk;
	uint8_t		  prewr_content_sum[EMUXFS_CHKSZ_MAX];
	struct emuxfs_chk wr_content_chk;

	struct emuxfs_range r;
	size_t	 rdsz, i_offset, wroff, off, beginsz, padsz, wrsz, endsz;
	size_t	 newoff;
	uint64_t i;
	int	 lfd;
	uint8_t *lfile;

	const size_t blksz = EMUXFS_BLOCK_SIZE;

	rc = EMUXFS_EINT;
	fd = -1;
	lfd = -1;
	lfile = MAP_FAILED;
	r.lfilesz = 0;

	if (wc != nullptr)
		emuxfs_eids_wrctx_set(wc);
	else
		emuxfs_eids_set();
	fd = openat(root_fd, args->path, O_RDWR | O_NOFOLLOW);
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
	if (ckd_add(&wrub, newoff, args->bufsz)) {
		rc = EMUXFS_EFS;
		goto out;
	}
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
		if ((lfile = mmap(nullptr, r.lfilesz, PROT_READ, MAP_SHARED,
			 lfd, (off_t)r.lfileoff)) == MAP_FAILED)
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
			emuxfs_chk_update(
			    &prewr_content_chk, content_buf, rdsz);
			emuxfs_chk_final(prewr_content_sum, &prewr_content_chk);
			if (bcmp(prewr_content_sum,
				&lfile[chksz * (i - r.blk_index_begin)],
				chksz) != 0) {
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
			if (emuxfs_lfile_resize(
				lfile_fd, chksz, st->st_ino, prewr_sz, wrub))
				goto out;
		}

		r.byte_begin = newoff;
		if (prewr_sz < r.byte_begin)
			r.byte_begin = prewr_sz;
		r.byte_end = newoff + args->bufsz;
		emuxfs_range_compute(&r, chksz);

		if (emuxfs_lfile_open(&lfd, lfile_fd, st->st_ino, O_RDWR))
			goto out;
		/*
		 * OpenBSD's mmap(2) documents a PROT_WRITE mapping without
		 * PROT_READ (and an O_WRONLY descriptor) as unusable, so map
		 * read/write exactly as the lfile tree helpers do.
		 */
		if ((lfile = mmap(nullptr, r.lfilesz, PROT_READ | PROT_WRITE,
			 MAP_SHARED, lfd, (off_t)r.lfileoff)) == MAP_FAILED)
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
					    prewr_sz :
					    newoff;
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
				if ((off < blksz) && (prewr_sz < newoff)) {
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
				memcpy(
				    &content_buf[off], &args->buf[wroff], wrsz);
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
			emuxfs_chk_final(
			    &lfile[chksz * (i - r.blk_index_begin)],
			    &wr_content_chk);
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
			lfile_fd, alg, st->st_ino, largest_sz,
			r.blk_index_begin, r.blk_index_end))
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

/*
 * Apply a metadata, truncate or write update to every usable device.  The
 * pre-image is fully verified before mutation, hard links are refused, and
 * the resulting metadata is fsynced before readback and ancestor
 * recomputation.  A failed truncate or write is treated as a partial mutation
 * and degrades the device even when no device has committed yet, because the
 * syscall may already have changed the file.  chmod, chown and utimens are
 * single syscalls, so their failure can be canceled cleanly.
 */
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

	int		   rc, err, subrc, subfd;
	bool		   has_write;
	struct stat	   prewr_st;
	ino_t		   prewr_ino;
	struct emuxfs_meta prewr_meta;
	uint64_t	   prewr_eno;
	struct emuxfs_desc prewr_desc;
	uint8_t		   prewr_meta_chk_buf[EMUXFS_CHKSZ_MAX];
	struct emuxfs_desc wr_desc;
	struct emuxfs_meta wr_meta;

	size_t mod_begin, mod_end, mod_size;

	struct emuxfs_cud cud;

	time_t now;

	const struct emuxfs_wrctx *wc;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = false;

	now = time(nullptr);

	wc = args->wc;

	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		if (emuxfs_working_push(i))
			exit(-1);

		fd = dev->root_fd;
		alg = dev->conf.chk_alg_type;
		chksz = emuxfs_chk_size(alg);

		if (wc != nullptr)
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

		/*
		 * Hard links share one metadata slot and one eno.  Rewriting,
		 * truncating, re-owning or renaming one name would silently
		 * change the other, so refuse before any mutation.
		 */
		if (emuxfs_is_hardlink(&prewr_st)) {
			if (has_write)
				goto fail;
			emuxfs_warn("Unsupported hard link: %lu:/%s\n", (dind)i,
			    args->path);
			rc = -EOPNOTSUPP;
			goto early;
		}

		if (emuxfs_meta_read(&prewr_meta, i, prewr_ino))
			goto fail;
		if (prewr_meta.header.flags != MF_ASSIGNED)
			goto fail;
		prewr_eno = prewr_meta.header.eno;
		if (emuxfs_desc_init_from_stat(
			&prewr_desc, &prewr_st, prewr_eno))
			goto fail;
		memcpy(prewr_desc.content_checksum,
		    &prewr_meta.checksums[chksz], chksz);
		emuxfs_desc_chk_meta(prewr_meta_chk_buf, &prewr_desc, alg);
		if (bcmp(prewr_meta_chk_buf, &prewr_meta.checksums[0], chksz) !=
		    0)
			goto fail;

		wr_desc = prewr_desc;
		wr_meta = prewr_meta;

		switch (args->type) {
		case EMUXFS_UT_CHMOD:
			emuxfs_eids_set();
			subrc = fchmodat(
			    fd, args->path, args->mode, AT_SYMLINK_NOFOLLOW);
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
			wr_desc.mode &= (~(uint64_t)(S_ISUID | S_ISGID));
			break;
		case EMUXFS_UT_UTIMENS:
			emuxfs_eids_set();
			subrc = utimensat(
			    fd, args->path, args->ts, AT_SYMLINK_NOFOLLOW);
			err = errno;
			emuxfs_eids_reset();
			if (subrc)
				subrc = EMUXFS_EFS;
			break;
		case EMUXFS_UT_TRUNCATE:
			subrc = emuxfs_truncate_inner(fd, args, alg, chksz,
			    &prewr_st, &prewr_meta, &wr_meta, &wr_desc, &err,
			    dev->lfile_fd);
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
			/*
			 * A failed truncate or write may already have changed
			 * the file before reporting the error, so it must be
			 * treated as a partial mutation and degraded.  chmod,
			 * chown and utimens are single syscalls: failure means
			 * no change and the operation can be cancelled.
			 */
			if ((args->type == EMUXFS_UT_TRUNCATE) ||
			    (args->type == EMUXFS_UT_WRITE))
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

		if (prewr_desc.type != EMUXFS_DT_LNK) {
			if ((subfd = openat(fd, args->path,
				 O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) == -1)
				goto fail;
			if (fsync(subfd))
				exit(-1);
			if (close(subfd))
				exit(-1);
		}
		if (fsync(dev->meta_fd))
			exit(-1);
		/*
		 * The fault point is after both the content and the metadata
		 * are durable, matching its name and TESTING.md.  A crash here
		 * leaves 'working = 1', so the operation is still uncommitted
		 * and 'sync' rolls it back from an intact mirror.
		 */
		emuxfs_fault_point("update/after_meta");

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
				if (emuxfs_lfile_readback(nullptr, i,
					args->path, mod_begin, mod_end,
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
			if (ckd_add(
				&mod_end, (size_t)args->offset, args->bufsz))
				goto fail;
			mod_size = mod_end;
			if (mod_size < (size_t)prewr_st.st_size)
				mod_size = (size_t)prewr_st.st_size;
			if (mod_size > EMUXFS_BLOCK_SIZE) {
				if (emuxfs_lfile_readback(nullptr, i,
					args->path, mod_begin, mod_end,
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

		has_write = true;
		emuxfs_working_pop(i, now);
		continue;
	fail:
		EMUXFS_TRACE("op_update: fail dev=%lu type=%d path=%s\n",
		    (unsigned long)i, (int)args->type, args->path);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
	early:
		/*
		 * No device mutation has happened on this device yet.  Clear
		 * the recovery marker without advancing 'seq': a failed
		 * operation must not look like an interrupted commit.
		 */
		emuxfs_working_cancel(i);
		return rc;
	}
	if (has_write) {
		if (args->type == EMUXFS_UT_WRITE)
			return (int)args->bufsz;
		return 0;
	}
	return -EIO;
}

/*
 * Rename a node on every usable device.  Rename is decomposed into one
 * directory-entry change at a time: the source is moved to a private
 * temporary, an existing destination is moved to a second temporary, then the
 * source is moved into place.  This keeps each ancestor recomputation's
 * pre-image unambiguous.  Type and emptiness rules are checked before any
 * name is vacated, and a replaced destination's metadata, assignment and
 * large-file tree are cleared only after the new name is committed.
 */
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

	int		   rc, err, subrc;
	bool		   has_write;
	struct stat	   prewr_st;
	ino_t		   prewr_ino;
	struct emuxfs_meta prewr_meta;
	uint64_t	   prewr_eno;
	struct emuxfs_desc prewr_desc;
	uint8_t		   prewr_meta_chk_buf[EMUXFS_CHKSZ_MAX];
	struct stat	   postwr_st;
	struct stat	   dest_st;
	struct emuxfs_meta dest_meta;
	uint64_t	   dest_eno;
	bool		   dest_exists;

	struct emuxfs_cud cud;

	time_t now;

	emuxfs_wrbuf_flush();

	if (emuxfs_path_sanitize(&from))
		return -EIO;
	if (emuxfs_path_sanitize(&to))
		return -EIO;

	/* rename(2) of a path onto itself is a successful no-op. */
	if (strcmp(from, to) == 0)
		return 0;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return -EIO;

	has_write = false;

	now = time(nullptr);

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

		/*
		 * Hard links share one metadata slot and one eno.  Rewriting,
		 * truncating, re-owning or renaming one name would silently
		 * change the other, so refuse before any mutation.
		 */
		if (emuxfs_is_hardlink(&prewr_st)) {
			if (has_write)
				goto fail;
			emuxfs_warn(
			    "Unsupported hard link: %lu:/%s\n", (dind)i, from);
			rc = -EOPNOTSUPP;
			goto early;
		}

		if (emuxfs_meta_read(&prewr_meta, i, prewr_ino))
			goto fail;
		if (prewr_meta.header.flags != MF_ASSIGNED)
			goto fail;
		prewr_eno = prewr_meta.header.eno;
		if (emuxfs_desc_init_from_stat(
			&prewr_desc, &prewr_st, prewr_eno))
			goto fail;
		memcpy(prewr_desc.content_checksum,
		    &prewr_meta.checksums[chksz], chksz);
		emuxfs_desc_chk_meta(prewr_meta_chk_buf, &prewr_desc, alg);
		if (bcmp(prewr_meta_chk_buf, &prewr_meta.checksums[0], chksz) !=
		    0)
			goto fail;

		/*
		 * A rename may replace an existing destination.  Validate the
		 * destination first, then vacate the source and (if present)
		 * the destination one at a time.  The ancestor-recompute
		 * protocol handles exactly one directory-entry change per step;
		 * replacing a destination directly would make the pre-image
		 * ambiguous because the source entry would already stand where
		 * the old destination used to be.
		 */
		dest_exists = false;
		if (fstatat(fd, to, &dest_st, AT_SYMLINK_NOFOLLOW) == 0) {
			dest_exists = true;
			if (emuxfs_is_hardlink(&dest_st)) {
				if (has_write)
					goto fail;
				emuxfs_warn("Unsupported hard link: %lu:/%s\n",
				    (dind)i, to);
				rc = -EOPNOTSUPP;
				goto early;
			}
			if (emuxfs_meta_read(&dest_meta, i, dest_st.st_ino))
				goto fail;
			if (dest_meta.header.flags != MF_ASSIGNED)
				goto fail;
			dest_eno = dest_meta.header.eno;

			/*
			 * Preserve rename(2)'s type rules: a file cannot
			 * replace a directory and a directory cannot replace a
			 * non-directory.  Both directions are checked before
			 * any name is vacated.
			 */
			if (S_ISDIR(dest_st.st_mode) !=
			    S_ISDIR(prewr_st.st_mode)) {
				if (has_write)
					goto fail;
				rc = S_ISDIR(dest_st.st_mode) ? -EISDIR :
								-ENOTDIR;
				goto early;
			}

			/*
			 * rename(2) replaces only an empty directory.  Check
			 * that before moving anything so a non-empty
			 * destination is not silently replaced.
			 */
			if (S_ISDIR(dest_st.st_mode)) {
				struct emuxfs_dir ddir;
				struct dirent	 *de;
				size_t		  di;
				bool		  empty;

				if (emuxfs_pushdir(&ddir, fd, to)) {
					if (has_write)
						goto fail;
					rc = -EIO;
					goto early;
				}
				empty = true;
				for (di = 0; di < ddir.ent_count; ++di) {
					de = ddir.ent_array[di];
					if ((de->d_namlen == 1) &&
					    (de->d_name[0] == '.'))
						continue;
					if ((de->d_namlen == 2) &&
					    (de->d_name[0] == '.') &&
					    (de->d_name[1] == '.'))
						continue;
					/*
					 * Do not skip .muxfs here: a
					 * destination directory containing
					 * one is not empty for rename(2)'s
					 * purposes and must be refused.
					 */
					empty = false;
					break;
				}
				if (emuxfs_popdir(&ddir))
					exit(-1);
				if (!empty) {
					rc = -ENOTEMPTY;
					goto early;
				}
			}
		} else if (errno != ENOENT) {
			if (has_write)
				goto fail;
			rc = -errno;
			goto early;
		}

		/*
		 * First move the source out of the tree.  Its old destination
		 * (if any) is still present, so the ancestor patch for 'from'
		 * sees exactly the expected pre-image.
		 */
		if (renameat(fd, from, dev->muxfs_fd, "rename.tmp"))
			goto fail;
		if (fsync(dev->muxfs_fd))
			exit(-1);
		if (fstatat(fd, from, &postwr_st, AT_SYMLINK_NOFOLLOW) != -1)
			goto fail;
		if (errno != ENOENT)
			goto fail;
		/*
		 * The namespace has changed but the operation is not
		 * committed.  A crash here leaves the source in
		 * '.muxfs/rename.tmp' and 'working = 1'; 'sync' removes the
		 * temporary and rebuilds the device.
		 */
		emuxfs_fault_point("rename/after_vacate");
		cud.type = EMUXFS_CUD_DELETE;
		cud.path = from;
		cud.pre_meta = prewr_meta;
		if (emuxfs_ancestors_meta_recompute(i, &cud))
			goto fail;

		/*
		 * If a destination existed, move it to a second temporary and
		 * account for its deletion.  The source is already out of the
		 * tree, so the root now changes by one entry.
		 */
		if (dest_exists) {
			if (renameat(fd, to, dev->muxfs_fd, "rename.dest.tmp"))
				goto fail;
			if (fsync(dev->muxfs_fd))
				exit(-1);
			if (fstatat(fd, to, &postwr_st, AT_SYMLINK_NOFOLLOW) !=
			    -1)
				goto fail;
			if (errno != ENOENT)
				goto fail;
			cud.type = EMUXFS_CUD_DELETE;
			cud.path = to;
			cud.pre_meta = dest_meta;
			if (emuxfs_ancestors_meta_recompute(i, &cud))
				goto fail;
			emuxfs_fault_point("rename/after_dest");
		}

		/*
		 * The source moves back to its destination.  The destination
		 * (if any) is in the private directory, so the root changes by
		 * one entry again.
		 */
		if (renameat(dev->muxfs_fd, "rename.tmp", fd, to))
			goto fail;
		if (fsync(dev->muxfs_fd))
			exit(-1);
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

		/*
		 * The destination is now committed.  Drop the replaced inode's
		 * large-file tree, metadata and assignment, then remove its
		 * private temporary.  A crash before this block leaves
		 * 'working = 1', so sync rebuilds the device and removes the
		 * temporary.
		 */
		if (dest_exists) {
			struct emuxfs_meta   zero_meta;
			struct emuxfs_assign zero_assign;
			int		     rmflags;

			if (S_ISREG(dest_st.st_mode) &&
			    ((size_t)dest_st.st_size > EMUXFS_BLOCK_SIZE) &&
			    emuxfs_lfile_delete(dev->lfile_fd, dest_st.st_ino))
				goto fail;
			memset(&zero_meta, 0, sizeof(zero_meta));
			memset(&zero_assign, 0, sizeof(zero_assign));
			if (emuxfs_meta_write(&zero_meta, i, dest_st.st_ino))
				goto fail;
			if (emuxfs_assign_write(&zero_assign, i, dest_eno))
				goto fail;
			if (fsync(dev->meta_fd))
				exit(-1);
			if (fsync(dev->assign_fd))
				exit(-1);
			rmflags = S_ISDIR(dest_st.st_mode) ? AT_REMOVEDIR : 0;
			if (unlinkat(dev->muxfs_fd, "rename.dest.tmp", rmflags))
				goto fail;
			if (fsync(dev->muxfs_fd))
				exit(-1);
		}

		emuxfs_fault_point("rename/after");
		has_write = true;
		emuxfs_working_pop(i, now);
		continue;
	fail:
		EMUXFS_TRACE("rename: fail dev=%lu from=%s to=%s\n",
		    (unsigned long)i, from, to);
		emuxfs_degraded_set(i);
		emuxfs_working_pop(i, now);
		continue;
	early:
		/*
		 * No device mutation has happened on this device yet.  Clear
		 * the recovery marker without advancing 'seq': a failed
		 * operation must not look like an interrupted commit.
		 */
		emuxfs_working_cancel(i);
		return rc;
	}
	return has_write ? 0 : -EIO;
}

static int
emuxfs_link(const char *, const char *)
{
	EMUXFS_TRACE("enter");

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
	args.wc = nullptr;

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
	args.wc = nullptr;

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
	args.wc = nullptr;

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
	args.wc = nullptr;

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
	dind		   dev_count;
	dind		   i;
	struct emuxfs_dev *dev;
	int		   err, subrc;

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

/*
 * The FUSE read/write callbacks return an int.  The legacy write-buffer
 * plumbing is still drained at callback entry so that a buffer left by an
 * older in-memory state is never silently ignored; new writes are committed
 * directly in emuxfs_write().
 */
static_assert(EMUXFS_WRBUF_SIZE <= INT_MAX,
    "the sequential write buffer size must fit in an int");

static void
emuxfs_wrbuf_flush(void)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;
	const struct emuxfs_wrbuf   *wr;
	int			     subrc;

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

/*
 * Write callback.  Writes are committed synchronously before success is
 * reported; the legacy global write buffer is only drained first so state left
 * by an older in-memory path is never ignored.  This costs throughput but
 * preserves error attribution and the "success means durable" contract.
 */
static int
emuxfs_write(const char *path, const char *buf, size_t bufsz, off_t offset,
    [[maybe_unused]] struct fuse_file_info *ffi)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_op_update_args args;

	if (emuxfs_path_sanitize(&path))
		return -EIO;

	/* A zero-length write is a no-op and has no buffer to consume. */
	if (bufsz == 0)
		return 0;

	/*
	 * Commit every write before returning success.  The former global
	 * sequential buffer reported success before any device was touched,
	 * flushed another file's data on fsync/flush/release, and killed the
	 * daemon when the deferred commit failed.  A direct update costs
	 * throughput but keeps the write contract and error attribution
	 * correct.
	 */
	if (bufsz > (size_t)INT_MAX)
		return -EINVAL;
	emuxfs_wrbuf_flush();

	args.type = EMUXFS_UT_WRITE;
	args.path = path;
	args.buf = buf;
	args.bufsz = bufsz;
	args.offset = offset;
	args.wc = nullptr;

	return emuxfs_op_update(&args);
}

const struct fuse_operations emuxfs_fuse_ops = {
    /* Pass-through */
    .statfs = emuxfs_statfs,
    /* Stateful */
    .init = emuxfs_fuse_init,
    .destroy = emuxfs_fuse_destroy,
    /* No-ops */
    .releasedir = emuxfs_releasedir,
    /* Create */
    .mknod = emuxfs_mknod,
    .mkdir = emuxfs_mkdir,
    .symlink = emuxfs_symlink,
    /* Read */
    .open = emuxfs_open,
    .opendir = emuxfs_opendir,
    .getattr = emuxfs_getattr,
    .read = emuxfs_read,
    .readlink = emuxfs_readlink,
    .readdir = emuxfs_readdir,
    .access = emuxfs_access,
    /* Update */
    .fsync = emuxfs_fsync,
    .flush = emuxfs_flush,
    .release = emuxfs_release,
    .rename = emuxfs_rename,
    .chmod = emuxfs_chmod,
    .chown = emuxfs_chown,
    .utimens = emuxfs_utimens,
    .truncate = emuxfs_truncate,
    .write = emuxfs_write,
    /* Delete */
    .unlink = emuxfs_unlink,
    .rmdir = emuxfs_rmdir,
    /* Explicitly not supported */
    .link = emuxfs_link,
    .lock = emuxfs_lock,

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
