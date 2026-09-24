/* sandbox.h */
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
 * OpenBSD pledge(2) and unveil(2) policy.  The promise strings are derived
 * from the system calls actually reachable from each sub-command; see
 * SECURITY.md for the derivation.  In particular:
 *
 *   stdio   memory management, descriptor I/O (read/write/pread/pwrite,
 *           fsync, ftruncate, mmap), gettimeofday, getentropy, getdents,
 *           kqueue/kevent
 *   rpath   openat for read, fstatat, fstatfs, faccessat, readlinkat
 *   wpath   openat for write, truncate
 *   cpath   mkdirat, unlinkat, renameat, symlinkat
 *   fattr   fchmodat, utimensat
 *   chown   fchownat with an arbitrary uid/gid (used only when restoring)
 *   id      seteuid/setegid (the FUSE callbacks impersonate the caller)
 *   unix    syslog(3) AF_UNIX socket operations
 *
 * mount(2)/unmount(2) are performed by libfuse before any pledge is taken.
 */

#ifndef _SANDBOX_H_
#define _SANDBOX_H_

#include "emuxfs.h"

#define EMUXFS_PLEDGE_VERSION	"stdio"
#define EMUXFS_PLEDGE_FORMAT	"stdio rpath wpath cpath fattr chown"
#define EMUXFS_PLEDGE_SCAN	"stdio rpath wpath cpath fattr chown unix"
#define EMUXFS_PLEDGE_SYNC	"stdio rpath wpath cpath fattr chown unix"
#define EMUXFS_PLEDGE_MOUNT	"stdio rpath wpath cpath fattr chown id unix"

/*
 * unveil(2) the given directory with read/write/create access.  The path is
 * normally absolute and must already exist.  Returns 0 on success.
 */
EMUXFS int emuxfs_sandbox_unveil_path(const char *);

/*
 * unveil(2) every directory in the array with read/write/create access
 * by calling emuxfs_sandbox_unveil_path() for each.
 */
EMUXFS int emuxfs_sandbox_unveil_mirrors(const struct emuxfs_args *);

/* unveil(nullptr, nullptr): no further changes to the filesystem view are allowed. */
EMUXFS int emuxfs_sandbox_unveil_lock(void);

/* pledge(2) with the given promise string. */
EMUXFS int emuxfs_sandbox_pledge(const char *);

#endif /* _SANDBOX_H_ */
