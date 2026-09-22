/* util.c */
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
#include <sys/types.h>

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chk.h"
#include "ds.h"
#include "fault.h"
#include "emuxfs.h"

struct emuxfs_args emuxfs_cmdline;

static int emuxfs_restore_reg(dind, dind, const char *, int, struct stat *,
    struct emuxfs_meta *);
static int emuxfs_restore_symlink(dind, dind, const char *, struct stat *,
    struct emuxfs_meta *);

/*
 * This enum classifies the difference of a directory's content with respect to
 * the influence of a single specified file.
 */
enum emuxfs_dir_patch_type {
	EMUXFS_SUBSTITUTE, /* The content of the file is different. */
	EMUXFS_PLUS, /* The filename is not listed in the base directory. */
	EMUXFS_MINUS, /* The filename is not listed in the patched directory. */
};
struct emuxfs_dir_patch {
	enum emuxfs_dir_patch_type type;
	const char *fname;
	const uint8_t *sum;
};
static int
emuxfs_dir_patch_sums(uint8_t *as_is_out, uint8_t *with_patch_out,
    enum emuxfs_chk_alg_type alg, dind dev_index, int dirfd,
    struct emuxfs_dir *dir, struct emuxfs_dir_patch *patch)
{
	int rc;
	size_t chksz;
	struct stat subst;
	struct dirent *dirent;
	size_t i, dnamelen, fnamelen, entind1, entind2;
	const char *dname;
	int match;
	ino_t subino;
	struct emuxfs_meta submeta;
	struct emuxfs_chk as_is_content_chk, with_patch_content_chk;
	int patched;

	rc = 1;

	chksz = emuxfs_chk_size(alg);
	fnamelen = strlen(patch->fname);

	emuxfs_chk_init(&as_is_content_chk, alg);
	emuxfs_chk_init(&with_patch_content_chk, alg);

	entind1 = 0;
	for (i = 0; i < dir->ent_count; ++i) {
		dirent = dir->ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 2) == 0))
			continue;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) ==
		    0))
			continue;
		if (strcmp(dname, patch->fname) >= 0)
			break;
		++entind1;
	}

	patched = 0;
	entind2 = 0;
	for (i = 0; i < dir->ent_count; ++i) {
		dirent = dir->ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 2) == 0))
			continue;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) == 0))
			continue;
		if (fstatat(dirfd, dname, &subst, AT_SYMLINK_NOFOLLOW))
			goto out;
		subino = subst.st_ino;
		if (emuxfs_meta_read(&submeta, dev_index, subino))
			goto out;
		emuxfs_chk_update(&as_is_content_chk, (uint8_t *)dname,
		    dnamelen);
		emuxfs_chk_update(&as_is_content_chk, &submeta.checksums[0],
		    chksz);
		if (entind2 == entind1) {
			patched = 1;
			match = ((dnamelen == fnamelen) &&
			    (strncmp(patch->fname, dname, fnamelen) == 0));
			if (patch->type == EMUXFS_MINUS) {
				if (!match)
					goto out;
				++entind2;
				continue;
			}
			emuxfs_chk_update(&with_patch_content_chk,
			    (uint8_t *)patch->fname, fnamelen);
			emuxfs_chk_update(&with_patch_content_chk, patch->sum,
			    chksz);
			switch (patch->type) {
			case EMUXFS_SUBSTITUTE:
				if (!match)
					goto out;
				++entind2;
				continue;
				break;
			case EMUXFS_PLUS:
				if (match)
					goto out;
				break;
			default:
				exit(-1); /* Programming error. */
			}
		}
		emuxfs_chk_update(&with_patch_content_chk, (uint8_t *)dname,
		    dnamelen);
		emuxfs_chk_update(&with_patch_content_chk,
		    &submeta.checksums[0], chksz);
		++entind2;
	}
	if (!patched) {
		if (patch->type != EMUXFS_PLUS)
			goto out;
		emuxfs_chk_update(&with_patch_content_chk,
		    (uint8_t *)patch->fname, fnamelen);
		emuxfs_chk_update(&with_patch_content_chk, patch->sum, chksz);
	}
	emuxfs_chk_final(as_is_out, &as_is_content_chk);
	emuxfs_chk_final(with_patch_out, &with_patch_content_chk);

	rc = 0;
out:
	return rc;
}

EMUXFS int
emuxfs_dir_meta_recompute(struct emuxfs_cud *pcud_out, dind dev_index,
    const struct emuxfs_cud *ccud_in)
{
	int			 rc;
	struct emuxfs_dev	*dev;
	int			 root_fd, dirfd;
	struct stat		 st;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	ino_t			 ino;
	struct emuxfs_dir	 dir;
	struct emuxfs_desc	 pre_desc, post_desc;
	uint64_t		 eno;
	struct emuxfs_meta	 db_pre_meta, pre_meta, post_meta;
	struct emuxfs_dir_patch	 patch;

	rc = 1;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		goto out;
	root_fd = dev->root_fd;
	alg = dev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);

	if (emuxfs_pushdir(&dir, root_fd, ccud_in->path))
		goto out;

	if ((dirfd = openat(root_fd, ccud_in->path, O_RDONLY|O_NOFOLLOW)) == -1)
		goto out2;
	if (fstat(dirfd, &st))
		goto out3;
	if (!S_ISDIR(st.st_mode))
		goto out3;
	ino = st.st_ino;
	if (emuxfs_meta_read(&db_pre_meta, dev_index, ino))
		goto out3;
	eno = db_pre_meta.header.eno;
	if (emuxfs_desc_init_from_stat(&pre_desc, &st, eno))
		goto out3;
	if (emuxfs_desc_init_from_stat(&post_desc, &st, eno))
		goto out3;

	/*
	 * If this seems backwards it is because we are differencing the
	 * directory after having applied the operation.
	 */
	switch (ccud_in->type) {
	case EMUXFS_CUD_CREATE:
		patch.type = EMUXFS_MINUS;
		break;
	case EMUXFS_CUD_UPDATE:
		patch.type = EMUXFS_SUBSTITUTE;
		break;
	case EMUXFS_CUD_DELETE:
		patch.type = EMUXFS_PLUS;
		break;
	default:
		exit(-1); /* Programming error. */
	}
	patch.fname = ccud_in->fname;
	patch.sum = &ccud_in->pre_meta.checksums[0];

	if (emuxfs_dir_patch_sums(post_desc.content_checksum,
	    pre_desc.content_checksum, alg, dev_index, dirfd, &dir,
	    &patch))
		goto out3;

	pre_meta.header = post_meta.header = (struct emuxfs_meta_header){
	    .flags = MF_ASSIGNED,
	    .eno = eno,
	};
	emuxfs_desc_chk_meta(&pre_meta.checksums[0], &pre_desc, alg);
	emuxfs_desc_chk_meta(&post_meta.checksums[0], &post_desc, alg);
	memcpy(&pre_meta.checksums[chksz], pre_desc.content_checksum, chksz);
	memcpy(&post_meta.checksums[chksz], post_desc.content_checksum, chksz);

	if (bcmp(&pre_meta.checksums[0], &db_pre_meta.checksums[0], chksz) != 0)
		goto out3;
	if (bcmp(&pre_meta.checksums[chksz], &db_pre_meta.checksums[chksz],
	    chksz) != 0)
		goto out3;

	if (emuxfs_meta_write(&post_meta, dev_index, ino))
		goto out3;

	if (fsync(dev->meta_fd))
		exit(-1);

	if (emuxfs_readback(dev_index, ccud_in->path, 0, &post_meta))
		goto out3;

	pcud_out->type = EMUXFS_CUD_UPDATE;
	pcud_out->pre_meta = pre_meta;
	rc = 0;
