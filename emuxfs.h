/* emuxfs.h */
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

#ifndef _EMUXFS_H_
#define _EMUXFS_H_

#include <sys/cdefs.h>
#include <sys/syslimits.h>
#include <sys/types.h>

#include <stdint.h>

struct dirent;
struct stat;

/* Internal error.  Usually unrecoverable like ENOMEM. */
#define EMUXFS_EINT 1
/* File system error.  For example ENOENT. */
#define EMUXFS_EFS 2
/* Checksum error.  Used to indicate that a data integrity check has failed. */
#define EMUXFS_ECHK 3

#define EMUXFS_DEV_COUNT_MAX 64
#define EMUXFS_CHKSZ_MAX 20
#define EMUXFS_BLOCK_SIZE (4*1024)
#define EMUXFS_MEM_ALIGN (sizeof(uint64_t))
#define EMUXFS_UUID_SIZE 16
#define EMUXFS_WRBUF_SIZE (1024*1024)

/*
 * The private on-disk directory and its configuration file keep their
 * historical names for compatibility with arrays created by muxfs.  The
 * project, the executable and the internal symbol prefix are "emuxfs", but
 * branding does not override on-disk compatibility.  dev.c derives every
 * private path from EMUXFS_PRIVATE_DIR; other files that must recognise the
 * name (the directory walkers) compare against the same literal.
 */
#define EMUXFS_PRIVATE_DIR ".muxfs"
#define EMUXFS_CONF_FILE "muxfs.conf"

/*
 * Persistent format version.  This value is written to the configuration file
 * and identifies the on-disk layout of the private directory.  It must only
 * be bumped when a change makes an old array unreadable by a newer program or
 * vice versa.  Adding a checksum algorithm does not require a bump; changing
 * the encoding of meta.db, assign.db, state.db or lfile does.
 */
#define EMUXFS_FORMAT_VERSION 1u
/*
 * Arrays formatted before the format_version key existed used the layout that
 * EMUXFS_FORMAT_VERSION 1 describes, so they are accepted as version 1.
 */
#define EMUXFS_FORMAT_VERSION_LEGACY 1u

/*
 * Return values of emuxfs_conf_parse().  Callers use these to distinguish a
 * malformed configuration from a well-formed but unsupported format.
 */
#define EMUXFS_CONF_OK		 0
#define EMUXFS_CONF_EPARSE	 1
#define EMUXFS_CONF_EBADVERSION	 2

#define EMUXFS_DT_REG 1u
#define EMUXFS_DT_DIR 2u
#define EMUXFS_DT_LNK 3u

/* chk.c */
struct emuxfs_chk;
enum emuxfs_chk_alg_type {
	CAT_CRC32,
	CAT_MD5,
	CAT_SHA1,
	CAT_NONE
};
EMUXFS size_t emuxfs_chk_size(enum emuxfs_chk_alg_type);
EMUXFS void emuxfs_chk_init(struct emuxfs_chk *, enum emuxfs_chk_alg_type);
EMUXFS void emuxfs_chk_update(struct emuxfs_chk *, const uint8_t *, size_t);
EMUXFS void emuxfs_chk_final(uint8_t *, struct emuxfs_chk *);
EMUXFS int emuxfs_chk_str_to_type(enum emuxfs_chk_alg_type *, const char *,
    size_t);
EMUXFS const char *emuxfs_chk_type_to_str(enum emuxfs_chk_alg_type);

/* conf.c */
enum emuxfs_version_flavor {
	VF_CURRENT,
	VF_RELEASE,
	VF_STABLE
};
struct emuxfs_version {
	uint32_t			number,
					revision;
	enum emuxfs_version_flavor	flavor;
};
struct emuxfs_dev_conf {
	struct emuxfs_version		version;
	uint32_t			format_version;
	enum emuxfs_chk_alg_type	chk_alg_type;

	/* UUIDs are encoded as binary, little-endian. */
	uint8_t				array_uuid[EMUXFS_UUID_SIZE];
	uint8_t				dev_uuid[EMUXFS_UUID_SIZE];

	/* The epoch time at which the sequence number was last at value 0. */
	time_t				seq_zero_time;
};
/* Returns one of EMUXFS_CONF_*. */
EMUXFS int emuxfs_conf_parse(struct emuxfs_dev_conf *, int);
EMUXFS int emuxfs_conf_write(struct emuxfs_dev_conf *, int);

/* dev.c */
/*
 * In emuxfs a 'device' is actually a directory that has been 'formatted' for
 * use by emuxfs.  Do not conflate this with an actual block device, it is only
 * analogous to a block device insofar as one is used by a more conventional
 * filesystem.
 */
