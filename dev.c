/* dev.c */
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

#include <sys/stat.h>
#include <sys/syslimits.h>
#include <endian.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "emuxfs.h"

#define EMUXFS_PATH_DIR	EMUXFS_PRIVATE_DIR
#define EMUXFS_PATH_CONF		EMUXFS_PATH_DIR"/"EMUXFS_CONF_FILE
#define EMUXFS_PATH_STATE_DB	EMUXFS_PATH_DIR"/state.db"
#define EMUXFS_PATH_META_DB	EMUXFS_PATH_DIR"/meta.db"
#define EMUXFS_PATH_ASSIGN_DB	EMUXFS_PATH_DIR"/assign.db"
#define EMUXFS_PATH_LFILE	EMUXFS_PATH_DIR"/lfile"

/*
 * We store the roots here instead of struct emuxfs_dev so files that include
 * emuxfs.h don't have to include sys/syslimits.h for PATH_MAX.
 */
static char			emuxfs_dev_roots[EMUXFS_DEV_COUNT_MAX][PATH_MAX];
static struct emuxfs_dev	emuxfs_dev_array[EMUXFS_DEV_COUNT_MAX];
static dind			emuxfs_dev_array_count;
static dind			emuxfs_dev_array_mounted_count;
static dind			emuxfs_dev_array_degraded_count;

/*
 * Validate the fields that a state.db record can express.  A record is
 * *structurally invalid* (return 0) when it contains values this
 * implementation can never produce; it is merely *dirty* when it is valid but
 * non-clean.  Only structural impossibility is rejected here: a dirty record
 * must remain readable so that recovery is possible.
 *
 * Derived from the code:
 *   - mounted, working, restoring and degraded are 0 or 1 (operations are
 *     performed one at a time and push/pop are balanced);
 *   - working and restoring are mutually exclusive (the write path uses
 *     working, the recovery path uses restoring, never both);
 *   - seq may be any uint64 value, including 0 and UINT64_MAX.
 */
EMUXFS int
emuxfs_dev_state_is_valid(const struct emuxfs_dev_state *state)
{
	EMUXFS_TRACE("enter");
	if (state->mounted > 1 || state->working > 1 ||
	    state->restoring > 1 || state->degraded > 1)
		return 0;
	if (state->working != 0 && state->restoring != 0)
		return 0;
	return 1;
}

/* Returns 0 on success, 1 on I/O or size error, 2 on a structurally invalid
 * record. */
static int
emuxfs_dev_state_read(struct emuxfs_dev_state *state, int fd)
{
	EMUXFS_TRACE("enter");
	struct stat st;
	struct emuxfs_dev_state disk_state;

	if (fstat(fd, &st))
		return 1;
	if (st.st_size != (off_t)sizeof(*state))
		return 1;
	memset(&disk_state, 0, sizeof(disk_state));
	if (emuxfs_pread_exact(fd, &disk_state, sizeof(disk_state), 0))
		return 1;

	*state = (struct emuxfs_dev_state){
	    .seq       = letoh64(disk_state.seq),
	    .mounted   = letoh64(disk_state.mounted),
	    .working   = letoh64(disk_state.working),
	    .restoring = letoh64(disk_state.restoring),
	    .degraded  = letoh64(disk_state.degraded),
	};

	if (!emuxfs_dev_state_is_valid(state))
		return 2;

	return 0;
}

static int
emuxfs_dev_state_is_clean(struct emuxfs_dev_state *state)
{
	EMUXFS_TRACE("enter");
	return !(state->mounted || state->restoring || state->working ||
	    state->degraded);
}

EMUXFS int
emuxfs_dev_state_write_fd(int fd, struct emuxfs_dev_state *state)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev_state disk_state;

	const size_t sz = sizeof(disk_state);

	EMUXFS_TRACE("state_write fd=%d seq=%llu mounted=%llu working=%llu "
	    "restoring=%llu degraded=%llu", fd, (unsigned long long)state->seq,
	    (unsigned long long)state->mounted,
	    (unsigned long long)state->working,
	    (unsigned long long)state->restoring,
	    (unsigned long long)state->degraded);

	disk_state = (struct emuxfs_dev_state){
	    .seq       = htole64(state->seq),
	    .mounted   = htole64(state->mounted),
	    .working   = htole64(state->working),
	    .restoring = htole64(state->restoring),
	    .degraded  = htole64(state->degraded),
	};

	/*
	 * state.db carries recovery state (seq, working, restoring, degraded),
	 * so it must be durable before the operation that follows is
	 * considered committed.  A torn write is not detectable (the format
	 * has no checksum), but an fsync removes the "write not yet on disk"
	 * window and makes the seq/working comparison meaningful after a
	 * power loss.
	 */
	if (emuxfs_pwrite_exact(fd, &disk_state, sz, 0))
		return 1;
	if (fsync(fd))
		return 1;

	return 0;
}

