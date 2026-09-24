/* lfile.c */
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
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chk.h"
#include "emuxfs.h"

#define EMUXFS_LEVEL_FACTOR (EMUXFS_BLOCK_SIZE / EMUXFS_CHKSZ_MAX)

/*
 * The tree fans in by EMUXFS_LEVEL_FACTOR entries per level; a zero factor
 * would make emuxfs_lfile_root_level() and friends loop forever.
 */
static_assert(EMUXFS_BLOCK_SIZE >= EMUXFS_CHKSZ_MAX,
    "lfile level factor must be at least one");

EMUXFS void
emuxfs_range_compute(struct emuxfs_range *range_inout, size_t chksz)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_range r;

	r.byte_begin = range_inout->byte_begin;
	r.byte_end = range_inout->byte_end;

	r.blk_begin = emuxfs_align_down(r.byte_begin, EMUXFS_BLOCK_SIZE);
	r.blk_end = emuxfs_align_up(r.byte_end, EMUXFS_BLOCK_SIZE);
	r.blk_index_begin = r.blk_begin / EMUXFS_BLOCK_SIZE;
	r.blk_index_end = r.blk_end / EMUXFS_BLOCK_SIZE;
	r.lfilesz = chksz * (r.blk_index_end - r.blk_index_begin);
	r.lfileoff = chksz * r.blk_index_begin;

	*range_inout = r;
}

static int
emuxfs_lfile_abs_range(uint64_t *index_out, uint64_t *count_out,
    uint64_t file_blk_count, uint64_t level)
{
	EMUXFS_TRACE("enter");
	uint64_t n, n2, l, i;

	if (file_blk_count == 0)
		return 1;

	i = 0;
	n = file_blk_count;
	for (n2 = 1; n2 < n; n2 *= 2)
		;

	for (l = 0; l < level; ++l) {
		if ((n == 1) && ((level - 1) > l))
			return 1;
		i += n2;
		n = (n / EMUXFS_LEVEL_FACTOR) +
		    ((n % EMUXFS_LEVEL_FACTOR) ? 1 : 0);
		n2 = (n2 / EMUXFS_LEVEL_FACTOR) +
		    ((n2 % EMUXFS_LEVEL_FACTOR) ? 1 : 0);
	}

	if (index_out)
		*index_out = i;
	if (count_out)
		*count_out = n;
	return 0;
}

static uint64_t
emuxfs_lfile_root_level(uint64_t file_blk_count)
{
	EMUXFS_TRACE("enter");
	uint64_t n, l;

	if (file_blk_count == 0)
		return 1;

	n = file_blk_count;
	l = 0;
	while (n != 1) {
		n = (n / EMUXFS_LEVEL_FACTOR) +
		    ((n % EMUXFS_LEVEL_FACTOR) ? 1 : 0);
		++l;
	}
	return l;
}

static uint64_t
emuxfs_lfile_root_abs_index(uint64_t file_blk_count)
{
	EMUXFS_TRACE("enter");
	uint64_t level, index;

	level = emuxfs_lfile_root_level(file_blk_count);

	if (emuxfs_lfile_abs_range(&index, nullptr, file_blk_count, level))
		exit(-1); /* Programming error. */

	return index;
}

EMUXFS int
emuxfs_lfile_open(int *fd_out, int lfile_fd, ino_t ino, int flags)
{
	EMUXFS_TRACE("enter");
	int rc;
	char path_buf[PATH_MAX];
	int path_len;
	int fd;

	rc = 1;

	path_len = snprintf(nullptr, 0, "%llu", ino);
	if (path_len < 0)
		goto out;
	if (path_len >= (PATH_MAX - 1))
		goto out;
	memset(path_buf, 0, PATH_MAX);
	snprintf(path_buf, PATH_MAX - 1, "%llu", ino);

	if ((fd = openat(lfile_fd, path_buf, flags)) == -1)
		goto out;

	*fd_out = fd;
	rc = 0;
out:
	return rc;
}

