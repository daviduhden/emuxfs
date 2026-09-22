# Change log

## Production readiness (2026-09-22)

emuxfs is declared **stable and ready for production use**.  The complete
validation sequence — strict-warning build, unit tests, FUSE integration, a
32-worker parallel-load stress test, fault-injection recovery (interrupted
create, update, delete, heal and sync), repeated mount/unmount cycles, the
refusal of a second mount and a bounded parser fuzz run — passes in CI on
OpenBSD 7.9 amd64 and arm64, and the on-disk format is frozen at version 1.
See README.md (Stability levels) and TESTING.md for the criteria and the
evidence.

## 1.4-enhanced (2026-09-22)

Fifth phase: commit ordering, identity and the v1/v2 decision.

* **`sync` now completes recovery.**  It clears `working` and `restoring`
  when it commits the rebuilt `state.db`, so a device left interrupted by a
  crash (the case the manual page documents as requiring `sync`) becomes
  mountable again.  Previously it kept the dirty marker and the device stayed
  unmountable after the very recovery that was supposed to fix it.
* **`sync` can repair a structurally invalid `state.db`.**  `mount`, `audit`
  and `heal` still refuse it, but the explicit `sync` recovery accepts it with
  a warning and rewrites a valid record.
* **`working = 1` is now enforced as durable before any mutation.**  The
  call sites check `emuxfs_working_push`/`emuxfs_restoring_push`, and the
  state helpers treat a failed `state.db` write or `fsync` as fatal, so a
  state-write failure can no longer be silently ignored.
* **`op_delete` no longer deletes the `lfile` checksum tree of a file whose
  `unlink` failed**, and `fsync`s the cleared `meta.db`/`assign.db` entries
  before committing `working`/`seq`.
* **`emuxfs_dev_open` opens every descriptor before persisting `mounted`**, so
  a transient `open` failure cannot brick a device.
* Confirmed against the original muxfs source that meta and assign were always
  written as an ordered pair, so the cross-check is compatible with clean
  legacy arrays; an interrupted legacy array is *detected*, not misreported
  as corruption.
* Made `emuxfs_desc_chk_dir_content` and the checksum table file-local, and
  made `emuxfs.h`/`ds.h` self-contained.
* Strengthened the read-only `audit` test to compare `muxfs.conf`, `state.db`,
  `meta.db` and `assign.db` before and after, and the crash suite to assert
  that `sync` leaves a clean `state.db`, including after a planted
  interrupted-operation marker.
* Added boundary tests: maximal `seq`, maximal counters, and out-of-range
  `ino`/`eno` indices.
* Documented: the exact commit order and crash windows, `working` as a
  recovery marker, the difference between readback (consistency) and
  durability, the v1 integrity guarantee, logical object identity, inode/eno
  limits, and a proposed format version 2 (not implemented).
* **One-command validation.**  `make stability` runs the strict-warning
  build, the unit tests, the FUSE integration suite, the parallel-load stress
  test, the fault-injection suite and a bounded libFuzzer run over the
  `muxfs.conf` parser; CI runs the same sequence on OpenBSD 7.9 amd64 and
  arm64.
* **`tests/integration/parallel.pl`.**  A new stress test runs 32 concurrent
  client processes (mkdir/create/write/read/rename/unlink/rmdir on private
  names) against a mounted array, then requires a clean `audit` and
  byte-for-byte identical mirrors, with a bounded wait that kills and reports
  a stuck worker.
* **FUSE-time crash recovery is now automated.**  The crash suite interrupts a
  create, an update and a delete at `*/after_meta`, recovers with `sync`, and
  requires `audit` to be clean and the uncommitted operation to be rolled back.
  Previously those points were only available for manual investigation.
* The FUSE integration suite now also performs five repeated mount/unmount
  cycles and checks that a second mount of a mounted array is refused, that a
  repair reproduces the source timestamps, and that mode changes and directory
  renames are mirrored; the fuzzer runs for a bounded number of iterations in
  CI (skipped, not failed, when the toolchain has no libFuzzer).