static int
emuxfs_dev_state_mount(struct emuxfs_dev_state *state, int fd)
{
	EMUXFS_TRACE("enter");
	state->mounted = 1;
	if (emuxfs_dev_state_write_fd(fd, state))
		return 1;
	return 0;
}

static int
emuxfs_dev_state_unmount(struct emuxfs_dev_state *state, int fd)
{
	EMUXFS_TRACE("enter");
	state->mounted = 0;
	if (emuxfs_dev_state_write_fd(fd, state))
		return 1;
	return 0;
}

EMUXFS dind
emuxfs_dev_count(void)
{
	EMUXFS_TRACE("enter");
	return emuxfs_dev_array_count;
}

EMUXFS int
emuxfs_dev_get(struct emuxfs_dev **dev_out, size_t dev_index, int force)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (dev_index >= emuxfs_dev_array_count)
		return 1;

	dev = &emuxfs_dev_array[dev_index];
	if (!force) {
		if (!dev->mounted_now)
			return 1;
		if (dev->state.degraded)
			return 1;
	}

	*dev_out = dev;
	return 0;
}

static void
emuxfs_dev_init(dind dev_index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	memset(&emuxfs_dev_roots[dev_index], 0, PATH_MAX);

	dev = &emuxfs_dev_array[dev_index];
	memset(dev, 0, sizeof(*dev));
	dev->root_fd =
	    dev->state_fd =
	    dev->meta_fd =
	    dev->assign_fd =
	    dev->lfile_fd = -1;
}

EMUXFS void
emuxfs_dev_module_init(void)
{
	EMUXFS_TRACE("enter");
	dind i;

	memset(&emuxfs_dev_roots, 0, EMUXFS_DEV_COUNT_MAX * (PATH_MAX));
	for (i = 0; i < EMUXFS_DEV_COUNT_MAX; ++i)
		emuxfs_dev_init(i);
	emuxfs_dev_array_count = 0;
	emuxfs_dev_array_mounted_count = 0;
	emuxfs_dev_array_degraded_count = 0;
}

EMUXFS int
emuxfs_dev_is_mounted(dind dev_index)
{
	EMUXFS_TRACE("enter");
	return emuxfs_dev_array[dev_index].mounted_now;
}

EMUXFS int
emuxfs_dev_append(dind *dev_index_out, const char *path)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	dind i;
	size_t len;

	if (emuxfs_dev_array_count == EMUXFS_DEV_COUNT_MAX)
		return 1;

	i = emuxfs_dev_array_count;
	dev = &emuxfs_dev_array[i];

	len = strlen(path);
	if (len >= PATH_MAX)
		return 1;

	strlcpy(emuxfs_dev_roots[i], path, PATH_MAX);
	dev->root_path = emuxfs_dev_roots[i];
	dev->attached_now = 1;
	if (dev_index_out != nullptr)
		*dev_index_out = i;
	++emuxfs_dev_array_count;

	return 0;
}

