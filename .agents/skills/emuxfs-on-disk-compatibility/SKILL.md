---
name: emuxfs-on-disk-compatibility
description: Preserve and review the emuxfs version 1 on-disk format and legacy compatibility. Use when changing conf.c, dev.c, format.c, sync.c, state.db, meta.db, assign.db, lfile, format_version, or compatibility documentation.
---

# emuxfs on-disk compatibility

## Activation

Use this skill when a change can affect persistent bytes or compatibility
claims:

- `conf.c`, `dev.c`, `format.c`, `sync.c`, `state.c` serialization;
- `muxfs.conf`, `state.db`, `meta.db`, `assign.db`, `lfile/`;
- `EMUXFS_FORMAT_VERSION`, checksum algorithm selection, record sizes, UUIDs;
- `ON_DISK_FORMAT.md`, `COMPATIBILITY.md`, `RECOVERY.md` format claims;
- a request to add metadata, change checksum coverage or migrate an array.

Do not use it for FUSE callback behavior that does not change persistent
bytes; use `fuse-operations-and-consistency` instead.

## Non-negotiable rule

**Format version 1 is frozen.**  Any change that makes an old array unreadable
by a new binary, or a new array unreadable by an old binary, requires an
explicit `format_version` bump, a migration and rollback design, and the
compatibility test matrix.  `mount` must never migrate automatically.  Until
such a proposal is approved, do not change the bytes.

The full normative text is in [`ON_DISK_FORMAT.md`](../../../ON_DISK_FORMAT.md).
This skill summarizes the surfaces you must not break.

## Version 1 surface

| surface | contract |
|---------|----------|
| private names | `.muxfs/`, `muxfs.conf`, `state.db`, `meta.db`, `assign.db`, `lfile/` |
| format discriminator | `format_version` in `muxfs.conf`; missing key means legacy version 1 |
| `state.db` | exactly 40 bytes, five little-endian `uint64`: `seq`, `mounted`, `working`, `restoring`, `degraded` |
| `meta.db` | entry offset `ino * msz`; `msz = align_up(16 + 2 * chksz, 8)`; `flags`, `eno`, metadata sum, content sum, zero padding |
| `assign.db` | 16 bytes per entry (`flags`, `ino`), indexed by `eno` |
| checksum encoding | CRC32 little-endian `uint32`; MD5 and SHA-1 raw digest bytes |
| `lfile/` | one file per large regular internal inode; `FACTOR = 4096 / 20 = 204`; level bases round up to a power of two |
| byte order | every multi-byte integer is little-endian; no host ABI types on disk |
| default algorithm | `md5` for legacy compatibility |

Key code anchors: `emuxfs.h` (`EMUXFS_FORMAT_VERSION`,
`EMUXFS_FORMAT_VERSION_LEGACY`, record structs and static asserts),
`conf.c` (`emuxfs_conf_parse`, `emuxfs_conf_write`), `dev.c`
(`emuxfs_meta_size`, `emuxfs_meta_read/write`, `emuxfs_assign_read/write`),
`format.c`, and `lfile.c`.

## Change classification

Before editing, write down which persistent bytes change.

1. **No persistent change.** Refactors, comments, in-memory types that keep
   `sizeof` and the byte encoding.  Version 1 stays.
2. **Compatible additive change.** Example: accepting an additional
   checksum algorithm name.  Existing arrays remain readable.  Version 1 may
   stay only if the encoding of existing records is unchanged.  Document the
   compatibility classification.
3. **Incompatible change.** New field, changed field order, changed checksum
   coverage, new `state.db` size, changed `lfile` geometry, enforced new
   invariant.  Stop and write a version 2 proposal before implementing.

For classification 3, the proposal must state: target `format_version`, exact
byte layout, reader/writer rules, refusal of mixed arrays, migration steps,
rollback, crash/resume behavior, and the tests listed below.  The existing
proposal and migration protocol are in `ON_DISK_FORMAT.md`; version 2 is
**not implemented** and must not be described as available.

## Procedure