out3:
	if (close(dirfd))
		exit(-1);
out2:
	if (emuxfs_popdir(&dir))
		exit(-1);
out:
	return rc;
}

/*
 * Returns 1 if 'path' points to the root directory, otherwise returns 0.
 * Assumes that 'path' has been through emuxfs_path_sanitize().
 */
static int
emuxfs_path_is_root(const char *path)
{
	return strcmp(".", path) == 0;
}

/*
 * Computes the meta checksum of 'path' in dev 'i'.  If the computed checksum
 * does not match that in the meta.db file then 1 is returned.  Otherwise 0 is
 * returned.  If 'shallow' is non-zero then the content checksum used to
 * compute the meta checksum will be taken from that in the meta.db file,
 * otherwise the content checksum will be computed from the file content.  If
 * 'expected' is not NULL and the computed checksum does not match that in
 * 'expected' then 1 is returned.
 */
EMUXFS int
emuxfs_readback(dind i, const char *path, int shallow,
    const struct emuxfs_meta *expected)
{
	struct emuxfs_dev	*dev;
	int			 root_fd;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	struct stat		 st;
	ino_t			 ino;
	struct emuxfs_meta	 meta;
	uint64_t		 eno;
	struct emuxfs_desc	 desc;
	uint8_t			 meta_chk_buf[EMUXFS_CHKSZ_MAX];

	if (emuxfs_dev_get(&dev, i, 0))
		goto fail;
	root_fd = dev->root_fd;
	alg = dev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);
	if (fstatat(root_fd, path, &st, AT_SYMLINK_NOFOLLOW))
		goto fail;
	ino = st.st_ino;
	if (emuxfs_meta_read(&meta, i, ino))
		goto fail;
	eno = meta.header.eno;
	if (emuxfs_desc_init_from_stat(&desc, &st, eno))
		goto fail;
	if (shallow)
		memcpy(desc.content_checksum, &meta.checksums[chksz], chksz);
	else {
		if (emuxfs_desc_chk_node_content(&desc, i, path))
			goto fail;
	}
	emuxfs_desc_chk_meta(meta_chk_buf, &desc, alg);
	if (bcmp(meta_chk_buf, &meta.checksums[0], chksz) != 0)
		goto fail;
	/*
	 * Cross-check the eno -> ino mapping.  This detects a crash between
	 * the metadata and assign writes, and stale mappings left by inode
	 * reuse.
	 */
	if (emuxfs_assign_validate(i, ino, eno))
		goto fail;
	if ((expected != NULL) &&
	    (bcmp(meta_chk_buf, &expected->checksums[0], chksz) != 0))
		goto fail;
	return 0;
fail:
	return 1;
}

EMUXFS int
emuxfs_parent_readback(dind i, const char *path)
{
	char	 ppath[PATH_MAX];
	size_t	 path_len;

	if (emuxfs_path_is_root(path))
		return 1;

	path_len = strlen(path);
	memcpy(ppath, path, path_len);
	ppath[path_len] = '\0';

	if (emuxfs_path_pop(NULL, ppath, NULL)) {
		memset(ppath, 0, PATH_MAX);
		strlcpy(ppath, ".", PATH_MAX);
	}

	return emuxfs_readback(i, ppath, 0, NULL);
}

static void
emuxfs_path_trailing_seps_strip(char *path, size_t path_len)
{
	char *sep;

	sep = strrchr(path, '/');
	if (sep == NULL)
		return;
	while ((size_t)(sep - path) == path_len) {
		*sep = '\0';
		--path_len;
		if (path_len == 0)
			return;
		sep = strrchr(path, '/');
		if (sep == NULL)
			return;
	}
}

/*
 * Replaces '/' in path with '\0' in order to split the path into directory and
 * filename components.  If fname_out is not NULL then *fname_out is set to
 * point to the start of the filename component after the replaced '/'.
 * Returns 1 if there was not a preceeding path component, otherwise returns 0.
 */
EMUXFS int
emuxfs_path_pop(const char **fname_out, char *path, size_t *path_len_inout)
{
	char	*sep, *fname;
	size_t	 path_len;

	if (path_len_inout != NULL)
		path_len = *path_len_inout;
	else
		path_len = strlen(path);

	emuxfs_path_trailing_seps_strip(path, path_len);
	path_len = strlen(path);
	if (path_len == 0)
		return 1;

	sep = strrchr(path, '/');
	if (sep == NULL)
		return 1;
	fname = sep + 1;
	*sep = '\0';
	path_len = strlen(path);
	if (path_len == 0)
		return 1;
	emuxfs_path_trailing_seps_strip(path, path_len);
	path_len = strlen(path);
	if (path_len == 0)
		return 1;

	if (path_len_inout != NULL)
		*path_len_inout = path_len;
	if (fname_out != NULL)
		*fname_out = fname;
	return 0;
}

/*
 * Sanitize a path supplied by the FUSE frontend.  Leading slashes are
 * stripped, an empty result denotes the root and is returned as ".".  Any
 * path containing a "." or ".." component, an empty ("//") component, or a
 * component named ".muxfs" is rejected: ".muxfs" is the private metadata
 * directory and must never be reachable through the filesystem namespace.
 *
 * The kernel normally hands over canonicalised paths, so a rejection here
 * indicates either a programming error or an attempt to traverse outside the
 * mirrored tree; both warrant failing the operation rather than resolving the
 * path.
 */
EMUXFS int
emuxfs_path_sanitize(const char **path_inout)
{
	const char	*path, *comp, *p;
	size_t		 len;

	path = *path_inout;
	while (path[0] == '/')
		++path;
	if (path[0] == '\0')
		path = ".";

	if (strcmp(path, ".") == 0) {
		*path_inout = path;
		return 0;
	}

	for (comp = p = path;; ++p) {
		if ((*p != '/') && (*p != '\0'))
			continue;

		len = (size_t)(p - comp);
		if (len == 0)
			return 1; /* Empty component ("//"). */
		if ((len == 1) && (comp[0] == '.'))
			return 1;
		if ((len == 2) && (comp[0] == '.') && (comp[1] == '.'))
			return 1;
		if ((len == 6) && (memcmp(comp, ".muxfs", 6) == 0))
			return 1;

		if (*p == '\0')
			break;
		comp = p + 1;
	}

	*path_inout = path;
	return 0;
}

EMUXFS int
emuxfs_ancestors_meta_recompute(dind dev_index, struct emuxfs_cud *cud)
{
	char			 path[PATH_MAX];
	const char		*fname;
	size_t			 path_len;
	struct emuxfs_cud	 pcud, ccud;

	/* There are no ancestors of the root path. */
	if (emuxfs_path_is_root(cud->path))
		return 0;

	ccud = *cud;

	if (strlen(cud->path) >= PATH_MAX)
		exit(-1);

	path_len = strlen(cud->path);
	memcpy(path, cud->path, path_len);
	path[path_len] = '\0';

	while (!emuxfs_path_pop(&fname, path, &path_len)) {
		ccud.path = path;
		ccud.fname = fname;

		if (emuxfs_dir_meta_recompute(&pcud, dev_index, &ccud))
			return 1;

		ccud = pcud;
	}
	if (path_len == 0)
		exit(-1); /* Programming error. */

	/* Account for the special case of the root directory. */
	ccud.path = ".";
	ccud.fname = path;
	if (emuxfs_dir_meta_recompute(&pcud, dev_index, &ccud))
		return 1;

	return 0;
}