struct emuxfs_dev_state {
	uint64_t	seq; /* Sequence number. */
	uint64_t	mounted;
	uint64_t	working;
	uint64_t	restoring;
	uint64_t	degraded;
};
EMUXFS int emuxfs_dev_state_write_fd(int, struct emuxfs_dev_state *);

struct emuxfs_dev {
	struct emuxfs_dev_state	 state;
	int			 root_fd,
				 state_fd,
				 meta_fd,
				 assign_fd,
				 lfile_fd;
	const char		*root_path;
	struct emuxfs_dev_conf	 conf;
	int			 attached_now,
				 mounted_now,
				 readonly_now;
};

typedef size_t dind;
EMUXFS void emuxfs_dev_module_init(void);
EMUXFS int emuxfs_dev_append(dind *dev_index_out, const char *);
EMUXFS int emuxfs_dev_open(dind, int, int);
EMUXFS int emuxfs_dev_mount(dind, int);
EMUXFS int emuxfs_dev_state_is_valid(const struct emuxfs_dev_state *);
EMUXFS int emuxfs_dev_unmount(dind);
EMUXFS int emuxfs_dev_is_mounted(dind);
EMUXFS dind emuxfs_dev_count(void);
EMUXFS int emuxfs_dev_get(struct emuxfs_dev **, dind, int);
EMUXFS int emuxfs_dev_seq_check(void);

enum emuxfs_meta_flag {
	MF_ASSIGNED = 0x1
};
struct emuxfs_meta_header {
	uint64_t	flags;
	uint64_t	eno;
};
struct emuxfs_meta {
	struct emuxfs_meta_header	header;
	/*
	 * When stack-allocated this buffer guarantees enough space for the
	 * sums.  If the content of the metadata file is loaded and cast via
	 * pointer to this struct then care must be taken to avoid reading
	 * beyond the actual bounds of the metadata entry.  Similarly the whole
	 * of this buffer should not necessarily be written to the metadata
	 * file.  Use emuxfs_meta_size() to compute the actual bounds of this
	 * struct, and emuxfs_chk_size() to compute the offset of the content
	 * checksum.
	 */
	uint8_t				checksums[2 * EMUXFS_CHKSZ_MAX];
};
_Static_assert(sizeof(struct emuxfs_meta) ==
    sizeof(struct emuxfs_meta_header) + 2 * EMUXFS_CHKSZ_MAX,
    "meta.db entry buffer must not contain padding");
EMUXFS int emuxfs_meta_size(size_t *, dind);
EMUXFS int emuxfs_meta_size_raw(size_t *, enum emuxfs_chk_alg_type);
EMUXFS int emuxfs_meta_read(struct emuxfs_meta *, dind, uint64_t);
EMUXFS int emuxfs_meta_write(const struct emuxfs_meta *, dind, uint64_t);
EMUXFS int emuxfs_meta_write_fd(int, const struct emuxfs_meta *, uint64_t,
    size_t);

enum emuxfs_assign_flag {
	AF_ASSIGNED = 0x1
};
struct emuxfs_assign {
	uint64_t	flags;
	uint64_t	ino;
};

/*
 * Serialisation invariants.  These structures are written to the persistent
 * databases with explicit little-endian conversion, so their in-memory size
 * must match the documented on-disk record sizes exactly.  A failure here
 * means the format changed; see ON_DISK_FORMAT.md.
 */
_Static_assert(sizeof(struct emuxfs_dev_state) == 5 * sizeof(uint64_t),
    "state.db record must be five 64-bit fields");
_Static_assert(sizeof(struct emuxfs_meta_header) == 2 * sizeof(uint64_t),
    "meta.db header must be two 64-bit fields");
_Static_assert(sizeof(struct emuxfs_assign) == 2 * sizeof(uint64_t),
    "assign.db record must be two 64-bit fields");
_Static_assert(EMUXFS_CHKSZ_MAX == 20,
    "the reference checksum size is SHA-1 and drives lfile geometry");

EMUXFS int emuxfs_assign_peek_next_eno(uint64_t *, dind);
EMUXFS int emuxfs_assign_read(struct emuxfs_assign *, dind, uint64_t);
EMUXFS int emuxfs_assign_write(const struct emuxfs_assign *, dind, uint64_t);
EMUXFS int emuxfs_assign_validate(dind, uint64_t, uint64_t);
EMUXFS int emuxfs_meta_assign_check(dind, size_t *);
EMUXFS int emuxfs_assign_write_fd(int, const struct emuxfs_assign *, uint64_t);

