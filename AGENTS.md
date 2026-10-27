# Agent instructions for emuxfs

emuxfs (The Enhanced Multiplexed File System) is a mirroring, checksumming
and self-healing FUSE filesystem layer for OpenBSD.  An **array** is a set of
**devices** (ordinary directories formatted by `emuxfs format`); each device
holds a full copy of the tree plus a private `.muxfs` directory.  The array is
mounted through the FUSE 2.6 high-level API shipped with the OpenBSD base
system.  The executable is `emuxfs`, the manual page is `emuxfs(1)`, and
internal symbols use the `emuxfs_` prefix.

Read this file first, then [`GLOSSARY.md`](GLOSSARY.md),
[`emuxfs.h`](emuxfs.h) and [`ARCHITECTURE.md`](ARCHITECTURE.md).  The canonical
behavior documents are [`ON_DISK_FORMAT.md`](ON_DISK_FORMAT.md),
[`RECOVERY.md`](RECOVERY.md), [`SECURITY.md`](SECURITY.md),
[`TESTING.md`](TESTING.md) and [`COMPATIBILITY.md`](COMPATIBILITY.md).  If a
comment or a summary disagrees with the code, the code is the fact to verify;
fix the documentation rather than copying the claim.

## Non-negotiable invariants

1. **On-disk format version 1 is frozen.**  The persistent bytes are
   `.muxfs/muxfs.conf`, the 40-byte `state.db`, the inode-indexed `meta.db`,
   the eno-indexed `assign.db` and `lfile/`.  Do not change their layout,
   encoding or checksum coverage without an explicit new `format_version`,
   a migration/rollback design and compatibility tests.  `mount` never
   migrates.  See [`ON_DISK_FORMAT.md`](ON_DISK_FORMAT.md).
2. **Per-device transaction order.**  `emuxfs_working_push` must be durable
   before the first mutation; `meta.db`/`assign.db`, content, `lfile`, and the
   affected parent directory must be durable before `emuxfs_working_pop`
   clears `working` and advances `seq`.  Do not reorder this without updating
   [`RECOVERY.md`](RECOVERY.md) and the crash suite.
3. **Validation and repair are separate.**  `emuxfs_readback` and
   `emuxfs_desc_chk_*` validate; `emuxfs_restore_*` repair.  `audit` is
   read-only; `heal` and `sync` are destructive.  Coherent divergence is
   never resolved automatically.
4. **Single-threaded by design.**  Use `fuse_loop()`, not `fuse_loop_mt()`.
   The device array, restore queue, dynamic stack and write path are not
   thread-safe.  Do not add mutexes or threads merely to enable concurrency.
5. **Path discipline.**  Every FUSE path goes through
   `emuxfs_path_sanitize`; operations use `*at()` against pinned descriptors;
   private objects use `O_NOFOLLOW` and long-lived descriptors use
   `O_CLOEXEC`.  A user-visible `O_TRUNC` is converted into the normal
   truncate operation, never passed through to the backing file; the restore
   path is the only exception and immediately rewrites metadata, assignment
   and checksums.
6. **Hard links are unsupported.**  Two names for one inode would share one
   metadata slot and one `eno`.  Refuse them with `EOPNOTSUPP` in create,
   delete, update, rename and restore paths.
7. **Checksums detect accidents, not attackers.**  CRC32, MD5 and SHA-1 are
   integrity checks against corruption; MD5 and SHA-1 are not collision
   resistant.  Do not describe them as authentication or encryption.
8. **Fail closed.**  A device that is dirty, structurally invalid or
   degraded must not be mounted, audited or healed.  `sync` is the explicit
   recovery path.  Never hand-edit `.muxfs`.
9. **No silent rollback or partial success.**  A device that fails after a
   mutation is marked degraded; the call succeeds only if at least one device
   completed.  A failure before any mutation must cancel `working` without
   advancing `seq`.

## Repository map

| path | responsibility |
|------|----------------|
| `emuxfs.c` | CLI dispatch, `version` |
| `mount.c` | FUSE lifecycle, canonical paths, mount topology, sandbox setup |
| `ops.c` | `struct fuse_operations`, write/read transactions, credentials |
| `util.c` | readback, restore primitives, restore queue drain, path helpers |
| `desc.c` | metadata descriptions and metadata/content checksums |
| `chk.c`, `chk.h` | CRC32/MD5/SHA-1 wrapper |
| `lfile.c` | per-block checksum tree for files larger than 4 KiB |
| `dev.c` | device array, `state.db`, `meta.db`, `assign.db`, `lfile/` |
| `conf.c` | `muxfs.conf` parser/writer and format-version check |
| `format.c`, `scan.c`, `sync.c` | `format`, `audit`/`heal`, `sync` |
| `state.c` | restore queue, eno allocator, logging, write-buffer state |
| `sandbox.c`, `sandbox.h` | the only `pledge(2)`/`unveil(2)` calls and promises |
| `fault.c`, `fault.h` | compile-time-gated fault injection |
| `ds.c`, `ds.h` | non-thread-safe dynamic stack used by walkers |
| `tests/unit/` | headless `test_core`, stdckdint conformance and compile-fail |
| `tests/integration/` | FUSE integration, 32-worker stress, crash/fault suites |
| `test.pl`, `test.conf.dist` | legacy end-to-end suite using operator paths |