EMUXFS int
emuxfs_existsat(int *exists_out, int fd, const char *path)
{
	struct stat st;

	if (fstatat(fd, path, &st, AT_SYMLINK_NOFOLLOW)) {
		if (errno != ENOENT)
			return 1;

		*exists_out = 0;
		return 0;
	}

	*exists_out = 1;
	return 0;
}

EMUXFS int
emuxfs_dir_is_empty(int *empty_out, char const *path)
{
	struct emuxfs_dir dir;
	size_t i, dnamelen;
	const char *dname;
	struct dirent *dirent;
	int empty;

	if (emuxfs_pushdir(&dir, AT_FDCWD, path))
		return 1;

	empty = 1;
	for (i = 0; i < dir.ent_count; ++i) {
		dirent = dir.ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 1) == 0))
			continue;
		empty = 0;
		break;
	}

	if (emuxfs_popdir(&dir))
		exit(-1);

	*empty_out = empty;
	return 0;
}

static int
emuxfs_alphasort(const void *v1, const void *v2)
{
	const struct dirent **d1, **d2;
	d1 = (const struct dirent **)v1;
	d2 = (const struct dirent **)v2;
	return alphasort(d1, d2);
}

/*
 * After use emuxfs_popdir() must be called on 'dir_out' and must be done
 * in-order with respect to all other emuxfs_dspop() and emuxfs_popdir() calls.
 * Read ds.h for more information.
 */
EMUXFS int
emuxfs_pushdir(struct emuxfs_dir *dir_out, int fd, const char *path)
{
	struct stat	  st;
	size_t		  blksz;
	uint8_t		 *dirbuf;
	int		  dirfd;
	struct dirent	 *dirent;
	ssize_t		  rdsz, i, rdend;
	struct dirent	**ent_array;
	size_t		  ent_count, j;

	if (fstatat(fd, path, &st, AT_SYMLINK_NOFOLLOW))
		return 1;
	if (!S_ISDIR(st.st_mode))
		return 1;

	blksz = (size_t)st.st_blksize;
	if (emuxfs_dspush((void **)&dirbuf, blksz)) {
		dprintf(2, "DBG pushdir: dspush dirbuf failed\n");
		exit(-1);
	}

	if ((dirfd = openat(fd, path, O_RDONLY|O_DIRECTORY|O_NOFOLLOW)) == -1)
		goto fail;

	rdend = 0;
	ent_count = 0;
	while ((rdsz = getdents(dirfd, &dirbuf[rdend], blksz)) > 0) {
		for (i = 0; i < rdsz; i += dirent->d_reclen) {
			dirent = (struct dirent *)&dirbuf[i];
			++ent_count;
		}
		rdend += i;
		if (emuxfs_dsgrow((void **)&dirbuf, blksz)) {
			dprintf(2, "DBG pushdir: dsgrow failed\n");
			exit(-1);
		}
	}
	if (rdsz == -1)
		goto fail2;
	if (close(dirfd))
		exit(-1);

	if (emuxfs_dspush((void **)&ent_array,
	    ent_count * sizeof(struct dirent *))) {
		dprintf(2, "DBG pushdir: dspush ent_array failed\n");
		exit(-1);
	}

	for (i = 0, j = 0; i < rdend; i += dirent->d_reclen, ++j)
		dirent = ent_array[j] = (struct dirent *)&dirbuf[i];

	qsort(ent_array, ent_count, sizeof(struct dirent *), emuxfs_alphasort);

	*dir_out = (struct emuxfs_dir){
	    .base = dirbuf,
	    .ent_array = ent_array,
	    .ent_count = ent_count,
	};
	return 0;
fail2:
	if (close(dirfd))
		exit(-1);
fail:
	if (emuxfs_dspop(dirbuf))
		exit(-1);
	return 1;
}

EMUXFS int
emuxfs_popdir(struct emuxfs_dir *dir)
{
	if (emuxfs_dspop(dir->ent_array)) {
		dprintf(2, "DBG popdir: dspop ent_array failed\n");
		exit(-1);
	}
	if (emuxfs_dspop(dir->base)) {
		dprintf(2, "DBG popdir: dspop base failed\n");
		exit(-1);
	}
	return 0;
}

/*
 * 'path' is required to be null-terminated and pointing to a buffer of
 * capacity PATH_MAX, 'len' is provided so that 'path' may be mutated, then
 * returned to its original state.
 */
static int
emuxfs_removeat_impl(int fd, char *path, size_t len)
{
	int			 rc;
	struct stat		 st;
	struct emuxfs_dir	 dir;
	struct dirent		*dirent;
	size_t			 i, sublen, dnamelen;
	const char		*dname;

	if (fstatat(fd, path, &st, AT_SYMLINK_NOFOLLOW))
		return 1;
	if (S_ISDIR(st.st_mode)) {
		rc = 1;
		if (emuxfs_pushdir(&dir, fd, path))
			goto dirout;
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
				goto dirout2;
			sublen = len + 1 + dnamelen;
			if (sublen >= PATH_MAX)
				goto dirout2;
			strlcat(path, "/", PATH_MAX);
			strlcat(path, dname, PATH_MAX);
			if (emuxfs_removeat_impl(fd, path, sublen))
				goto dirout2;
			path[len] = '\0';
		}
		if (unlinkat(fd, path, AT_REMOVEDIR))
			goto dirout2;
		rc = 0;
dirout2:
		if (emuxfs_popdir(&dir))
			exit(-1);
dirout:
		return rc;
	}
	if (!(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)))
		return 1;
	if (unlinkat(fd, path, 0))
		return 1;
	return 0;
}

EMUXFS int
emuxfs_removeat(int fd, const char *_path)
{
	char	 path[PATH_MAX];
	size_t	 len;

	len = strlen(_path);
	memset(path, 0, PATH_MAX);
	memcpy(path, _path, len);

	return emuxfs_removeat_impl(fd, path, len);
}

