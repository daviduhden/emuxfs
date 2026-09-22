/* test_core.c */
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

/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 *
 * Headless unit tests.  They link the non-FUSE part of emuxfs and
 * exercise checksums, metadata serialisation, configuration parsing, the
 * restore queue, path sanitisation and (when running as root) device
 * formatting, mounting and the large-file checksum tree.
 *
 * All files are created beneath a private mkdtemp(3) sandbox; if the sandbox
 * cannot be created the tests abort rather than touch anything else.
 */

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "chk.h"
#include "emuxfs.h"

static int failures;
static char sandbox[PATH_MAX];

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
			    __FILE__, __LINE__, #cond);			\
			++failures;					\
		}							\
	} while (0)

static void
fail_hex(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
	size_t i;

	fprintf(stderr, "FAIL %s: got ", what);
	for (i = 0; i < n; ++i)
		fprintf(stderr, "%02x", got[i]);
	fprintf(stderr, " want ");
	for (i = 0; i < n; ++i)
		fprintf(stderr, "%02x", want[i]);
	fprintf(stderr, "\n");
	++failures;
}

static char *
sandbox_path(char *buf, size_t bufsz, const char *name)
{
	if (snprintf(buf, bufsz, "%s/%s", sandbox, name) >= (int)bufsz) {
		fprintf(stderr, "FAIL: sandbox path too long\n");
		exit(2);
	}
	return buf;
}

static int
rm_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)type;
	(void)ftw;
	if (remove(path))
		return -1;
	return 0;
}

static void
sandbox_create(void)
{
	const char *base;
	char tmpl[PATH_MAX];

	base = getenv("TMPDIR");
	if ((base == NULL) || (base[0] == '\0'))
		base = "/tmp";

	if (snprintf(tmpl, sizeof(tmpl), "%s/emuxfs-unit.XXXXXX", base) >=
	    (int)sizeof(tmpl)) {
		fprintf(stderr, "FAIL: TMPDIR too long\n");
		exit(2);
	}
	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		exit(2);
	}
	/*
	 * Refuse to continue unless the sandbox really is inside the expected
	 * temporary directory.
	 */
	if (strncmp(tmpl, base, strlen(base)) != 0) {
		fprintf(stderr, "FAIL: sandbox escaped %s\n", base);
		exit(2);
	}
	strlcpy(sandbox, tmpl, sizeof(sandbox));
}

static void
sandbox_destroy(void)
{
	if (nftw(sandbox, rm_cb, 32, FTW_DEPTH|FTW_PHYS) != 0)
		perror("nftw");
}

static void
test_checksums(void)
{
	static const uint8_t abc[] = { 'a', 'b', 'c' };
	static const uint8_t digits[] = "123456789";
	static const uint8_t crc32_expect[] = { 0x26, 0x39, 0xf4, 0xcb }; /* 0xcbf43926, little-endian */
	static const uint8_t md5_expect[] = { 0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
	  0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72 };
	static const uint8_t sha1_expect[] = { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
	  0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
	  0x9c, 0xd0, 0xd8, 0x9d };
	struct emuxfs_chk chk;
	uint8_t got[EMUXFS_CHKSZ_MAX];
	enum emuxfs_chk_alg_type type;

	CHECK(emuxfs_chk_size(CAT_CRC32) == 4);
	CHECK(emuxfs_chk_size(CAT_MD5) == 16);
	CHECK(emuxfs_chk_size(CAT_SHA1) == 20);

	memset(got, 0, sizeof(got));
	emuxfs_chk_init(&chk, CAT_CRC32);
	emuxfs_chk_update(&chk, digits, sizeof(digits) - 1);
	emuxfs_chk_final(got, &chk);
	if (memcmp(got, crc32_expect, sizeof(crc32_expect)) != 0)
		fail_hex("crc32", got, crc32_expect, sizeof(crc32_expect));

	memset(got, 0, sizeof(got));
	emuxfs_chk_init(&chk, CAT_MD5);
	emuxfs_chk_update(&chk, abc, sizeof(abc));
	emuxfs_chk_final(got, &chk);
	if (memcmp(got, md5_expect, sizeof(md5_expect)) != 0)
		fail_hex("md5", got, md5_expect, sizeof(md5_expect));

	memset(got, 0, sizeof(got));
	emuxfs_chk_init(&chk, CAT_SHA1);
	emuxfs_chk_update(&chk, abc, sizeof(abc));
	emuxfs_chk_final(got, &chk);
	if (memcmp(got, sha1_expect, sizeof(sha1_expect)) != 0)
		fail_hex("sha1", got, sha1_expect, sizeof(sha1_expect));

	CHECK(emuxfs_chk_str_to_type(&type, "md5", 3) == 0 &&
	    type == CAT_MD5);
	CHECK(strcmp(emuxfs_chk_type_to_str(CAT_SHA1), "sha1") == 0);
}