## Commands that actually exist

All commands below are defined in `Makefile` or `emuxfs(1)`.  Do not invent
flags, targets or environment variables.  Distinguish four evidence levels:
**static analysis** (reading/grep only), **build**, **automated tests**, and
**real runtime verification** (a mount on OpenBSD).  A green static reading is
not a test pass.

| command | purpose | requirements |
|---------|---------|--------------|
| `make` | build `emuxfs` with clang, C23, zlib and base libfuse | OpenBSD toolchain |
| `make check` | clean rebuild with the full strict warning set and `-Werror` | OpenBSD toolchain |
| `make test` | build unit tests, run `test_stdckdint` and the compile-fail check | no FUSE |
| `make unittest` | build `tests/unit/test_core` and `test_stdckdint` | no FUSE |
| `./tests/unit/test_core` | headless checks; device tests require root | root for device cases |
| `make integration` | FUSE integration suite, temporary sandbox | root and `/dev/fuse0` |
| `make paralleltest` | 32 workers × 20 iterations, then audit and mirror comparison | root and `/dev/fuse0` |
| `make faultbuild` | build `emuxfs-fault` with `EMUXFS_FAULT_INJECTION` | OpenBSD toolchain |
| `make faulttest` | run `tests/integration/crash.pl` against both binaries | root and `/dev/fuse0` |
| `make fuzz-conf` | build the libFuzzer parser target | clang with libFuzzer |
| `make fuzz-smoke` | bounded parser fuzz run (`FUZZ_RUNS`, default 20000) | libFuzzer or ASan mutation driver |
| `make legacytest` | legacy `test.pl` suite | `test.conf`, root, scratch paths |
| `make stability` | `check`, `test`, `test_core`, integration, parallel, fault, fuzz | root/FUSE for the FUSE steps |
| `make install` | install binary and man page under `/usr/local` | root; system change |
| `make clean` | remove build products and test binaries | — |

`make stability` is the core local sequence, not byte-for-byte CI: CI also
installs, runs `mandoc` and linkage checks, rebuilds with
`EMUXFS_DS_MALLOC=1` and performs a final clean rebuild.  A skipped FUSE step
is **not** a pass.  The actual CI target is OpenBSD 8.0 amd64 and arm64; see
`.github/workflows/ci.yml`, not a prose summary.

## Safety and testing policy

* Never `format`, `mount`, `audit`, `heal` or `sync` a real device or a
  directory containing user data.  Use only the temporary sandbox created by
  a test, or a disposable OpenBSD VM.
* Do not install packages, create device nodes, change system settings or run
  destructive commands on the host.  `make install`, `MAKEDEV fuse` and
  `test.pl` are not routine development steps.
* `test.pl` uses the paths in `test.conf` and recursively deletes them.  Treat
  `test.conf` as dangerous; never point it at a live filesystem.
* Never run `heal` or `sync` against a live mount.  `sync` force-opens a dirty
  destination and can race the daemon.
* `audit` reports corrupt paths on stdout but exits 0 on detected corruption;
  check the output, not only the exit status.  `heal`/`sync` exit non-zero on
  ambiguity or failed repair.
* Fault injection models a process crash, not torn sectors, controller caches
  or storage reordering.  Do not claim power-loss safety from it.
* If a required runtime step is skipped for lack of root or `/dev/fuse0`,
  report it as unverified, not as passing.

## Continuous Agent & Skills Maintenance

**Agent instructions and skills are living repository artifacts.**

Agent instructions and skills are living repository artifacts.
Every repository modification MUST include an agent/skills synchronization step.
A code change without the corresponding agent knowledge maintenance is considered incomplete.
Never claim completion when the synchronization step has been skipped or has failed.

**Every repository modification MUST include an agent/skills synchronization
step.** This applies to code, configuration, dependencies, functionality,
bug fixes, refactors, tests, workflows, documentation, architecture and
repository structure, including changes to these instructions and skills.

Before completing any task, inspect the applicable `AGENTS.md` and
`.agents/skills/**/SKILL.md` files, update the affected instructions, and
validate their consistency with the implementation. A code change without the
corresponding agent knowledge maintenance is considered incomplete. Never
claim completion when the synchronization step has been skipped or has failed.

Procedure (one complete review at the end of the task; repeat validation only
after correcting a detected error):