EMUXFS int
emuxfs_lfile_create(int lfile_fd, size_t chksz, ino_t ino, size_t filesz)
{
	EMUXFS_TRACE("enter");
	int rc;
	uint64_t blk_count, blk_count2;
	size_t lfilesz;
	int path_len;
	char path_buf[PATH_MAX];
	int fd;

	rc = 1;

	blk_count = (filesz / EMUXFS_BLOCK_SIZE) +
	    ((filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	for (blk_count2 = 1; blk_count2 < blk_count; blk_count2 *= 2)
		;

	lfilesz = chksz * (emuxfs_lfile_root_abs_index(blk_count) + 1);

	path_len = snprintf(nullptr, 0, "%llu", ino);
	if (path_len < 0)
		goto out;
	if (path_len >= (PATH_MAX - 1))
		goto out;
	memset(path_buf, 0, PATH_MAX);
	snprintf(path_buf, PATH_MAX - 1, "%llu", ino);
	if ((fd = openat(lfile_fd, path_buf, O_RDWR|O_CREAT|O_EXCL, 0700)) ==
	    -1)
		goto out;
	if (ftruncate(fd, (off_t)lfilesz))
		goto out2;

	rc = 0;
out2:
	if (close(fd))
		exit(-1);
out:
	return rc;
}

static int
emuxfs_lfile_grow(int lfile_fd, size_t chksz, ino_t ino, size_t old_filesz,
    size_t new_filesz)
{
	EMUXFS_TRACE("enter");
	int rc;
	uint64_t old_blk_count, new_blk_count, new_blk_count2;
	size_t new_lfilesz, new_lfilesz2;
	uint64_t old_root_level, l, old_index, new_index, old_count, new_count;
	int fd;
	uint8_t *lfile;

	rc = 1;

	old_blk_count = (old_filesz / EMUXFS_BLOCK_SIZE) +
	    ((old_filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	new_blk_count = (new_filesz / EMUXFS_BLOCK_SIZE) +
	    ((new_filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	for (new_blk_count2 = 1; new_blk_count2 < new_blk_count;
	    new_blk_count2 *= 2)
		;

	new_lfilesz = chksz * (emuxfs_lfile_root_abs_index(new_blk_count) + 1);
	new_lfilesz2 = chksz *
	    (emuxfs_lfile_root_abs_index(new_blk_count2) + 1);
	old_root_level = emuxfs_lfile_root_level(old_blk_count);

	if (emuxfs_lfile_open(&fd, lfile_fd, ino, O_RDWR))
		goto out;
	if (ftruncate(fd, (off_t)new_lfilesz2))
		goto out2;
	if ((lfile = mmap(nullptr, new_lfilesz, PROT_READ|PROT_WRITE, MAP_SHARED,
	    fd, 0)) == MAP_FAILED)
		goto out2;

	for (l = old_root_level; l > 0; --l) {
		if (emuxfs_lfile_abs_range(&old_index, &old_count,
		    old_blk_count, l))
			exit(-1); /* Programming error. */
		if (emuxfs_lfile_abs_range(&new_index, &new_count,
		    new_blk_count, l))
			exit(-1); /* Programming error. */
		memmove(&lfile[new_index * chksz], &lfile[old_index * chksz],
		    old_count * chksz);
	}

	rc = 0;
/*out3:*/
	if (munmap(lfile, new_lfilesz))
		exit(-1);
out2:
	if (close(fd))
		exit(-1);
out:
	return rc;
}

static int
emuxfs_lfile_shrink(int lfile_fd, size_t chksz, ino_t ino, size_t old_filesz,
    size_t new_filesz)
{
	EMUXFS_TRACE("enter");
	int rc;
	uint64_t old_blk_count, new_blk_count, new_blk_count2;
	size_t old_lfilesz, new_lfilesz2;
	uint64_t new_root_level, l, old_index, new_index, old_count, new_count;
	int fd;
	uint8_t *lfile;

	rc = 1;
	lfile = MAP_FAILED;

	old_blk_count = (old_filesz / EMUXFS_BLOCK_SIZE) +
	    ((old_filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	new_blk_count = (new_filesz / EMUXFS_BLOCK_SIZE) +
	    ((new_filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	for (new_blk_count2 = 1; new_blk_count2 < new_blk_count;
	    new_blk_count2 *= 2)
		;

	old_lfilesz = chksz * (emuxfs_lfile_root_abs_index(old_blk_count) + 1);
	new_lfilesz2 = chksz *
	    (emuxfs_lfile_root_abs_index(new_blk_count2) + 1);
	new_root_level = emuxfs_lfile_root_level(new_blk_count);

	if (emuxfs_lfile_open(&fd, lfile_fd, ino, O_RDWR))
		goto out;
	if ((lfile = mmap(nullptr, old_lfilesz, PROT_READ|PROT_WRITE, MAP_SHARED,
	    fd, 0)) == MAP_FAILED)
		goto out2;

	for (l = 1; l <= new_root_level; ++l) {
		if (emuxfs_lfile_abs_range(&old_index, &old_count,
		    old_blk_count, l))
			exit(-1); /* Programming error. */
		if (emuxfs_lfile_abs_range(&new_index, &new_count,
		    new_blk_count, l))
			exit(-1); /* Programming error. */
		memmove(&lfile[new_index * chksz], &lfile[old_index * chksz],
		    new_count * chksz);
	}

	if (munmap(lfile, old_lfilesz))
		exit(-1);
	lfile = MAP_FAILED;

	if (ftruncate(fd, (off_t)new_lfilesz2))
		goto out2;

	rc = 0;
/*out3:*/
	if (lfile != MAP_FAILED) {
		if (munmap(lfile, old_lfilesz))
			exit(-1);
	}
out2:
	if (close(fd))
		exit(-1);
out:
	return rc;
}

EMUXFS int
emuxfs_lfile_resize(int lfile_fd, size_t chksz, ino_t ino, size_t old_filesz,
    size_t new_filesz)
{
	EMUXFS_TRACE("enter");
	size_t n, o, n2, o2;

	n = (new_filesz / EMUXFS_BLOCK_SIZE) +
	    ((new_filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	o = (old_filesz / EMUXFS_BLOCK_SIZE) +
	    ((old_filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	for (n2 = 1; n2 < n; n2 *= 2)
		;
	for (o2 = 1; o2 < o; o2 *= 2)
		;

	if (n2 == o2)
		return 0;
	else if (n2 > o2) {
		return emuxfs_lfile_grow(lfile_fd, chksz, ino, old_filesz,
		    new_filesz);
	} else {
		return emuxfs_lfile_shrink(lfile_fd, chksz, ino, old_filesz,
		    new_filesz);
	}
	exit(-1); /* Unreachable. */
}

EMUXFS int
emuxfs_lfile_delete(int lfile_fd, ino_t ino)
{
	EMUXFS_TRACE("enter");
	int rc;
	char path_buf[PATH_MAX];
	int path_len;

	rc = 1;

	path_len = snprintf(nullptr, 0, "%llu", ino);
	if (path_len < 0)
		goto out;
	if (path_len >= (PATH_MAX - 1))
		goto out;
	memset(path_buf, 0, PATH_MAX);
	snprintf(path_buf, PATH_MAX - 1, "%llu", ino);

	if (unlinkat(lfile_fd, path_buf, 0))
		goto out;

	rc = 0;
out:
	return rc;
}

EMUXFS int
emuxfs_lfile_exists(int *exists_out, int lfile_fd, ino_t ino)
{
	EMUXFS_TRACE("enter");
	char path_buf[PATH_MAX];
	int path_len;

	path_len = snprintf(nullptr, 0, "%llu", ino);
	if (path_len < 0)
		return 1;
	if (path_len >= (PATH_MAX - 1))
		return 1;
	memset(path_buf, 0, PATH_MAX);
	snprintf(path_buf, PATH_MAX - 1, "%llu", ino);

	return emuxfs_existsat(exists_out, lfile_fd, path_buf);
}

EMUXFS int
emuxfs_lfile_ancestors_recompute(uint8_t *root_sum, int lfile_fd,
    enum emuxfs_chk_alg_type alg, ino_t ino, size_t filesz, uint64_t ibegin,
    uint64_t iend)
{
	EMUXFS_TRACE("enter");
	int rc, fd;
	size_t chksz, lfilesz;
	uint64_t blk_count, root_level, l, li, ln, pli, pln, pi, i, j;
	uint8_t *lfile;
	struct emuxfs_chk chk;

	rc = 1;
	fd = -1;
	lfile = MAP_FAILED;
	lfilesz = 0;

	chksz = emuxfs_chk_size(alg);

	if (iend < 1)
		exit(-1); /* Programming error. */

	blk_count = (filesz / EMUXFS_BLOCK_SIZE) +
	    ((filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	lfilesz = chksz * (emuxfs_lfile_root_abs_index(blk_count) + 1);
	root_level = emuxfs_lfile_root_level(blk_count);

	if (emuxfs_lfile_open(&fd, lfile_fd, ino, O_RDWR))
		goto out;
	if ((lfile = mmap(nullptr, lfilesz, PROT_READ|PROT_WRITE, MAP_SHARED, fd,
	    0)) == MAP_FAILED)
		goto out;

	for (l = 0; l < root_level; ++l) {
		/* Align ibegin. */
		ibegin -= (ibegin % EMUXFS_LEVEL_FACTOR);

		if (emuxfs_lfile_abs_range(&li, &ln, blk_count, l))
			exit(-1); /* Programming error. */
		if (emuxfs_lfile_abs_range(&pli, &pln, blk_count, l + 1))
			exit(-1); /* Programming error. */
		if (ibegin >= ln)
			exit(-1); /* Programming error. */
		if (iend > ln)
			exit(-1); /* Programming error. */

		for (i = ibegin; i < iend; i += EMUXFS_LEVEL_FACTOR) {
			emuxfs_chk_init(&chk, alg);
			j = i + EMUXFS_LEVEL_FACTOR;
			/*
			 * A tree entry covers a full LEVEL_FACTOR range (the
			 * last one may be short), so hash up to the level's
			 * entry count rather than up to the modified range:
			 * the unmodified entries in the same tree entry are
			 * part of it.  Using iend here made the ancestor of a
			 * partial range omit the leaves after it.
			 */
			if (j > ln)
				j = ln;
			emuxfs_chk_update(&chk, &lfile[chksz * (li + i)],
			    (j - i) * chksz);
			pi = i / EMUXFS_LEVEL_FACTOR;
			emuxfs_chk_final(&lfile[chksz * (pli + pi)], &chk);
		}

		ibegin /= EMUXFS_LEVEL_FACTOR;
		iend = (iend + (EMUXFS_LEVEL_FACTOR - 1)) / EMUXFS_LEVEL_FACTOR;
	}

	if (root_sum != nullptr) {
		memcpy(root_sum, &lfile[chksz *
		    emuxfs_lfile_root_abs_index(blk_count)], chksz);
	}
	rc = 0;
out:
	if (lfile != MAP_FAILED) {
		if (lfilesz == 0)
			exit(-1); /* Programming error. */
		if (munmap(lfile, lfilesz))
			exit(-1);
	} else if (lfilesz != 0)
		exit(-1); /* Programming error. */
	if (fd != -1) {
		if (close(fd))
			exit(-1);
	}
	return rc;
}

EMUXFS int
emuxfs_lfile_readback(uint8_t *root_sum, dind dev_index, const char *path,
    size_t begin, size_t end, const uint8_t *expected)
{
	EMUXFS_TRACE("enter");
	int rc;
	struct emuxfs_dev *dev;
	enum emuxfs_chk_alg_type alg;
	size_t chksz;
	struct stat st;
	ino_t ino;
	int fd, lfd;
	size_t filesz, lfilesz;
	uint8_t *lfile;
	uint64_t blk_count, root_level, l, li, ln, pli, pln, pi, i, j, ibegin,
		 iend;
	uint8_t buf[EMUXFS_BLOCK_SIZE], sum[EMUXFS_CHKSZ_MAX];
	size_t i_offset, rdsz;
	struct emuxfs_chk chk;

	rc = 1;
	fd = -1;
	lfd = -1;
	lfile = MAP_FAILED;
	lfilesz = 0;
	/*
	 * A file that fits in a single block has no internal tree level: its
	 * content checksum is the root entry at index 0.  The loops below do
	 * not run in that case, so keep the parent index defined for the
	 * expected/root_sum comparisons (and to avoid reading an
	 * indeterminate value).
	 */
	pli = 0;
	pln = 0;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		goto out;
	alg = dev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);

	if (fstatat(dev->root_fd, path, &st, AT_SYMLINK_NOFOLLOW))
		goto out;
	ino = st.st_ino;
	if (st.st_size < 0)
		goto out;
	filesz = (size_t)st.st_size;
	if (begin >= filesz)
		goto out;
	if (end > filesz)
		goto out;

	blk_count = (filesz / EMUXFS_BLOCK_SIZE) +
	    ((filesz % EMUXFS_BLOCK_SIZE) ? 1 : 0);
	lfilesz = chksz * (emuxfs_lfile_root_abs_index(blk_count) + 1);
	root_level = emuxfs_lfile_root_level(blk_count);
	ibegin = emuxfs_align_down(begin, EMUXFS_BLOCK_SIZE) /
	    EMUXFS_BLOCK_SIZE;
	iend = emuxfs_align_up(end, EMUXFS_BLOCK_SIZE) / EMUXFS_BLOCK_SIZE;

	if ((ibegin + 1) > iend)
		exit(-1); /* Programming error. */
	ibegin -= (ibegin % EMUXFS_LEVEL_FACTOR);

	if ((fd = openat(dev->root_fd, path, O_RDONLY)) == -1)
		goto out;
	if (emuxfs_lfile_open(&lfd, dev->lfile_fd, ino, O_RDONLY))
		goto out;
	if ((lfile = mmap(nullptr, lfilesz, PROT_READ, MAP_SHARED, lfd, 0)) ==
	    MAP_FAILED)
		goto out;

	for (i = ibegin; i < iend; ++i) {
		i_offset = i * EMUXFS_BLOCK_SIZE;
		rdsz = EMUXFS_BLOCK_SIZE;
		if (i_offset + rdsz > filesz) {
			if ((i + 1) != iend)
				exit(-1); /* Programming error. */
			rdsz = filesz - i_offset;
		}
		if (pread(fd, buf, rdsz, (off_t)i_offset) != (ssize_t)rdsz)
			goto out;
		emuxfs_chk_init(&chk, alg);
		emuxfs_chk_update(&chk, buf, rdsz);
		emuxfs_chk_final(sum, &chk);
		if (bcmp(sum, &lfile[chksz * i], chksz) != 0)
			goto out;
	}
	for (l = 0; l < root_level; ++l) {
		/* Align ibegin. */
		ibegin -= (ibegin % EMUXFS_LEVEL_FACTOR);

		if (emuxfs_lfile_abs_range(&li, &ln, blk_count, l))
			exit(-1); /* Programming error. */
		if (emuxfs_lfile_abs_range(&pli, &pln, blk_count, l + 1))
			exit(-1); /* Programming error. */
		if (ibegin >= ln)
			exit(-1); /* Programming error. */
		if (iend > ln)
			exit(-1); /* Programming error. */

		for (i = ibegin; i < iend; i += EMUXFS_LEVEL_FACTOR) {
			emuxfs_chk_init(&chk, alg);
			j = i + EMUXFS_LEVEL_FACTOR;
			/*
			 * A tree entry covers a full LEVEL_FACTOR range (the
			 * last one may be short), so hash up to the level's
			 * entry count rather than up to the modified range:
			 * the unmodified entries in the same tree entry are
			 * part of it.  Using iend here made the ancestor of a
			 * partial range omit the leaves after it.
			 */
			if (j > ln)
				j = ln;
			emuxfs_chk_update(&chk, &lfile[chksz * (li + i)],
			    (j - i) * chksz);
			pi = i / EMUXFS_LEVEL_FACTOR;
			emuxfs_chk_final(sum, &chk);
			if (bcmp(sum, &lfile[chksz * (pli + pi)], chksz) != 0)
				goto out;
		}

		ibegin /= EMUXFS_LEVEL_FACTOR;
		iend = (iend + (EMUXFS_LEVEL_FACTOR - 1)) / EMUXFS_LEVEL_FACTOR;
	}

	if ((expected != nullptr) && (bcmp(expected, &lfile[chksz * pli], chksz) !=
	    0))
		goto out;

	if (root_sum != nullptr)
		memcpy(root_sum, &lfile[chksz * pli], chksz);
	rc = 0;
out:
	if (lfile != MAP_FAILED) {
		if (lfilesz == 0)
			exit(-1); /* Programming error. */
		if (munmap(lfile, lfilesz))
			exit(-1);
	} else if (lfilesz != 0)
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
