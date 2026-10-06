# Change log

## C23 intensive pass (2026-10-06)

An explicitly aggressive, low-conservatism pass that applies every C23 feature
that could be applied to this codebase, even where the benefit is marginal.
The persistent format (`format_version` 1), the on-disk record layouts and the
externally visible behaviour are deliberately unchanged; the C23 features that
cannot be applied without breaking those are listed as non-applicable rather
than forced.

Applied:

* `constexpr` for the integer constants that were preprocessor macros in
  `emuxfs.h` and `fault.h` (`EMUXFS_EINT`/`EFS`/`ECHK`, the size constants, the
  format version, the `EMUXFS_CONF_*` results and the `EMUXFS_DT_*` values).
  The string constants stay macros: C23 `constexpr` pointer initialisers must
  be null, so `constexpr const char *` cannot hold `".muxfs"`.
* Digit separators (`4'096`, `1'048'576`) where they were already written in
  decimal.
* Fixed underlying types for every enumeration (`: int`, or `: unsigned` for
  the flag enums), matching the default representation so no ABI or format
  changes.
* Native `bool`, `true` and `false` for the boolean-valued predicates,
  out-parameters, flags and locals: `emuxfs_dev_state_is_valid`,
  `emuxfs_dev_is_mounted`, `emuxfs_is_hardlink`, `emuxfs_assign_validate`,
  `emuxfs_state_is_restore_only`, `emuxfs_state_wrbuf_is_set`,
  `emuxfs_existsat`/`emuxfs_dir_is_empty`/`emuxfs_lfile_exists` out-parameters,
  `emuxfs_readback`'s `shallow`, `emuxfs_dev_get`/`emuxfs_dev_open`/
  `emuxfs_dev_mount`/`emuxfs_init`'s `force`/`readonly`, the `has_*`/`is_*`
  locals, and the internal boolean struct fields.  Error-code returns stay
  `int`.
* `auto` type inference in two places where a pointer is assigned immediately
  (`emuxfs_restore_queue_reserve`, `emuxfs_eids_set`).
* `<stdckdint.h>` `ckd_add()` for the size sums that could overflow:
  `emuxfs_read_inner`'s range end, `emuxfs_write_inner`'s new end and
  `emuxfs_op_update`'s modification end.
* `[[nodiscard]]` extended to the remaining integrity/availability results
  (`emuxfs_conf_parse`/`write`, `emuxfs_meta_write`/`_fd`,
  `emuxfs_assign_write`/`_fd`, the lfile helpers,
  `emuxfs_lfile_ancestors_recompute`, `emuxfs_pread_exact`,
  `emuxfs_pwrite_exact`, `emuxfs_fsync_parent`).
* `compat/stdckdint.h`, modeled on the openutils shim: the OpenBSD 7.9 libc
  does not ship `<stdckdint.h>`, so `compat/` is searched before the system
  directories (`CPPFLAGS += -Icompat`), the shim forwards to a real header via
  `#include_next` when one exists, and otherwise provides `ckd_add`,
  `ckd_sub` and `ckd_mul` with the compiler overflow builtins.
* The size constants became `size_t`, so the two signed/unsigned comparisons
  they participated in (`prewr_st.st_size > EMUXFS_BLOCK_SIZE` and the UUID
  loop in the unit test) were made explicit.

Evaluated and not applied, with the reason:

* `constexpr` string objects: C23 only allows null pointer constants.
* `alignas`: the allocator already aligns explicitly to `EMUXFS_MEM_ALIGN` and
  the persistent records are laid out by byte offset; adding alignment would
  only risk changing `sizeof` and the format.
* `thread_local`: the daemon is single-threaded by design (`fuse_loop`); making
  the global state thread-local would change the storage model, not the code.
* `typeof`/`typeof_unqual`: no expression where the type is not trivially
  spelled, and the `typeof(T)` form is ambiguous with a cast.
* `_BitInt`: no integer widths outside those `<stdint.h>` already provides.
* `#embed`: no binary blobs are embedded.
* `<stdbit.h>`: no manual bit operations to replace.
* `u8` literals: in C23 these are plain `char` arrays, so they add nothing.
* `#elifdef`/`#elifndef`/`#warning`: there is only one `#ifdef`.
* `__VA_OPT__`: there is no `, ##__VA_ARGS__` hack.
* `[[noreturn]]`: no function is provably non-returning.

## Memory-safety audit follow-up (2026-10-06)

A second pass over the C23 baseline and the memory model.  The standard was
already adopted (see the C23 migration below), so this pass changes only code
with a demonstrated or well-founded defect and keeps the ABI, the on-disk
format and the observable behaviour.

* **Partial reads copied whole blocks.**  `emuxfs_read_inner` checksums the
  large-file tree by whole 4096-byte blocks, but it also copied the whole
  block to the caller's buffer.  For a read whose end did not fall on a block
  boundary (`offset + size < file size`), the first and last blocks of the
  range extended past `byte_end`, so the daemon copied more bytes than
  requested — past the end of the buffer FUSE supplied — and reported a
  length larger than the request.  The copy now intersects each block with
  `[byte_begin, byte_end)`.  Added a FUSE regression test that reads prefixes
  and unaligned ranges of a large file.
