/* conf.c */
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

/*
 * This file belongs to emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 *
 * muxfs.conf is a line-oriented, newline-terminated key=value text file.
 * Two versions are recognised and written:
 *
 *   version=MAJOR.MINOR-flavor   human-readable program version (provenance)
 *   format_version=N             enforced on-disk format version
 *
 * The original Multiplexed File System wrote version=MAJOR.MINORflavor (no separator) and, due
 * to a parsing defect, never recovered the revision component, which made an
 * array formatted with any revision other than zero unmountable by the same
 * program.  The parser below accepts both the legacy and the new spelling.
 * format_version was not written by the original; its absence is treated as
 * EMUXFS_FORMAT_VERSION_LEGACY, which describes the same layout as version 1.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid.h>
#include <errno.h>

#include "emuxfs.h"

/*
 * The configuration stores UUIDs as 16 raw bytes and both uuid_enc_le() and
 * uuid_dec_le() move sizeof(uuid_t) bytes, so the sizes must agree.
 */
static_assert(sizeof(uuid_t) == EMUXFS_UUID_SIZE,
    "uuid_t must be EMUXFS_UUID_SIZE bytes for uuid_enc_le/uuid_dec_le");

/*
 * 2 decimals at most 10 digits long (the string length of the decimal
 * representation of UINT32_MAX), plus 2 dots, plus the length of "release".
 */
#define EMUXFS_VERSION_STRING_LENGTH_MAX ((2*10)+2+7)

/* 2^64 is about 1.8e19 */
#define EMUXFS_DECIMAL_UINT64_LENGTH_MAX 20

struct emuxfs_dev_conf_checklist {
	int has_version, has_format_version, has_alg, has_array_uuid,
	    has_dev_uuid, has_seq_zero_time;
};

static int
emuxfs_conf_key_is(const char *key, size_t key_len, const char *literal)
{
	EMUXFS_TRACE("enter");
	size_t n;

	n = strlen(literal);
	return (key_len == n) && (memcmp(key, literal, n) == 0);
}

static int
emuxfs_conf_version_parse(struct emuxfs_dev_conf *conf, const char *version,
    size_t version_len)
{
	EMUXFS_TRACE("enter");
	char		 version_string_buf[EMUXFS_VERSION_STRING_LENGTH_MAX +
			 1];
	char		*b, *e;
	char		 saved;
	const char	*errstr;
	long long	 num;

	if ((version_len == 0) ||
	    (version_len > EMUXFS_VERSION_STRING_LENGTH_MAX))
		return 1;

	memset(version_string_buf, 0, sizeof(version_string_buf));
	memcpy(version_string_buf, version, version_len);

	b = version_string_buf;

	/* Major number: everything up to the first dot. */
	if ((e = memchr(b, '.', version_len)) == nullptr)
		return 1;
	*e = '\0';
	num = strtonum(b, 0, INT32_MAX, &errstr);
	if (errstr != nullptr)
		return 1;
	conf->version.number = (uint32_t)num;
	b = e + 1;

	/*
	 * Revision: the leading decimal integer of what remains.  It is
	 * terminated by the flavor, which either directly follows (legacy
	 * "0.5current") or is separated by '-' or '.' ("1.0-current").
	 */
	for (e = b; (*e >= '0') && (*e <= '9'); ++e)
		continue;
	if (e == b)
		return 1;
	saved = *e;
	*e = '\0';
	num = strtonum(b, 0, INT32_MAX, &errstr);
	*e = saved;
	if (errstr != nullptr)
		return 1;
	conf->version.revision = (uint32_t)num;
	b = e;

	while ((*b == '-') || (*b == '.'))
		++b;

	if (strcmp(b, "current") == 0)
		conf->version.flavor = VF_CURRENT;
	else if (strcmp(b, "release") == 0)
		conf->version.flavor = VF_RELEASE;
	else if (strcmp(b, "stable") == 0)
		conf->version.flavor = VF_STABLE;
	else
		return 1;

	return 0;
}

