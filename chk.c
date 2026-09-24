/* chk.c */
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
 * This file belongs to emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 *
 * The algorithm table is a stable, on-disk ABI: the 'name' strings are
 * persisted in muxfs.conf and the order fixes the values of
 * enum emuxfs_chk_alg_type.  Do not reorder or rename entries; add new
 * algorithms only at the end, before CAT_NONE.
 */

#include <endian.h>
#include <stdlib.h>
#include <string.h>

#include "chk.h"

static void
emuxfs_crc32_init(struct emuxfs_chk *chk, struct emuxfs_chk_alg *alg)
{
	chk->alg = alg;
	chk->impl.ulong = crc32_z(0L, nullptr, 0);
}

static void
emuxfs_crc32_update(struct emuxfs_chk *chk, const uint8_t *data, size_t size)
{
	chk->impl.ulong = crc32_z(chk->impl.ulong, data, size);
}

static void
emuxfs_crc32_final(uint8_t *buf_out, struct emuxfs_chk *chk)
{
	uint32_t u32;

	u32 = htole32((uint32_t)chk->impl.ulong);
	memcpy(buf_out, &u32, chk->alg->chk_size);
}

static void
emuxfs_md5_init(struct emuxfs_chk *chk, struct emuxfs_chk_alg *alg)
{
	chk->alg = alg;
	MD5Init(&chk->impl.md5_ctx);
}

static void
emuxfs_md5_update(struct emuxfs_chk *chk, const uint8_t *data, size_t size)
{
	MD5Update(&chk->impl.md5_ctx, data, size);
}

static void
emuxfs_md5_final(uint8_t *buf_out, struct emuxfs_chk *chk)
{
	MD5Final(buf_out, &chk->impl.md5_ctx);
}

static void
emuxfs_sha1_init(struct emuxfs_chk *chk, struct emuxfs_chk_alg *alg)
{
	chk->alg = alg;
	SHA1Init(&chk->impl.sha1_ctx);
}

static void
emuxfs_sha1_update(struct emuxfs_chk *chk, const uint8_t *data, size_t size)
{
	SHA1Update(&chk->impl.sha1_ctx, data, size);
}

static void
emuxfs_sha1_final(uint8_t *buf_out, struct emuxfs_chk *chk)
{
	SHA1Final(buf_out, &chk->impl.sha1_ctx);
}

static struct emuxfs_chk_alg emuxfs_chk_alg_tab[] = {
	{ CAT_CRC32,  4, emuxfs_crc32_init, emuxfs_crc32_update,
	  emuxfs_crc32_final, "crc32" },
	{ CAT_MD5, 16, emuxfs_md5_init, emuxfs_md5_update,
	  emuxfs_md5_final, "md5"   },
	{ CAT_SHA1, 20, emuxfs_sha1_init, emuxfs_sha1_update,
	  emuxfs_sha1_final, "sha1"  },
	{ CAT_NONE,  0, nullptr, nullptr,
	  nullptr, "none"  }
};

EMUXFS int
emuxfs_chk_str_to_type(enum emuxfs_chk_alg_type *type, const char *name,
    size_t name_len)
{
	size_t i;

	for (i = 0; emuxfs_chk_alg_tab[i].type != CAT_NONE; ++i) {
		if ((strlen(emuxfs_chk_alg_tab[i].name) == name_len) &&
		    (strncmp(emuxfs_chk_alg_tab[i].name, name, name_len) ==
		     0)) {
			*type = emuxfs_chk_alg_tab[i].type;
			return 0;
		}
	}

	return 1;
}

EMUXFS const char *
emuxfs_chk_type_to_str(enum emuxfs_chk_alg_type type)
{
	if (type >= CAT_NONE)
		exit(-1); /* Programming error. */
	return emuxfs_chk_alg_tab[type].name;
}

EMUXFS size_t
emuxfs_chk_size(enum emuxfs_chk_alg_type type)
{
	if (type >= CAT_NONE)
		exit(-1); /* Programming error. */
	return emuxfs_chk_alg_tab[type].chk_size;
}

EMUXFS void
emuxfs_chk_init(struct emuxfs_chk *chk, enum emuxfs_chk_alg_type type)
{
	struct emuxfs_chk_alg *alg;

	if (type >= CAT_NONE)
		exit(-1); /* Programming error. */
	alg = &emuxfs_chk_alg_tab[type];
	alg->chk_init(chk, alg);
}

EMUXFS void
emuxfs_chk_update(struct emuxfs_chk *chk, const uint8_t *data, size_t size)
{
	chk->alg->chk_update(chk, data, size);
}

EMUXFS void
emuxfs_chk_final(uint8_t *buf_out, struct emuxfs_chk *chk)
{
	chk->alg->chk_final(buf_out, chk);
}
