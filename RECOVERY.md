# Recovery model

emuxfs treats an array as a set of redundant copies plus a sequence
number that orders operations.  A crash never leaves "half a transaction"
that is silently treated as valid: either the sequence numbers agree and the
checksums decide what is corrupted, or the sequence numbers disagree and the
administrator must choose a source with `sync`.

This document lists the failure points of a multi-device write and the
recoverable state expected after each.  It is the contract that the crash
tests in `tests/integration/crash.pl` try to verify.

## Operation order

For each device, a create/update/delete/rename does:

1. `emuxfs_working_push`: write `working += 1`.
2. Apply the change to the tree.
3. Write and `fsync` `meta.db` (and `assign.db`, and the `lfile` tree).
4. `fsync` the affected data and the parent directory.
5. Re-read and verify the node.
6. Recompute ancestor directory metadata checksums.
7. `emuxfs_working_pop`: write `working -= 1` and `seq += 1`.

All devices are visited in turn.  Nothing about the array is considered
committed until every device has reached step 7.

## Failure points

| failure                                   | on-disk state                              | recovery |
|-------------------------------------------|--------------------------------------------|----------|
| before any device's step 1                | unchanged; `seq` equal, `working` 0        | nothing to do |
| after step 1, before step 7 on some devices | `working != 0` on those devices; `seq` may differ | `sync` the affected devices from a good one |
| after first device's step 7, before second's | `seq` differs between devices            | `sync` the lagging device from a leading one |
| between content write (2) and meta write (3) | content and `meta.db` disagree          | `audit`/`heal`; if `seq` agrees and the sources agree, `heal` repairs from another device |
| between meta write (3) and ancestor recompute (6) | ancestor directory checksums are stale | `audit` reports ancestors; `heal` recomputes them |
| before `fsync` of data/meta                | change may be lost after power loss       | checksums detect it; `heal` restores |
| after `fsync`, before `seq` commit (7)     | `working != 0`; `seq` unchanged           | as for "after step 1" |
| during restoration                         | `restoring != 0`                          | re-run `heal` (restoration is idempotent) |
| device removed or lost                     | `seq` mismatch; missing file              | `sync` a replacement device from the survivors |
| external modification of one device        | content/metadata checksum mismatch        | reads self-heal; or `audit` then `heal` |
| several valid but different copies          | every copy matches its own checksums      | ambiguous: **no repair**; resolve with `sync` |

## Commands

* `emuxfs audit dir...` — read-only inspection.  Requires the array not to be
  mounted and sequence numbers to agree.
* `emuxfs heal dir...` — validates and restores corrupted nodes from another
  device.  Refuses to act when the valid copies disagree (see below).
  Requires the array not to be mounted and sequence numbers to agree.
* `emuxfs sync dest src...` — rewrites `dest` to match the state of the
  `src` device(s).  This is the explicit, administrator-directed recovery
  path when sequence numbers do not agree, when replacing a dead device, or
  when copies diverge.  The destination may be an empty directory (it will be
  formatted) or an existing device.
* `emuxfs mount [-f] mp dir...` — mounts the array.  `-f` only keeps the
  process in the FUSE foreground; it is **not** a force-mount of an
  inconsistent array and it does not bypass the clean-state check.  A device
  whose `state.db` is not clean (`mounted`, `working` or `restoring` non-zero,
  or `degraded`) cannot be mounted at all; recover it offline first.

## Hard links

emuxfs does not support hard links and does not pretend to.  Two names for
one inode share one metadata slot and one external inode number, so unlinking
or restoring one name would silently change the other.  The array therefore
refuses them:

* creating a hard link through the filesystem fails with `EOPNOTSUPP`;
* deleting a name of a hard-linked file fails with `EOPNOTSUPP`;
* `audit` and `heal` report `Unsupported hard link` and fail;
* restoration refuses to create or overwrite a hard-linked node.

Hard links cannot be present at `format` time because `format` requires empty
directories.  They can only appear through direct modification of a device, or
from a filesystem that already contained them before it was turned into an
array by other means.

## Ambiguous copies (coherent divergence)

A checksum proves that a copy matches its own metadata; it does not prove that
the copy is the newest or the correct one.  When two or more devices hold an
internally valid but different copy of the same path, emuxfs refuses to
choose:

* automatic restoration is not performed;
* an alert is logged and the device is marked degraded;
* `heal` exits non-zero so an operator must resolve it;
* the resolution is explicit: `emuxfs sync destination source`.

A majority of devices does not establish truth, only consensus; emuxfs does
not implement quorum, because a majority of coherently wrong copies would win.

Because `partially written` files are never accepted (checksums are verified
before use), a half-written file is treated as corruption, not as data.

## Device state machine

Each device's `state.db` is a small state machine over four flags
(`mounted`, `working`, `restoring`, `degraded`).  The transitions are
performed by `emuxfs_working_*`, `emuxfs_restoring_*` and
`emuxfs_degraded_*`:

| state     | mounted | working | restoring | degraded |
|-----------|---------|---------|-----------|----------|
| clean     | 0       | 0       | 0         | 0        |
| in use    | 1       | 0       | 0         | 0        |
| writing   | 1       | 1       | 0         | 0 or 1   |
| restoring | 0 or 1  | 0       | 1         | 0 or 1   |
| degraded  | any     | any     | any       | 1        |