EMUXFS int
emuxfs_dev_open(dind dev_index, int force, int readonly)
{
	EMUXFS_TRACE("enter");
	int rc, root_fd, conf_fd, state_fd, meta_fd, assign_fd, lfile_fd;
	int file_flags;
	struct emuxfs_dev *dev, *first;
	dind i, dev_count;

	root_fd =
	    conf_fd =
	    state_fd =
	    meta_fd =
	    assign_fd =
	    lfile_fd = -1;

	dev = &emuxfs_dev_array[dev_index];
	if (!dev->attached_now)
		return 1;
	if (dev->mounted_now)
		return 1;

	if ((root_fd = open(dev->root_path, O_RDONLY|O_DIRECTORY|O_CLOEXEC)) ==
	    -1)
		goto fail;

	if ((conf_fd = openat(root_fd, EMUXFS_PATH_CONF,
	    O_RDONLY|O_NOFOLLOW|O_CLOEXEC)) == -1)
		goto fail;
	rc = emuxfs_conf_parse(&dev->conf, conf_fd);
	if (rc != EMUXFS_CONF_OK) {
		if (rc == EMUXFS_CONF_EBADVERSION)
			dprintf(2, "Error: %s: unsupported on-disk format "
			    "version.\n", dev->root_path);
		goto fail;
	}
	if (close(conf_fd))
		goto fail;
	conf_fd = -1;

	if (emuxfs_dev_array_mounted_count > 0) {
		first = nullptr;
		dev_count = emuxfs_dev_count();
		for (i = 0; i < dev_count; ++i) {
			if (i == dev_index)
				continue;
			if (!emuxfs_dev_get(&first, i, 0))
				break;
		}
		if ((first == nullptr) ||
		    (bcmp(dev->conf.array_uuid, first->conf.array_uuid,
		     EMUXFS_UUID_SIZE) != 0))
			goto fail;
	}

	file_flags = readonly ?
	    (O_RDONLY|O_NOFOLLOW|O_CLOEXEC) :
	    (O_RDWR|O_NOFOLLOW|O_CLOEXEC);

	if ((state_fd = openat(root_fd, EMUXFS_PATH_STATE_DB, file_flags)) ==
	    -1)
		goto fail;
	rc = emuxfs_dev_state_read(&dev->state, state_fd);
	if (rc == 2) {
		/*
		 * A structurally impossible record cannot come from correct
		 * code; it is refused by mount/audit/heal.  'force' is only
		 * used by the explicit 'sync' recovery, which rebuilds the
		 * device from a source and rewrites state.db, so allow it to
		 * proceed with a loud warning instead of leaving the device
		 * unrecoverable.
		 */
		if (!force) {
			dprintf(2, "Error: %s: structurally invalid state.db "
			    "(impossible field combination).\n",
			    dev->root_path);
			goto fail;
		}
		dprintf(2, "Warning: %s: structurally invalid state.db; "
		    "continuing because force was requested.\n",
		    dev->root_path);
	} else if (rc != 0)
		goto fail;
	if (!force && !emuxfs_dev_state_is_clean(&dev->state))
		goto fail;

	if ((meta_fd = openat(root_fd, EMUXFS_PATH_META_DB, file_flags)) == -1)
		goto fail;

	if ((assign_fd = openat(root_fd, EMUXFS_PATH_ASSIGN_DB, file_flags)) ==
	    -1)
		goto fail;

	/*
	 * Open every descriptor before recording 'mounted': if a later open
	 * fails, the device must not be left marked as mounted on disk, or a
	 * transient failure (for example EMFILE) would refuse all subsequent
	 * mounts until an explicit recovery.
	 */
	if ((lfile_fd = openat(root_fd, EMUXFS_PATH_LFILE,
	    O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) == -1)
		goto fail;

	if (!readonly && emuxfs_dev_state_mount(&dev->state, state_fd))
		goto fail;

	dev->root_fd = root_fd;
	dev->state_fd = state_fd;
	dev->meta_fd = meta_fd;
	dev->assign_fd = assign_fd;
	dev->lfile_fd = lfile_fd;
	dev->readonly_now = readonly;
	dev->mounted_now = 1;
	++emuxfs_dev_array_mounted_count;

	return 0;
fail:
	if (lfile_fd != -1)
		close(lfile_fd);
	if (assign_fd != -1)
		close(assign_fd);
	if (meta_fd != -1)
		close(meta_fd);
	if (state_fd != -1)
		close(state_fd);
	if (conf_fd != -1)
		close(conf_fd);
	if (root_fd != -1)
		close(root_fd);
	return 1;
}

EMUXFS int
emuxfs_dev_mount(dind dev_index, int force)
{
	EMUXFS_TRACE("enter");
	return emuxfs_dev_open(dev_index, force, 0);
}

EMUXFS int
emuxfs_dev_unmount(size_t index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	/* force=1 so that a degraded device can still be unmounted. */
	if (emuxfs_dev_get(&dev, index, 1))
		return 1;
	if (!dev->mounted_now)
		return 1;

	if (dev->state.working)
		return 1; /* Device busy. */
	if (dev->state.restoring)
		return 1; /* Device busy. */

	if (!dev->readonly_now) {
		if (emuxfs_dev_state_unmount(&dev->state, dev->state_fd))
			exit(-1);
	}

	if (dev->lfile_fd != -1) {
		if (close(dev->lfile_fd))
			exit(-1);
		dev->lfile_fd = -1;
	}
	if (dev->assign_fd != -1) {
		if (close(dev->assign_fd))
			exit(-1);
		dev->assign_fd = -1;
	}
	if (dev->meta_fd != -1) {
		if (close(dev->meta_fd))
			exit(-1);
		dev->meta_fd = -1;
	}
	if (dev->state_fd != -1) {
		if (close(dev->state_fd))
			exit(-1);
		dev->state_fd = -1;
	}
	if (dev->root_fd != -1) {
		if (close(dev->root_fd))
			exit(-1);
		dev->root_fd = -1;
	}

	dev->readonly_now = 0;
	dev->mounted_now = 0;
	if (emuxfs_dev_array_mounted_count > 0)
		--emuxfs_dev_array_mounted_count;
	return 0;
}

EMUXFS int
emuxfs_working_push(size_t index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (emuxfs_dev_get(&dev, index, 0))
		return 1;
	if (dev->state.working == UINT64_MAX)
		return 1;

	dev->state.working++;
	if (emuxfs_dev_state_write_fd(dev->state_fd, &dev->state)) {
		/*
		 * 'working = 1' could not be made durable before the
		 * mutation that follows.  Continuing would break the
		 * recovery contract (an interrupted operation must be
		 * detectable), so fail closed.
		 */
		exit(-1);
	}

	return 0;
}

EMUXFS int
emuxfs_working_pop(size_t index, [[maybe_unused]] time_t now)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (emuxfs_dev_get(&dev, index, 1))
		return 1;

	if (dev->state.working == 0)
		exit(-1);

	/*
	 * Seq exhaustion fails closed: the device is marked degraded rather
	 * than wrapping.  Wrapping would need a persistent epoch and a
	 * non-atomic config rewrite; saturating and refusing is simpler and
	 * auditable.  2^64 successful operations per device is far beyond any
	 * realistic lifetime.
	 */
	if (dev->state.seq == UINT64_MAX) {
		dev->state.degraded = 1;
		emuxfs_alert("Sequence number exhausted: %lu degraded",
		    (unsigned long)index);
	} else
		dev->state.seq++;

	dev->state.working--;
	if (emuxfs_dev_state_write_fd(dev->state_fd, &dev->state)) {
		/*
		 * The commit (or the degraded flag set above) could not be
		 * made durable.  Abort rather than leave in-memory and
		 * on-disk state divergent; recovery then sees an interrupted
		 * device and requires 'sync'.
		 */
		exit(-1);
	}

	return 0;
}