## 1.3-enhanced (2026-09-22)

Fourth phase: formalise persistence and recovery.

* **`state.db` validation.**  A record with structurally impossible values
  (`mounted`/`working`/`restoring`/`degraded` greater than 1, or `working` and
  `restoring` both set) is refused with a distinct diagnostic instead of being
  treated as merely dirty.  A dirty but valid record remains readable so that
  recovery is possible.
* **Sequence number exhaustion fails closed.**  `seq` no longer wraps at
  `UINT64_MAX`; the device is marked degraded and requires administrative
  intervention.  This removes the epoch protocol and the last in-place
  rewrite of `muxfs.conf`, so that file is now immutable after `format`.
* **meta.db / assign.db cross-check.**  Every validation verifies the
  `eno -> ino` mapping, and `audit`/`heal` additionally scan `assign.db` in
  O(n).  Orphaned or mismatched mappings are reported; `heal` refuses rather
  than guessing.
* **`audit` is read-only.**  Devices are opened read-only for `audit`; it no
  longer sets the `mounted` flag or modifies `state.db`.  `heal` is the
  explicitly destructive command.
* **Restore reproduces timestamps.**  Repair copies the source `atime`/`mtime`
  to the restored node.  Timestamps remain outside the checksum (see below).
* **Removed the unity build.**  It is no longer needed: `make faultbuild`
  compiles the same modules with `-DEMUXFS_FAULT_INJECTION`.
* Added unit tests for `state.db` validation and the meta/assign cross-check,
  and an integration test that `audit` does not modify `state.db`.
* Documentation: exact `state.db` byte layout and invariants, device state
  machine, meta/assign invariant and orphan policy, audit read-only semantics,
  sequence-exhaustion policy.

## 1.2-enhanced (2026-09-22)

Third phase: residual integrity and recovery risks.

* **Hard links are now an enforced policy.**  They are refused on creation
  and on deletion of one name, detected and rejected by `audit`/`heal`, and
  never restored as independent files.  They remain unsupported; the point is
  that emuxfs no longer treats two names for one inode as unrelated objects.
* **Automatic recovery refuses coherent divergence.**  When two or more
  devices hold internally valid but different copies, nothing is overwritten,
  the device is marked degraded and `heal` fails, so an operator must resolve
  the ambiguity explicitly with `emuxfs sync`.
* `statfs` now reports the **minimum** across devices for inode counts and
  `f_namemax` as well as for capacity.  A mirror is not striping, so
  capacities are never summed.
* Documentation corrected: a dirty array cannot be mounted at all; `-f` only
  selects foreground operation and is not a force-mount of an inconsistent
  array.
* Added a unit test for the hard-link predicate and a headless test for
  coherent divergence, plus integration checks that hard links are refused
  through FUSE and detected by `audit`.
* Documented timestamp coverage, direct-modification detectability and statfs
  semantics.

## 1.1-enhanced (2026-09-22)

Second, independent audit and modernisation phase.

* **FUSE backend: OpenBSD's native implementation.**  emuxfs keeps using the
  `libfuse` shipped in the OpenBSD base system (FUSE 2.6 high-level API) via
  `fuse_setup`/`fuse_loop`/`fuse_destroy`.  No external FUSE dependency is
  introduced; the event loop is single-threaded.
* **C17 and Clang only.**  The build uses `clang` and `-std=c17`, with the
  same warning policy as openbar, openutils and wip-openbsd-src.  `make check`
  adds the full strict set (`-Wshadow -Wformat=2 -Wundef
  -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion`)
  and `-Werror`; no `-Wno-*` flags are used.
* **Restored the historical persistent names.**  `.muxfs` and `muxfs.conf`
  are kept so arrays created by muxfs remain usable; only the program-facing
  names are `emuxfs`.  The on-disk format is unchanged.
* Configuration parser hardening: duplicate keys are rejected; CRLF line
  endings are tolerated; trailing garbage and out-of-range values are
  rejected.
* Persistent offset guards now use `INT64_MAX` because `off_t` is signed.
* Added `_Static_assert` checks for the serialised record sizes and the
  metadata buffer.
