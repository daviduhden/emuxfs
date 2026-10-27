---
name: crash-recovery-and-fault-injection
description: Preserve emuxfs crash consistency, state.db transitions, sync recovery and fault-injection coverage. Use when editing working, restoring or degraded transitions, commit ordering, sync.c, crash.pl, or RECOVERY and TESTING documentation.
---

# Crash recovery and fault injection

## Activation

Use this skill when a change affects:

- `state.db` fields or transitions (`mounted`, `working`, `restoring`,
  `degraded`, `seq`);
- the durable order of a create, update, delete, rename or restore;
- `sync.c`, `scan.c`, `emuxfs_restore_now`, or degraded/sequence handling;
- `fault.c`, `fault.h`, fault-point call sites, `tests/integration/crash.pl`;
- `RECOVERY.md` or the crash sections of `TESTING.md`.

## Model

emuxfs treats an array as redundant copies plus a sequence number that orders
operations.  A crash must leave either an agreeable sequence number, so
checksums decide what is corrupt, or a disagreeing sequence number, so an
operator must choose a source with `sync`.  There is no journal and no atomic
cross-file commit.

Fault injection models a **process crash** (`_exit(70)` or `abort()`), not torn
sectors, controller caches or storage reordering.  Every state transition is
fsynced, so the suite exercises recovery decisions, but it is not a proof of
power-loss safety.

## state.db state machine

`state.db` is 40 bytes, five little-endian `uint64`:
`seq`, `mounted`, `working`, `restoring`, `degraded`.

A record is structurally invalid when any counter is greater than 1 or when
`working` and `restoring` are both non-zero.  A structurally invalid record is
refused by mount/audit/heal with a distinct diagnostic; `sync` may force it
through with a warning.  A wrong-sized `state.db` is not force-acceptable.

| state | mounted | working | restoring | degraded |
|-------|---------|---------|-----------|----------|
| clean | 0 | 0 | 0 | 0 |
| in use | 1 | 0 | 0 | 0 |
| writing | 1 | 1 | 0 | 0 or 1 |
| restoring | 0 or 1 | 0 | 1 | 0 or 1 |
| degraded | any | any | any | 1 |

- Read-write open sets `mounted = 1` and fsyncs.  Read-only `audit` does not.
- `emuxfs_working_push` sets `working = 1` before the first mutation.
- `emuxfs_working_pop` clears `working` and advances that device's `seq`.
- `emuxfs_working_cancel` clears `working` without advancing `seq`; use it only
  when no mutation occurred.
- `emuxfs_restoring_push/pop` bracket a repair.
- `degraded` records an I/O, integrity or sequence-exhaustion fault.  It is
  not cleared by mount, heal or sync; only `format` or replacing the directory
  resolves it.
- `seq` saturates at `UINT64_MAX`: the device is marked degraded and refuses
  to advance rather than wrapping.

## Commit order

The create-shaped canonical order is:

```
1. working_push            working=1, fsynced
2. physical mutation
3. meta.db write
4. assign.db write         create/delete only
5. fsync(meta), fsync(assign)
6. fsync content, lfile, parent directory
7. readback
8. ancestor metadata recompute and fsync
9. working_pop             working=0, seq++, fsynced
```

Real operations differ in detail but preserve the invariant that `working = 1`
is durable before the first mutation and `working = 0` only after all data,
metadata, mapping and namespace changes are durable:

- create fsyncs the new node before metadata/assignment;
- delete unlinks first, then clears metadata/assignment;
- update writes no assignment and fsyncs content after metadata;
- rename moves through `.muxfs/rename.tmp`, writes no node metadata or
  assignment, and clears a replaced destination;
- restore writes content and the `lfile` tree before metadata/assignment.

`readback` proves consistency, not durability.  Only the fsyncs address
durability.

## Fault points

| name | location |
|------|----------|
| `create/before_meta` | after creating the node, before metadata |
| `create/after_meta` | after metadata, before assignment |
| `update/before_meta` | after content mutation, before metadata |
| `update/after_meta` | after content and metadata are fsynced |
| `delete/after_unlink` | after unlink, before clearing metadata |
| `delete/after_meta` | after clearing metadata, before assignment |
| `restore/before` | before a restoration begins |
| `restore/after_meta` | after restoring metadata, before assignment |
| `restore/after` | after a restoration is verified (including deletes) |
| `rename/after_vacate` | after the source is moved to `.muxfs/rename.tmp` |
| `rename/after_dest` | after a replaced destination is moved to `.muxfs/rename.dest.tmp` |
| `rename/after` | after the destination is read back and verified |

