# Architecture of emuxfs

This document describes the internal structure of emuxfs.  It is derived from
the original Multiplexed File System; the original design and terminology are
preserved.

## Terminology

An **array** is a set of **devices**.  A device (called a *dev*) is not a
block device: it is a directory that has been formatted with `emuxfs format`.
The array is mounted at a **mount point** through FUSE, and presents a single
filesystem tree.  Each dev holds a full copy of that tree plus a private
`.muxfs` directory with the checksum databases.

## Layers

The code is organised in layers; data flows down from the CLI or FUSE
frontend to the device layer and back.

1. **CLI / entry point** — `emuxfs.c` dispatches to one of the sub-commands.
2. **Command frontends**
   * `mount.c` — FUSE frontend; sets up libfuse and the sandbox.
   * `format.c` — `format` sub-command.
   * `scan.c` — `audit` and `heal` sub-commands.
   * `sync.c` — `sync` sub-command (rebuild or seed a device).
3. **FUSE operations** — `ops.c` implements the `struct fuse_operations`
   callbacks.  emuxfs uses the FUSE 2.6 high-level API provided by the libfuse
   in the OpenBSD base system.  All callbacks are dispatched on a single
   thread: `fuse_loop()` is used, not `fuse_loop_mt()`, so the shared state
   needs no locking.
4. **Filesystem operations / integrity engine** — `util.c` implements the
   per-node checksum verification (`emuxfs_readback`), metadata recomputation
   for ancestors (`emuxfs_ancestors_meta_recompute`), restoration
   (`emuxfs_restore_*`) and the restore queue.  `desc.c` builds and hashes
   metadata descriptions.
5. **Large-file checksum tree** — `lfile.c` maintains the per-block checksum
   tree for regular files larger than one block, stored under
   `.muxfs/lfile/<inode>`.
6. **Checksum engine** — `chk.c` (with `chk.h`) wraps CRC32, MD5 and SHA-1.
7. **Device / on-disk format** — `dev.c` owns the device array and the
   `.muxfs` files (`muxfs.conf`, `state.db`, `meta.db`, `assign.db`,
   `lfile/`).  `conf.c` parses and writes `muxfs.conf`, including the format
   version.
8. **Global state** — `state.c` owns the restore queue, the external inode
   allocator, the write buffer and logging.  `ds.c` provides the dynamic
   stack used by the recursive directory walkers.
9. **OpenBSD sandbox** — `sandbox.c` is the only place that calls `pledge(2)`
   and `unveil(2)`.
10. **Fault injection** — `fault.c` is inert unless `EMUXFS_FAULT_INJECTION`
    is defined at compile time.

`version.c` holds the program version.  There is no unity build: every `.c` is
its own translation unit, and the fault-injection build uses the same objects.

## Identity: internal vs external inodes

Each dev is a normal filesystem, so every node already has an on-disk inode
number, called the **internal inode** (*ino*).  These differ between devs for
the same logical file.  emuxfs therefore assigns each node a stable
**external inode** number (*eno*).

* `.muxfs/meta.db` is indexed by *internal* inode; each entry stores the
  node's *eno* together with a metadata checksum and a content checksum.
* `.muxfs/assign.db` is indexed by *eno*; each entry stores the *internal*
  inode that currently has that *eno* in this dev.
* The `use_ino` FUSE option makes the kernel expose *eno* through `stat(2)`.

Because internal inode numbers may be reused after an unlink, `assign.db` is
the authoritative map from logical identity to physical inode.

## Write path

Every write operation (create, update, delete, rename) is applied to each
mounted dev in turn.  For each dev the sequence is:

1. `emuxfs_working_push(dev)` increments a persistent *working* counter in
   `state.db`, so an interrupted operation is visible at the next mount.
2. The change is applied to the tree.
3. The affected `meta.db` and `assign.db` entries are written and `fsync`ed.
4. The content (for large files, the `lfile` checksum tree too) is `fsync`ed.
5. The parent directory is `fsync`ed for create/rename/unlink.
6. `emuxfs_readback` re-reads and verifies the node.
7. `emuxfs_ancestors_meta_recompute` updates the metadata checksums of all
   ancestor directories up to the root.
8. `emuxfs_working_pop(dev)` decrements *working* and increments the array
   sequence number in `state.db`.

