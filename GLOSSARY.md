# Glossary

* **agg** — Aggregate.
* **alg** — Algorithm; typically a checksum algorithm.
* **blk** — Block; a contiguous region of memory or data, typically of a
  standard size.
* **c** — Child; as in an entry from a "parent" directory.
* **chk** — Checksum.
* **cmd** — Command.
* **comp** — Component.
* **conf** — Configuration.
* **cud** — Create, Update, Delete; a categorization of write operations.
* **d** — Destination; typically the target "dev" for a write operation.
* **desc** — Description; relating to file metadata.  More specifically a
  description of an inode and its relevant metadata, including a checksum of
  the file content.  Metadata checksums are produced from these descriptions.
* **dev** — Device; not a block device but a directory used internally by
  emuxfs to store persistent data.  emuxfs mirrors data across an array of
  "dev"s.
* **dind** — Device index; ranging from zero to the number of "dev"s minus
  one.  Also used as the type for counting "dev"s.
* **dir** — Directory.
* **ds** — Dynamic stack.  Read `ds.h` for more information.
* **eno** — An "external" inode number, exposed through FUSE to the user via
  `stat(2)`.  See also `use_ino` in `fuse_new(3)`.
* **ent** — Entry; as in directory entry.
* **ino** — An "internal" inode number specific to one "dev", hidden from the
  user.
* **inout** — In-Out; a parameter to a function that is both an input and an
  output.
* **lfile** — Large file; pertaining to files larger than
  `EMUXFS_BLOCK_SIZE`, and their corresponding checksums.
* **lnk** — Link; typically a symlink.
* **mnt(s)** — Mount(s).
* **mp** — Mount-point.
* **mt** — Multi-threaded.
* **emuxfs** — The Enhanced Multiplexed File System.
* **node** — A file of any type.  This term is used to make clear that the
  "file" is not necessarily a regular file.  More generally an element of a
  tree structure.
* **off** — Offset.
* **op(s)** — Operation(s).
* **p** — Parent; as in parent directory.  More generally a node "one step
  higher" in a tree structure.
* **rc** — Return code; typically an `int` that is zero upon successful return
  from a function.
* **rd** — Read.
* **reg** — Regular file.
* **s** — Source; typically the information source "dev" of a restore
  operation.
* **sep** — Separator; the `/` that separates files and directories in paths.
* **seq** — Sequence number.
* **sz** — Size; a number of bytes.
* **tx** — Transfer.
* **ub** — Upper bound.
* **unity** — A unity build is a compilation technique whereby all modules are
  concatenated into a single module.  emuxfs no longer uses one: each `.c` is a
  translation unit and the fault-injection build reuses the same objects.
* **wr** — Write.

## Terms introduced by emuxfs

* **fault point** — A named location in the code (enabled only in
  fault-injection builds) at which the process can be made to terminate, to
  test crash consistency.  See [`TESTING.md`](TESTING.md).
* **format version** — The integer (`format_version` in `muxfs.conf`) that
  identifies the layout of the `.muxfs` directory.  Distinct from the program
  version, which is informational.  See [`ON_DISK_FORMAT.md`](ON_DISK_FORMAT.md).
* **legacy array** — An array formatted before `format_version` existed.  It
  has the layout of format version 1 and is accepted as such.
* **coherent divergence** — A state in which two or more devices hold
  internally valid but different copies of the same path.  No checksum can
  decide which is correct, so emuxfs refuses automatic repair.
* **hard link** — Two or more names for one inode.  Unsupported: it would give
  two paths one metadata slot and one `eno`.  Detected and refused.
* **restore queue** — A FIFO of (device, path) corruption notices, drained by
  `emuxfs_restore_now()`, that drives self-healing.
* **sandbox** — The `pledge(2)`/`unveil(2)` restrictions applied by the
  sandbox module; see [`SECURITY.md`](SECURITY.md).
