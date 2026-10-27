---
name: checksum-and-self-healing
description: Work on emuxfs checksums, large-file checksum trees, validation, audit and self-healing. Use when editing chk.c, desc.c, lfile.c, util.c readback or restore, scan.c, or diagnosing corruption and repair behavior.
---

# Checksums and self-healing

## Activation

Use this skill when a change affects:

- `chk.c`/`chk.h` algorithm selection, digest size or encoding;
- `desc.c` metadata checksum inputs or content checksum per node type;
- `lfile.c` geometry, create/resize/delete, mmap or readback;
- `util.c` `emuxfs_readback`, `emuxfs_desc_chk_*`, restore primitives,
  ancestor recomputation or `emuxfs_restore_now`;
- `scan.c` audit/heal, or the restore queue in `state.c`;
- corruption, self-healing, ambiguity or checksum documentation.

## Checksum contract

| algorithm | digest size | storage |
|-----------|-------------|---------|
| `crc32` | 4 bytes | little-endian `uint32` |
| `md5` | 16 bytes | raw digest bytes |
| `sha1` | 20 bytes | raw digest bytes |

`EMUXFS_CHKSZ_MAX` is 20 (SHA-1) and drives `lfile` geometry.  The default for
new arrays is MD5.  The algorithm name is persisted in `muxfs.conf`; devices
with different algorithms are refused at open time.

The metadata checksum covers, in order: `eno`, `owner`, `group`, `mode`
(including type bits), `size` only for regular files, and the content
checksum.  It does **not** cover `atime`, `mtime`, `ctime`, nanoseconds,
birth time, `nlink`, `st_flags`, xattrs or ACLs.  File type is covered through
`mode`; hard links are refused rather than represented.

Content checksum by type:

- regular file at most one block: checksum of the file bytes;
- regular file larger than one block: root of the `lfile` tree;
- directory: sorted bytewise list of `(name bytes, child metadata checksum)`,
  excluding `.muxfs`, `.` and `..`;
- symbolic link: checksum of the target bytes from `readlink(2)`.

`meta.db`'s `flags` field is outside the metadata checksum.  `readback`
requires `MF_ASSIGNED`; do not remove that check.

## Large-file tree

- Level 0 has `ceil(size / 4096)` entries.
- Each higher level has `ceil(child_count / 204)` entries, where
  `FACTOR = EMUXFS_BLOCK_SIZE / EMUXFS_CHKSZ_MAX = 204`.
- Each level's absolute base comes from the next power of two at or above the
  child count, then repeated division by `FACTOR`.  Unused slots can exist
  between levels.  For three blocks, leaves are 0-2, index 3 is unused and the
  root is 4.
- A partial final block is hashed over its actual bytes, not zero-padded.
- The root entry is copied into the node's `meta.db` content checksum.
- The tree is memory-mapped and updated in place.  On OpenBSD, a write
  mapping needs `PROT_READ | PROT_WRITE`.
- `emuxfs_lfile_readback` verifies the touched leaves, every ancestor in the
  touched range, and the root against the expected content checksum.  It
  rejects a tree shorter than the computed size before mapping.

## Procedure

1. Decide whether you are changing **validation** or **repair**.  Keep them
   separate: `readback`/`desc_chk_*` must not write; `restore_*` performs
   writes.
2. Identify every place the same checksum is computed.  The directory
   checksum algorithm appears in the full hash, the ancestor patch and the
   directory restore; keep them identical.
3. If content handling changes, update the metadata checksum, the content
   checksum, the `lfile` root and the write/truncate/restore paths together.
4. Preserve partial-block and range semantics.  A read verifies only the
   leaves covering the requested range; `audit`/`readback` verify the whole
   node.
5. Before repairing, re-validate the source.  A checksum proves a copy matches
   its own metadata, not that it is the newest copy.
6. For a directory restore, verify the source directory's recomputed content
   checksum against its metadata before copying children.  Check the result of
   every removal and readback.
7. When repairing a large file, flush the copied tree and the `lfile`
   directory before writing metadata and assignment.
8. Update `ON_DISK_FORMAT.md`, `ARCHITECTURE.md`, `RECOVERY.md` and the tests
   when the behavior or coverage changes.

## Restore queue and ambiguity

- The queue stores `(device index, path)` in a byte-addressed ring.  It grows
  and compacts; paths at or above `PATH_MAX` cannot be queued.
- `emuxfs_restore_now` drains the queue and returns 0 only when every repair
  succeeded or was unnecessary.  It returns 1 for a failure, an ambiguous
  copy, or a device that cannot start a restoration.
- If `emuxfs_restoring_push` fails, the popped item cannot be retried in that
  run; report the failure, mark the device degraded and require `sync`.
- Coherent divergence compares every usable device, including the
  destination.  Two valid but different copies must not be repaired
  automatically; `heal` fails and `sync` is the explicit override.
- `sync` sets restore-only mode, which bypasses the ambiguity check because
  the operator designates the source.  Do not bypass it in `heal`.

## Validation

- **Static:** check that the same fields and exclusions are used in the full
  content hash, the ancestor patch and restore.  Check every `mmap` length
  against `fstat`.  Check the metadata field order.
- **Build:** `make check`.
- **Unit:** `make test`; `./tests/unit/test_core` (root enables device cases).
  Relevant cases include CRC32/MD5/SHA-1 known answers and byte order,
  metadata checksum determinism and sensitivity, metadata entry sizes,
  restore-queue growth, lfile create/resize/delete, assign cross-check and
  coherent divergence.
- **FUSE (root and `/dev/fuse0`):** `make integration` covers self-healing
  from content corruption, missing nodes, corrupt symlinks, timestamps,
  `audit`, `heal` and `sync`.
- **Crash ordering:** if repair write order changes, run `make faulttest`.

## Common problems

- Hashing a large file by copying whole blocks into the caller's buffer.  A
  partial read must copy only the requested byte range.
- Forgetting that the root of the `lfile` tree is the metadata content
  checksum; a resize or partial write must recompute ancestors and copy the
  root.
- Using `iend` instead of the full level entry width when recomputing
  ancestors; unmodified siblings are part of the parent hash.
- Treating a directory checksum as insertion-order dependent.  It is sorted;
  the process stays in the C locale.
- Repairing before validating the source, or choosing the first valid copy
  when several valid copies disagree.
- Forgetting to fsync the restored checksum tree before committing
  `meta.db`/`assign.db`.
- Assuming `readback` checks `nlink`, timestamps, flags or xattrs.  It does
  not; those are outside format version 1.
- Describing CRC32/MD5/SHA-1 as cryptographic authentication.  They detect
  accidental corruption only.
- Expecting a read of a large file to detect corruption outside the requested
  range.  Only audit/readback/restore verify the whole tree.


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
4. Validate changed references and run `python3 scripts/validate-agent-knowledge.py --scope range --base <base-ref> --head HEAD` (or omit `--base` for
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

- [`ON_DISK_FORMAT.md`](../../../ON_DISK_FORMAT.md) — metadata/content
  checksum inputs, directory hash, lfile geometry and limits.
- [`ARCHITECTURE.md`](../../../ARCHITECTURE.md) — read path and separation of
  validation and repair.
- [`RECOVERY.md`](../../../RECOVERY.md) — ambiguity, restore order and
  timestamps.
- [`SECURITY.md`](../../../SECURITY.md) — what checksums do and do not prove.
- `chk.c`, `desc.c`, `lfile.c`, `util.c`, `scan.c`, `state.c`,
  `tests/unit/test_core.c`.