static int
emuxfs_restore_dir(dind ddev_index, dind sdev_index, const char *path,
    int sfd, struct stat *sst, struct emuxfs_meta *expected)
{
	int			 rc;
	struct emuxfs_dev	*sdev,
				*ddev;
	struct stat		 dst;
	char			 pathbuf[PATH_MAX];
	size_t			 pathlen,
				 dnamelen;
	const char		*dname;
	struct emuxfs_dir	 dir;
	struct dirent		*dirent;
	size_t			 i;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	struct emuxfs_chk	 chk;
	uint8_t			 sum[EMUXFS_CHKSZ_MAX];
	struct stat		 subst;
	ino_t			 subino;
	struct emuxfs_meta	 submeta;
	int			 subfd;
	int			 dfd;
	ino_t			 dino;
	struct emuxfs_assign	 assign;

	int exists;

	rc = 1;
	subfd = -1;

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		goto out;
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		goto out;
	alg = sdev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);
	pathlen = strlen(path);

	if (fstatat(ddev->root_fd, path, &dst, AT_SYMLINK_NOFOLLOW)) {
		if (errno != ENOENT)
			goto out;
		exists = 0;
	} else {
		exists = 1;
		if (!S_ISDIR(dst.st_mode)) {
			if (!(S_ISREG(dst.st_mode) || S_ISLNK(dst.st_mode)))
				goto out;
			if (unlinkat(ddev->root_fd, path, 0))
				goto out;
			exists = 0;
		}
	}

	if (!exists) {
		if (mkdirat(ddev->root_fd, path, sst->st_mode))
			goto out;
	}

	/* Chmod first to prevent privilege escalation. */
	if (fchmodat(ddev->root_fd, path, sst->st_mode, AT_SYMLINK_NOFOLLOW))
		goto out;
	if (fchownat(ddev->root_fd, path, sst->st_uid, sst->st_gid,
	    AT_SYMLINK_NOFOLLOW))
		goto out;
	/* Chmod again since chown can unset suid/sgid bits. */
	if (fchmodat(ddev->root_fd, path, sst->st_mode, AT_SYMLINK_NOFOLLOW))
		goto out;

	/*
	 * Timestamps are not checksummed in format version 1, but a repair
	 * should still reproduce the source times as far as possible.
	 */
	{
		struct timespec ts[2];

		ts[0] = sst->st_atim;
		ts[1] = sst->st_mtim;
		if (utimensat(ddev->root_fd, path, ts, AT_SYMLINK_NOFOLLOW))
			goto out;
	}

	if (emuxfs_pushdir(&dir, sdev->root_fd, path))
		goto out;

	emuxfs_chk_init(&chk, alg);
	for (i = 0; i < dir.ent_count; ++i) {
		dirent = dir.ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 2) == 0))
			continue;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) == 0))
			continue;
		if (pathlen + 1 + dnamelen >= PATH_MAX)
			goto out2;
		if (fstatat(sfd, dname, &subst, AT_SYMLINK_NOFOLLOW))
			goto out2;
		subino = subst.st_ino;
		if (emuxfs_meta_read(&submeta, sdev_index, subino))
			goto out2;
		emuxfs_chk_update(&chk, (uint8_t *)dname, dnamelen);
		emuxfs_chk_update(&chk, &submeta.checksums[0], chksz);
	}
	emuxfs_chk_final(sum, &chk);
	if (bcmp(sum, &expected->checksums[chksz], chksz) != 0) {
		if (emuxfs_state_restore_push_back(sdev_index, path))
			exit(-1);
		goto out2;
	}

	for (i = 0; i < dir.ent_count; ++i) {
		dirent = dir.ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 1) == 0))
			continue;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) == 0))
			continue;
		if (pathlen + 1 + dnamelen >= PATH_MAX)
			goto out2;
		memset(pathbuf, 0, PATH_MAX);
		strlcpy(pathbuf, path, PATH_MAX);
		strlcat(pathbuf, "/", PATH_MAX);
		strlcat(pathbuf, dname, PATH_MAX);
		if (fstatat(sfd, dname, &subst, AT_SYMLINK_NOFOLLOW))
			goto out2;
		subino = subst.st_ino;
		if (emuxfs_meta_read(&submeta, sdev_index, subino))
			goto out2;
		if (S_ISLNK(subst.st_mode)) {
			if (emuxfs_restore_symlink(ddev_index, sdev_index,
			    pathbuf, &subst, &submeta))
				goto subout;
		} else {
			if ((subfd = openat(sdev->root_fd, pathbuf,
			    O_RDONLY|O_NOFOLLOW)) == -1)
				goto out2;
			if (S_ISDIR(subst.st_mode)) {
				if (emuxfs_restore_dir(ddev_index, sdev_index,
				    pathbuf, subfd, &subst, &submeta))
					goto subout;
			} else if (S_ISREG(subst.st_mode)) {
				if (emuxfs_restore_reg(ddev_index, sdev_index,
				    pathbuf, subfd, &subst, &submeta))
					goto subout;
			} else {
				if (emuxfs_state_restore_push_back(sdev_index,
				    pathbuf))
					exit(-1);
				goto subout;
			}
		}
		if (emuxfs_readback(ddev_index, pathbuf, 0, &submeta))
			goto subout;

		if (subfd != -1) {
			if (close(subfd))
				exit(-1);
			subfd = -1;
		}
		continue;
subout:
		if (subfd != -1) {
			if (close(subfd))
				exit(-1);
			subfd = -1;
		}
		goto out2;
	}

	if (emuxfs_popdir(&dir))
		exit(-1);
	if (emuxfs_pushdir(&dir, ddev->root_fd, path))
		goto out;

	for (i = 0; i < dir.ent_count; ++i) {
		dirent = dir.ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 1) == 0))
			continue;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) == 0))
			continue;
		if (emuxfs_existsat(&exists, sfd, dname))
			goto out2;
		if (!exists) {
			memset(pathbuf, 0, PATH_MAX);
			if (pathlen + 1 + dnamelen >= PATH_MAX)
				goto out2;
			strlcpy(pathbuf, path, PATH_MAX);
			strlcat(pathbuf, "/", PATH_MAX);
			strlcat(pathbuf, dname, PATH_MAX);
			emuxfs_removeat(ddev->root_fd, pathbuf);
		}
	}

	if ((dfd = openat(ddev->root_fd, path,
	    O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) == -1)
		goto out2;
	if (fstat(dfd, &dst))
		goto out3;
	dino = dst.st_ino;
	assign = (struct emuxfs_assign){
	    .flags = AF_ASSIGNED,
	    .ino = dino
	};
	if (emuxfs_meta_write(expected, ddev_index, dino))
		goto out3;
	if (emuxfs_assign_write(&assign, ddev_index, expected->header.eno))
		goto out3;
	if (fsync(dfd))
		exit(-1);
	if (fsync(ddev->meta_fd))
		exit(-1);
	if (fsync(ddev->assign_fd))
		exit(-1);
	if (emuxfs_fsync_parent(ddev->root_fd, path))
		goto out3;

	rc = 0;
out3:
	if (close(dfd))
		exit(-1);
out2:
	if (emuxfs_popdir(&dir))
		exit(-1);
out:
	return rc;
}

static int
emuxfs_copy_reg(int dfd, int sfd, size_t content_sz)
{
	int rc;
	struct emuxfs_range r;
	size_t i_offset, txsz;
	uint64_t i;
	uint8_t content_buf[EMUXFS_BLOCK_SIZE];

	rc = 1;

	r.byte_begin = 0;
	r.byte_end = content_sz;
	emuxfs_range_compute(&r, 0);

	for (i = r.blk_index_begin; i < r.blk_index_end; ++i) {
		i_offset = i * EMUXFS_BLOCK_SIZE;
		txsz = EMUXFS_BLOCK_SIZE;
		if (i_offset + txsz > content_sz)
			txsz = content_sz - i_offset;
		if (emuxfs_pread_exact(sfd, content_buf, txsz, (off_t)i_offset))
			goto out;
		if (emuxfs_pwrite_exact(dfd, content_buf, txsz,
		    (off_t)i_offset))
			goto out;
	}

	rc = 0;
out:
	return rc;
}

