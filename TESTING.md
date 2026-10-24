# Testing emuxfs

The unit, integration, parallel-load and crash suites create their own
temporary sandboxes and refuse to touch anything outside them.  The legacy
`test.pl` suite is the exception: it uses the dedicated paths in `test.conf`
and recursively deletes them, so those paths must be unused scratch
directories.  The integration, parallel-load and crash suites require root and
a working `/dev/fuse0`; they skip and exit 0 if those are unavailable.

## Test tooling

The complex test suites are written in Perl (using Perl's string handling) and
run with the base-system `perl(1)`.  Perl scripts use `strict` and `warnings`.
Shell helpers are mixed: the stdckdint compile-fail test is POSIX `sh`, while
the legacy permission tests invoke `/bin/ksh`; do not assume a single shell
for every script.

## Build policy (Clang, C23, strict warnings)

The supported toolchain is `clang` with `-std=c23`.  The default warning set is
`-Wall -Wextra -Wpedantic`; `make check` rebuilds with the full strict set used
by the other projects (`-Wshadow -Wformat=2 -Wundef -Wstrict-prototypes
-Wmissing-prototypes -Wconversion -Wsign-conversion`) and `-Werror`.  No
`-Wno-*` flags are used.  The FUSE implementation is the one in the OpenBSD
base system (FUSE 2.6 high-level API); no external FUSE package is required.

## Unit tests (no FUSE, no root required)

```
make unittest
./tests/unit/test_core
```

`tests/unit/test_core.c` links everything except the FUSE frontend and the
program entry point.  It covers:

* CRC32/MD5/SHA-1 known-answer vectors and stored byte order;
* metadata entry size and alignment for each algorithm;
* metadata checksum determinism and sensitivity to owner, mode, size and
  content checksum;
* configuration write/parse round-trip, including UUIDs and `seq_zero_time`;
* legacy (`0.5current`, no `format_version`) parsing, and rejection of an
  unknown `format_version` and of malformed input;
* path sanitisation (root, `.muxfs`, `.`/`..`, empty components, trailing
  slash);
* little-endian encoding of `state.db`, `meta.db` and `assign.db`;
* the restore queue: 200 oversized entries, forcing growth and compaction,
  then verifying order and content.  This is the regression test for the
  original queue's heap overflow and use-after-free;
* the hard-link predicate: a single regular file is not a hard link, a
  two-name regular file is, and a symbolic link never is;
* when running as root: `emuxfs format`, device mount/unmount, assignment and
  metadata reads, and large-file checksum-tree create/resize/delete;
* `state.db` validation: a clean record, individually valid non-clean states,
  structurally impossible records (`mounted = 2`, `working = 2`, `working` and
  `restoring` together, etc.), the maximal `seq`, and maximal counters;
* when running as root: the meta/assign cross-check.  A node is fabricated,
  its `assign.db` mapping is corrupted, cleared and repaired, and both the
  whole-database check and per-node readback are required to detect and then
  recover.  Out-of-range `ino`/`eno` indices (`UINT64_MAX`, beyond the file)
  must fail cleanly rather than overflow;
* when running as root: **coherent divergence**.  Three devices are formatted
  and given three individually valid but different copies of one path; the
  copy on one device is then damaged and a restore is requested.  The test
  checks that the ambiguity counter becomes 1 and that the damaged copy is
  left untouched (no arbitrary repair).

## Integration and FUSE tests (root, `/dev/fuse0`)

```
make
EMUXFS="$PWD/emuxfs" perl tests/integration/integration.pl
# or
make integration
```

The suite formats two fresh devices, mounts them and then exercises: create,
mkdir, symlink, write, read, truncate, rename (file and directory), unlink,
rmdir, chmod, boundary sizes (0, 1, 4095, 4096, 4097, 8191, 8192, 8193,
65537), a deep
tree, unusual but valid names, the refusal of a second mount of an
already-mounted array, unmount/remount, self-healing from content
corruption, a deleted node and a corrupt symlink, `audit`, `heal`, `sync` to
a replacement device, the refusal of hard links (both through the filesystem
and as a direct modification detected by `audit`), the read-only property of
`audit` (`muxfs.conf`, `state.db`, `meta.db` and `assign.db` are
byte-identical before and after), the reproduction of source timestamps by a
repair, renaming over an existing name, the 512-byte unit of `st_blocks`,
five repeated mount/unmount cycles, and `version`.

A second Perl suite, `test.pl`, is the end-to-end suite ported from the
original shell tests.  Unlike the integration suite it uses the dedicated
scratch paths from `test.conf` and deletes them recursively.  It checks for
root and `/dev/fuse0` and skips when either is missing, but those paths must
still be disposable:

```
cp test.conf.dist test.conf   # edit the paths first
make legacytest               # or: perl test.pl
```

## Parallel-load stress test (root, `/dev/fuse0`)

```
make paralleltest        # runs tests/integration/parallel.pl
```

`tests/integration/parallel.pl` formats and mounts a two-device array and then
runs **32 concurrent client processes** (override with `PARALLEL_WORKERS`),
each doing `PARALLEL_ITERS` (default 20) rounds of
mkdir/create/write/read/rename/read/unlink/rmdir on names of its own, and each
leaving one deterministic marker file.  The FUSE daemon is single-threaded, so
the requests are serialized inside it; the test exercises the concurrent
arrival path through the kernel and, more importantly, that the committed
state is correct afterwards.  When every worker has exited it unmounts, runs
`audit` (which must be clean), checks every marker file on the mount and on
both mirrors, and compares the two device trees byte-for-byte (modes, sizes,
contents and symlink targets), excluding `.muxfs`.  A worker that does not
finish within `PARALLEL_TIMEOUT` seconds (default 900) is killed and reported.

## Crash consistency (`EMUXFS_FAULT_INJECTION`)

```
make faultbuild          # builds ./emuxfs-fault
make faulttest           # runs tests/integration/crash.pl
```

With `-DEMUXFS_FAULT_INJECTION`, `emuxfs_fault_point(name)` terminates the
process when `EMUXFS_FAULT_POINT=name` is set in the environment
(`EMUXFS_FAULT_ACTION=abort` aborts instead; the default is `_exit(70)`).
Fault points currently exist at:

| name                  | location                                            |
|-----------------------|-----------------------------------------------------|
| `create/before_meta`  | before writing metadata for a new node              |
| `create/after_meta`   | after metadata, before the assign entry             |
| `update/before_meta`  | after content write, before metadata write          |
| `update/after_meta`   | after content and metadata are both fsynced         |
| `delete/after_unlink` | after unlink, before clearing metadata              |
| `delete/after_meta`   | after clearing metadata, before the assign entry    |
| `restore/before`      | before a restoration begins                         |
| `restore/after_meta`  | after restoring metadata, before its assign entry   |
| `restore/after`       | after a restoration has been verified               |
| `rename/after_vacate` | after the source is moved to `.muxfs/rename.tmp`    |
| `rename/after_dest`   | after a replaced destination is moved to `.muxfs/rename.dest.tmp` |
| `rename/after`        | after the destination is read back and verified     |

For `create/after_meta`, the metadata entry is written but the assign entry is
missing, which the meta/assign cross-check must detect.  `delete/after_meta`
leaves a stale assignment for a cleared metadata entry; `update/after_meta`
does not write an assignment at all because the inode does not change.  All
three are still protected by the durable `working = 1` marker, so mount,
audit and heal refuse the device and `sync` is the recovery path.

`crash.pl` corrupts a device, interrupts `heal`/`sync` at `restore/before`
and `restore/after`, and then checks that an uninterrupted run converges, that
`audit` is clean, and that the recovered destination's `state.db` is fully
clean (`working = restoring = degraded = mounted = 0`).  It also plants a
`working = 1` record — the state a power loss leaves after an interrupted
operation — and requires `sync` to clear it, which is the documented
post-power-loss recovery path.

The FUSE-time points are automated too: the suite mounts the fault build and
performs a create, an update and a delete that are each interrupted at
`*/after_meta`, plus renames interrupted at `rename/after_vacate` and
`rename/after`.  It requires `sync` to rebuild the interrupted device from the
intact mirror and `audit` to be clean.  Because the interrupted operation had
not committed, reverting it is the correct outcome: the created file is gone,
the updated file has its old content, the deleted file is back and a renamed
file keeps its original name.

Fault injection is compiled out of normal builds.  The fault points model a
process crash (`_exit`), not torn sectors, controller caches or storage
reordering; the suite is a crash-recovery suite, not a power-loss proof.

## Legacy compatibility

The meta/assign invariant is verified against the *original* muxfs source
rather than against the modern code: the original always wrote the metadata
entry and its assignment as an ordered pair (create, delete, restore, format)
or left the assignment untouched (ancestor metadata recomputation), so a clean
legacy array satisfies the cross-check.  The unit test fabricates records by
hand rather than only through the production write path, but it cannot import
a byte-for-byte historical array without running the original program; that
limitation is recorded here.  The on-disk layout itself is documented in
ON_DISK_FORMAT.md and is unchanged from the original apart from
`format_version` in `muxfs.conf`.

## Fuzzing

```
make fuzz-conf            # build the libFuzzer entry point
./tests/fuzz/fuzz_conf    # run it interactively
make fuzz-smoke           # bounded run, part of 'make stability'
```

`tests/fuzz/fuzz_conf.c` is a libFuzzer entry point for the `muxfs.conf`
parser.  `make fuzz-smoke` runs a bounded session (`FUZZ_RUNS`, 20000 by
default) and is part of `make stability` and of CI.  When the toolchain has
libFuzzer the entry point is run directly under `-fsanitize=fuzzer,address`.
OpenBSD's base clang has no libFuzzer, so there the bounded run uses the
standalone mutation driver `tests/fuzz/fuzz_main.c` (half documented-seed
mutations, half random bytes), built with AddressSanitizer when available and
without it otherwise.  The step only reports `SKIP` if neither can be built.
Fuzzing is never enabled by the default build.

## One-command validation

```
make stability
```

`make stability` is the core local sequence: strict-warning build
(`make check`), unit tests, the FUSE integration suite, the parallel-load
stress test, the fault-injection suite and the bounded fuzz run.  The FUSE
steps require root and `/dev/fuse0` and skip themselves when either is
missing.  It is not byte-for-byte the CI sequence: CI also installs, lints the
manual page, checks linkage, rebuilds with the fallback allocator and does a
final clean rebuild.  It also does not run the legacy suite.  This is the
evidence that the stability levels in README.md refer to; nothing should be
described as Tested or Hardened on the strength of static analysis alone.

## Continuous integration

`.github/workflows/ci.yml` runs on `ubuntu-latest` but executes inside a real
OpenBSD virtual machine through `vmactions/openbsd-vm@v1.4.9`.  The target is
**OpenBSD 8.0**, on two architectures: **amd64** and **arm64**
(`arch: aarch64`).  Both run the same sequence: build (clang, C23), strict
warnings (`make check`, `-Werror`), unit tests (`make test` and
`test_core`, including the stdckdint conformance and compile-fail checks),
FUSE integration tests, the parallel-load stress test,
fault-injection tests, bounded parser fuzzing, install, manual page
(`mandoc -T lint`), linkage, a fallback-allocator rebuild and unit test, and a
final clean rebuild.  The FUSE steps are skipped (not failed) where
`/dev/fuse0` is unavailable.  No external FUSE package is needed because the
FUSE implementation is part of the OpenBSD base system.

CI does not run `make stability`, the legacy suite or `yamllint`; those are
local/manual gates.

## Verification workflow

Changes are developed and reviewed statically first, then verified by the CI
run above: the compiler, the unit tests, the FUSE suites and the fault suite
are the point at which behaviour is first observed.  A green run on both
architectures is the minimum bar for merging; a claim of stability additionally
requires `make stability` to pass locally.