EMUXFS int emuxfs_working_push(dind);
EMUXFS int emuxfs_working_pop(dind, time_t);

EMUXFS int emuxfs_restoring_push(dind);
EMUXFS int emuxfs_restoring_pop(dind);

EMUXFS int emuxfs_degraded_set(dind);
EMUXFS int emuxfs_degraded_clear(dind);

/* desc.c */
typedef uint64_t emuxfs_desc_type;
struct emuxfs_desc {
	uint64_t		eno;
	emuxfs_desc_type	type;
	uint64_t		owner;
	uint64_t		group;
	uint64_t		mode;
	uint64_t		size;
	uint8_t			content_checksum[EMUXFS_CHKSZ_MAX];
};
EMUXFS int emuxfs_desc_type_from_mode(emuxfs_desc_type *, mode_t);
EMUXFS int emuxfs_desc_init_from_stat(struct emuxfs_desc *, struct stat *,
    uint64_t);
EMUXFS void emuxfs_desc_chk_provided_content(struct emuxfs_desc *,
    const uint8_t *,
    size_t, enum emuxfs_chk_alg_type);
EMUXFS int emuxfs_desc_chk_reg_content(struct emuxfs_desc *, dind,
    const char *);
EMUXFS int emuxfs_desc_chk_symlink_content(struct emuxfs_desc *, dind,
    const char *);
EMUXFS int emuxfs_desc_chk_node_content(struct emuxfs_desc *, dind,
    const char *);
EMUXFS void emuxfs_desc_chk_meta(uint8_t *, const struct emuxfs_desc *,
    enum emuxfs_chk_alg_type);

/* format.c */
EMUXFS int emuxfs_dev_format(const char *, enum emuxfs_chk_alg_type, size_t,
    size_t, time_t, const uint8_t *);
EMUXFS int emuxfs_format_main(int, char *[]);

/* lfile.c */
struct emuxfs_range {
	size_t		byte_begin,
			byte_end,
			blk_begin,
			blk_end,
			lfilesz,
			lfileoff;
	uint64_t	blk_index_begin,
			blk_index_end;
};
EMUXFS void emuxfs_range_compute(struct emuxfs_range *, size_t);
EMUXFS int emuxfs_lfile_open(int *, int, ino_t, int);
EMUXFS int emuxfs_lfile_create(int, size_t, ino_t, size_t);
EMUXFS int emuxfs_lfile_resize(int, size_t, ino_t, size_t, size_t);
EMUXFS int emuxfs_lfile_exists(int *, int, ino_t);
EMUXFS int emuxfs_lfile_delete(int, ino_t);
EMUXFS int emuxfs_lfile_ancestors_recompute(uint8_t *, int,
    enum emuxfs_chk_alg_type, ino_t, size_t, uint64_t, uint64_t);
EMUXFS int emuxfs_lfile_readback(uint8_t *, dind, const char *, size_t, size_t,
    const uint8_t *);

/* mount.c */
EMUXFS int emuxfs_mount_main(int, char *[]);

/* scan.c */
enum emuxfs_scan_mode {
	EMUXFS_SCAN_AUDIT,
	EMUXFS_SCAN_HEAL,
};
EMUXFS int emuxfs_scan_main(enum emuxfs_scan_mode, int, char *[]);

/* state.c */
struct emuxfs_wrctx {
	uid_t user;
	gid_t group;
};
struct emuxfs_wrbuf {
	char path[PATH_MAX];
	struct emuxfs_wrctx wc;
	size_t sz;
	size_t off;
	uint8_t buf[EMUXFS_WRBUF_SIZE];
};
EMUXFS int  emuxfs_init(int);
EMUXFS int  emuxfs_final(void);
EMUXFS int  emuxfs_state_syslog_init(void);
EMUXFS int  emuxfs_state_syslog_final(void);
EMUXFS void emuxfs_debug(const char *, ...) __printflike(1, 2);
EMUXFS void emuxfs_info(const char *, ...) __printflike(1, 2);
EMUXFS void emuxfs_warn(const char *, ...) __printflike(1, 2);
EMUXFS void emuxfs_alert(const char *, ...) __printflike(1, 2);

EMUXFS int  emuxfs_state_restore_queue_init(void);
EMUXFS void emuxfs_state_restore_queue_final(void);
EMUXFS int  emuxfs_state_restore_only_set(dind);
EMUXFS int  emuxfs_state_is_restore_only(void);
EMUXFS int  emuxfs_state_restore_push_back(dind, const char *);
EMUXFS int  emuxfs_state_restore_next_path_len(size_t *);
EMUXFS int  emuxfs_state_restore_pop_front(dind *, char *);