EMUXFS int
emuxfs_restoring_push(size_t index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (emuxfs_dev_get(&dev, index, 0))
		return 1;
	if (dev->state.restoring == UINT64_MAX)
		return 1;

	dev->state.restoring++;
	if (emuxfs_dev_state_write_fd(dev->state_fd, &dev->state))
		exit(-1);

	return 0;
}

EMUXFS int
emuxfs_restoring_pop(size_t index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (emuxfs_dev_get(&dev, index, 1))
		return 1;

	if (dev->state.restoring == 0)
		exit(-1);

	dev->state.restoring--;
	if (emuxfs_dev_state_write_fd(dev->state_fd, &dev->state))
		exit(-1);

	return 0;
}

static int
emuxfs_degraded_set_val(size_t dev_index, uint64_t val)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	uint64_t *deg;

	if (emuxfs_dev_get(&dev, dev_index, 1))
		return 1;

	deg = &dev->state.degraded;
	if (*deg != val) {
		*deg = val;
		val ? ++emuxfs_dev_array_degraded_count :
		    --emuxfs_dev_array_degraded_count;
		if (emuxfs_dev_state_write_fd(dev->state_fd, &dev->state))
			exit(-1);
	}

	return 0;
}

EMUXFS int
emuxfs_degraded_set(size_t dev_index)
{
	EMUXFS_TRACE("enter");
	emuxfs_alert("Degraded: %lu", (unsigned long)dev_index);
	return emuxfs_degraded_set_val(dev_index, 1);
}

EMUXFS int
emuxfs_degraded_clear(size_t dev_index)
{
	EMUXFS_TRACE("enter");
	return emuxfs_degraded_set_val(dev_index, 0);
}

EMUXFS int
emuxfs_meta_size_raw(size_t *size_out, enum emuxfs_chk_alg_type alg)
{
	EMUXFS_TRACE("enter");
	size_t chk_size, base_size;

	const size_t a = EMUXFS_MEM_ALIGN;

	chk_size = emuxfs_chk_size(alg);
	base_size = sizeof(struct emuxfs_meta_header) + (2 * chk_size);

	*size_out = a * ((base_size / a) + ((base_size % a) ? 1 : 0));
	return 0;
}