static int
emuxfs_restore_reg(dind ddev_index, dind sdev_index, const char *path,
    int sfd, struct stat *sst, struct emuxfs_meta *expected)
{
	int			 rc;
	struct emuxfs_dev	*sdev,
				*ddev;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	uint64_t		 eno;
	size_t			 content_sz;
	int			 dfd;
	struct stat		 dst;
	ino_t			 dino;
	struct emuxfs_assign	 assign;
	struct stat		 slfile_st;
	int			 dlfd,
				 slfd;
	int			 exists;

	rc = 1;
	dfd = -1;
	dlfd = -1;
	slfd = -1;

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		goto out;
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		goto out;
	alg = sdev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);
	eno = expected->header.eno;
	if (sst->st_size < 0)
		goto out;
	content_sz = (size_t)sst->st_size;

	if (emuxfs_readback(sdev_index, path, 0, expected))
		goto out;

	/*
	 * Refuse hard-linked nodes.  A source name shares its inode (and
	 * metadata) with every other name, so restoring it as an independent
	 * file would silently break the relationship.  Refusing is safer than
	 * destroying a link.
	 */
	if (emuxfs_is_hardlink(sst)) {
		emuxfs_alert("Refusing to restore hard link: %lu:/%s\n",
		    (dind)sdev_index, path);
		goto out;
	}

	/*
	 * If the destination node exists but is not a regular file, remove it
	 * first so that openat(2) with O_NOFOLLOW below cannot be redirected
	 * by a stale symlink.
	 */
	if (fstatat(ddev->root_fd, path, &dst, AT_SYMLINK_NOFOLLOW) == 0) {
		if (emuxfs_is_hardlink(&dst)) {
			emuxfs_alert("Refusing to overwrite hard link: "
			    "%lu:/%s\n", (dind)ddev_index, path);
			goto out;
		}
		if (!S_ISREG(dst.st_mode)) {
			if (emuxfs_removeat(ddev->root_fd, path))
				goto out;
		}
	}

	if ((dfd = openat(ddev->root_fd, path,
	    O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,
	    sst->st_mode)) == -1)
		goto out;
	if (emuxfs_copy_reg(dfd, sfd, content_sz))
		goto out;

	/* chmod before chown (chown may clear setuid/setgid). */
	if (fchmod(dfd, sst->st_mode & 07777))
		goto out;
	if (fchown(dfd, sst->st_uid, sst->st_gid))
		goto out;
	if (fchmod(dfd, sst->st_mode & 07777))
		goto out;
	{
		struct timespec ts[2];

		ts[0] = sst->st_atim;
		ts[1] = sst->st_mtim;
		if (futimens(dfd, ts))
			goto out;
	}
	if (fstat(dfd, &dst))
		goto out;
	dino = dst.st_ino;

	if (emuxfs_lfile_exists(&exists, ddev->lfile_fd, dino))
		goto out;
	if (exists && emuxfs_lfile_delete(ddev->lfile_fd, dino))
		goto out;
	if (content_sz > EMUXFS_BLOCK_SIZE) {
		if (emuxfs_lfile_create(ddev->lfile_fd, chksz, dino,
		    content_sz))
			goto out;
		if (emuxfs_lfile_open(&dlfd, ddev->lfile_fd, dino, O_WRONLY))
			goto out;
		if (emuxfs_lfile_open(&slfd, sdev->lfile_fd, sst->st_ino,
		    O_RDONLY))
			goto out;
		if (fstat(slfd, &slfile_st))
			goto out;
		if (emuxfs_copy_reg(dlfd, slfd, (size_t)slfile_st.st_size))
			goto out;
	}
	if (emuxfs_meta_write(expected, ddev_index, dino))
		goto out;
	emuxfs_fault_point("restore/after_meta");
	assign = (struct emuxfs_assign){
	    .flags = AF_ASSIGNED,
	    .ino = dino
	};
	if (emuxfs_assign_write(&assign, ddev_index, eno))
		goto out;

	if (fsync(dfd))
		exit(-1);
	if (fsync(ddev->meta_fd))
		exit(-1);
	if (fsync(ddev->assign_fd))
		exit(-1);
	if (emuxfs_fsync_parent(ddev->root_fd, path))
		goto out;

	rc = 0;
out:
	if (slfd != -1) {
		if (close(slfd))
			exit(-1);
	}
	if (dlfd != -1) {
		if (close(dlfd))
			exit(-1);
	}
	if (dfd != -1) {
		if (close(dfd))
			exit(-1);
	}
	return rc;
}

static int
emuxfs_restore_symlink(dind ddev_index, dind sdev_index, const char *path,
    struct stat *sst, struct emuxfs_meta *expected)
{
	int			 rc;
	struct emuxfs_dev	*sdev,
				*ddev;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	uint64_t		 eno;
	char			 content_buf[PATH_MAX];
	ssize_t			 content_sz;
	struct emuxfs_chk	 chk;
	uint8_t			 sum[EMUXFS_CHKSZ_MAX];
	struct stat		 dst;
	ino_t			 dino;
	struct emuxfs_assign	 assign;
	int			 exists;

	rc = 1;

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		goto out;
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		goto out;
	alg = sdev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);
	eno = expected->header.eno;

	memset(content_buf, 0, PATH_MAX);
	if ((content_sz = readlinkat(sdev->root_fd, path, content_buf,
	    PATH_MAX - 1)) == -1)
		goto out;
	if (content_sz >= PATH_MAX)
		goto out;
	emuxfs_chk_init(&chk, alg);
	emuxfs_chk_update(&chk, (uint8_t *)content_buf, (size_t)content_sz);
	emuxfs_chk_final(sum, &chk);
	if (bcmp(sum, &expected->checksums[chksz], chksz) != 0) {
		if (emuxfs_state_restore_push_back(sdev_index, path))
			exit(-1);
		goto out;
	}

	if (emuxfs_existsat(&exists, ddev->root_fd, path))
		goto out;
	if (exists) {
		if (emuxfs_removeat(ddev->root_fd, path))
			goto out;
	}
	if (symlinkat(content_buf, ddev->root_fd, path))
		goto out;

	if (fchmodat(ddev->root_fd, path, sst->st_mode, AT_SYMLINK_NOFOLLOW))
		goto out;
	if (fchownat(ddev->root_fd, path, sst->st_uid, sst->st_gid,
	    AT_SYMLINK_NOFOLLOW))
		goto out;
	{
		struct timespec ts[2];

		ts[0] = sst->st_atim;
		ts[1] = sst->st_mtim;
		if (utimensat(ddev->root_fd, path, ts, AT_SYMLINK_NOFOLLOW))
			goto out;
	}
	if (fstatat(ddev->root_fd, path, &dst, AT_SYMLINK_NOFOLLOW))
		goto out;
	dino = dst.st_ino;

	assign = (struct emuxfs_assign){
	    .flags = AF_ASSIGNED,
	    .ino = dino
	};
	if (emuxfs_meta_write(expected, ddev_index, dino))
		goto out;
	if (emuxfs_assign_write(&assign, ddev_index, eno))
		goto out;

	if (fsync(ddev->meta_fd))
		exit(-1);
	if (fsync(ddev->assign_fd))
		exit(-1);
	if (emuxfs_fsync_parent(ddev->root_fd, path))
		goto out;

	rc = 0;
out:
	return rc;
}

