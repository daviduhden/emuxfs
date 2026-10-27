---
name: openbsd-sandbox-and-stress-tests
description: Maintain emuxfs OpenBSD pledge and unveil sandboxing and run its integration, parallel stress and legacy suites safely. Use when adding syscalls or subcommands, editing sandbox.c or sandbox.h, test scripts, CI or test safety rules.
---

# OpenBSD sandbox and stress tests

## Activation

Use this skill when a change affects:

- `sandbox.c`, `sandbox.h`, a `pledge(2)` promise string or `unveil(2)`
  policy;
- a new subcommand or a syscall reachable from an existing subcommand;
- `mount.c` lifecycle, canonical paths or mount topology;
- `tests/integration/integration.pl`, `parallel.pl`, `crash.pl`, `test.pl`,
  `test.conf.dist` or the shell helpers;
- `.github/workflows/ci.yml`, `Makefile` test targets or `.yamllint`;
- anything that runs, mounts or deletes test data.

## Sandbox policy

`sandbox.c` is the only file that calls `pledge(2)` or `unveil(2)`; the promise
strings live in `sandbox.h`.  Do not add a direct call elsewhere.

| command | promises |
|---------|----------|
| `version` | `stdio` |
| `format` | `stdio rpath wpath cpath fattr chown` |
| `audit` / `heal` | `stdio rpath wpath cpath fattr chown unix` |
| `sync` | `stdio rpath wpath cpath fattr chown unix` |
| `mount` | `stdio rpath wpath cpath fattr chown id unix` |

Promise coverage, as documented in `sandbox.h` and `SECURITY.md`:

- `stdio`: read/write/pread/pwrite, fsync, ftruncate, mmap, gettimeofday,
  getentropy, getdents, kqueue/kevent, sysconf, fstatvfs.
- `rpath`: openat for read, fstatat, fstatfs/fstatvfs, faccessat, readlinkat.
- `wpath`: openat for write, truncate.
- `cpath`: mkdirat, unlinkat, renameat, symlinkat.
- `fattr`: fchmodat, utimensat.
- `chown`: fchownat with an arbitrary owner/group (format, restore, chown).
- `id`: seteuid/setegid/setgroups for FUSE caller impersonation.
- `unix`: syslog(3) over an AF_UNIX socket.

`mount(2)` happens in `fuse_setup()` before pledge.  `unmount(2)` is never
called; the kernel detaches the filesystem when the process closes the FUSE
device on exit.

## Ordering

- `mount`: `fuse_setup()` (mounts and daemonizes) → unveil every device with
  `rwc` → `unveil(NULL, NULL)` → pledge → `fuse_loop()` → `fuse_destroy()` and
  `emuxfs_final()`.
- `format`, `audit`, `heal`, `sync`: unveil → lock → pledge → open devices.
- `version`: pledge and print.

A promise may be slightly broader than the minimum; the cost of an over-tight
promise in storage software is a killed process and possible loss of a
pending operation.  Every promise must still be tied to a concrete operation.

## Unveil and topology

- Each array directory is unveiled with `rwc`; nothing else is unveiled.
- `mount` canonicalises the mount point and every device with `realpath(3)`
  before configuring the sandbox.  `format`, `audit`, `heal` and `sync`
  unveil the paths as given.
- `mount` refuses a mount point that contains or is contained in a device,
  and refuses devices that contain one another.  `format` requires empty,
  non-overlapping directories.
- `.muxfs`, each private database and the `lfile` files are opened relative to
  a pinned descriptor with `O_NOFOLLOW`; long-lived descriptors use
  `O_CLOEXEC`.

## Privilege model

The daemon runs as root for chown/chmod and mirror access.  FUSE callbacks
switch effective uid/gid to the requesting user, drop root's supplementary
groups first, and restore credentials afterwards.  Callbacks are serialized by
the single-threaded event loop, so credentials cannot race.  No privilege
separation is implemented; do not add one casually.

## Test suites

| suite | command | requirements and behavior |
|-------|---------|---------------------------|
| unit | `make test`, `make unittest`, `./tests/unit/test_core` | no FUSE; device cases need root; temporary `mkdtemp` sandbox |
| integration | `make integration` | root and `/dev/fuse0`; private `emuxfs-test.XXXXXX` sandbox; skips with exit 0 if unavailable |
| parallel stress | `make paralleltest` | root and `/dev/fuse0`; 32 workers × 20 iterations by default |
| crash/fault | `make faulttest` | root and `/dev/fuse0`; needs `emuxfs` and `emuxfs-fault`; private sandbox |
| legacy | `make legacytest` / `perl test.pl` | `test.conf`; uses and recursively deletes the configured paths; checks root and `/dev/fuse0` but still needs disposable paths |
| fuzz | `make fuzz-smoke` | libFuzzer if available, otherwise standalone mutation driver under ASan |