static void
test_meta_size_and_align(void)
{
	size_t msz;

	CHECK(emuxfs_align_up(1, 8) == 8);
	CHECK(emuxfs_align_up(8, 8) == 8);
	CHECK(emuxfs_align_up(9, 8) == 16);
	CHECK(emuxfs_align_down(7, 8) == 0);
	CHECK(emuxfs_align_down(8, 8) == 8);

	if (emuxfs_meta_size_raw(&msz, CAT_CRC32)) {
		CHECK(0);
		return;
	}
	CHECK(msz == 24); /* header(16) + 2 * 4 checksums = 24. */
	if (emuxfs_meta_size_raw(&msz, CAT_MD5)) {
		CHECK(0);
		return;
	}
	CHECK(msz == 48); /* 16 + 32. */
	if (emuxfs_meta_size_raw(&msz, CAT_SHA1)) {
		CHECK(0);
		return;
	}
	CHECK(msz == 56); /* 16 + 40. */
}

static void
test_desc_meta(void)
{
	struct emuxfs_desc a, b;
	uint8_t sum_a[EMUXFS_CHKSZ_MAX], sum_b[EMUXFS_CHKSZ_MAX];

	memset(&a, 0, sizeof(a));
	a.eno = 7;
	a.type = EMUXFS_DT_REG;
	a.owner = 1000;
	a.group = 1000;
	a.mode = S_IFREG | 0644;
	a.size = 3;
	memcpy(a.content_checksum, "xyz", 3);

	b = a;
	emuxfs_desc_chk_meta(sum_a, &a, CAT_MD5);
	emuxfs_desc_chk_meta(sum_b, &b, CAT_MD5);
	CHECK(memcmp(sum_a, sum_b, 16) == 0); /* Deterministic. */

	b.size = 4;
	emuxfs_desc_chk_meta(sum_b, &b, CAT_MD5);
	CHECK(memcmp(sum_a, sum_b, 16) != 0); /* Size is covered. */

	b = a;
	b.owner = 1001;
	emuxfs_desc_chk_meta(sum_b, &b, CAT_MD5);
	CHECK(memcmp(sum_a, sum_b, 16) != 0); /* Owner is covered. */

	b = a;
	b.type = EMUXFS_DT_DIR;
	b.mode = S_IFDIR | 0644;
	emuxfs_desc_chk_meta(sum_b, &b, CAT_MD5);
	CHECK(memcmp(sum_a, sum_b, 16) != 0); /* Mode/type is covered. */
}

