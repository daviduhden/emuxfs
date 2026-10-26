/* ops.h */

/*
 * This file belongs to emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
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