static int
emuxfs_restore_possible_inner(int *is_delete_out, dind ddev_index,
    dind sdev_index, const char *path, struct emuxfs_dev *ddev,
    struct emuxfs_dev *sdev, enum emuxfs_chk_alg_type alg, size_t chksz,
    const char *ppath, struct emuxfs_dir_patch *patch, int expect_substitute)
{
	int			 rc, err, is_delete, exists;
	struct emuxfs_meta	 smeta, spmeta, dpmeta;
	struct emuxfs_dir	 ddir;
	int			 dpfd;
	struct stat		 sst, spst, dst, dpst;
	ino_t			 sino, spino, dpino;
	uint8_t			 as_is_sum[EMUXFS_CHKSZ_MAX];
	uint8_t			 with_patch_sum[EMUXFS_CHKSZ_MAX];
	uint8_t			 as_is_meta_sum[EMUXFS_CHKSZ_MAX];
	uint64_t		 dpeno;
	struct emuxfs_desc	 dpdesc;

	rc = 1;

	if (emuxfs_existsat(&exists, ddev->root_fd, ppath))
		goto out;
	if (!exists) {
		if (emuxfs_state_restore_push_back(ddev_index, ppath))
			exit(-1);
		rc = 2;
		goto out;
	}
	if (emuxfs_existsat(&exists, sdev->root_fd, ppath))
		goto out;
	if (!exists) {
		if (emuxfs_state_restore_push_back(sdev_index, ppath))
			exit(-1);
		rc = 3;
		goto out;
	}
	if (emuxfs_readback(sdev_index, ppath, 0, NULL)) {
		if (emuxfs_state_restore_push_back(sdev_index, ppath))
			exit(-1);
		rc = 3;
		goto out;
	}
	exists = 1;
	if (fstatat(sdev->root_fd, path, &sst, AT_SYMLINK_NOFOLLOW)) {
		err = errno;
		if (err == ENOENT)
			exists = 0;
		else
			goto out;
	}
	if (exists) {
		if (!expect_substitute) {
			if (emuxfs_readback(sdev_index, path, 0, NULL)) {
				if (emuxfs_state_restore_push_back(sdev_index,
				    path))
					exit(-1);
				rc = 3;
				goto out;
			}
		}
		sino = sst.st_ino;
		if (emuxfs_meta_read(&smeta, sdev_index, sino))
			goto out;
		patch->sum = &smeta.checksums[0];
		patch->type = EMUXFS_SUBSTITUTE;
	} else {
		if (expect_substitute)
			goto out;
		patch->type = EMUXFS_MINUS;
	}
	is_delete = !exists;

	exists = 1;
	if (fstatat(ddev->root_fd, path, &dst, AT_SYMLINK_NOFOLLOW)) {
		err = errno;
		if (err == ENOENT)
			exists = 0;
		else
			goto out;
	}
	if (exists) {
		/* Do nothing. */
	} else if (patch->type == EMUXFS_MINUS) {
		/*
		 * It is likely that the restore has already been done,
		 * and that the metadata checksum will be tested below to
		 * confirm this, so there is nothing to do here.
		 */
	} else {
		if (patch->type != EMUXFS_SUBSTITUTE)
			exit(-1); /* Programming error. */
		if (expect_substitute)
			goto out;
		patch->type = EMUXFS_PLUS;
	}

	exists = 1;
	if (fstatat(sdev->root_fd, ppath, &spst, AT_SYMLINK_NOFOLLOW))
		goto out;
	spino = spst.st_ino;
	if (emuxfs_meta_read(&spmeta, sdev_index, spino))
		goto out;

	if (emuxfs_pushdir(&ddir, ddev->root_fd, ppath))
		goto out;
	if ((dpfd = openat(ddev->root_fd, ppath, O_RDONLY|O_NOFOLLOW)) == -1)
		goto out2;

	if (emuxfs_dir_patch_sums(as_is_sum, with_patch_sum, alg, ddev_index,
	    dpfd, &ddir, patch))
		goto out3;
	if (bcmp(with_patch_sum, &spmeta.checksums[chksz], chksz) != 0) {
		if (emuxfs_state_restore_push_back(ddev_index, ppath))
			exit(-1);
		rc = 2;
		goto out3;
	}
	/*
	 * The unnecessary restore check will remain disabled until readback
	 * depth is implemented.
	 */
	if (0 && bcmp(with_patch_sum, as_is_sum, chksz) == 0) {
		if (fstatat(ddev->root_fd, ppath, &dpst, AT_SYMLINK_NOFOLLOW))
			goto out3;
		dpino = dpst.st_ino;
		if (emuxfs_meta_read(&dpmeta, ddev_index, dpino))
			goto out3;
		dpeno = dpmeta.header.eno;
		if (emuxfs_desc_init_from_stat(&dpdesc, &dpst, dpeno))
			goto out3;
		memcpy(dpdesc.content_checksum, as_is_sum, chksz);
		emuxfs_desc_chk_meta(as_is_meta_sum, &dpdesc, alg);
		if (bcmp(as_is_meta_sum, &spmeta.checksums[0], chksz) == 0) {
			rc = 4;
			goto out3;
		}
	}

	if (is_delete_out != NULL)
		*is_delete_out = is_delete;
	rc = 0;
out3:
	if (close(dpfd))
		exit(-1);
out2:
	if (emuxfs_popdir(&ddir))
		exit(-1);
out:
	return rc;
}

/*
 * This function performs preliminary checks on both sdev and ddev, ensuring
 * that either that the restore can be performed, or if not that the
 * restoration needed has been queued.  It also informs whether or not the
 * restore would be a delete operation.
 * Returns: 0 on success, 1 on hard failure, 2 on ddev restore delegated, 3 on
 * sdev corrupted and ddev restore queued, 4 on restore unnecessary (already
 * done).
 */
static int
emuxfs_restore_possible(int *is_delete_out, dind ddev_index, dind sdev_index,
    const char *_path)
{
	int			 is_first, is_last, is_unnecessary, subrc,
				 is_delete;
	char			 path[PATH_MAX], ppathbuf[PATH_MAX];
	const char		*ppath, *fname;
	size_t			 ppathlen;
	struct emuxfs_dev	*ddev, *sdev;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	struct emuxfs_dir_patch	 patch;

	memset(path, 0, PATH_MAX);
	strlcpy(path, _path, PATH_MAX);

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		return 1;
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		return 1;
	alg = ddev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);

	if (emuxfs_path_is_root(path)) {
		if (emuxfs_readback(sdev_index, path, 0, NULL)) {
			if (emuxfs_state_restore_push_back(sdev_index, path))
				exit(-1);
			return 3;
		}
		*is_delete_out = 0;
		return 0;
	}

	memset(ppathbuf, 0, PATH_MAX);
	strlcpy(ppathbuf, path, PATH_MAX);
	ppath = ppathbuf;
	ppathlen = strlen(ppath);
	is_first = 1;
	is_last = 0;
	is_unnecessary = 1;
	for (;;) {
		if (emuxfs_path_pop(&fname, ppathbuf, &ppathlen)) {
			patch.fname = ppathbuf;
			ppath = ".";
			ppathlen = strlen(ppath);
			is_last = 1;
		} else
			patch.fname = fname;

		subrc = emuxfs_restore_possible_inner((is_first ? &is_delete :
		    NULL), ddev_index, sdev_index, path, ddev, sdev, alg, chksz,
		    ppath, &patch, !is_first);

		switch (subrc) {
		case 0:
			is_unnecessary = 0;
			break;
		case 1:
		case 2:
		case 3:
			return subrc;
		case 4:
			break;
		default:
			exit(-1); /* Programming error. */
		}
		if (is_last)
			break;
		memset(path, 0, PATH_MAX);
		strlcpy(path, ppath, PATH_MAX);
		is_first = 0;
	}
	*is_delete_out = is_delete;
	return is_unnecessary ? 4 : 0;
}