EMUXFS int
emuxfs_meta_size(size_t *size_out, dind dev_index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	return emuxfs_meta_size_raw(size_out, dev->conf.chk_alg_type);
}

EMUXFS int
emuxfs_meta_read(struct emuxfs_meta *meta, dind dev_index, uint64_t ino)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	size_t msz;
	struct emuxfs_meta disk_meta;

	if (emuxfs_dev_get(&dev, dev_index, 0)) {
		EMUXFS_TRACE("meta_read: dev_get dev=%lu ino=%llu\n",
		    (unsigned long)dev_index, (unsigned long long)ino);
		return 1;
	}

	if (emuxfs_meta_size(&msz, dev_index)) {
		EMUXFS_TRACE("meta_read: meta_size dev=%lu ino=%llu\n",
		    (unsigned long)dev_index, (unsigned long long)ino);
		return 1;
	}
	if (msz == 0) {
		EMUXFS_TRACE("meta_read: msz=0 dev=%lu ino=%llu\n",
		    (unsigned long)dev_index, (unsigned long long)ino);
		return 1;
	}
	if (ino > ((uint64_t)INT64_MAX / msz)) {
		EMUXFS_TRACE("meta_read: ino too large dev=%lu ino=%llu "
		    "msz=%zu\n", (unsigned long)dev_index,
		    (unsigned long long)ino, msz);
		return 1;
	}

	memset(&disk_meta, 0, sizeof(disk_meta));
	if (emuxfs_pread_exact(dev->meta_fd, &disk_meta, msz,
	    (off_t)(ino * msz))) {
		EMUXFS_TRACE("meta_read: pread dev=%lu ino=%llu off=%lld "
		    "msz=%zu\n", (unsigned long)dev_index,
		    (unsigned long long)ino, (long long)(ino * msz), msz);
		return 1;
	}

	memcpy(meta, &disk_meta, msz);
	meta->header.flags = letoh64(disk_meta.header.flags);
	meta->header.eno = letoh64(disk_meta.header.eno);

	return 0;
}

EMUXFS int
emuxfs_meta_write_fd(int fd, const struct emuxfs_meta *meta, uint64_t ino,
    size_t msz)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_meta disk_meta;

	if (msz == 0)
		return 1;
	if (msz > sizeof(disk_meta))
		return 1;
	if (ino > ((uint64_t)INT64_MAX / msz))
		return 1;

	memset(&disk_meta, 0, sizeof(disk_meta));
	memcpy(&disk_meta, meta, msz);
	disk_meta.header.flags = htole64(meta->header.flags);
	disk_meta.header.eno = htole64(meta->header.eno);

	return emuxfs_pwrite_exact(fd, &disk_meta, msz, (off_t)(ino * msz));
}

EMUXFS int
emuxfs_meta_write(const struct emuxfs_meta *meta, dind dev_index, uint64_t ino)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	size_t msz;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	if (emuxfs_meta_size(&msz, dev_index))
		return 1;

	return emuxfs_meta_write_fd(dev->meta_fd, meta, ino, msz);
}

EMUXFS int
emuxfs_assign_peek_next_eno(uint64_t *eno_out, dind dev_index)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	struct stat st;

	const size_t asz = sizeof(struct emuxfs_assign);

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	if (fstat(dev->assign_fd, &st))
		return 1;
	if (st.st_size < 0 || (st.st_size % (off_t)asz) != 0)
		return 1;

	*eno_out = (uint64_t)(st.st_size / (off_t)asz);
	return 0;
}

EMUXFS int
emuxfs_assign_read(struct emuxfs_assign *assign, dind dev_index, uint64_t eno)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	struct emuxfs_assign disk_assign;

	const size_t asz = sizeof(struct emuxfs_assign);

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	if (eno > ((uint64_t)INT64_MAX / asz))
		return 1;

	memset(&disk_assign, 0, sizeof(disk_assign));
	if (emuxfs_pread_exact(dev->assign_fd, &disk_assign, asz,
	    (off_t)(eno * asz)))
		return 1;

	assign->flags = letoh64(disk_assign.flags);
	assign->ino = letoh64(disk_assign.ino);

	return 0;
}