static int
emuxfs_uuid_read(uint8_t *dest, const char *src, size_t len)
{
	EMUXFS_TRACE("enter");
	char *str;
	uint32_t status;
	uuid_t uuid;

	if (len > EMUXFS_UUID_SIZE * 5)
		return 1;
	str = calloc(len + 1, sizeof(char));
	if (str == nullptr)
		return 1;

	memcpy(str, src, len);
	uuid_from_string(str, &uuid, &status);
	free(str);
	if (status != uuid_s_ok)
		return 1;

	uuid_enc_le(dest, &uuid);

	return 0;
}

static int
emuxfs_conf_line_parse(struct emuxfs_dev_conf *conf, const char *line,
    size_t len, struct emuxfs_dev_conf_checklist *cl)
{
	EMUXFS_TRACE("enter");
	const char *eq, *key, *val;
	size_t key_len, val_len;
	char num_buf[EMUXFS_DECIMAL_UINT64_LENGTH_MAX + 1];
	const char *errstr;
	long long num;

	if ((eq = memchr(line, '=', len)) == nullptr)
		return 1;

	key = line;
	key_len = (size_t)(eq - line);
	val = eq + 1;
	val_len = len - (key_len + 1);

	if (emuxfs_conf_key_is(key, key_len, "version")) {
		if (cl->has_version)
			return 1; /* Duplicate key. */
		if (emuxfs_conf_version_parse(conf, val, val_len))
			return 1;
		cl->has_version = 1;
	} else if (emuxfs_conf_key_is(key, key_len, "format_version")) {
		if (cl->has_format_version)
			return 1; /* Duplicate key. */
		if (val_len > EMUXFS_DECIMAL_UINT64_LENGTH_MAX)
			return 1;
		memset(num_buf, 0, sizeof(num_buf));
		memcpy(num_buf, val, val_len);
		num = strtonum(num_buf, 0, UINT32_MAX, &errstr);
		if (errstr != nullptr)
			return 1;
		conf->format_version = (uint32_t)num;
		cl->has_format_version = 1;
	} else if (emuxfs_conf_key_is(key, key_len, "chk_alg")) {
		if (cl->has_alg)
			return 1; /* Duplicate key. */
		if (emuxfs_chk_str_to_type(&conf->chk_alg_type, val, val_len))
			return 1;
		cl->has_alg = 1;
	} else if (emuxfs_conf_key_is(key, key_len, "array_uuid")) {
		if (cl->has_array_uuid)
			return 1; /* Duplicate key. */
		if (emuxfs_uuid_read(conf->array_uuid, val, val_len))
			return 1;
		cl->has_array_uuid = 1;
	} else if (emuxfs_conf_key_is(key, key_len, "dev_uuid")) {
		if (cl->has_dev_uuid)
			return 1; /* Duplicate key. */
		if (emuxfs_uuid_read(conf->dev_uuid, val, val_len))
			return 1;
		cl->has_dev_uuid = 1;
	} else if (emuxfs_conf_key_is(key, key_len, "seq_zero_time")) {
		if (cl->has_seq_zero_time)
			return 1; /* Duplicate key. */
		if (val_len > EMUXFS_DECIMAL_UINT64_LENGTH_MAX)
			return 1;
		memset(num_buf, 0, sizeof(num_buf));
		memcpy(num_buf, val, val_len);
		conf->seq_zero_time = strtonum(num_buf, 0, INT64_MAX, &errstr);
		if (errstr != nullptr)
			return 1;
		cl->has_seq_zero_time = 1;
	} else
		return 1;

	return 0;
}

static int
emuxfs_conf_check(struct emuxfs_dev_conf *conf,
    struct emuxfs_dev_conf_checklist cl)
{
	EMUXFS_TRACE("enter");
	if (!(cl.has_alg && cl.has_array_uuid && cl.has_dev_uuid &&
	    cl.has_version && cl.has_seq_zero_time))
		return EMUXFS_CONF_EPARSE;

	if (conf->format_version != EMUXFS_FORMAT_VERSION)
		return EMUXFS_CONF_EBADVERSION;

	return EMUXFS_CONF_OK;
}