static int
emuxfs_restore_delete(dind ddev_index, dind sdev_index, const char *path)
{
	int			 rc;
	struct emuxfs_dev	*ddev;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz;
	int			 dpfd;
	char			 ppath[PATH_MAX];
	struct emuxfs_dev	*sdev;
	struct stat		 spst;
	ino_t			 spino;
	struct emuxfs_meta	 spmeta, dpmeta;
	struct stat		 dpst;
	ino_t			 dpino;
	int			 exists;

	rc = 1;

	if (emuxfs_path_is_root(path))
		goto out;

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		goto out;
	alg = ddev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);

	if (emuxfs_removeat(ddev->root_fd, path))
		goto out;
	if (emuxfs_fsync_parent(ddev->root_fd, path))
		goto out;

	memset(ppath, 0, PATH_MAX);
	strlcpy(ppath, path, PATH_MAX);
	if (emuxfs_path_pop(NULL, ppath, NULL)) {
		memset(ppath, 0, PATH_MAX);
		strlcpy(ppath, ".", PATH_MAX);
	}

	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		goto out;
	if (fstatat(sdev->root_fd, ppath, &spst, AT_SYMLINK_NOFOLLOW))
		goto out;
	spino = spst.st_ino;
	if (emuxfs_meta_read(&spmeta, sdev_index, spino))
		goto out;

	if ((dpfd = openat(ddev->root_fd, ppath,
	    O_RDONLY|O_NOFOLLOW|O_CLOEXEC)) == -1)
		goto out;
	if (fstat(dpfd, &dpst))
		goto out2;
	dpino = dpst.st_ino;
	if (emuxfs_meta_read(&dpmeta, ddev_index, dpino))
		goto out2;

	if (dpmeta.header.eno != spmeta.header.eno)
		goto out2;
	memcpy(&dpmeta.checksums[0], &spmeta.checksums[0], 2 * chksz);
	if (emuxfs_meta_write(&dpmeta, ddev_index, dpino))
		goto out2;

	if (fsync(dpfd))
		exit(-1);
	if (fsync(ddev->meta_fd))
		exit(-1);

	if (emuxfs_existsat(&exists, ddev->root_fd, path))
		goto out2;
	if (exists)
		goto out2;

	if (emuxfs_readback(ddev_index, ppath, 0, &spmeta))
		goto out2;

	rc = 0;
out2:
	if (close(dpfd))
		exit(-1);
out:
	return rc;
}

static int
emuxfs_dir_meta_restore(dind ddev_index, dind sdev_index, const char *path)
{
	struct emuxfs_dev	*ddev, *sdev;
	struct stat		 dst, sst;
	ino_t			 dino, sino;
	struct emuxfs_meta	 meta;

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		return 1;
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		return 1;
	if (fstatat(ddev->root_fd, path, &dst, AT_SYMLINK_NOFOLLOW))
		return 1;
	if (fstatat(sdev->root_fd, path, &sst, AT_SYMLINK_NOFOLLOW))
		return 1;
	dino = dst.st_ino;
	sino = sst.st_ino;
	if (emuxfs_meta_read(&meta, sdev_index, sino))
		return 1;
	if (emuxfs_meta_write(&meta, ddev_index, dino))
		return 1;
	if (fsync(ddev->meta_fd))
		exit(-1);
	if (emuxfs_readback(ddev_index, path, 0, NULL))
		return 1;
	return 0;
}

static int
emuxfs_ancestors_meta_restore(dind ddev_index, dind sdev_index,
    const char *_path)
{
	char		 path[PATH_MAX];
	const char	*fname;
	size_t		 path_len;

	/* There are no ancestors of the root path. */
	if (emuxfs_path_is_root(_path))
		return 0;

	if (strlen(_path) >= PATH_MAX)
		exit(-1);

	path_len = strlen(_path);
	memcpy(path, _path, path_len);
	path[path_len] = '\0';

	while (!emuxfs_path_pop(&fname, path, &path_len)) {
		if (emuxfs_dir_meta_restore(ddev_index, sdev_index, path))
			return 1;
	}
	if (path_len == 0)
		exit(-1); /* Programming error. */

	/* Account for the special case of the root directory. */
	if (emuxfs_dir_meta_restore(ddev_index, sdev_index, "."))
		return 1;

	return 0;
}

/*
 * Detect *coherent divergence*: two or more mounted devices holding an
 * internally valid but different copy of 'path'.  A valid copy here means
 * that the node passes its own metadata and content checksums; it does not
 * prove that the copy is the newest or the correct one.  When copies
 * disagree, no automatic repair is safe, so the caller must refuse.
 *
 * Returns 0 on success (with *ambiguous_out set), 1 on internal error.
 */
static int
emuxfs_restore_source_ambiguous(int *ambiguous_out, dind ddev_index,
    const char *path)
{
	struct emuxfs_dev	*sdev;
	struct emuxfs_meta	 meta;
	struct stat		 st;
	uint8_t			 first[EMUXFS_CHKSZ_MAX];
	size_t			 chksz;
	dind			 si, dev_count;
	int			 have_first, ambiguous;

	*ambiguous_out = 0;
	have_first = 0;
	ambiguous = 0;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return 1;

	for (si = 0; si < dev_count; ++si) {
		if (si == ddev_index)
			continue;
		if (emuxfs_dev_get(&sdev, si, 0))
			continue;
		if (emuxfs_readback(si, path, 0, NULL))
			continue; /* Not a valid copy; not eligible. */
		if (fstatat(sdev->root_fd, path, &st, AT_SYMLINK_NOFOLLOW))
			continue;
		if (emuxfs_meta_read(&meta, si, st.st_ino))
			continue;
		chksz = emuxfs_chk_size(sdev->conf.chk_alg_type);
		if (!have_first) {
			memcpy(first, &meta.checksums[0], chksz);
			have_first = 1;
			continue;
		}
		if (bcmp(first, &meta.checksums[0], chksz) != 0)
			ambiguous = 1;
	}

	*ambiguous_out = ambiguous;
	return 0;
}

static int
emuxfs_restore_impl(dind ddev_index, const char *path)
{
	struct emuxfs_dev	*ddev, *sdev;
	int			 is_delete;
	int			 ambiguous;
	dind			 si, dev_count;
	int			 sfd;
	struct stat		 sst;
	ino_t			 sino;
	struct emuxfs_meta	 smeta;

	if ((dev_count = emuxfs_dev_count()) == 0)
		return 1;
	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		return 1;

	/*
	 * Refuse to choose between copies that are all internally valid but
	 * different.  This is the decisive check that prevents a repair from
	 * confidently destroying the only correct copy.  `sync` (the
	 * explicit, administrator-directed recovery path) is exempt: there the
	 * operator designates the source.
	 */
	if (!emuxfs_state_is_restore_only()) {
		if (emuxfs_restore_source_ambiguous(&ambiguous, ddev_index,
		    path))
			return 1;
		if (ambiguous)
			return 5; /* Ambiguous: no repair performed. */
	}

	for (si = 0; si < dev_count; ++si) {
		if (si == ddev_index)
			continue;
		if (emuxfs_dev_get(&sdev, si, 0))
			continue;

		sfd = -1;

		switch (emuxfs_restore_possible(&is_delete, ddev_index, si,
		    path)) {
		case 0:
			break; /* Restore is possible. */
		case 1:
			return 1; /* Hard failure. */
		case 2:
			return 2; /* Delegated. */
		case 3:
			goto fail; /* sdev is corrupted. */
		case 4:
			return 4; /* Unnecessary. */
		default:
			exit(-1); /* Programming error. */
		}

		emuxfs_fault_point("restore/before");
		if (is_delete) {
			if (emuxfs_restore_delete(ddev_index, si, path))
				goto fail;
			if (emuxfs_ancestors_meta_restore(ddev_index, si, path))
				goto fail;
			return 0;
		}

		if (fstatat(sdev->root_fd, path, &sst, AT_SYMLINK_NOFOLLOW))
			goto fail2;
		sino = sst.st_ino;
		if (emuxfs_meta_read(&smeta, si, sino))
			goto fail2;

		if (S_ISLNK(sst.st_mode)) {
			if (emuxfs_restore_symlink(ddev_index, si, path, &sst,
			    &smeta))
				goto fail2;
		} else {
			if ((sfd = openat(sdev->root_fd, path,
			    O_RDONLY|O_NOFOLLOW|O_CLOEXEC)) == -1)
				goto fail;
			if (S_ISDIR(sst.st_mode)) {
				if (emuxfs_restore_dir(ddev_index, si, path,
				    sfd, &sst, &smeta))
					goto fail2;
			} else if (S_ISREG(sst.st_mode)) {
				if (emuxfs_restore_reg(ddev_index, si, path,
				    sfd, &sst, &smeta))
					goto fail2;
			} else {
				if (emuxfs_state_restore_push_back(si, path))
					exit(-1);
				goto fail2;
			}
		}
		if (sfd != -1) {
			if (close(sfd))
				exit(-1);
			sfd = -1;
		}
		if (emuxfs_ancestors_meta_restore(ddev_index, si, path))
			goto fail2;
		if (emuxfs_readback(ddev_index, path, 0, &smeta))
			goto fail2;

		emuxfs_fault_point("restore/after");
		return 0;
fail2:
		if (sfd != -1) {
			if (close(sfd))
				exit(-1);
			sfd = -1;
		}
fail:
		continue;
	}
	return 1;
}

