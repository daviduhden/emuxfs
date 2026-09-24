/* ops.h */
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
 * emuxfs uses the FUSE implementation shipped with OpenBSD (libfuse in the
 * base system), which provides the FUSE 2.6 high-level API.  This header is
 * the single point at which emuxfs depends on FUSE; see COMPATIBILITY.md for
 * why the native implementation is used instead of libfuse3.  OpenBSD
 * installs the headers under /usr/include/fuse, so the Makefile adds
 * -I/usr/include/fuse and this file includes <fuse.h> unchanged.
 */

#ifndef _OPS_H_
#define _OPS_H_

#include <fuse.h>

extern const struct fuse_operations emuxfs_fuse_ops;

#endif /* _OPS_H_ */
