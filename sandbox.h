/* sandbox.h */

/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
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
 *   chown   fchownat with an arbitrary uid/gid (format, restore and the
 *           chown operation)
 *   id      seteuid/setegid/setgroups (the FUSE callbacks impersonate the
 *           caller and drop root's supplementary groups)
 *   unix    syslog(3) AF_UNIX socket operations
 *
 * mount(2) is performed by libfuse before any pledge is taken.  unmount(2) is
 * never called: the kernel detaches the filesystem when the process closes
 * the FUSE device on exit.
 */

#ifndef _SANDBOX_H_
#define _SANDBOX_H_

#include "emuxfs.h"

#define EMUXFS_PLEDGE_VERSION "stdio"
#define EMUXFS_PLEDGE_FORMAT "stdio rpath wpath cpath fattr chown"
#define EMUXFS_PLEDGE_SCAN "stdio rpath wpath cpath fattr chown unix"
#define EMUXFS_PLEDGE_SYNC "stdio rpath wpath cpath fattr chown unix"
#define EMUXFS_PLEDGE_MOUNT "stdio rpath wpath cpath fattr chown id unix"

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

/* unveil(nullptr, nullptr): no further changes to the filesystem view are
 * allowed. */
EMUXFS int emuxfs_sandbox_unveil_lock(void);

/* pledge(2) with the given promise string. */
EMUXFS int emuxfs_sandbox_pledge(const char *);

#endif /* _SANDBOX_H_ */