EMUXFS void
emuxfs_restore_now(void)
{
	dind ddev_index;
	char path[PATH_MAX];

	memset(path, 0, PATH_MAX);
	while (!emuxfs_state_restore_pop_front(&ddev_index, path)) {
		emuxfs_info("Restoring: %lu:/%s\n", ddev_index, path);
		if (emuxfs_restoring_push(ddev_index))
			continue; /* Device unavailable; nothing pushed. */
		switch (emuxfs_restore_impl(ddev_index, path)) {
		case 0:
			emuxfs_info("Restored: %lu:/%s\n",
			    ddev_index, path);
			break;
		case 1:
			emuxfs_alert("Restoration Failure: %lu:/%s\n",
			    ddev_index, path);
			goto fail;
		case 2:
			goto next;
		case 4:
			goto next;
		case 5:
			/*
			 * Several internally valid copies disagree.  Do not
			 * overwrite anything; record the ambiguity so that an
			 * operator is required to resolve it (for example with
			 * 'emuxfs sync destination source').
			 */
			emuxfs_alert("Ambiguous copies, refusing to restore: "
			    "%lu:/%s\n", ddev_index, path);
			emuxfs_state_ambiguity_note();
			goto fail;
		default:
			exit(-1); /* Programming error. */
		}
		goto next;
fail:
		emuxfs_degraded_set(ddev_index);
next:
		emuxfs_restoring_pop(ddev_index);
		memset(path, 0, PATH_MAX);
	}
}

EMUXFS int
emuxfs_parent_gid(gid_t *parent_gid_out, const char *path)
{
	char			 pbuf[PATH_MAX], *ppath;
	size_t			 ppathlen;
	struct emuxfs_dev	*dev;
	dind			 dev_count, i;
	struct stat		 st;

	ppathlen = strlen(path);
	if (ppathlen >= PATH_MAX)
		return 1;
	ppath = pbuf;
	memset(ppath, 0, PATH_MAX);
	strlcpy(ppath, path, PATH_MAX);
	if (emuxfs_path_pop(NULL, ppath, &ppathlen)) {
		ppath = ".";
		ppathlen = 1;
	}

	if ((dev_count = emuxfs_dev_count()) == 0)
		return 1;
	for (i = 0; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			continue;
		if (fstatat(dev->root_fd, ppath, &st, AT_SYMLINK_NOFOLLOW))
			continue;
		*parent_gid_out = st.st_gid;
		return 0;
	}
	return 1;
}

EMUXFS int
emuxfs_dir_content_chk(uint8_t *sum_out, dind dev_index, struct emuxfs_dir *dir)
{
	int			 rc;
	struct dirent		*dirent;
	size_t			 i;
	struct emuxfs_dev	*dev;
	struct emuxfs_chk	 chk;
	size_t			 chksz;
	enum emuxfs_chk_alg_type alg;
	const char		*dname;
	size_t			 dnamelen;
	struct emuxfs_meta	 meta;
	ino_t			 ino;

	rc = 1;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		goto out;
	alg = dev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);

	emuxfs_chk_init(&chk, alg);
	for (i = 0; i < dir->ent_count; ++i) {
		dirent = dir->ent_array[i];
		dname = dirent->d_name;
		dnamelen = dirent->d_namlen;
		if ((dnamelen == 1) && (strncmp(".", dname, 1) == 0))
			continue;
		if ((dnamelen == 2) && (strncmp("..", dname, 2) == 0))
			continue;
		if ((dnamelen == 6) && (strncmp(".muxfs", dname, 6) ==
		    0))
			continue;
		ino = dirent->d_fileno;
		if (emuxfs_meta_read(&meta, dev_index, ino))
			goto out;
		emuxfs_chk_update(&chk, (uint8_t *)dname, dnamelen);
		emuxfs_chk_update(&chk, &meta.checksums[0], chksz);
	}
	emuxfs_chk_final(sum_out, &chk);

	rc = 0;
out:
	return rc;
}

EMUXFS size_t
emuxfs_align_up(size_t s, size_t a)
{
	return a * ((s / a) + ((s % a) ? 1 : 0));
}

EMUXFS size_t
emuxfs_align_down(size_t s, size_t a)
{
	return a * (s / a);
}

EMUXFS int
emuxfs_is_hardlink(const struct stat *st)
{
	return S_ISREG(st->st_mode) && (st->st_nlink > 1);
}

EMUXFS int
emuxfs_pread_exact(int fd, void *buf, size_t sz, off_t off)
{
	size_t done;
	ssize_t r;

	done = 0;
	while (done < sz) {
		r = pread(fd, (uint8_t *)buf + done, sz - done,
		    off + (off_t)done);
		if (r == -1) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (r == 0)
			return 1; /* Unexpected end of file. */
		done += (size_t)r;
	}
	return 0;
}

EMUXFS int
emuxfs_pwrite_exact(int fd, const void *buf, size_t sz, off_t off)
{
	size_t done;
	ssize_t r;

	done = 0;
	while (done < sz) {
		r = pwrite(fd, (const uint8_t *)buf + done, sz - done,
		    off + (off_t)done);
		if (r == -1) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (r == 0)
			return 1; /* No progress. */
		done += (size_t)r;
	}
	return 0;
}

EMUXFS int
emuxfs_fsync_parent(int root_fd, const char *path)
{
	char		 pbuf[PATH_MAX];
	const char	*ppath;
	size_t		 len;
	int		 fd, rc;

	len = strlen(path);
	if (len >= PATH_MAX)
		return 1;
	memcpy(pbuf, path, len + 1);

	if (emuxfs_path_pop(NULL, pbuf, NULL))
		ppath = ".";
	else
		ppath = pbuf;

	if ((fd = openat(root_fd, ppath,
	    O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) == -1)
		return 1;

	rc = fsync(fd);
	if (close(fd))
		exit(-1);

	return rc ? 1 : 0;
}

EMUXFS int
emuxfs_parse_args(int argc, char **argv, int no_mp)
{
	int i, c;
	size_t len;
	struct emuxfs_args *args = &emuxfs_cmdline;
	char *dest;

	memset(args, 0, sizeof(*args));

	/* Shift one argument to account for the sub-command. */
	++argv;
	--argc;

	c = 0;
	while ((c = getopt(argc, argv, "f")) != -1) {
		switch (c) {
		case 'f':
			args->f = 1;
			break;
		default:
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	args->dev_count = 0;
	for (i = 0; i < argc; ++i) {
		len = strlen(argv[i]);
		if (len == 0 || len >= PATH_MAX)
			return 1;
		if (no_mp || (i > 0)) {
			if (args->dev_count == EMUXFS_DEV_COUNT_MAX)
				return 1;
			dest = args->dev_paths[args->dev_count++];
		} else
			dest = args->mp_path;
		memcpy(dest, argv[i], len + 1);
	}
	return 0;
}