* **A zero-length read aborted the daemon.**  A zero-length read of a large
  file whose offset was block-aligned computed a zero-sized checksum-tree
  range and called `mmap(2)` with length 0, which fails; the failure was
  classified as an unrecoverable internal error.  A zero-length read now
  returns zero bytes before the tree is mapped, and a zero-length write is a
  no-op instead of requiring a buffer.
* **`truncate` could not grow a small file past one block.**  Case 2 of
  `emuxfs_truncate_inner` read a full block from a file shorter than a block
  and rejected the short read (EOF), so growing a file that fits in one block
  to more than one block failed with `EIO`.  It now reads only the bytes that
  exist and zero-fills the rest of the block, which is the sparse hole the
  checksum tree must describe.  Added a FUSE regression test.
* **`emuxfs_truncate_inner` leaked the checksum-tree mapping and descriptor
  on error.**  Its cleanup label closed only the user file descriptor; every
  error after the lfile was opened (a short read, an checksum mismatch, an
  ancestor recomputation failure) left the `mmap` and the `lfd` behind.
  Repeated failures could exhaust descriptors and address space.  The cleanup
  now mirrors `emuxfs_write_inner` and releases the mapping and `lfd`.
* **The sequential write buffer mishandled a partial append.**
  `emuxfs_buffered_write` re-buffered the bytes that did not fit from the
  start of the request instead of from `buf + wrsz`, duplicating data, and
  returned only the re-buffered count instead of the number of bytes
  accepted.  A write larger than the 1 MiB buffer could therefore corrupt the
  file and report a short write.
* **`mmap` of the checksum tree used `PROT_WRITE` without `PROT_READ`.**
  OpenBSD's `mmap(2)` documents a `PROT_WRITE`-only mapping (with an
  `O_WRONLY` descriptor) as unusable; the four write-side mappings now use
  `PROT_READ | PROT_WRITE` and `O_RDWR`, matching the lfile helpers.
* **`emuxfs_ds*` compared unrelated pointers with `<`.**  The dynamic stack
  ranged over separately allocated nodes and used relational comparison on
  the resulting pointers, which is undefined for pointers into different
  objects.  The comparisons are now made on `uintptr_t` values, and the
  remaining space checks use integer sizes instead of forming an
  out-of-bounds pointer.  A dead `emuxfs_ds_entcount` counter was removed
  (it was written but never read, and newer compilers warn for it).  The
  allocator was exercised under AddressSanitizer and UndefinedBehaviorSanitizer
  for both `ds.c` and the `ds_malloc.c` fallback.

## Security-model audit (2026-09-24)

Focused hardening of the documented security model; no new external
dependencies and no change to the persistent format (`format_version` stays
1).

* **Root's supplementary groups leaked into impersonated operations.**  FUSE
  callbacks switched only the effective uid/gid to the requesting user, so an
  operation attributed to that user still carried root's supplementary groups
  and could pass a group permission check the user's own credentials would
  fail.  `emuxfs_eids_set` and `emuxfs_eids_wrctx_set` now call
  `setgroups(0, nullptr)` before lowering the effective uid.  The daemon needs
  no supplementary groups of its own (its privileged work runs with euid 0),
  and clearing them fails closed for group access.  No privilege separation
  was introduced: the FUSE event loop is already single-threaded
  (`fuse_loop`, null multithreaded flag), so credentials cannot race between
  callbacks.
* **`O_TRUNC` could bypass metadata.**  `emuxfs_open` passed the caller's
  `O_TRUNC` straight to `openat(2)`, which truncates the backing file without
  updating `meta.db` and the content checksum.  An open with `O_TRUNC` now
  runs the normal truncate operation first and the flag is cleared, so the
  truncation is always recorded.  This removes any dependence on how the
  kernel delivers `O_TRUNC`.
* **The private directory is now a pinned descriptor.**  `.muxfs` is opened
  once with `O_RDONLY|O_DIRECTORY|O_NOFOLLOW` and every private object
  (`muxfs.conf`, `state.db`, `meta.db`, `assign.db`, `lfile`) is opened
  relative to that descriptor.  The rename temporary is addressed the same
  way (`renameat` against the private directory descriptor instead of the
  literal path `.muxfs/rename.tmp`).  A symlink planted at `.muxfs` can no
  longer redirect a metadata read, write or rename.
* **Self-referential mount topology is refused.**  `mount` canonicalises the
  mount point and every array directory and exits with an explicit diagnostic
  if the mount point and a mirror directory contain one another; otherwise the
  serving process could traverse the mount it is serving and deadlock.

## C23 migration (2026-09-24)