`emuxfs_fault_point(name)` is a normal function whose body is inert unless
`EMUXFS_FAULT_INJECTION` is defined.  `make faultbuild` compiles separate
`.fault.o` objects with that define.  The environment variables are
`EMUXFS_FAULT_POINT` and `EMUXFS_FAULT_ACTION`; the default action is
`_exit(70)`.  Normal builds must never enable the define.

## Recovery decision tree

1. **Any dirty flag (`mounted`, `working`, `restoring`) or sequence mismatch?**
   Do not mount, audit or heal.  Unmount first, choose the authoritative
   source, and run `emuxfs sync destination source ...`.
2. **Structurally invalid `state.db`?**  `sync` is the only command that may
   force it; a wrong-sized file is unrecoverable without replacing the
   device.
3. **Degraded device?**  `sync` does not clear `degraded`.  `format` the
   device (empty directory) or replace it, then `sync` from a survivor.
4. **Clean array, corruption detected?**  `audit` reports; `heal` repairs from
   an agreeing source.  If valid copies disagree, `heal` refuses and `sync`
   is the explicit override.
5. **Interrupted rename?**  A node may be in `.muxfs/rename.tmp`; `sync`
   removes it while rebuilding.  Do not rename manually inside `.muxfs`.

`sync` rebuilds the destination from every usable source in index order,
bypasses the ambiguity check because the operator designated the source, then
writes a fresh `working = 0`, `restoring = 0`, `seq = source.seq` record.  It
does not clear `degraded`.  It returns non-zero if a repair fails or is
ambiguous; zero means the destination was rebuilt.

## Procedure for a transaction change

1. Write the new durable order, including every fsync and the crash outcome
   after each step.
2. Check `working_push`/`working_pop`/`working_cancel` on every exit path.
   A pre-mutation failure cancels; a post-mutation failure degrades and pops.
3. Verify that `seq` is advanced per device, not once globally after all
   devices, unless you are deliberately changing the recovery contract.
4. Update `RECOVERY.md` and the fault-point table in `TESTING.md`.
5. Add a fault test if you introduce a new durable boundary.  A fault point
   must sit at a meaningful boundary, be named `operation/location`, and have
   an assertion that the array converges after `sync` and ends clean.
6. Run `make faultbuild` and `make faulttest` in a disposable OpenBSD
   environment with root and `/dev/fuse0`.

## Validation

- **Static:** enumerate every `working_*`, `restoring_*`, `degraded_*` and
  `seq` write.  Check that every `working_pop` is paired and every
  `working_cancel` is genuinely pre-mutation.
- **Build:** `make check`.
- **Unit:** `make test`; `./tests/unit/test_core` includes `state.db`
  validation, working push/pop/cancel and assign cross-check.
- **Crash:** `make faultbuild`, then `make faulttest`.  The suite covers
  `restore/before`, `restore/after`, create/update/delete `*/after_meta`,
  rename `after_vacate`/`after`, a planted `working = 1`, and requires
  convergence, a clean `audit`, and clean `state.db` fields.
- **FUSE:** `make integration` and `make paralleltest` for the non-crash path.

## Common problems

- Advancing `seq` on a failure before any mutation.  The next mount then sees a
  spurious mismatch; use `emuxfs_working_cancel`.
- Leaving `working = 0` after a partial mutation.  That state looks clean and
  lets the next mount trust unverified data; degrade instead.
- Forgetting `emuxfs_final()` when exiting on a sequence mismatch after
  `emuxfs_init` marked devices mounted.
- Describing `sync` as clearing `degraded` or as accepting a wrong-sized
  `state.db`.  It does neither.
- Describing `heal` as safe to rerun on a device with `restoring = 1`.  Heal
  refuses a non-clean device; `sync` is the recovery path.
- Claiming `*/after_meta` always leaves the assignment missing.  That is true
  for create; delete leaves a stale assignment and update writes none.
- Treating a zero exit from an old `sync` as proof of success.  The current
  code propagates restore failure; keep it that way.
- Claiming power-loss safety from process-crash fault injection.


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

- [`RECOVERY.md`](../../../RECOVERY.md) — failure points, state machine,
  commit order and recovery commands.
- [`ON_DISK_FORMAT.md`](../../../ON_DISK_FORMAT.md) — `state.db`, sequence
  invariants and the v2 proposal.
- [`TESTING.md`](../../../TESTING.md) — fault-point table and suite coverage.
- `dev.c`, `state.c`, `sync.c`, `scan.c`, `util.c`, `fault.c`,
  `tests/integration/crash.pl`.
