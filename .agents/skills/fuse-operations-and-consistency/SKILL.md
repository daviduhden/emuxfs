---
name: fuse-operations-and-consistency
description: Implement or review emuxfs FUSE callbacks and multi-device consistency. Use when editing ops.c, mount.c, util.c, desc.c, adding callbacks, or changing write, read, rename, truncate, permission, buffered I/O or statfs behavior.
---

# FUSE operations and consistency

## Activation

Use this skill when a change touches:

- `ops.c` callbacks or the `struct fuse_operations` table;
- `mount.c` FUSE lifecycle, mount options or topology checks;
- `util.c` transaction helpers, readback, ancestor recomputation or restore;
- `desc.c` metadata used by a write or read;
- write, read, rename, truncate, unlink, mkdir, symlink, chmod, chown,
  utimens, statfs, directory iteration, credentials or path handling.

Use `checksum-and-self-healing` when the core question is checksum coverage or
repair, and `crash-recovery-and-fault-injection` when the core question is a
durable state transition.

## Operating model

- The event loop is `fuse_loop()`, single-threaded.  Shared state has no
  locks; do not add threads or mutexes.
- A mutating callback visits usable devices in ascending index order.  Each
  device runs its own durable transaction.  There is no rollback: a device
  that fails after a mutation is marked degraded, the loop continues, and the
  callback succeeds if at least one device completed (`has_write`).
- Before touching the tree, callbacks that can observe or change it must drain
  the legacy write buffer (`emuxfs_wrbuf_flush`).  New writes are committed
  directly, so the buffer should be empty; do not reintroduce deferred writes.
- `emuxfs_eids_set`/`emuxfs_eids_reset` switch to the requesting user for
  syscalls made on its behalf.  Privileged internal work (metadata, lfile,
  state, restore) runs as root.  Never leave the effective uid lowered.

## Callback inventory

| group | callbacks |
|-------|-----------|
| lifecycle | `.init`, `.destroy` |
| create | `.mknod`, `.mkdir`, `.symlink` |
| read | `.open`, `.opendir`, `.getattr`, `.read`, `.readlink`, `.readdir`, `.access` |
| update | `.fsync`, `.flush`, `.release`, `.rename`, `.chmod`, `.chown`, `.utimens`, `.truncate`, `.write` |
| delete | `.unlink`, `.rmdir` |
| pass-through | `.statfs` |
| explicit `EOPNOTSUPP` | `.link`, `.lock` |
| deliberately absent | `.create`, `.fsyncdir`, xattr, `.ioctl`, `.poll`, `.fallocate`, `.lseek` |

The OpenBSD kernel does not issue `FUSE_CREATE`, `FUSE_ACCESS`,
`FUSE_READDIRPLUS`, xattr or remote-lock opcodes.  `.access` is installed but
must not be relied on for permission enforcement.  Do not add a callback the
kernel never delivers.

## Mutating transaction procedure

For create, update, delete and rename:

1. Flush the legacy buffer and sanitize every path.
2. For each usable device, call `emuxfs_working_push(i)` and fail closed if it
   cannot be made durable.
3. Validate the pre-image: stat, metadata, checksum, mapping.  Refuse hard
   links.
4. Apply the mutation.
5. Write metadata; create/delete also write the assignment.  Rename keeps the
   moved inode's metadata and assignment, but must clear those of a replaced
   destination.
6. `fsync` content, `lfile` when present, metadata, assignment and affected
   parent directories.
7. `emuxfs_readback` the result.
8. `emuxfs_ancestors_meta_recompute` with the correct `CUD_*` type.
9. `emuxfs_working_pop(i, now)` to clear `working` and advance `seq`.

On a failure **before any mutation**, call `emuxfs_working_cancel(i)`: it
clears `working` without advancing `seq`.  On a failure **after a mutation**,
mark the device degraded and call `emuxfs_working_pop`.  A failed truncate or
write may already have changed data; treat it as a partial mutation and
degrade rather than cancelling.

Per-operation deviations that are intentional:

- create fsyncs the new node before metadata and assignment;
- delete unlinks before clearing metadata and deletes the large-file tree
  after the name is gone;
- update writes no assignment and fsyncs content after metadata;
- rename vacates the source to `.muxfs/rename.tmp` and a replaced destination
  to `.muxfs/rename.dest.tmp`, updates ancestors one entry at a time, writes
  no node metadata or assignment for the moved inode, and clears a replaced
  destination after the commit.

## Read path

- `emuxfs_op_read` validates the metadata checksum and, for the requested
  range, the content checksum.  A large file verifies only the leaves covering
  that range; `audit`/`readback`/restore verify the full tree.
- A missing node on one mirror is remembered, not returned immediately.  If
  another mirror serves it, the missing copy is queued for repair.  If every
  mirror reports it missing, return `ENOENT` without starting a repair.
- Permission errors try the next mirror; `EINVAL` is a caller error and
  returns immediately.  I/O and consistency errors are queued for repair and
  the next mirror is tried.  If every mirror fails, drain the restore queue
  and return the last error (without draining for `ENOENT` or permission
  errors).