/*
 * Number of restorations that were refused because several internally valid
 * copies disagreed.  heal reports failure when this is non-zero; the caller
 * must resolve the ambiguity explicitly (for example with 'emuxfs sync').
 */
EMUXFS void   emuxfs_state_ambiguity_note(void);
EMUXFS size_t emuxfs_state_ambiguity_count(void);
EMUXFS int    emuxfs_state_ambiguity_clear(void);

EMUXFS int emuxfs_state_eno_next_init(uint64_t);
EMUXFS int emuxfs_state_eno_next_acquire(uint64_t *);
EMUXFS int emuxfs_state_eno_next_return(uint64_t);

EMUXFS int emuxfs_state_wrbuf_is_set(void);
EMUXFS int emuxfs_state_wrbuf_reset(void);
EMUXFS int emuxfs_state_wrbuf_set(const char *, uid_t, gid_t, size_t,
    size_t, const uint8_t *);
EMUXFS int emuxfs_state_wrbuf_append(size_t *, const char *, uid_t, gid_t,
    size_t, size_t, const uint8_t *);
EMUXFS int emuxfs_state_wrbuf_get(const struct emuxfs_wrbuf **);

/* sync.c */
EMUXFS int emuxfs_sync_main(int, char *[]);

/* util.c */
enum emuxfs_cud_type {
	EMUXFS_CUD_CREATE,
	EMUXFS_CUD_UPDATE,
	EMUXFS_CUD_DELETE
};
struct emuxfs_cud {
	enum emuxfs_cud_type	 type;
	const char		*path,
				*fname;
	struct emuxfs_meta	 pre_meta;
};
struct emuxfs_dir {
	void *base;
	struct dirent **ent_array;
	size_t ent_count;
};
struct emuxfs_args {
	char	mp_path[PATH_MAX];
	char	dev_paths[EMUXFS_DEV_COUNT_MAX][PATH_MAX];
	size_t	dev_count;
	int	f;
	int	readonly;
};
extern struct emuxfs_args emuxfs_cmdline;
EMUXFS int emuxfs_parse_args(int, char **, int);
EMUXFS int emuxfs_existsat(int *, int, const char *);
EMUXFS int emuxfs_removeat(int, const char *);
EMUXFS int emuxfs_dir_is_empty(int *, char const *);
EMUXFS int emuxfs_path_sanitize(const char **);
EMUXFS int emuxfs_path_pop(const char **, char *, size_t *);
EMUXFS int emuxfs_pushdir(struct emuxfs_dir *, int, const char *);
EMUXFS int emuxfs_popdir(struct emuxfs_dir *);
EMUXFS int emuxfs_readback(dind, const char *, int, const struct emuxfs_meta *);
EMUXFS int emuxfs_parent_readback(dind, const char *);
EMUXFS int emuxfs_ancestors_meta_recompute(dind, struct emuxfs_cud *);
EMUXFS int emuxfs_dir_meta_recompute(struct emuxfs_cud *, dind,
    const struct emuxfs_cud *);
EMUXFS int emuxfs_parent_gid(gid_t *, const char *);
EMUXFS int emuxfs_dir_content_chk(uint8_t *, dind, struct emuxfs_dir *);
EMUXFS size_t emuxfs_align_up(size_t, size_t);
EMUXFS size_t emuxfs_align_down(size_t, size_t);
EMUXFS void emuxfs_restore_now(void);

/*
 * True when 'st' describes a regular file with more than one hard link.
 * emuxfs does not support hard links: two names for one inode share a single
 * metadata slot and a single eno, so unlinking or restoring one name would
 * silently affect the other.  Callers refuse such nodes instead of treating
 * them as independent objects.
 */
EMUXFS int emuxfs_is_hardlink(const struct stat *);

/*
 * Exact positioning I/O.  Both return 0 on success and 1 on error or
 * unexpected short I/O.  Unlike a bare pread(2)/pwrite(2) call they cannot
 * silently perform partial transfers: they loop over EINTR and over short
 * transfers until the requested byte count has been moved.
 */
EMUXFS int emuxfs_pread_exact(int, void *, size_t, off_t);
EMUXFS int emuxfs_pwrite_exact(int, const void *, size_t, off_t);

/*
 * fsync(2) the directory that contains 'path' (relative to 'root_fd'), so that
 * a preceding create, rename or unlink is durable.  Returns 0 on success.
 */
EMUXFS int emuxfs_fsync_parent(int, const char *);

/* version.c */
extern struct emuxfs_version emuxfs_program_version;
EMUXFS void emuxfs_version_print(void);

#endif /* _EMUXFS_H_ */