EMUXFS int
emuxfs_assign_write_fd(int fd, const struct emuxfs_assign *assign, uint64_t eno)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_assign disk_assign;

	const size_t asz = sizeof(struct emuxfs_assign);

	if (eno > ((uint64_t)INT64_MAX / asz))
		return 1;

	disk_assign.flags = htole64(assign->flags);
	disk_assign.ino = htole64(assign->ino);

	return emuxfs_pwrite_exact(fd, &disk_assign, asz, (off_t)(eno * asz));
}

EMUXFS int
emuxfs_assign_write(const struct emuxfs_assign *assign, dind dev_index,
    uint64_t eno)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;

	return emuxfs_assign_write_fd(dev->assign_fd, assign, eno);
}

/*
 * Verify the invariant meta.db[ino].eno == eno  <=>  assign.db[eno].ino == ino
 * for one node.  Returns 0 when the mapping is consistent, 1 otherwise.
 */
EMUXFS int
emuxfs_assign_validate(dind dev_index, uint64_t ino, uint64_t eno)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_assign assign;

	if (emuxfs_assign_read(&assign, dev_index, eno))
		return 1;
	if (assign.flags != AF_ASSIGNED)
		return 1;
	if (assign.ino != ino)
		return 1;

	return 0;
}

/*
 * Scan assign.db and verify that every assigned eno points at an inode whose
 * metadata records the same eno.  This finds orphaned assignments and mappings
 * left behind by a crash between the metadata and assign writes, in O(n) where
 * n is the number of assign records.  It is intended for audit/heal, not for
 * every read.  Returns 0 on success (with *bad_out set), 1 on a hard error.
 */
EMUXFS int
emuxfs_meta_assign_check(dind dev_index, size_t *bad_out)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_dev *dev;
	struct emuxfs_assign assign;
	struct emuxfs_meta meta;
	struct stat st;
	uint64_t eno, count;
	size_t bad;

	if (emuxfs_dev_get(&dev, dev_index, 0))
		return 1;
	if (fstat(dev->assign_fd, &st))
		return 1;
	if (st.st_size < 0 || (st.st_size % (off_t)sizeof(assign)) != 0)
		return 1;
	count = (uint64_t)(st.st_size / (off_t)sizeof(assign));

	bad = 0;
	for (eno = 0; eno < count; ++eno) {
		if (emuxfs_assign_read(&assign, dev_index, eno)) {
			++bad;
			continue;
		}
		if (assign.flags != AF_ASSIGNED)
			continue;
		if (emuxfs_meta_read(&meta, dev_index, assign.ino)) {
			++bad;
			continue;
		}
		if ((meta.header.flags != MF_ASSIGNED) ||
		    (meta.header.eno != eno))
			++bad;
	}

	*bad_out = bad;
	return 0;
}

/*
 * Returns 2 if there is any sequence number mismatch, 1 on error, 0 otherwise.
 * Prints a message to stderr on mismatch.
 */
EMUXFS int
emuxfs_dev_seq_check(void)
{
	EMUXFS_TRACE("enter");
	dind i, dev_count;
	uint64_t seq;
	time_t seq_zero_time;
	struct emuxfs_dev *dev;
	char tbuf1[26], tbuf2[26];

	if ((dev_count = emuxfs_dev_count()) == 0)
		return 0;
	if (emuxfs_dev_get(&dev, 0, 0))
		return 1;
	seq_zero_time = dev->conf.seq_zero_time;
	seq = dev->state.seq;
	for (i = 1; i < dev_count; ++i) {
		if (emuxfs_dev_get(&dev, i, 0))
			return 1;
		if ((dev->conf.seq_zero_time != seq_zero_time) ||
		    (dev->state.seq != seq)) {
			memset(tbuf1, 0, 26);
			memset(tbuf2, 0, 26);
			if (ctime_r(&seq_zero_time, tbuf1) == nullptr)
				return 1;
			if (ctime_r(&dev->conf.seq_zero_time, tbuf2) == nullptr)
				return 1;
			dprintf(2, "Error: Sequence number mismatch.\n");
			dprintf(2, "Index 0:\n");
			dprintf(2, "\tWas zero at: %s\n", tbuf1);
			dprintf(2, "\tCurrent value: %llu\n", seq);
			dprintf(2, "Index %lu:\n", i);
			dprintf(2, "\tWas zero at: %s\n", tbuf2);
			dprintf(2, "\tCurrent value: %llu\n", dev->state.seq);
			return 2;
		}
	}

	return 0;
}