The required standard is now **C23** (`-std=c23`); the OpenBSD 7.9 base clang
(19.1.7) accepts it.  The migration is deliberately conservative and only
adopts features that make an existing intention clearer or safer:

* `static_assert` and `alignof` are now used as keywords instead of
  `_Static_assert`/`_Alignof`, with no `<assert.h>`/`<stdalign.h>` needed.
* `nullptr` replaces `NULL`.  Every use was a null pointer constant (the
  code already wrote integer zero as `0`), so the change is mechanical but
  type-safe; the one diagnostic string that spelled "NULL" was updated too.
* `[[fallthrough]]` marks the single intentional `switch` fallthrough
  (`emuxfs_truncate_inner`).
* `[[maybe_unused]]` and unnamed parameters replace the `(void)param;`
  workaround for genuinely unused parameters (the trivial FUSE callbacks and
  the few mixed helpers).
* `[[nodiscard]]` marks the integrity/availability functions whose result
  must not be ignored: `emuxfs_dev_get`, `emuxfs_meta_read`,
  `emuxfs_assign_read` and `emuxfs_readback`.

Evaluated and deliberately not applied, because C23 does not improve them or
the toolchain/library support cannot be demonstrated statically: native
`bool` (the project uses `int` predicates and flags; a mass conversion would
change internal layouts without fixing a defect), `constexpr` (the numeric
constants are macros used as array bounds and in `static_assert`), fixed
underlying enum types, `_BitInt`, `#embed`, `#elifdef`/`#warning`,
`<stdckdint.h>`/`<stdbit.h>` (OpenBSD libc availability not verified),
`typeof`, `[[noreturn]]` (no function is provably non-returning),
`[[unsequenced]]`/`[[reproducible]]` (contract and support not demonstrable),
`u8` literals and digit separators.  The `__attribute__((format))` extension
and the `#pragma clang diagnostic` around `vsnprintf` remain: C23 has no
equivalent for either.

## Memory-safety audit (2026-09-24)

* **Write starting inside a file block.**  `emuxfs_write_inner` copied the
  old block contents up to the previous end of file before applying the new
  data, so a write whose offset was not block-aligned left the bytes before
  the offset unchanged and placed the new bytes at the wrong position (and,
  when the write ended inside the same block, the length computation
  underflowed).  The copy now stops at the write offset, which is inert for
  block-aligned writes.  Found by a new integration test that writes in place
  at offset 4000 of a 10000-byte file.
* **Partial lfile tree entries.**  The ancestor recomputation and the
  ancestor verification in the large-file checksum tree hashed
  `min(i + LEVEL_FACTOR, iend)` leaves, where `iend` is the end of the
  modified block range, so a partial range omitted the unmodified leaves that
  share a tree entry: the content was correct but the root checksum was not,
  and `audit`/`heal` then reported the file as corrupt (found by the same
  test).  Full-range recomputations are unaffected.
* **Bounds on fixed-size path buffers.**  `emuxfs_removeat`,
  `emuxfs_parent_readback` and `emuxfs_state_restore_push_back` now reject
  paths that do not fit in the `PATH_MAX` buffer they are copied into
  (`emuxfs_state_restore_pop_front` copies the queued path into the caller's
  buffer).
* **`emuxfs_lfile_readback` parent index.**  The index of the parent entry
  was read only from the level loop, so it was indeterminate for a file that
  fits in one block (no internal level); it is now initialised to the root
  entry at index 0.
* **`open`/`opendir` no longer pass `O_CREAT`.**  Creation is performed by
  `mknod`/`mkdir`/`symlink`; forwarding `O_CREAT` would create an untracked
  node and is undefined behaviour because FUSE supplies no mode argument.
* **Directory-entry comparison was one byte short.**  Three loops matched
  `..` with `strncmp("..", name, 1)`, which also skipped any two-character
  name beginning with `.` (for example a file named `.a`); most visibly,
  `emuxfs_dir_is_empty` could report a non-empty directory as empty, and
  `emuxfs format`/`sync` could then overwrite it.
* **`ds_malloc` fallback grew allocations incorrectly.**  The optional
  `EMUXFS_DS_MALLOC=1` build defined `emuxfs_dsgrow()` as `realloc(p, n)`
  (setting the size) while `ds.h` requires *growing* by `n` bytes, so
  `emuxfs_pushdir` would write past the allocation.  The fallback now tracks
  each allocation's size and grows by the requested number of bytes.

## Free directory entries (2026-09-24)

* **`emuxfs_pushdir` now skips free directory entries.**  `getdents(2)` on
  OpenBSD returns the space that a deletion leaves in the first entry of a
  directory block, keeping the old name with `d_fileno == 0` (`ufs_readdir`
  does not filter it).  emuxfs treated those entries as directory members, so
  under a workload that creates and removes many names a directory
  recomputation would `fstat(2)` a name that no longer existed, fail, and
  mark the device degraded, after which every operation failed with `EIO`.
  Found by the 32-worker parallel-load stress test.

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