The sequence number is the ordering token: all devs must agree on it before
an array may be mounted, audited or healed.  Writing only at step 8 means an
interrupted operation leaves some devs at sequence *n* and others at *n+1*,
which is detected as a mismatch and requires a `sync`.

## Read path and self-healing

A read verifies the node's metadata checksum and its content checksum.  If
either fails, the path is pushed onto the restore queue and the next dev is
tried.  If no dev has an intact copy, the operation fails.  After a
successful read, any queued corruption is drained by `emuxfs_restore_now()`,
which chooses a source dev, restores the damaged copy and recomputes ancestor
metadata.

Before a source is chosen, `emuxfs_restore_now()` checks for *coherent
divergence*: if two or more mounted devices hold an internally valid but
different copy of the path, no repair is performed, an alert is logged and the
device is marked degraded.  A checksum proves a copy matches its own metadata;
it does not prove that it is the newest or the correct copy.  The ambiguity is
resolved explicitly by the administrator with `emuxfs sync destination
source`.

## Hard links

emuxfs assigns one metadata slot and one external inode number per inode, so
two names for the same inode cannot be represented independently.  Hard links
are therefore refused: `link(2)` through the filesystem returns
`EOPNOTSUPP`; deleting a name of a hard-linked file is refused; `audit` and
`heal` report `Unsupported hard link` and fail; restoration refuses to create
or overwrite a hard-linked node.  `format` cannot encounter them because it
requires empty directories.  This differs from *symbolic* links, which are
fully supported and checksummed by their target text.

## Checksum trees for large files

Regular files larger than `EMUXFS_BLOCK_SIZE` (4 KiB) have a checksum tree in
`.muxfs/lfile/<internal inode>`.  Leaf entries are per-block checksums; each
parent level hashes `EMUXFS_BLOCK_SIZE / EMUXFS_CHKSZ_MAX` children.  The root
entry is the file's content checksum, which is what `meta.db` stores.  The
tree is memory-mapped and updated in place as blocks change.

## Concurrency

emuxfs calls `fuse_loop()`, the single-threaded event loop of the OpenBSD
base libfuse, rather than `fuse_loop_mt()`.  Multi-mirror operations resemble
small transactions and the global state (restore queue, metadata, sequence
number, `working`/`restoring` counters, healing) is not made thread-safe; the
single-threaded loop is the chosen policy.  No mutexes are added merely to
enable multithreading.  The dynamic stack in `ds.c` is explicitly not
thread-safe, and neither is the global state in `state.c`.

## External modification of a device

A device is an ordinary directory tree.  Anything that writes to it outside
emuxfs changes the content or metadata and will be detected by the
next checksum verification, which either heals the device from another copy
or reports it as corrupted.  This is the mechanism used by the resilience
tests.

## Separation of validation and repair

`emuxfs_readback` / `emuxfs_desc_chk_*` only validate.  Repair lives in
`emuxfs_restore_*` (driven by the restore queue).  `audit` validates and only
reports; `heal` validates then repairs (and refuses on ambiguity).  Keeping
these separate makes it possible to test validation logic without performing
writes.

## Logical capacity (statfs)

An array is a mirror, not striping: every node must exist on every device, so
the logical filesystem is limited by the most restrictive device.  `statfs`
therefore reports the **minimum** across mounted devices for the block
counts, the free counts and the inode counts, the logical block size
(`EMUXFS_BLOCK_SIZE`), and the minimum `f_namemax`.  Capacities are never
summed.  A device that is not clean is not mounted, so it does not take part
in the aggregate.

## Direct modification of a device

A device is an ordinary directory tree; emuxfs cannot assume it is untouched.
Detectability of external changes is:

| change | detected |
|--------|----------|
| file content modified | on access; `audit` |
| file created or removed | `audit` (parent directory checksum) |
| `chmod`/`chown` changed | on access; `audit` |
| `rename` performed directly | `audit` |
| hard link created directly | `audit`/`heal` (fails explicitly) |
| `.muxfs` corrupted | at mount (configuration/state) or on access (metadata) |
| coherent divergence (all devices valid but different) | `heal` refuses; `audit` does not compare devices |

`audit` walks each device independently; it detects corruption *within* a
device, not disagreement *between* devices.  Disagreement is surfaced when an
automatic repair would need to choose a source: `heal` then refuses.