1. Read `ON_DISK_FORMAT.md`, `COMPATIBILITY.md`, `emuxfs.h`, and the relevant
   serializer/parser.  Confirm the actual constants and `static_assert`s.
2. List every offset, size, byte order and checksum input affected.  Include
   `lfile` root and `meta.db` content checksum if content handling changes.
3. Decide compatible versus incompatible.  If incompatible, stop at a written
   proposal; do not change code or silently bump the version.
4. For a compatible change, update the writer, reader, validation, and every
   cross-device check.  Keep legacy acceptance: a missing `format_version`
   is version 1; malformed values are rejected; an unsupported value is
   refused with `unsupported on-disk format version`.
5. Update `ON_DISK_FORMAT.md`, `COMPATIBILITY.md` and any affected recovery
   or testing text.  Keep program version and format version distinct.
6. Add or extend tests before claiming compatibility.  Do not rely on the
   existing prose alone.

## Validation

Run only in an environment where you are allowed to build and test.  Record
what was actually run; a static reading is not a compatibility test.

- **Static:** verify constants with `rg`/`grep`, check `static_assert`s, and
  check that no serializer writes a host `time_t`, `ino_t`, `off_t` or
  `size_t` directly.  Check links in the changed Markdown.
- **Build:** `make check` (strict warnings, `-Werror`).
- **Unit:** `make test`, `make unittest`, `./tests/unit/test_core`.  Relevant
  cases include checksum known answers, `msz` 24/48/56, config round-trip,
  legacy `0.5current` parsing, `format_version=99` rejection, endianness,
  `state.db` validation, meta/assign cross-check, and `lfile` create/resize/
  delete.
- **FUSE/runtime (root and `/dev/fuse0` only):** `make integration`,
  including `audit` leaving `muxfs.conf`, `state.db`, `meta.db` and
  `assign.db` byte-identical, and `sync` to a replacement device.
- **Compatibility proposal:** v1 read by the new binary; clean v1 migration;
  interruption at each migration step plus resume; rollback; refusal of
  dirty/degraded/ambiguous input; refusal of a mixed v1/v2 array; and v1
  arrays untouched when no migration is requested.

## Common problems

- **Do not assume `format_version=0` is a `strtonum` failure.**  It parses and
  is rejected later as `EMUXFS_CONF_EBADVERSION`; `-1`, trailing garbage,
  empty values and values above `UINT32_MAX` are parse failures.
- **Do not claim `eno` is never reused.**  In-session LIFO reuse of the most
  recently allocated value is real; across a remount the next value is
  recomputed from the unchanged `assign.db` size.
- **Do not describe `lfile` levels as a tight cumulative array.**  Each level
  starts at a power-of-two-rounded base; for three blocks the root is at
  index 4 and index 3 is unused.
- **Do not say `seq` advances only after every device.**  Each device advances
  its own `seq` in its own `emuxfs_working_pop`; a crash between devices
  produces the mismatch that forces `sync`.
- **Do not omit `MF_ASSIGNED`.**  `readback` requires it; the flag is outside
  the metadata checksum.
- **Do not claim checksum algorithms are cryptographic.**  They detect
  accidental corruption; they do not authenticate a writer.
- **Do not claim byte-for-byte compatibility with a historical muxfs array was
  proven by this repository alone.**  The layout and legacy parser are
  present, but the original source is not vendored and the unit test
  fabricates records.
- **Do not treat `chk_alg` mismatch as harmless.**  Devices are now refused at
  open time when their algorithms differ, because `meta.db` entry size and
  every checksum depend on it.

## References

- [`ON_DISK_FORMAT.md`](../../../ON_DISK_FORMAT.md) — normative layout,
  invariants, limits, version 2 proposal and migration protocol.
- [`COMPATIBILITY.md`](../../../COMPATIBILITY.md) — target system, FUSE
  backend, persistent naming and format classification.
- [`RECOVERY.md`](../../../RECOVERY.md) — sequence, dirty-state and `sync`
  semantics.
- `emuxfs.h`, `conf.c`, `dev.c`, `format.c`, `lfile.c`,
  `tests/unit/test_core.c`.