static void
test_conf_roundtrip(void)
{
	struct emuxfs_dev_conf out, in;
	char path[PATH_MAX];
	int fd, rc, i;

	sandbox_path(path, sizeof(path), "muxfs.conf");
	if ((fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1) {
		perror(path);
		++failures;
		return;
	}

	memset(&out, 0, sizeof(out));
	out.version = emuxfs_program_version;
	out.format_version = EMUXFS_FORMAT_VERSION;
	out.chk_alg_type = CAT_SHA1;
	for (i = 0; i < EMUXFS_UUID_SIZE; ++i) {
		out.array_uuid[i] = (uint8_t)i;
		/*
		 * uuid_from_string(3) rejects UUIDs whose variant bits are
		 * not 0xx, 10x or 110, so byte 8 must be a valid variant.
		 */
		out.dev_uuid[i] = (uint8_t)(0x10 | i);
	}
	out.seq_zero_time = (time_t)1234567890;

	CHECK(emuxfs_conf_write(&out, fd) == 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);

	rc = emuxfs_conf_parse(&in, fd);
	CHECK(rc == EMUXFS_CONF_OK);
	CHECK(in.version.number == out.version.number);
	CHECK(in.version.revision == out.version.revision);
	CHECK(in.version.flavor == out.version.flavor);
	CHECK(in.format_version == EMUXFS_FORMAT_VERSION);
	CHECK(in.chk_alg_type == CAT_SHA1);
	CHECK(memcmp(in.array_uuid, out.array_uuid, EMUXFS_UUID_SIZE) == 0);
	CHECK(memcmp(in.dev_uuid, out.dev_uuid, EMUXFS_UUID_SIZE) == 0);
	CHECK(in.seq_zero_time == out.seq_zero_time);

	close(fd);
}

static void
test_conf_legacy_and_errors(void)
{
	static const char legacy[] =
	    "version=0.5current\n"
	    "chk_alg=md5\n"
	    "array_uuid=00112233-4455-6677-8899-aabbccddeeff\n"
	    "dev_uuid=ffeeddcc-bbaa-9988-7766-554433221100\n"
	    "seq_zero_time=1600000000\n";
	static const char badver[] =
	    "version=1.0-current\n"
	    "format_version=99\n"
	    "chk_alg=md5\n"
	    "array_uuid=00112233-4455-6677-8899-aabbccddeeff\n"
	    "dev_uuid=ffeeddcc-bbaa-9988-7766-554433221100\n"
	    "seq_zero_time=1600000000\n";
	char path[PATH_MAX];
	struct emuxfs_dev_conf conf;
	int fd, rc;

	sandbox_path(path, sizeof(path), "legacy.conf");
	if ((fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1) {
		perror(path);
		++failures;
		return;
	}
	CHECK(write(fd, legacy, sizeof(legacy) - 1) ==
	    (ssize_t)(sizeof(legacy) - 1));
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	rc = emuxfs_conf_parse(&conf, fd);
	CHECK(rc == EMUXFS_CONF_OK);
	CHECK(conf.version.number == 0);
	CHECK(conf.version.revision == 5); /* The original parser lost this. */
	CHECK(conf.version.flavor == VF_CURRENT);
	CHECK(conf.format_version == EMUXFS_FORMAT_VERSION_LEGACY);
	close(fd);

	sandbox_path(path, sizeof(path), "badver.conf");
	if ((fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1) {
		perror(path);
		++failures;
		return;
	}
	CHECK(write(fd, badver, sizeof(badver) - 1) ==
	    (ssize_t)(sizeof(badver) - 1));
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	rc = emuxfs_conf_parse(&conf, fd);
	CHECK(rc == EMUXFS_CONF_EBADVERSION);
	close(fd);

	sandbox_path(path, sizeof(path), "garbage.conf");
	if ((fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1) {
		perror(path);
		++failures;
		return;
	}
	CHECK(write(fd, "not a config\n", 13) == 13);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	rc = emuxfs_conf_parse(&conf, fd);
	CHECK(rc == EMUXFS_CONF_EPARSE);
	close(fd);
}

static void
test_path_sanitize(void)
{
	const char *p;

	p = "/a/b";
	CHECK(emuxfs_path_sanitize(&p) == 0 && strcmp(p, "a/b") == 0);
	p = "/";
	CHECK(emuxfs_path_sanitize(&p) == 0 && strcmp(p, ".") == 0);
	p = "";
	CHECK(emuxfs_path_sanitize(&p) == 0 && strcmp(p, ".") == 0);
	p = "/.hidden";
	CHECK(emuxfs_path_sanitize(&p) == 0 && strcmp(p, ".hidden") == 0);
	p = "/.muxfs";
	CHECK(emuxfs_path_sanitize(&p) == 1);
	p = "/a/.muxfs";
	CHECK(emuxfs_path_sanitize(&p) == 1);
	p = "/.muxfs/x";
	CHECK(emuxfs_path_sanitize(&p) == 1);
	p = "/a/../b";
	CHECK(emuxfs_path_sanitize(&p) == 1);
	p = "/a/./b";
	CHECK(emuxfs_path_sanitize(&p) == 1);
	p = "/a//b";
	CHECK(emuxfs_path_sanitize(&p) == 1);
	p = "/a/b/";
	CHECK(emuxfs_path_sanitize(&p) == 1);
}

static void
test_serialisation_endianness(void)
{
	struct emuxfs_dev_state st;
	struct emuxfs_meta meta;
	struct emuxfs_assign assign;
	uint8_t buf[64];
	char path[PATH_MAX];
	uint64_t u64;
	size_t msz;
	int fd;

	sandbox_path(path, sizeof(path), "ser.bin");
	if ((fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1) {
		perror(path);
		++failures;
		return;
	}

	st.seq = 0x0102030405060708ULL;
	st.mounted = 1;
	st.working = 2;
	st.restoring = 3;
	st.degraded = 0;
	CHECK(emuxfs_dev_state_write_fd(fd, &st) == 0);
	CHECK(emuxfs_pread_exact(fd, buf, 8, 0) == 0);
	memcpy(&u64, buf, 8);
	CHECK(letoh64(u64) == st.seq); /* Stored little-endian. */

	memset(&meta, 0, sizeof(meta));
	meta.header.flags = MF_ASSIGNED;
	meta.header.eno = 0x1122334455667788ULL;
	if (emuxfs_meta_size_raw(&msz, CAT_MD5)) {
		CHECK(0);
	} else {
		CHECK(emuxfs_meta_write_fd(fd, &meta, 0, msz) == 0);
		CHECK(emuxfs_pread_exact(fd, buf, 16, 0) == 0);
		memcpy(&u64, buf, 8);
		CHECK(letoh64(u64) == meta.header.flags);
		memcpy(&u64, buf + 8, 8);
		CHECK(letoh64(u64) == meta.header.eno);
	}

	assign.flags = AF_ASSIGNED;
	assign.ino = 0xdeadbeefcafebabeULL;
	CHECK(emuxfs_assign_write_fd(fd, &assign, 0) == 0);
	CHECK(emuxfs_pread_exact(fd, buf, 16, 0) == 0);
	memcpy(&u64, buf, 8);
	CHECK(letoh64(u64) == assign.flags);
	memcpy(&u64, buf + 8, 8);
	CHECK(letoh64(u64) == assign.ino);

	close(fd);
}

static void
test_restore_queue(void)
{
	char path[512], got[512];
	dind dev;
	size_t len;
	int i;

	if (emuxfs_state_restore_queue_init())
		exit(2);

	/*
	 * Enough entries, each larger than a page, to force the queue to grow
	 * several times and to compact.  This is the regression test for the
	 * original queue, which overflowed its buffer and wrote through a
	 * pointer invalidated by realloc(3).
	 */
	for (i = 0; i < 200; ++i) {
		snprintf(path, sizeof(path), "dir%03d/%0*d", i, 300 + i, i);
		if (emuxfs_state_restore_push_back((dind)i, path))
			exit(2);
	}

	for (i = 0; i < 200; ++i) {
		snprintf(path, sizeof(path), "dir%03d/%0*d", i, 300 + i, i);
		memset(got, 0, sizeof(got));
		if (emuxfs_state_restore_pop_front(&dev, got)) {
			CHECK(0);
			break;
		}
		CHECK(dev == (dind)i);
		CHECK(strcmp(got, path) == 0);
	}
	CHECK(emuxfs_state_restore_pop_front(&dev, got) == 1); /* Empty. */
	CHECK(emuxfs_state_restore_next_path_len(&len) == 1);

	emuxfs_state_restore_queue_final();
}

static void
test_dev_format_mount(void)
{
	struct emuxfs_dev *dev;
	struct emuxfs_meta meta;
	struct emuxfs_assign assign;
	struct emuxfs_range range;
	struct stat st;
	char root[PATH_MAX];
	uint8_t uuid[EMUXFS_UUID_SIZE];
	size_t chksz, metasz;
	dind i;
	uint64_t next;
	time_t now;
	int exists;
	ino_t root_ino;
	int have_root_ino;

	if (geteuid() != 0) {
		printf("SKIP: device tests require root\n");
		return;
	}

	sandbox_path(root, sizeof(root), "dev");
	if (mkdir(root, 0700)) {
		perror(root);
		++failures;
		return;
	}

	chksz = emuxfs_chk_size(CAT_SHA1);
	if (emuxfs_meta_size_raw(&metasz, CAT_SHA1)) {
		CHECK(0);
		return;
	}
	memset(uuid, 0x5a, sizeof(uuid));
	now = time(NULL);
	if (emuxfs_dev_format(root, CAT_SHA1, chksz, metasz, now, uuid)) {
		fprintf(stderr, "FAIL: emuxfs_dev_format\n");
		++failures;
		return;
	}

	emuxfs_dev_module_init();
	if (emuxfs_dev_append(&i, root)) {
		CHECK(0);
		return;
	}
	if (emuxfs_dev_mount(i, 0)) {
		fprintf(stderr, "FAIL: emuxfs_dev_mount\n");
		++failures;
		return;
	}
	CHECK(emuxfs_dev_is_mounted(i) == 1);
	CHECK(emuxfs_dev_get(&dev, i, 0) == 0);
	CHECK(dev->state.mounted == 1);
	CHECK(dev->conf.chk_alg_type == CAT_SHA1);
	CHECK(dev->conf.format_version == EMUXFS_FORMAT_VERSION);

	if (stat(root, &st)) {
		CHECK(0);
		have_root_ino = 0;
		root_ino = 0;
	} else {
		have_root_ino = 1;
		root_ino = st.st_ino;
		if (emuxfs_meta_read(&meta, i, root_ino)) {
			CHECK(0);
		} else {
			CHECK(meta.header.flags == MF_ASSIGNED);
			CHECK(meta.header.eno == 0);
		}
	}
	if (emuxfs_assign_read(&assign, i, 0)) {
		CHECK(0);
	} else {
		CHECK(assign.flags == AF_ASSIGNED);
		if (have_root_ino)
			CHECK(assign.ino == root_ino);
	}
	CHECK(emuxfs_assign_peek_next_eno(&next, i) == 0 && next == 1);

	/* Large-file checksum tree file management. */
	if (emuxfs_lfile_create(dev->lfile_fd, chksz, (ino_t)4242, 10000)) {
		CHECK(0);
	} else {
		CHECK(emuxfs_lfile_exists(&exists, dev->lfile_fd,
		    (ino_t)4242) == 0 && exists == 1);
		CHECK(emuxfs_lfile_resize(dev->lfile_fd, chksz, (ino_t)4242,
		    10000, 40000) == 0);
		CHECK(emuxfs_lfile_resize(dev->lfile_fd, chksz, (ino_t)4242,
		    40000, 5000) == 0);
		CHECK(emuxfs_lfile_delete(dev->lfile_fd, (ino_t)4242) == 0);
		CHECK(emuxfs_lfile_exists(&exists, dev->lfile_fd,
		    (ino_t)4242) == 0 && exists == 0);
	}

	range.byte_begin = 1;
	range.byte_end = EMUXFS_BLOCK_SIZE + 1;
	emuxfs_range_compute(&range, chksz);
	CHECK(range.blk_index_begin == 0);
	CHECK(range.blk_index_end == 2);

	CHECK(emuxfs_working_push(i) == 0);
	CHECK(emuxfs_working_pop(i, now) == 0);
	CHECK(dev->state.seq == 1);

	/* A clean unmount and a successful remount. */
	CHECK(emuxfs_dev_unmount(i) == 0);
	CHECK(emuxfs_dev_is_mounted(i) == 0);
	CHECK(emuxfs_dev_mount(i, 0) == 0);
	CHECK(emuxfs_dev_unmount(i) == 0);

	/* Metadata write through the device wrapper for the root inode. */
	if (have_root_ino) {
		memset(&meta, 0, sizeof(meta));
		meta.header.flags = MF_ASSIGNED;
		meta.header.eno = 99;
		if (emuxfs_dev_mount(i, 0) == 0) {
			CHECK(emuxfs_meta_write(&meta, i, root_ino) == 0);
			CHECK(emuxfs_meta_read(&meta, i, root_ino) == 0);
			CHECK(meta.header.eno == 99);
			emuxfs_dev_unmount(i);
		}
	}
}

static int
emuxfs_test_fabricate(dind idx, const char *name, const char *content)
{
	char			 pbuf[PATH_MAX];
	struct emuxfs_dev	*dev;
	struct emuxfs_desc	 desc;
	struct emuxfs_meta	 meta;
	struct emuxfs_assign	 assign;
	struct stat		 st;
	size_t			 chksz, len;
	uint64_t		 eno;
	int			 fd;

	if (emuxfs_dev_get(&dev, idx, 0))
		return 1;
	if (snprintf(pbuf, sizeof(pbuf), "%s/%s", dev->root_path, name)
	    >= (int)sizeof(pbuf))
		return 1;

	if ((fd = open(pbuf, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1)
		return 1;
	len = strlen(content);
	if (write(fd, content, len) != (ssize_t)len) {
		close(fd);
		return 1;
	}
	if (close(fd))
		return 1;

	if (fstatat(dev->root_fd, name, &st, AT_SYMLINK_NOFOLLOW))
		return 1;
	/* The same logical file has the same eno on every device. */
	eno = 1000;
	if (emuxfs_desc_init_from_stat(&desc, &st, eno))
		return 1;
	if (emuxfs_desc_chk_reg_content(&desc, idx, name))
		return 1;

	chksz = emuxfs_chk_size(dev->conf.chk_alg_type);
	memset(&meta, 0, sizeof(meta));
	meta.header.flags = MF_ASSIGNED;
	meta.header.eno = eno;
	emuxfs_desc_chk_meta(&meta.checksums[0], &desc,
	    dev->conf.chk_alg_type);
	memcpy(&meta.checksums[chksz], desc.content_checksum, chksz);
	if (emuxfs_meta_write(&meta, idx, st.st_ino))
		return 1;

	assign = (struct emuxfs_assign) {
		.flags = AF_ASSIGNED,
		.ino = st.st_ino,
	};
	if (emuxfs_assign_write(&assign, idx, eno))
		return 1;

	return 0;
}

static int
emuxfs_test_file_has_prefix(const char *path, const char *prefix)
{
	char buf[64];
	size_t len;
	int fd;
	ssize_t r;

	len = strlen(prefix);
	if (len >= sizeof(buf))
		return 0;
	if ((fd = open(path, O_RDONLY)) == -1)
		return 0;
	r = read(fd, buf, len);
	if (close(fd))
		exit(-1);

	return (r == (ssize_t)len) && (memcmp(buf, prefix, len) == 0);
}

static void
test_hardlink_predicate(void)
{
	char a[PATH_MAX], b[PATH_MAX];
	struct stat st;
	int fd;

	sandbox_path(a, sizeof(a), "hl-a");
	sandbox_path(b, sizeof(b), "hl-b");

	if ((fd = open(a, O_RDWR|O_CREAT|O_TRUNC, 0600)) == -1) {
		CHECK(0);
		return;
	}
	if (write(fd, "x\n", 2) != 2) {
		close(fd);
		CHECK(0);
		return;
	}
	if (close(fd))
		exit(-1);

	CHECK(stat(a, &st) == 0 && !emuxfs_is_hardlink(&st));
	if (link(a, b)) {
		CHECK(0);
		unlink(a);
		return;
	}
	CHECK(stat(a, &st) == 0 && emuxfs_is_hardlink(&st));
	CHECK(stat(b, &st) == 0 && emuxfs_is_hardlink(&st));
	unlink(b);
	CHECK(stat(a, &st) == 0 && !emuxfs_is_hardlink(&st));
	unlink(a);

	/* A symlink is not a regular file and is never a hard link. */
	if (symlink("target", a) == 0) {
		CHECK(lstat(a, &st) == 0 && !emuxfs_is_hardlink(&st));
		unlink(a);
	}
}

static void
test_state_validation(void)
{
	struct emuxfs_dev_state s;

	/* Clean. */
	memset(&s, 0, sizeof(s));
	CHECK(emuxfs_dev_state_is_valid(&s));

	/* Individually valid non-clean states. */
	memset(&s, 0, sizeof(s));
	s.mounted = 1;
	CHECK(emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.mounted = 1;
	s.working = 1;
	CHECK(emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.restoring = 1;
	CHECK(emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.degraded = 1;
	CHECK(emuxfs_dev_state_is_valid(&s));

	/* Structurally impossible states. */
	memset(&s, 0, sizeof(s));
	s.mounted = 2;
	CHECK(!emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.working = 2;
	CHECK(!emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.restoring = 2;
	CHECK(!emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.degraded = 2;
	CHECK(!emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.working = 1;
	s.restoring = 1;
	CHECK(!emuxfs_dev_state_is_valid(&s));

	/* Boundary values: seq may be maximal; counters may not. */
	memset(&s, 0, sizeof(s));
	s.seq = UINT64_MAX;
	CHECK(emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.working = UINT64_MAX;
	CHECK(!emuxfs_dev_state_is_valid(&s));
	memset(&s, 0, sizeof(s));
	s.restoring = UINT64_MAX;
	CHECK(!emuxfs_dev_state_is_valid(&s));
}

static void
test_assign_crosscheck(void)
{
	char			 root[PATH_MAX];
	struct emuxfs_dev	*dev;
	struct emuxfs_assign	 assign;
	struct emuxfs_meta	 meta;
	struct stat		 st;
	uint8_t			 uuid[EMUXFS_UUID_SIZE];
	size_t			 chksz, metasz, bad;
	time_t			 now;
	dind			 i;

	if (geteuid() != 0) {
		printf("SKIP: assign cross-check test requires root\n");
		return;
	}

	sandbox_path(root, sizeof(root), "xcheck");
	if (mkdir(root, 0700)) {
		CHECK(0);
		return;
	}
	chksz = emuxfs_chk_size(CAT_MD5);
	if (emuxfs_meta_size_raw(&metasz, CAT_MD5)) {
		CHECK(0);
		return;
	}
	memset(uuid, 0x44, sizeof(uuid));
	now = time(NULL);
	if (emuxfs_dev_format(root, CAT_MD5, chksz, metasz, now, uuid)) {
		CHECK(0);
		return;
	}
	emuxfs_dev_module_init();
	if (emuxfs_dev_append(&i, root) || emuxfs_dev_mount(i, 0) || i != 0) {
		CHECK(0);
		return;
	}
	if (emuxfs_test_fabricate(0, "x", "data\n")) {
		CHECK(0);
		emuxfs_dev_unmount(0);
		return;
	}
	if (emuxfs_dev_get(&dev, 0, 0) ||
	    fstatat(dev->root_fd, "x", &st, AT_SYMLINK_NOFOLLOW)) {
		CHECK(0);
		emuxfs_dev_unmount(0);
		return;
	}

	/* Consistent mapping. */
	CHECK(emuxfs_meta_assign_check(0, &bad) == 0 && bad == 0);
	CHECK(emuxfs_readback(0, "x", 0, NULL) == 0);

	/* Out-of-range indices must fail cleanly, not overflow. */
	CHECK(emuxfs_meta_read(&meta, 0, UINT64_MAX) != 0);
	CHECK(emuxfs_assign_read(&assign, 0, UINT64_MAX) != 0);
	CHECK(emuxfs_assign_read(&assign, 0, (uint64_t)1000000) != 0);

	/* Corrupt the eno -> ino mapping: both checks must notice. */
	assign.flags = AF_ASSIGNED;
	assign.ino = st.st_ino + 1;
	CHECK(emuxfs_assign_write(&assign, 0, 1000) == 0);
	CHECK(emuxfs_meta_assign_check(0, &bad) == 0 && bad >= 1);
	CHECK(emuxfs_readback(0, "x", 0, NULL) != 0);

	/* Clear the mapping entirely: readback must fail. */
	assign.flags = 0;
	assign.ino = 0;
	CHECK(emuxfs_assign_write(&assign, 0, 1000) == 0);
	CHECK(emuxfs_readback(0, "x", 0, NULL) != 0);

	/* Repair it: both checks recover. */
	assign.flags = AF_ASSIGNED;
	assign.ino = st.st_ino;
	CHECK(emuxfs_assign_write(&assign, 0, 1000) == 0);
	CHECK(emuxfs_meta_assign_check(0, &bad) == 0 && bad == 0);
	CHECK(emuxfs_readback(0, "x", 0, NULL) == 0);

	emuxfs_dev_unmount(0);
}

static void
test_recovery_ambiguity(void)
{
	char			 roots[3][PATH_MAX];
	char			 name[16];
	char			 pbuf[PATH_MAX];
	static const char	*contents[3] = {
		"alpha\n", "beta\n", "gamma\n"
	};
	struct emuxfs_dev	*ddev;
	uint8_t			 uuid[EMUXFS_UUID_SIZE];
	size_t			 chksz, metasz;
	time_t			 now;
	dind			 i, j, k;
	int			 fd;

	if (geteuid() != 0) {
		printf("SKIP: ambiguity test requires root\n");
		return;
	}

	chksz = emuxfs_chk_size(CAT_MD5);
	if (emuxfs_meta_size_raw(&metasz, CAT_MD5)) {
		CHECK(0);
		return;
	}
	memset(uuid, 0x33, sizeof(uuid));
	now = time(NULL);

	emuxfs_dev_module_init();
	for (i = 0; i < 3; ++i) {
		snprintf(name, sizeof(name), "amb%u", (unsigned)i);
		sandbox_path(roots[i], sizeof(roots[i]), name);
		if (mkdir(roots[i], 0700)) {
			CHECK(0);
			return;
		}
		if (emuxfs_dev_format(roots[i], CAT_MD5, chksz, metasz, now,
		    uuid)) {
			CHECK(0);
			return;
		}
	}
	for (i = 0; i < 3; ++i) {
		if (emuxfs_dev_append(&k, roots[i]) || emuxfs_dev_mount(k, 0)) {
			CHECK(0);
			return;
		}
		if (k != i) {
			CHECK(0);
			return;
		}
	}

	/* Three internally valid but different copies of "f". */
	for (i = 0; i < 3; ++i) {
		if (emuxfs_test_fabricate(i, "f", contents[i])) {
			CHECK(0);
			emuxfs_dev_unmount(i);
			return;
		}
	}

	/* Damage the copy on device 0 so that a restore would be attempted. */
	if (emuxfs_dev_get(&ddev, 0, 0)) {
		CHECK(0);
		return;
	}
	snprintf(pbuf, sizeof(pbuf), "%s/%s", ddev->root_path, "f");
	if ((fd = open(pbuf, O_WRONLY|O_TRUNC)) == -1) {
		CHECK(0);
		return;
	}
	if (write(fd, "corrupt\n", 8) != 8) {
		close(fd);
		CHECK(0);
		return;
	}
	if (close(fd))
		exit(-1);

	emuxfs_state_restore_queue_final();
	if (emuxfs_state_restore_queue_init()) {
		CHECK(0);
		return;
	}
	emuxfs_state_ambiguity_clear();
	if (emuxfs_state_restore_push_back(0, "f")) {
		CHECK(0);
		return;
	}
	emuxfs_restore_now();

	/* The ambiguous restoration must have been refused, not performed. */
	CHECK(emuxfs_state_ambiguity_count() == 1);
	CHECK(emuxfs_test_file_has_prefix(pbuf, "corrupt\n"));

	for (j = 3; j > 0; --j) {
		if (emuxfs_dev_is_mounted(j - 1))
			emuxfs_dev_unmount(j - 1);
	}
	emuxfs_state_restore_queue_final();
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	if (emuxfs_state_syslog_init())
		return 2;

	sandbox_create();

#define RUN(fn) do { fprintf(stderr, "[%s]\n", #fn); fn(); } while (0)
	RUN(test_checksums);
	RUN(test_meta_size_and_align);
	RUN(test_desc_meta);
	RUN(test_conf_roundtrip);
	RUN(test_conf_legacy_and_errors);
	RUN(test_path_sanitize);
	RUN(test_serialisation_endianness);
	RUN(test_restore_queue);
	RUN(test_dev_format_mount);
	RUN(test_state_validation);
	RUN(test_assign_crosscheck);
	RUN(test_hardlink_predicate);
	RUN(test_recovery_ambiguity);
#undef RUN

	sandbox_destroy();

	if (failures != 0) {
		fprintf(stderr, "%d unit test(s) failed\n", failures);
		return 1;
	}
	printf("All unit tests passed\n");
	return 0;
}