- After a successful read, `emuxfs_restore_now()` drains queued corruption.
  A repaired copy does not retroactively satisfy the failed read.

## Credentials, paths and descriptors

- Sanitize every FUSE path with `emuxfs_path_sanitize`; reject `.`, `..`,
  empty components and `.muxfs`.
- Use `*at()` relative to the pinned `root_fd`/`muxfs_fd`.
- Use `O_NOFOLLOW` for private objects, reads and data files that must not be
  redirected; use `O_CLOEXEC` for long-lived descriptors.
- Never pass `O_TRUNC` through on a user-visible open.  Convert it to the
  normal truncate operation.  Restore is the only sanctioned `O_TRUNC` path,
  and it immediately rewrites metadata, assignment and checksums.
- Hard links are refused on create, delete, update, rename and restore.

## statfs

`statfs` reports the minimum capacity and inode counts across usable devices,
not a sum.  The logical block size is 4096.  A device that becomes degraded at
runtime is skipped by `emuxfs_dev_get(..., 0)` and drops out of the aggregate.
Use checked byte arithmetic; do not multiply `frsize` by block counts without
an overflow check.

## Validation

- **Static:** trace every changed callback through flush, sanitize, per-device
  loop, pre-validation, mutation, metadata/assignment, fsyncs, readback,
  ancestors and `working_pop`/`working_cancel`.
- **Build:** `make check`.
- **Unit:** `make test`; `./tests/unit/test_core` (device cases need root).
- **FUSE (root and `/dev/fuse0`):** `make integration` and
  `make paralleltest`.  Integration covers boundary sizes, unaligned writes,
  partial reads, truncate grow/shrink, rename over an existing name,
  `st_blocks`, hard links, remounts and audit read-only behavior.
- **Crash ordering:** if you change the transaction, run `make faulttest` and
  update `RECOVERY.md` and `TESTING.md`.

## Common problems

- Moving `working_pop` before every durable write, or calling it on a path
  that has not mutated.  Use `working_cancel` for the latter.
- Forgetting to clear a replaced destination's metadata, assignment and large
  file tree on rename, or trying to replace it in one step.  Clear it after
  the commit and keep each ancestor update to one directory-entry change.
  This leaves orphan records and can break inode reuse otherwise.
- Treating a failed write/truncate as safely cancellable.  It may have
  partially changed the file.
- Returning `-EIO` for every read failure without distinguishing `ENOENT`,
  permission errors and corruption.
- Ignoring the `readdir` cookie or the fill callback's full-buffer result;
  this truncates or repeats directory listings.
- Assuming `emuxfs_open`/`opendir`/`access` see every mirror; they consult the
  first usable device.  Do not build consistency guarantees on them.
- Adding per-open state or thread-local credentials; the design is
  single-threaded and path-based.
- Using `fsync` failure as a normal error return.  The existing contract is
  that a pinned-descriptor fsync failure exits; user-visible syscall errors
  return a negated errno or degrade.
- Reporting success when every device failed, or adding a rollback that
  `RECOVERY.md` does not describe.


## Maintenance & Continuous Learning

When using this skill to modify the repository, complete one post-change review
before reporting the task done:

1. Re-read this skill and applicable `AGENTS.md` and specialized agent
   instructions; verify their responsibilities, assumptions and references
   still match the implementation.
2. Update this skill when procedures, tools, configuration, paths, validation
   commands or assumptions changed, or when implementation work produced
   reusable knowledge. Correct guidance that proved inaccurate and remove
   obsolete or contradictory instructions. Add only knowledge useful to
   future work; omit run-specific details.
3. Review other skills mapped to the changed behavior and update any that are
   affected. Create a skill only for a distinct durable responsibility that
   no existing skill covers. Keep specialized agent instructions synchronized
   with this skill's scope and workflow.
4. Validate changed references and run `python3
   scripts/check_agent_maintenance.py --base <base-ref>` (or omit `--base` for
   working-tree changes). Structural validation does not judge semantic
   relevance; inspect the diff for that explicitly. If an update is impossible
   or validation fails, report the task incomplete with the reason.
5. Include reviewed/updated instruction files, knowledge changed, actual
   validation results and `PASS`/`FAIL` in the final **Agent & Skills
   Synchronization** report. A code change without relevant knowledge
   maintenance is incomplete.

If this task changes agent/skill policy itself, review the complete policy,
all skill sections, validator and CI integration together. One complete review
is sufficient; repeat validation only after fixing an error.


## References

- [`ARCHITECTURE.md`](../../../ARCHITECTURE.md) — layers, write/read path,
  concurrency, statfs.
- [`RECOVERY.md`](../../../RECOVERY.md) — commit order and failure outcomes.
- [`SECURITY.md`](../../../SECURITY.md) — credentials, path handling and
  descriptor policy.
- [`COMPATIBILITY.md`](../../../COMPATIBILITY.md) — FUSE 2.6 high-level API
  and the opcode subset.
- `ops.c`, `mount.c`, `util.c`, `desc.c`, `tests/integration/integration.pl`.
