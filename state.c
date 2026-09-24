/* state.c */
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

#include <sys/types.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "ds.h"
#include "emuxfs.h"

struct emuxfs_restore_item {
	size_t dev_index;
	size_t path_len;
	char path[];
};

/*
 * Queue items are stored in a byte buffer returned by reallocarray(3) and
 * their offsets are rounded up to sizeof(size_t); that rounding only keeps
 * each item aligned if the struct's alignment is no larger than that.
 */
static_assert(alignof(struct emuxfs_restore_item) <= sizeof(size_t),
    "restore queue item alignment must not exceed the offset rounding");

struct emuxfs_state {
	/*
	 * The restore queue is a byte buffer holding a sequence of
	 * struct emuxfs_restore_item records, each of which is followed by a
	 * NUL-terminated path.  It is addressed by byte offsets rather than by
	 * pointers so that reallocarray(3) cannot invalidate anything.
	 */
	uint8_t			*restore_queue;
	size_t			 restore_queue_size;
	size_t			 restore_front;
	size_t			 restore_back;
	uint64_t		 next_eno;
	size_t			 ambiguities;
	struct syslog_data	 log;
	int			 is_restore_only;
	dind			 restore_only_dind;
	struct emuxfs_wrbuf	 wr;
};

static struct emuxfs_state emuxfs_global_state;

/* Debug tracing, enabled by EMUXFS_TRACE / EMUXFS_TRACE_FILE. */
static int emuxfs_trace_fd = -1;
static int emuxfs_trace_stderr;

static size_t
emuxfs_restore_item_next_offset(const char *path)
{
	EMUXFS_TRACE("enter");
	size_t risz, path_len;

	const size_t szsz = sizeof(size_t);

	path_len = strlen(path);
	risz = sizeof(struct emuxfs_restore_item) + path_len + 1;
	return szsz * ((risz / szsz) + ((risz % szsz) ? 1 : 0));
}

/*
 * Ensure that 'extra' bytes can be appended at restore_back, compacting or
 * growing the buffer as needed.  Returns 0 on success, 1 on size overflow.
 * Exits on allocation failure, matching the rest of the program.
 */
static int
emuxfs_restore_queue_reserve(size_t extra)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_state *st;
	size_t used, newsz;
	uint8_t *q;

	st = &emuxfs_global_state;
	used = st->restore_back - st->restore_front;

	/*
	 * If the live region has drifted away from the start of the buffer,
	 * slide it back to make room before considering a reallocation.
	 */
	if ((st->restore_front > 0) &&
	    (used + extra <= st->restore_queue_size)) {
		memmove(st->restore_queue,
		    st->restore_queue + st->restore_front, used);
		st->restore_front = 0;
		st->restore_back = used;
	}

	if (st->restore_back + extra <= st->restore_queue_size)
		return 0;
	if (st->restore_back > SIZE_MAX - extra)
		return 1;

	newsz = st->restore_queue_size;
	if (newsz == 0)
		newsz = (size_t)sysconf(_SC_PAGESIZE);
	if (newsz == 0)
		newsz = 4096;
	while (newsz < st->restore_back + extra) {
		if (newsz > (SIZE_MAX / 2))
			return 1;
		newsz *= 2;
	}

	q = reallocarray(st->restore_queue, newsz, 1);
	if (q == nullptr)
		exit(-1);
	st->restore_queue = q;
	st->restore_queue_size = newsz;

	return 0;
}

EMUXFS int
emuxfs_state_restore_push_back(dind dev_index, const char *path)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_state *st;
	struct emuxfs_restore_item *curr;
	size_t item_offset, path_len;

	emuxfs_warn("Corrupted: %lu:/%s\n", dev_index, path);

	if (emuxfs_global_state.is_restore_only &&
	    (dev_index != emuxfs_global_state.restore_only_dind))
		return 0;

	st = &emuxfs_global_state;
	path_len = strlen(path);
	/*
	 * A queued path is later copied into a PATH_MAX buffer by
	 * emuxfs_state_restore_pop_front(), so reject anything that would not
	 * fit instead of letting it overflow the caller's buffer.
	 */
	if (path_len >= PATH_MAX)
		return 1;
	item_offset = emuxfs_restore_item_next_offset(path);

	if (emuxfs_restore_queue_reserve(item_offset))
		return 1;

	curr = (struct emuxfs_restore_item *)
	    (st->restore_queue + st->restore_back);
	curr->dev_index = dev_index;
	curr->path_len = path_len;
	memcpy(curr->path, path, path_len + 1);

	st->restore_back += item_offset;

	return 0;
}