EMUXFS int
emuxfs_conf_parse(struct emuxfs_dev_conf *conf, int fd)
{
	EMUXFS_TRACE("enter");
	char buf[EMUXFS_BLOCK_SIZE];
	ssize_t readsz;
	size_t bufsz, rawlen, linesz;
	char *eol;
	struct emuxfs_dev_conf_checklist cl;
	int rc;

	memset(conf, 0, sizeof(*conf));
	conf->format_version = EMUXFS_FORMAT_VERSION_LEGACY;
	memset(&cl, 0, sizeof(cl));

	bufsz = 0;
	for (;;) {
		readsz = read(fd, buf + bufsz, sizeof(buf) - bufsz);
		if (readsz == -1) {
			if (errno == EINTR)
				continue;
			return EMUXFS_CONF_EPARSE;
		}
		if (readsz == 0)
			break;

		bufsz += (size_t)readsz;
		while ((eol = memchr(buf, '\n', bufsz)) != nullptr) {
			/* Bytes before the newline, CR included if present. */
			rawlen = (size_t)(eol - buf);
			linesz = rawlen;
			/* Tolerate CRLF line endings by stripping one CR. */
			if ((rawlen > 0) && (buf[rawlen - 1] == '\r'))
				--linesz;
			if (emuxfs_conf_line_parse(conf, buf, linesz, &cl))
				return EMUXFS_CONF_EPARSE;
			memmove(buf, eol + 1, bufsz - (rawlen + 1));
			bufsz -= (rawlen + 1);
		}
		if (bufsz == sizeof(buf)) {
			/* The line is longer than the buffer. */
			return EMUXFS_CONF_EPARSE;
		}
	}
	if (bufsz > 0) {
		/* The last line doesn't end with a newline character. */
		return EMUXFS_CONF_EPARSE;
	}

	rc = emuxfs_conf_check(conf, cl);
	if (rc != EMUXFS_CONF_OK)
		return rc;

	if (!cl.has_format_version)
		emuxfs_info("Legacy array (no format_version); assuming %u.",
		    (unsigned)EMUXFS_FORMAT_VERSION_LEGACY);

	return EMUXFS_CONF_OK;
}

static const char *
emuxfs_version_flavor_str(enum emuxfs_version_flavor flavor)
{
	EMUXFS_TRACE("enter");
	switch (flavor) {
	case VF_CURRENT:
		return "current";
	case VF_RELEASE:
		return "release";
	case VF_STABLE:
		return "stable";
	}
	exit(-1); /* Programming error. */
}

EMUXFS int
emuxfs_conf_write(struct emuxfs_dev_conf *conf, int fd)
{
	EMUXFS_TRACE("enter");
	uuid_t uuid;
	char *uuid_str;
	uint32_t uuid_status;

	if (ftruncate(fd, 0))
		return 1;

	if (dprintf(fd, "version=%u.%u-%s\n", conf->version.number,
	    conf->version.revision,
	    emuxfs_version_flavor_str(conf->version.flavor)) < 0)
		return 1;
	if (dprintf(fd, "format_version=%u\n",
	    (unsigned)conf->format_version) < 0)
		return 1;
	if (dprintf(fd, "chk_alg=%s\n",
	    emuxfs_chk_type_to_str(conf->chk_alg_type)) < 0)
		return 1;

	uuid_dec_le(conf->array_uuid, &uuid);
	uuid_to_string(&uuid, &uuid_str, &uuid_status);
	if (uuid_status != uuid_s_ok)
		return 1;
	if (dprintf(fd, "array_uuid=%s\n", uuid_str) < 0) {
		free(uuid_str);
		return 1;
	}
	free(uuid_str);

	uuid_dec_le(conf->dev_uuid, &uuid);
	uuid_to_string(&uuid, &uuid_str, &uuid_status);
	if (uuid_status != uuid_s_ok)
		return 1;
	if (dprintf(fd, "dev_uuid=%s\n", uuid_str) < 0) {
		free(uuid_str);
		return 1;
	}
	free(uuid_str);

	if (dprintf(fd, "seq_zero_time=%lld\n",
	    (long long)conf->seq_zero_time) < 0)
		return 1;

	if (fsync(fd))
		return 1;

	return 0;
}