Authorised transitions:

* clean -> in use: opening the device read-write sets `mounted` and fsyncs.
* in use -> writing: `emuxfs_working_push`; writing -> in use:
  `emuxfs_working_pop`.
* any -> restoring: `emuxfs_restoring_push`; restoring -> previous:
  `emuxfs_restoring_pop`.
* any -> degraded: on an I/O or integrity failure, or on sequence exhaustion.
* in use -> clean: `emuxfs_dev_unmount` clears `mounted`.

A device that is not clean cannot be mounted.  `degraded` is never cleared by a
normal mount or by `sync`; it records a hardware, integrity or
sequence-exhaustion fault, and only `format` (or replacing the directory)
resolves it.  A structurally impossible record (for example `working = 2`) is
refused by `mount`, `audit` and `heal` with a distinct diagnostic and is not
treated as merely dirty.

`sync` is the explicit recovery path.  It reopens the destination with force —
accepting both a dirty device and a structurally impossible record, with a
warning — rebuilds it from the source, and then commits a fresh, valid
`state.db` with `working = 0` and `restoring = 0`.  A device left interrupted,
or left with a torn/invalid record, therefore becomes mountable again.  `sync`
does not clear `degraded`; that requires `format` (or replacing the directory).

`audit` is **read-only**: it opens the devices read-only, does not set
`mounted`, and does not modify `state.db` or any other persistent byte.
`heal` is the explicitly destructive command.

Because `partially written` files are never accepted (checksums are verified
before use), a half-written file is treated as corruption, not as data.

## Sequence number exhaustion

In format version 1 the `seq` counter does not wrap.  If it reaches
`UINT64_MAX`, `emuxfs_working_pop` marks the device degraded and refuses to
advance it, so the array cannot be mounted again until an operator intervenes.
This fails closed and avoids the epoch protocol (`seq_zero_time` rewriting) of
earlier versions.  `2^64` successful operations per device is far beyond any
realistic lifetime.


## What is guaranteed

* Metadata and content are checksummed before being accepted.
* A device that did not complete an operation is detectable via `working`
  and/or `seq`.
* Restoration is idempotent and writes checksums only after the data.
* No command silently migrates or rewrites the on-disk format.

## Commit order and crash windows

For every mutating operation on a device the order is:

```
1. working_push            state.db: working=1, fsynced
2. physical mutation       create/write/truncate/unlink/rename
3. meta.db write           checksummed metadata
4. assign.db write         eno -> ino mapping
5. fsync(meta.db, assign.db)
6. fsync the affected data and the parent directory
7. readback                metadata, content and the assign mapping
8. ancestor metadata       recomputed and fsynced
9. working_pop             state.db: working=0, seq++, fsynced
```

`working = 1` is durable **before** any mutation (step 1) and `working = 0`
becomes durable **after** all data, metadata, the mapping and the namespace
have been made durable (step 9).  This makes `working` a **recovery marker**:
if it is non-zero after a reboot, the operation was interrupted at some step
and the device must not be mounted; recover it explicitly with `sync`.

Crash after each step:

| crash after | state.db | mount | required action |
|-------------|----------|-------|-----------------|
| 1 | `working=1` | refused | `sync` from an intact device |
| 2..6 | `working=1` | refused | `sync` |
| 7..8 | `working=1` | refused | `sync` (data is complete but uncommitted) |
| 9 (fully) | clean, `seq` advanced | allowed | none |

There is no journal and no atomic cross-file commit: `meta.db` and `assign.db`
are two files, so a crash between steps 3 and 4 leaves one side written.  That
state is always covered by `working = 1`, hence refused at mount and resolved
by `sync`.  The per-node and whole-database meta/assign cross-checks detect the
same condition if it is ever observed on a device that is otherwise clean
(for example a legacy array damaged before this format was in use).

## Readback is consistency, not durability

`readback` (step 7) proves that the kernel returns the expected metadata and
content, including the assign mapping.  It does **not** prove that the data
has reached stable storage: only the preceding `fsync` calls address
durability.  Recovery therefore relies on the `fsync` ordering above, not on
`readback`.

## Timestamps on restore

Format version 1 does not checksum timestamps, but a restore still copies them
as far as possible: regular files and directories are restored with
`futimens(2)`/`utimensat(2)` from the source's `st_atim` and `st_mtim`
(nanosecond precision), and symbolic links with
`utimensat(2, AT_SYMLINK_NOFOLLOW)` so the link itself, not its target, is
stamped.  `UTIME_OMIT`/`UTIME_NOW` are not used: explicit values are written.

`mtime` is the meaningful one and is reproduced exactly when the operation
succeeds.  `atime` is **best effort**: reading the source to reproduce it can
itself advance the source's `atime` depending on the source filesystem's mount
options, so the destination may receive the pre-read value.  No integrity
decision depends on either timestamp.

## Fault injection models process crash, not power loss

The fault points terminate the process with `_exit` at a chosen boundary.
That models a process crash faithfully, and, because every state transition is
`fsync`ed, it also exercises the recovery decisions that a power loss would
trigger.  It does **not** model torn sectors, controller write caching or
storage reordering.  The suite is a *crash-recovery* suite, not a proof of
power-loss safety.