EMUXFS int
emuxfs_state_restore_next_path_len(size_t *path_len_out)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_state *st;
	struct emuxfs_restore_item *curr;

	st = &emuxfs_global_state;
	if (st->restore_front == st->restore_back)
		return 1;

	curr = (struct emuxfs_restore_item *)
	    (st->restore_queue + st->restore_front);
	*path_len_out = curr->path_len;
	return 0;
}

EMUXFS int
emuxfs_state_restore_pop_front(size_t *dev_index_out, char *path_out)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_state *st;
	struct emuxfs_restore_item *curr;
	size_t item_offset;

	st = &emuxfs_global_state;
	if (st->restore_front == st->restore_back)
		return 1;

	curr = (struct emuxfs_restore_item *)
	    (st->restore_queue + st->restore_front);

	*dev_index_out = curr->dev_index;
	memcpy(path_out, curr->path, curr->path_len);
	path_out[curr->path_len] = '\0';

	item_offset = emuxfs_restore_item_next_offset(curr->path);
	st->restore_front += item_offset;

	if (st->restore_front == st->restore_back)
		st->restore_front = st->restore_back = 0;

	return 0;
}

EMUXFS int
emuxfs_state_restore_queue_init(void)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_state *state;

	state = &emuxfs_global_state;

	state->restore_queue = nullptr;
	state->restore_queue_size = 0;
	state->restore_front = 0;
	state->restore_back = 0;

	if (emuxfs_restore_queue_reserve(1))
		return 1;

	state->is_restore_only = 0;
	state->restore_only_dind = 0;

	return 0;
}

EMUXFS void
emuxfs_state_restore_queue_final(void)
{
	EMUXFS_TRACE("enter");
	free(emuxfs_global_state.restore_queue);
	emuxfs_global_state.restore_queue = nullptr;
	emuxfs_global_state.restore_queue_size = 0;
	emuxfs_global_state.restore_front = 0;
	emuxfs_global_state.restore_back = 0;
}

EMUXFS void
emuxfs_state_ambiguity_note(void)
{
	EMUXFS_TRACE("enter");
	if (emuxfs_global_state.ambiguities != SIZE_MAX)
		++emuxfs_global_state.ambiguities;
}

EMUXFS size_t
emuxfs_state_ambiguity_count(void)
{
	EMUXFS_TRACE("enter");
	return emuxfs_global_state.ambiguities;
}

EMUXFS int
emuxfs_state_ambiguity_clear(void)
{
	EMUXFS_TRACE("enter");
	emuxfs_global_state.ambiguities = 0;
	return 0;
}

EMUXFS int
emuxfs_state_eno_next_init(uint64_t eno)
{
	EMUXFS_TRACE("enter");
	emuxfs_global_state.next_eno = eno;
	return 0;
}

EMUXFS int
emuxfs_state_eno_next_acquire(uint64_t *eno_out)
{
	EMUXFS_TRACE("enter");
	uint64_t *ne;

	ne = &emuxfs_global_state.next_eno;

	/* Using the largest possible value for an eno as the invalid value. */
	if (*ne == UINT64_MAX)
		return 1;

	*eno_out = (*ne)++;
	return 0;
}

EMUXFS int
emuxfs_state_eno_next_return(uint64_t eno)
{
	EMUXFS_TRACE("enter");
	uint64_t *ne;

	/* Using the largest possible value for an eno as the invalid value. */
	if (eno == UINT64_MAX)
		exit(-1);

	ne = &emuxfs_global_state.next_eno;
	if (*ne == 0)
		return 1;
	if (eno == (*ne) - 1)
		--(*ne);
	return 0;
}