* Removed `SHA512SUMS`: it hashed the repository's own files and was
  regenerated by the same change, so it gave no independent guarantee.
* Documented the OpenBSD kernel FUSE protocol (7.19), the supported opcode
  subset, and the ino/eno identity analysis.

## 1.0-enhanced (2026-09-22)

* Renamed the project to **The Enhanced Multiplexed File System** (`emuxfs`):
  the executable is `emuxfs`, the manual page `emuxfs(1)`, the on-disk
  directory `.muxfs` and the internal symbol prefix `emuxfs_`.
* Fixed a critical version parser defect that made any array whose revision
  was not zero impossible to mount; both the legacy (`0.5current`) and the new
  (`1.0-current`) version spellings are now accepted.
* Added an explicit on-disk format version (`format_version`) to
  `muxfs.conf`.  Arrays without it are treated as version 1 (the original
  layout); other versions are rejected with a clear message rather than
  mounted.
* Fixed a heap overflow and use-after-free in the restore queue.
* Fixed an uninitialised `emuxfs_dev_conf_checklist` in the configuration
  parser.
* Fixed a formatting bug that suppressed the "directory is not empty"
  diagnostic.
* Fixed file descriptor leaks, a double close and an uninitialised descriptor
  in the unmount and restore paths.
* Replaced the opaque, misaligned checksum type (generated by `gen.c`) with a
  concrete, correctly aligned type and removed the associated type punning.
* Added exact `pread(2)`/`pwrite(2)` helpers (EINTR- and short-transfer-safe)
  and use them for all persistent metadata.
* Added directory `fsync(2)` after create, rename, unlink and restore; added
  mode restoration and `O_NOFOLLOW` to regular-file restore.
* Removed the default unity build; the incremental build is now the default
  and each `.c` is a normal translation unit.  Removed
  `-Wno-unused-function`.
* Added OpenBSD `pledge(2)`/`unveil(2)` sandboxing derived per sub-command.
* Added optional, compile-time-gated fault injection for crash-consistency
  testing (never enabled in normal builds).
* Added a headless unit-test binary, Perl integration/FUSE and
  crash-consistency suites (using Perl string handling), and a port of the
  legacy end-to-end suite to Perl.  Shell code targets ksh, not POSIX sh.
  Also added an optional libFuzzer target.
* Added GitHub Actions CI using `vmactions/openbsd-vm`, targeting OpenBSD 7.9
  amd64 only.
* Added `ARCHITECTURE.md`, `ON_DISK_FORMAT.md`, `RECOVERY.md`, `SECURITY.md`,
  `TESTING.md`, `COMPATIBILITY.md` and `NOTICE.md`.

## 0.5-current (2022-08-08)

* Added a sequential write buffer.
* Bug fixes.

## 0.4-current (2022-07-28)

* Added tests for audit, heal, and sync.
* Bug fixes.

## 0.3-current (2022-07-14)

* Replaced the multiple binaries with a single binary with sub-commands.
* Turned the separate format and check tools into `format` and `heal`
  sub-commands, and added `audit`.
* Added the `sync` command to allow for drive replacement.
* Replaced the array of UUIDs in the conf with a single UUID for the
  whole array.
* Added a `restoring` field to the `state.db` file.
* Added manual page.
* Updated `README.md` and `GLOSSARY.md`.
* Added checking of sequence numbers before mount, audit, and heal.
* Replaced first-come-first-serve `statfs(2)` with aggregated `statfs(2)`.
* Bug fixes.

## 0.2-current (2022-06-28)

* Optimization for large files.
* Added the check tool.
* Removed the redundant `type` from the metadata checksum.
* Added the `size` to the metadata checksum for regular files.
* Removed `sign` from the configuration file.
* Updated `README.md` and `GLOSSARY.md`.
* Bug fixes.

## 0.1-current (2022-06-22)

* Test suite.
* Access from other users.
* Dynamic stack allocator.
* Bug fixes.

## 0.0-current (2022-06-10)

* Initial publication.
