# Compatibility

## Target system

emuxfs targets OpenBSD 7.9 (amd64).  There is no version-conditional code;
OpenBSD 8.0 is not a CI target and no preparation is made for it.

## FUSE backend: the native OpenBSD implementation

emuxfs uses the FUSE implementation shipped with OpenBSD: the `libfuse` in
the base system, through its **FUSE 2.6 high-level API**.  This is the
natively supported implementation and the one written for OpenBSD's
`fuse(4)`.

* The client includes `<fuse.h>` from the base system and links `-lfuse`.
* The mount lifecycle is `fuse_setup(3)` / `fuse_loop(3)` / `fuse_destroy(3)`.
  `fuse_setup()` mounts and daemonizes, so the FUSE device is opened and the
  filesystem is mounted before any `pledge(2)`/`unveil(2)` policy is applied.
* The event loop is `fuse_loop()` (single-threaded).  `fuse_loop_mt()` is not
  used.  In this libfuse `fuse_loop_mt()` returns -1 and the `mt` output of
  `fuse_parse_cmdline(3)` is always 0, so callbacks are serialised.
* `fuse_setup()` internally installs signal handlers; `fuse_destroy()`
  invokes the `destroy` callback, which flushes buffered data and detaches
  the array.  `unmount(2)` is never called afterwards because it is not
  permitted under `pledge(2)`; the kernel detaches the filesystem when the
  process closes the FUSE device on exit.

### Why not libfuse3

OpenBSD's kernel `fuse(4)` emulates Linux FUSE protocol **7.19** and exchanges
a specific byte layout over `/dev/fuse0`.  The base `libfuse` is the
implementation written for that interface; libfuse3 has no OpenBSD backend
and would require an out-of-tree port with a new mount/transport backend and
protocol adjustments.  emuxfs therefore stays on the native implementation
rather than depending on a port.  The analysis of libfuse3 remains available
in the project history but is not part of the build.

For reference, the base `libfuse` gained a low-level API in 2025, but it is
still FUSE 2.6 semantics; emuxfs deliberately remains on the high-level API,
which keeps the FUSE dependency to `ops.h`, `ops.c` and `mount.c`.

### OpenBSD kernel operation subset

The OpenBSD kernel only issues these protocol operations:

```
FUSE_LOOKUP  FUSE_FORGET  FUSE_GETATTR FUSE_SETATTR FUSE_READLINK
FUSE_SYMLINK FUSE_MKNOD   FUSE_MKDIR   FUSE_UNLINK   FUSE_RMDIR
FUSE_RENAME  FUSE_LINK    FUSE_OPEN    FUSE_READ     FUSE_WRITE
FUSE_STATFS  FUSE_RELEASE FUSE_FSYNC   FUSE_FLUSH    FUSE_INIT
FUSE_OPENDIR FUSE_READDIR FUSE_RELEASEDIR FUSE_DESTROY
```

There is no `FUSE_CREATE`, `FUSE_ACCESS`, `FUSE_READDIRPLUS`, `FUSE_INTERRUPT`,
xattr or remote-lock opcode.  Consequently the `.create`, `.access`,
`.fsyncdir`, xattr, `.ioctl`, `.poll`, `.fallocate` and `.lseek` callbacks are
never invoked.  File creation is `mknod` followed by `open`, as the kernel
performs it.  `struct fuse_operations` in the base header still declares
`access`, `create`, `fsyncdir`, `ftruncate` and `fgetattr`; they are simply
not delivered.

## Inode identity (`ino` / `eno`)

The high-level API is not inode-controlled: the kernel and libfuse use their
own node identifiers and only expose the `st_ino` returned by callbacks.
emuxfs passes `-ouse_ino` so that the kernel reports the external inode
(`eno`) through `stat(2)` and `readdir(2)`, while the internal node identity
remains libfuse's.  The `eno` mapping itself is owned by emuxfs
(`meta.db`/`assign.db`); see ON_DISK_FORMAT.md and the audit notes.  The
low-level API would allow emuxfs to control node identifiers directly, but it
would also require implementing lookup/forget and would not change the
underlying mapping, so the high-level API is retained.

## On-disk naming

The project, the executable (`emuxfs`) and the internal symbol prefix
(`emuxfs_`) were renamed, but the persistent names were **kept from muxfs**:

* private directory `.muxfs`;
* configuration file `muxfs.conf`;
* `state.db`, `meta.db`, `assign.db`, `lfile/`;
* checksum algorithm names `crc32`, `md5`, `sha1`.

Branding does not override on-disk compatibility.  `dev.c` derives every
private path from `EMUXFS_PRIVATE_DIR`.

## Format compatibility classification

**Compatible with existing muxfs arrays.**  An array created by muxfs is read
unchanged: the private directory, configuration file and database layouts are
identical; the legacy `version=MAJOR.MINORflavor` spelling is parsed including
the revision the original parser discarded; a missing `format_version` key is
accepted as format version 1; the default algorithm remains `md5`.

**Additive change.**  emuxfs writes `format_version=1` to `muxfs.conf`.  This
is the only persistent addition, and it is what lets emuxfs refuse a future
incompatible layout instead of mounting it.

**Internally changed, no persistent effect.**  The checksum type refactor, the
build-system change, sandboxing, logging, exact-I/O helpers, `fsync`
additions and the tests do not alter the on-disk layout.

**Program version.**  The program version is `1.0-current` (from
`0.5-current`).  Program version mismatch does not prevent mounting; only the
format version controls compatibility.  The program version is stored for
provenance.

## CI

The CI matrix is OpenBSD **7.9**, using `vmactions/openbsd-vm@v1` on
`ubuntu-latest`; the 7.9 image is the only one used.  The same full test set
runs on two architectures, **amd64** and **arm64** (`arch: aarch64`): build
with clang and C17, the strict warning build (`make check`), the unit tests,
the FUSE integration tests, the fault-injection tests, install, `mandoc`, and a
clean rebuild.  No external FUSE package is needed
because the native implementation is part of the base system.  On an
architecture where `/dev/fuse0` is unavailable the FUSE integration test is
skipped rather than failed.

## Version 1 guarantee and the decision on version 2

Format version 1 protects the node type, permissions, owner, group, the
`eno` mapping, the content (or the `lfile` checksum tree), directory entry
sets and symlink targets.  It does not protect timestamps, `nlink`, OpenBSD
file flags or extended attributes.  This is documented in ON_DISK_FORMAT.md.

**Decision for now: version 1 is retained.**  Its weaknesses are bounded and
documented, and each is either detected or fails closed:

* interrupted operations are marked by `working = 1` and refused at mount;
  `sync` is the explicit recovery that rebuilds the device and clears the
  marker;
* cross-device inconsistency is caught by `seq` and by checksums;
* a structurally invalid `state.db` record is refused by `mount`/`audit`/`heal`
  (only the explicit `sync` recovery rewrites it);
* meta/assign mismatches are cross-checked;
* `seq` saturates and degrades rather than wrapping.

Version 2 is **designed but not implemented**; it is required only if any of
these is added: a checksum/generation for `state.db`, protected `mtime`,
OpenBSD file flags, or a per-inode generation.  A v2 array would be
identified by `format_version` in `muxfs.conf` and by a magic in `state.db`;
migration would be an explicit command that refuses dirty or ambiguous
arrays.  See ON_DISK_FORMAT.md for the proposed byte layout.