1. Inventory instruction files with `rg --files --hidden` for `AGENTS.md`,
   `*.agent.md` and `SKILL.md` across `.agents`, `.github`, `.claude`,
   `.cursor` and other tool-specific directories. Inspect tool instruction
   files such as `.github/copilot-instructions.md` and `*.instructions.md`
   when present. Do not assume only this list exists.
2. Map changed paths and behavior to skill Activation sections, then read
   those skills and their cited canonical docs. Review specialized agent
   instructions when the repository has them. Also check whether another
   skill would benefit from reusable knowledge learned during the change.
3. Update an existing skill when its scope applies and the task changes its
   procedures, tools, assumptions, commands, paths, validations or reusable
   knowledge. Create a skill only for a durable, distinct responsibility
   without a suitable existing skill; avoid duplicating coverage. Update
   specialized agent instructions when their role, workflow, validation or
   skill relationships change. Keep the root instructions and specialized
   instructions aligned.
4. Remove stale or contradictory guidance and verify every changed local
   instruction reference and documented path that can be checked
   automatically. Record semantic review explicitly; structural checks cannot
   determine whether a knowledge update is relevant.
5. Run `perl scripts/check-agent-maintenance.pl --base <base-ref>` for a
   branch/PR comparison (or omit `--base` to include staged and unstaged
   changes against `HEAD`). The checker enforces a substantive instruction
   diff when project files change, required policy sections/frontmatter, and
   resolvable local Markdown links. CI runs it on pull requests. Its pass is
   structural evidence only; the agent must still review semantic accuracy.
6. Validate the implementation using the applicable commands in this file
   and the relevant skill, then inspect the complete diff. If a related skill
   or instruction cannot be updated, document the concrete reason and report
   the task incomplete. Do not silently omit synchronization.

### Post-change checklist (mandatory definition of done)

- [ ] Repository changes implemented.
- [ ] Relevant agent instructions reviewed.
- [ ] Relevant skills reviewed.
- [ ] Affected `AGENTS.md` / `SKILL.md` files updated substantively.
- [ ] Obsolete knowledge removed.
- [ ] New reusable knowledge incorporated.
- [ ] Agent and skill references validated.
- [ ] Automated maintenance checks passed.
- [ ] Final synchronization status reported.

Every final report must include **Agent & Skills Synchronization** with agents
and skills reviewed, instruction files updated, knowledge incorporated or
corrected, validations actually run, and status `PASS` or `FAIL`. If any item
fails, correct it or state the limitation and report `FAIL`; never claim a
check that was not run.

## Working with existing changes

* Run `git status --short` and read the diff before editing.  Do not revert,
  reformat or delete work you did not create; if it conflicts with your
  change, stop and ask.
* Do not commit, amend, push, tag or rewrite history unless explicitly asked.
* Preserve the ISC license and every existing copyright notice.  New C files
  follow the existing header style; see `NOTICE.md`.
* Follow the surrounding C23 style: clang, hard tabs, `emuxfs_`/`EMUXFS_`
  prefixes, `[[nodiscard]]` on fallible integrity helpers, `goto` cleanup
  ladders, and the existing error-code conventions (`0` success, `1` general
  failure, `EMUXFS_EINT`/`EFS`/`ECHK` only where already used).
* Update the canonical document and the tests with any behavior change.  A
  format change additionally needs the v2 proposal and compatibility matrix
  in [`ON_DISK_FORMAT.md`](ON_DISK_FORMAT.md).
* Record exactly what you ran.  Do not claim a build, test or stability result
  that was not observed.

## Skills

Load the skill that matches the change.  Each one has activation conditions,
a reproducible procedure, validations and known failure modes.

| skill | use when |
|-------|----------|
| [`emuxfs-on-disk-compatibility`](.agents/skills/emuxfs-on-disk-compatibility/SKILL.md) | touching `conf.c`, `dev.c`, `format.c`, `sync.c`, `state.db`, `meta.db`, `assign.db`, `lfile/`, `format_version`, or compatibility claims |
| [`fuse-operations-and-consistency`](.agents/skills/fuse-operations-and-consistency/SKILL.md) | editing `ops.c`, `mount.c`, `util.c` callbacks, write/rename/truncate/read paths, permissions or `statfs` |
| [`checksum-and-self-healing`](.agents/skills/checksum-and-self-healing/SKILL.md) | editing `chk.c`, `desc.c`, `lfile.c`, readback, audit/heal or restore behavior |
| [`crash-recovery-and-fault-injection`](.agents/skills/crash-recovery-and-fault-injection/SKILL.md) | editing `state.db` transitions, `sync.c`, fault points, `crash.pl`, or recovery documentation |
| [`openbsd-sandbox-and-stress-tests`](.agents/skills/openbsd-sandbox-and-stress-tests/SKILL.md) | adding syscalls/subcommands, editing `sandbox.c`/`sandbox.h`, or changing/running integration, stress, CI or test safety rules |
