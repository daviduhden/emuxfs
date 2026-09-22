# Testing emuxfs

All tests create their own temporary sandboxes and refuse to touch anything
outside them.  The integration and crash suites require root and a working
`/dev/fuse0`; they skip and exit 0 if those are unavailable.

## Test tooling

The complex test suites are written in Perl (using Perl's string handling) and
run with the base-system `perl(1)`.  Any remaining shell code targets **ksh**,
not POSIX `sh`; the Makefile executes recipes with `/bin/ksh`.

## Build policy (Clang, C17, strict warnings)

The supported toolchain is `clang` with `-std=c17`.  The default warning set is
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
mkdir, symlink, write, read, truncate, rename, unlink, rmdir, chmod,
boundary sizes (0, 1, 4095, 4096, 4097, 8191, 8192, 8193, 65537), a deep
tree, unusual but valid names, unmount/remount, self-healing from content
corruption, a deleted node and a corrupt symlink, `audit`, `heal`, `sync` to
a replacement device, the refusal of hard links (both through the filesystem
and as a direct modification detected by `audit`), the read-only property of
`audit` (`muxfs.conf`, `state.db`, `meta.db` and `assign.db` are
byte-identical before and after), and `version`.

A second Perl suite, `test.pl`, is the end-to-end suite ported from the
original shell tests.  Unlike the integration suite it uses the dedicated
scratch paths from `test.conf`:

```
cp test.conf.dist test.conf   # edit the paths first
make legacytest               # or: perl test.pl
```

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
| `update/after_meta`   | after metadata write and fsync                      |
| `delete/after_unlink` | after unlink, before clearing metadata              |
| `delete/after_meta`   | after clearing metadata, before the assign entry    |
| `restore/before`      | before a restoration begins                         |
| `restore/after_meta`  | after restoring metadata, before its assign entry   |
| `restore/after`       | after a restoration has been verified               |

The `*/after_meta` points are specifically designed to leave metadata written
but the assign entry missing, which the meta/assign cross-check must then
detect.

`crash.pl` corrupts a device, interrupts `heal`/`sync` at `restore/before`
and `restore/after`, and then checks that an uninterrupted run converges, that
`audit` is clean, and that the recovered destination's `state.db` is fully
clean (`working = restoring = degraded = mounted = 0`).  It also plants a
`working = 1` record — the state a power loss leaves after an interrupted
operation — and requires `sync` to clear it, which is the documented
post-power-loss recovery path.  The FUSE-time points are available for manual
investigation (mount, run an operation with the variable set, then recover).
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

## Fuzzing (optional, not run by CI)

```
make fuzz-conf
./tests/fuzz/fuzz_conf
```

`tests/fuzz/fuzz_conf.c` is a libFuzzer entry point for the `muxfs.conf`
parser.  The build target uses `-fsanitize=fuzzer,address`; for full
instrumentation rebuild the tree with those sanitizer flags in `CFLAGS`.
Fuzzing is never enabled by the default build.

## Continuous integration

`.github/workflows/ci.yml` runs on `ubuntu-latest` but executes inside a real
OpenBSD virtual machine through `vmactions/openbsd-vm@v1`.  The target is
**OpenBSD 7.9**, on two architectures: **amd64** and **arm64**
(`arch: aarch64`).  Both run the same full sequence: build (clang, C17),
strict warnings (`make check`, `-Werror`), unit tests, FUSE integration tests,
fault-injection tests, install, manual page (`mandoc -T lint`), and a final
clean rebuild.  The FUSE integration step is skipped
(not failed) where `/dev/fuse0` is unavailable.  No external FUSE package is
needed because the FUSE implementation is part of the OpenBSD base system.

Only the OpenBSD 7.9 vmactions image is used; 8.0 is not prepared.

## Local static analysis only

This repository's changes were developed under a no-compilation constraint:
no `make`, compiler, mount, `fsck`, sanitizer or fuzzer was run while making
them.  The first real verification is the CI run.  Reviewers should expect
the first CI run to be the point at which compiler diagnostics are first
seen.