EMUXFS int
emuxfs_state_syslog_init(void)
{
	EMUXFS_TRACE("enter");
	const char *trace_file;

	emuxfs_global_state.log = (struct syslog_data)SYSLOG_DATA_INIT;
	openlog_r("emuxfs", LOG_PID|LOG_NDELAY, LOG_USER,
	    &emuxfs_global_state.log);

	emuxfs_trace_fd = -1;
	emuxfs_trace_stderr = (getenv("EMUXFS_TRACE") != nullptr);
	trace_file = getenv("EMUXFS_TRACE_FILE");
	if ((trace_file != nullptr) && (trace_file[0] != '\0'))
		emuxfs_trace_fd = open(trace_file,
		    O_WRONLY|O_CREAT|O_APPEND, 0600);

	return 0;
}

EMUXFS int
emuxfs_state_syslog_final(void)
{
	EMUXFS_TRACE("enter");
	if (emuxfs_trace_fd >= 0) {
		if (close(emuxfs_trace_fd))
			exit(-1);
		emuxfs_trace_fd = -1;
	}
	closelog_r(&emuxfs_global_state.log);
	return 0;
}

EMUXFS void
emuxfs_debug(const char *msg, ...)
{
	va_list va_args;

	va_start(va_args, msg);
	vsyslog_r(LOG_DEBUG, &emuxfs_global_state.log, msg, va_args);
	va_end(va_args);
}

EMUXFS void
emuxfs_info(const char *msg, ...)
{
	va_list va_args;

	va_start(va_args, msg);
	vsyslog_r(LOG_INFO, &emuxfs_global_state.log, msg, va_args);
	va_end(va_args);
}

EMUXFS void
emuxfs_warn(const char *msg, ...)
{
	va_list va_args;

	va_start(va_args, msg);
	vsyslog_r(LOG_WARNING, &emuxfs_global_state.log, msg, va_args);
	va_end(va_args);
}

EMUXFS void
emuxfs_alert(const char *msg, ...)
{
	va_list va_args;

	va_start(va_args, msg);
	vsyslog_r(LOG_ALERT, &emuxfs_global_state.log, msg, va_args);
	va_end(va_args);
}

EMUXFS void
emuxfs_trace(const char *file, int line, const char *fmt, ...)
{
	char buf[1024];
	va_list va_args;
	int n, fd;

	fd = emuxfs_trace_fd;
	if ((fd < 0) && !emuxfs_trace_stderr)
		return;

	va_start(va_args, fmt);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	n = vsnprintf(buf, sizeof(buf), fmt, va_args);
#pragma clang diagnostic pop
	va_end(va_args);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;

	if (fd >= 0) {
		dprintf(fd, "%s:%d: %.*s\n", file, line, n, buf);
	} else {
		dprintf(STDERR_FILENO, "%s:%d: %.*s\n", file, line, n, buf);
	}
}

/* Assumes that emuxfs_dsinit() has already been called. */
EMUXFS int
emuxfs_init(int skip_first_mount, int force)
{
	EMUXFS_TRACE("enter");
	uint64_t next_eno, max_next_eno;
	dind i, j, mnts;
	struct emuxfs_args *args;

	args = &emuxfs_cmdline;

	emuxfs_dev_module_init();
	if (emuxfs_state_restore_queue_init())
		exit(-1);

	mnts = 0;
	max_next_eno = 0;
	for (i = 0; i < args->dev_count; ++i) {
		if (emuxfs_dev_append(&j, args->dev_paths[i]))
			exit(-1);
		if (skip_first_mount && (i == 0))
			continue;
		if (emuxfs_dev_open(j, force, args->readonly)) {
			dprintf(2, "Error: Unable to mount %s.\n",
			    args->dev_paths[i]);
			emuxfs_final();
			exit(1);
		}
		if (emuxfs_assign_peek_next_eno(&next_eno, j)) {
			emuxfs_final();
			exit(-1);
		}
		++mnts;
		if (next_eno > max_next_eno)
			max_next_eno = next_eno;
	}
	if (mnts == 0)
		exit(-1);
	EMUXFS_TRACE("init: opened %lu devices\n", (unsigned long)mnts);

	if (emuxfs_state_eno_next_init(max_next_eno))
		exit(-1);

	if (emuxfs_state_wrbuf_reset())
		exit(-1);

	return 0;
}