Stress-test defaults and overrides: `PARALLEL_WORKERS` (32),
`PARALLEL_ITERS` (20), `PARALLEL_TIMEOUT` (900 seconds; CI sets 1800 on both
architectures).  Each worker creates a private tag, runs
mkdir/create/write/read/rename/read/unlink/rmdir, leaves a marker file, then
the parent unmounts, waits for clean `state.db`, runs `audit`, checks markers
on the mount and both mirrors, and compares the trees.  The comparison covers
content, mode, size and symlink target, excluding `.muxfs`; it does not compare
ownership or timestamps.

## Procedure for a new syscall or subcommand

1. List every syscall reachable after the pledge point, including libc and
   libfuse internals.
2. Map each to the narrowest promise.  Add it to the promise string for every
   command that needs it, not just the one you are testing.
3. Verify the ordering: mount must pledge after `fuse_setup`; other commands
   must unveil and pledge before opening devices.
4. Check whether the new path needs `id` (caller impersonation), `unix`
   (syslog), `chown` (arbitrary owner) or `fattr` (mode/times).
5. Update `sandbox.h`, `SECURITY.md` and the promise table in this skill.
6. Run in a disposable OpenBSD VM with root and `/dev/fuse0`; a killed
   process or `pledge` violation is a hard failure, not a skip.

## Procedure for a test change

1. Create a private temporary sandbox and prove containment before any
   destructive action.  Use `tempdir(... CLEANUP => 0)` plus a containment
   regex, or `mkdtemp` plus a prefix check in C.
2. Install an `END` cleanup and `INT`/`TERM` handlers.  Make cleanup
   idempotent and never run the parent's cleanup in a forked worker.
3. Prefer `fork`/`exec` over shell strings when a pid, exit status or path with
   spaces matters.  Reap the daemon.
4. Wait for readiness by reading the trace for `entering fuse_loop`; a timeout
   is a failure.  Bound every wait and unmount.
5. Poll `state.db` until `mounted`, `working` and `restoring` are zero before
   audit/heal/sync/mount.  Check `degraded` separately.
6. Distinguish skip (missing privilege or `/dev/fuse0`) from failure (missing
   binary, unsafe sandbox, timeout).  A skip is not a pass.
7. Remember that `audit` prints corrupt paths but exits 0 on detected
   corruption.  Check stdout, not only the exit status, when expecting a clean
   tree.

## CI

The real target is OpenBSD **8.0** on amd64 and arm64, through
`vmactions/openbsd-vm@v1.4.9`.  Each job runs: build, `make check`, `make
test` and `test_core`, integration (guarded by `/dev/fuse0`), parallel stress,
fault build and crash suite, bounded fuzzing, install, `mandoc -T lint`,
`ldd`, a fallback-allocator rebuild and unit test, and a final clean rebuild.
CI does not run `make stability`, the legacy suite or `yamllint`.

## Validation

- **Static:** `rg 'pledge|unveil'` must find only `sandbox.c`; check promise
  strings in `sandbox.h`; check every test sandbox is proven to be inside the
  temporary base before deletion.
- **Build:** `make check` on OpenBSD.
- **Tests:** `make test`, `make integration`, `make paralleltest`,
  `make faulttest` in a disposable VM.  FUSE steps without root or
  `/dev/fuse0` are unverified, not passing.
- **Manual page:** `mandoc -T lint emuxfs.1` (CI fails only on ERROR or
  WARNING).

## Common problems

- Removing a promise that a libc or libfuse path needs.  The process is killed
  mid-operation; fail closed and add the promise back.
- Pledging before `fuse_setup`, which needs `mount(2)`, or calling
  `unmount(2)` under pledge.
- Unveiling only the mount point or only some devices.  Every array directory
  must be unveiled with `rwc`.
- Treating `.access` as an enforced permission check.  The OpenBSD kernel does
  not deliver `FUSE_ACCESS`; the callback is installed for completeness.
- Running `heal` or `sync` on a live mount.  `sync` force-opens and can race
  the daemon.
- Using `test.pl` with live paths.  It recursively deletes every configured
  path; it checks root and `/dev/fuse0`, but that does not make the paths
  disposable.
- Checking only the exit status of `audit`.  It exits 0 while reporting corrupt
  paths.
- Assuming `make stability` is identical to CI.  It is the core sequence;
  installation, `mandoc`, linkage, allocator rebuild and final rebuild are
  CI-only.
- Copying the old 7.9 CI claim.  The workflow is the source of truth.
- Leaving a daemon or mount behind after a failed test.  Cleanup must unmount,
  terminate and reap, and re-prove the sandbox before `remove_tree`.


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
   scripts/check-agent-maintenance.pl --base <base-ref>` (or omit `--base` for
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

- [`SECURITY.md`](../../../SECURITY.md) — threat model, privilege model,
  pledge/unveil derivation and path handling.
- [`COMPATIBILITY.md`](../../../COMPATIBILITY.md) — OpenBSD release, FUSE
  backend and opcode subset.
- [`TESTING.md`](../../../TESTING.md) — suite coverage, environment variables
  and CI sequence.
- `sandbox.c`, `sandbox.h`, `mount.c`, `tests/integration/*.pl`,
  `tests/unit/*`, `.github/workflows/ci.yml`, `Makefile`.