EMUXFS int
emuxfs_final(void)
{
	EMUXFS_TRACE("enter");
	static int done;
	dind i, j, dev_count;

	if (done)
		return 0;
	done = 1;

	dev_count = emuxfs_dev_count();
	for (i = dev_count; i > 0; --i) {
		j = i - 1;
		if (emuxfs_dev_is_mounted(j)) {
			if (emuxfs_dev_unmount(j))
				exit(-1);
		}
	}
	emuxfs_state_restore_queue_final();
	if (emuxfs_state_syslog_final())
		exit(-1);
	if (emuxfs_dsfinal())
		exit(-1);

	return 0;
}

EMUXFS int
emuxfs_state_restore_only_set(dind dev_index)
{
	EMUXFS_TRACE("enter");
	emuxfs_global_state.restore_only_dind = dev_index;
	emuxfs_global_state.is_restore_only = 1;
	return 0;
}

EMUXFS int
emuxfs_state_is_restore_only(void)
{
	EMUXFS_TRACE("enter");
	return emuxfs_global_state.is_restore_only;
}

EMUXFS int
emuxfs_state_wrbuf_is_set(void)
{
	EMUXFS_TRACE("enter");
	return emuxfs_global_state.wr.path[0] != '\0';
}

EMUXFS int
emuxfs_state_wrbuf_reset(void)
{
	EMUXFS_TRACE("enter");
	/*
	 * The write buffer holds file content supplied by users; clear it
	 * explicitly so the compiler cannot elide the wipe.
	 */
	explicit_bzero(&emuxfs_global_state.wr, sizeof(emuxfs_global_state.wr));
	return 0;
}

EMUXFS int
emuxfs_state_wrbuf_set(const char *path, uid_t user, gid_t group, size_t sz,
    size_t off, const uint8_t *buf)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_wrbuf *wrbuf;

	wrbuf = &emuxfs_global_state.wr;

	if (emuxfs_state_wrbuf_is_set())
		return 1;
	if (path == nullptr)
		return 1;
	if (path[0] == '\0')
		return 1;
	if (strlen(path) >= PATH_MAX)
		return 1;
	if (sz > EMUXFS_WRBUF_SIZE)
		return 1;
	if (buf == nullptr)
		return 1;

	*wrbuf = (struct emuxfs_wrbuf){
	    .wc = {
		.user = user,
		.group = group,
	    },
	    .sz = sz,
	    .off = off,
	};
	strlcpy(wrbuf->path, path, sizeof(wrbuf->path));
	memcpy(wrbuf->buf, buf, sz);

	return 0;
}

EMUXFS int
emuxfs_state_wrbuf_append(size_t *wrsz_out, const char *path, uid_t user,
    gid_t group, size_t sz, size_t off, const uint8_t *buf)
{
	EMUXFS_TRACE("enter");
	struct emuxfs_wrbuf *wrbuf;

	wrbuf = &emuxfs_global_state.wr;

	if (!emuxfs_state_wrbuf_is_set())
		return 1;
	if (path == nullptr)
		return 1;
	if (path[0] == '\0')
		return 1;
	if (strlen(path) >= PATH_MAX)
		return 1;
	if (strcmp(wrbuf->path, path) != 0)
		return 1;
	if (wrbuf->wc.user != user)
		return 1;
	if (wrbuf->wc.group != group)
		return 1;
	if (off != (wrbuf->off + wrbuf->sz))
		return 1;
	if (buf == nullptr)
		return 1;
	if (wrsz_out == nullptr)
		return 1;

	if ((wrbuf->sz + sz) > EMUXFS_WRBUF_SIZE)
		sz = (EMUXFS_WRBUF_SIZE - wrbuf->sz);
	memcpy(&wrbuf->buf[wrbuf->sz], buf, sz);
	wrbuf->sz += sz;
	*wrsz_out = sz;

	return 0;
}

EMUXFS int
emuxfs_state_wrbuf_get(const struct emuxfs_wrbuf **wrbuf_out)
{
	EMUXFS_TRACE("enter");
	if (!emuxfs_state_wrbuf_is_set())
		return 1;
	*wrbuf_out = &emuxfs_global_state.wr;
	return 0;
}
