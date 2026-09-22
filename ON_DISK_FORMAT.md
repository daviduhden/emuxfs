# On-disk format of the `.muxfs` directory

This document is normative for the persistent format.  Any change to what is
described here is a format change and requires a `EMUXFS_FORMAT_VERSION` bump
(see below).  See also RECOVERY.md and COMPATIBILITY.md.

All multi-byte integers are **little-endian**.  There are no alignment
requirements on the on-disk structures beyond the padding described below;
reads are done with exact `pread(2)` into naturally aligned stack objects.
There are no endianness assumptions about the host: values are converted with
`htole64`/`letoh64`.

```
<device root>/
    .muxfs/                 mode 0700, owned by root
        muxfs.conf          text, newline-terminated key=value lines
        state.db            40 bytes: 5 × uint64
        meta.db             indexed by internal inode
        assign.db           indexed by external inode (eno)
        lfile/              directory; one file per large regular inode
```

A node named `.muxfs` is reserved everywhere and is never mirrored.

## muxfs.conf

A newline-terminated text file.  Lines are `key=value`; unknown keys and keys
longer than the line buffer are rejected.  Keys (all required except
`format_version`):

| key             | meaning                                                |
|-----------------|--------------------------------------------------------|
| `version`       | program version, informational (e.g. `1.0-current`)   |
| `format_version`| persistent layout version (currently `1`)             |
| `chk_alg`       | `crc32`, `md5` or `sha1`                               |
| `array_uuid`    | UUID shared by all devices in the array                |
| `dev_uuid`      | UUID unique to this device                             |
| `seq_zero_time` | epoch time at which `seq` last wrapped to zero         |

The original Multiplexed File System wrote `version=MAJOR.MINORflavor` (for
example `0.5current`) and had a defect that discarded the revision.  emuxfs
accepts that legacy spelling and also writes the readable `MAJOR.MINOR-flavor`.

### Format versioning

* `format_version` identifies the layout of the `.muxfs` directory.  It is
  separate from the program version.
* An array with no `format_version` key predates the key.  Its layout is
  version 1, so it is accepted as `EMUXFS_FORMAT_VERSION_LEGACY` (1); an
  informational message is logged.  This policy is meaningful because the
  persistent names were kept from muxfs (see COMPATIBILITY.md).
* An array whose `format_version` is not `EMUXFS_FORMAT_VERSION` is rejected
  with `unsupported on-disk format version`; it is never mounted.
* `mount` never rewrites or migrates the format.  A migration, if one is ever
  needed, must be an explicit command; none exists yet.

Parser robustness (all statically verifiable in `conf.c`):

* Duplicate keys are rejected, so a later line cannot silently override an
  earlier `format_version`.
* Values must be fully valid: `format_version=-1`, `format_version=0`,
  `format_version=1garbage`, empty values, values above `UINT32_MAX`, and
  values with trailing characters are all rejected by `strtonum(3)`.
* A CRLF line ending is accepted by stripping one trailing `\r`; a file that
  does not end in a newline is rejected.
* Unknown keys and unknown checksum algorithms are rejected.
* A line longer than the 4 KiB parse buffer is rejected.

### ABI independence

The records written by emuxfs use fixed-width integers and an explicit
byte order, and the structure sizes are asserted at compile time:

* `state.db` is five `uint64` fields (40 bytes).
* `meta.db` is a 16-byte header plus two fixed-width checksum fields.
* `assign.db` is two `uint64` fields (16 bytes).
* `muxfs.conf` stores numbers as decimal text and UUIDs as text.

No `time_t`, `ino_t`, `off_t`, `dev_t` or `size_t` value is written to disk
directly.  `ino_t` values are used as indices but are not serialised, so the
format does not depend on the C ABI beyond little-endian byte order.  This is
a static property of the code, not a claim that cross-architecture use has
been tested.

## state.db

A single fixed record of exactly 40 bytes.  All fields are unsigned 64-bit
integers in little-endian byte order, at fixed offsets:

| offset | width | field       | meaning                                        |
|--------|-------|-------------|------------------------------------------------|
| 0      | 8     | `seq`       | array sequence number; equal across devices    |
| 8      | 8     | `mounted`   | 1 while the device is in use by any command    |
| 16     | 8     | `working`   | 1 during a write operation                     |
| 24     | 8     | `restoring` | 1 during a restoration                         |
| 32     | 8     | `degraded`  | 1 after a detected unrecoverable condition      |

There is no padding, no header, no magic and no checksum.  Any other file size
is rejected.  The record is written whole at offset 0 and `fsync`ed.

### Field invariants

Derived from the implementation, a record is **structurally invalid** when:

* `mounted`, `working`, `restoring` or `degraded` is greater than 1;
* `working` and `restoring` are both non-zero.

Anything else is at least *readable*.  In particular `working = 1` alone,
`restoring = 1` alone, `mounted = 1` alone and `degraded = 1` are valid but
non-clean states produced by an interrupted operation, and must remain
readable so that recovery is possible.  A structurally invalid record causes
the device to be refused with a distinct diagnostic; it must not be treated as
merely dirty.

### Torn writes

There is no generation, no checksum and no second slot.  A torn write that
leaves a mixture of old and new values within the 40 bytes is therefore *not*
detectable from the record itself.  The `seq`/`working` comparison across
devices is the only cross-device check.  Adding a checksum, a generation or a
double-buffered slot would require a new `format_version`; it is not done in
version 1.  The reasoning behind retaining version 1 is the analysis below.

Worked example.  A clean device commits `seq=10, mounted=1, working=1` and
writes `seq=11, mounted=1, working=0`.  The two records differ only in `seq`
and `working`.  A torn write can persist any combination of old and new
fields:

| persisted        | classification                              | outcome           |
|------------------|---------------------------------------------|-------------------|
| old / old        | dirty (`working=1`)                         | refused at mount  |
| new / new        | the real commit                             | correct           |
| new seq, old work| dirty (`working=1`)                         | refused at mount  |
| old seq, new work| clean at the old `seq`                      | see below         |

A tear *inside* a field can also yield an arbitrary value; any value greater
than one for a counter, or `working` and `restoring` both set, is structurally
invalid and is refused (it cannot be mistaken for clean).

The only interesting row is `old seq, new working`.  The device looks clean at
the old sequence number while the operation that preceded the state write may
already be durable (its data and metadata are `fsync`ed before `working` is
cleared).  Detection is layered:

* if any other device did reach the new `seq`, `emuxfs_dev_seq_check` reports
  a mismatch and the whole array is refused;
* if the torn device actually applied the operation and another device did
  not, the two trees differ, and the per-node content and directory checksums
  no longer agree, so `audit`/`heal` report the mismatch;
* if every device tore the same way and every device also applied the
  operation, the array is internally consistent with the old `seq` and the
  operation is simply *not* recorded as a new commit, which is harmless:
  `seq` orders commits, it does not establish correctness.

The residual risk is therefore a *narrow window*: a power loss during the
40-byte state write, on storage that tears within a single sector, on the last
device to apply an operation, producing exactly `old seq, new working`, with
the other devices left at the old `seq`.  FFS stores the 40-byte record inside
one filesystem block and a single-sector write is atomic on the target
hardware in practice, so this is not reachable under the fsync model emuxfs
relies on.  It is, however, the reason a checksummed, double-slotted `state.db`
is proposed for version 2 (see below); that design removes the assumption.

`seq_zero_time` lives in `muxfs.conf`, not here.  In format version 1 emuxfs no
longer wraps `seq`: at `UINT64_MAX` the device is marked degraded and requires
administrative intervention.  `seq_zero_time` is therefore written once at
`format` time and never changes afterwards.

## meta.db

An array indexed by *internal inode*.  Entry size:

```
msz = align_up(sizeof(emuxfs_meta_header) + 2 * chksz, EMUXFS_MEM_ALIGN)
```

where `EMUXFS_MEM_ALIGN` is 8.  The entry is:

| offset            | size    | contents                          |
|-------------------|---------|-----------------------------------|
| 0                 | 8       | `flags` (uint64), `MF_ASSIGNED`=1  |
| 8                 | 8       | `eno` (uint64)                    |
| 16                | `chksz` | metadata checksum                 |
| 16 + `chksz`      | `chksz` | content checksum                  |
| 16 + 2*`chksz`    | pad     | zero padding to `msz`             |

The entry for internal inode *i* is at byte offset `i * msz`.  This is why the
database size is proportional to the largest internal inode number (see the
manual page caveat).

The **metadata checksum** is the checksum of the concatenation, in order, of:

* `eno` (uint64, little-endian)
* `owner` (uint64)
* `group` (uint64)
* `mode` (uint64, including the file type bits)
* `size` (uint64) — only for regular files
* `content_checksum` (`chksz` bytes)

Timestamps are not covered: neither `atime`, `mtime`, `ctime`, their
nanoseconds, nor any birth time.  This is deliberate and keeps format version
1 stable:

* `atime` changes on ordinary reads and cannot be part of integrity.
* `ctime` changes on any metadata change and is an artefact of the underlying
  filesystem.
* `mtime` is the only timestamp with replication semantics, but adding it
  would change the metadata checksum and therefore the format; it would need
  an explicit new `format_version`, so it is not done here.

File type is covered implicitly through the type bits of `mode`; there is no
separate `type` field in the hash.  Hard links are refused rather than
represented (see the invariants), so `nlink` is not hashed either.

The **content checksum** depends on the node type:

* regular file ≤ one block: checksum of the file bytes;
* regular file > one block: the root of the `lfile` checksum tree;
* directory: checksum of the sorted (bytewise ascending) list of entries,
  where each entry contributes its name bytes followed by the child's metadata
  checksum.  `.muxfs` is excluded;
* symbolic link: checksum of the target bytes returned by `readlink(2)`.

## assign.db

An array indexed by *external inode* (`eno`).  Each entry is 16 bytes:
`flags` (uint64, `AF_ASSIGNED`=1) and `ino` (uint64).  The next free `eno` is
`filesize / 16`.  Deleting a node zeroes its entry; the `eno` is not reused by
`emuxfs_state_eno_next_acquire` until the array is recreated.

### Invariant with meta.db

For every live node the two databases must agree:

```
meta.db[ino].flags == MF_ASSIGNED  and
meta.db[ino].eno   == E           implies  assign.db[E].flags == AF_ASSIGNED
                                            and assign.db[E].ino == ino
```

and conversely, every `assign.db[E]` with `AF_ASSIGNED` set must point at an
inode whose metadata records `eno == E`.

This is checked in two directions:

* on every validation (`emuxfs_readback`), for the node being checked;
* during `audit` and `heal`, over the whole `assign.db` in O(n) where n is the
  number of assign records (`emuxfs_meta_assign_check`).

### Orphan records

* **Orphan assign** (`assign.db[E]` set but `meta.db[ino]` missing or recording
  a different `eno`): reported by `audit`; it is a sign of a crash between the
  metadata and assign writes.  `heal` refuses rather than guessing; recover
  with `sync`.
* **Orphan meta** (`meta.db[ino]` set but `assign.db[eno]` missing or pointing
  elsewhere): detected by the per-node check during validation and by `audit`.
  Not cleaned automatically.
* The `eno` of a cleared entry is never reused, so an orphan record can never
  be mistaken for a newly created object.

## lfile/

For each regular file larger than one block, a file named after its *internal*
inode contains a checksum tree.

* Level 0 holds one checksum per file block (`ceil(size / 4096)` entries).
* Each higher level holds `ceil(child_count / FACTOR)` entries, where
  `FACTOR = EMUXFS_BLOCK_SIZE / EMUXFS_CHKSZ_MAX = 204`.  Note that `FACTOR`
  uses the maximum checksum size, so it does not depend on the chosen
  algorithm.
* Entries are stored consecutively by absolute index
  (`emuxfs_lfile_abs_range`); the file size is
  `chksz * (root_absolute_index + 1)`.
* A partial final block is hashed over its actual bytes only; it is not
  zero-padded for the leaf checksum.
* The root entry is copied into the node's `content_checksum` in meta.db.

`crc32` checksums are stored as a little-endian `uint32`.  `md5` and `sha1`
are stored as their raw digest bytes.

## Invariants

1. All devices agree on `array_uuid`, `chk_alg` and, when versioned,
   `format_version`.
2. All devices agree on `seq` and `seq_zero_time`, checked before mount,
   audit and heal.
3. `seq` is incremented only after every device has completed an operation,
   so a mismatch means an interrupted or failed operation.
4. `working`/`restoring` are non-zero only while an operation is in progress;
   a non-clean device cannot be mounted and must be recovered offline.
5. Every live node has exactly one assignment entry in each device.
6. For a regular file > one block, the root of its `lfile` tree equals the
   content checksum in meta.db.
7. Truth is relative to the array: an item is *valid* if its checksums match
   at least one device, and *corrupt* on a device where they do not.
8. No regular file has more than one hard link.  Two names for one inode would
   share a metadata slot and an eno, so hard links are refused by
   `format`/`audit`/`heal`/`unlink`/restore rather than misrepresented.
9. When two or more devices hold an internally valid but different copy of a
   path (coherent divergence), no automatic restoration is performed; the
   ambiguity is reported and must be resolved explicitly with `sync`.

## Checksum algorithms

`crc32` is 32 bits and is not cryptographic: it detects accidental corruption
only.  `md5` and `sha1` are stronger but are also broken as collision
resistant hashes; they are used here for accidental-corruption detection, not
for security.  The default remains `md5` for compatibility with existing
arrays.  Adding an algorithm does not change the format; selecting a different
algorithm for an existing array does, and is not supported.

## Version 1 integrity guarantee

Format version 1 guarantees, for every node, equality between replicas with
respect to:

* the node type;
* the permission bits;
* the owner and group;
* the external inode number (`eno`) and the `meta.db`/`assign.db` mapping;
* for regular files, the size and the content (directly, or through the
  `lfile` checksum tree);
* for directories, the sorted set of `(name, child metadata checksum)` pairs;
* for symbolic links, the target bytes.

Format version 1 does **not** cover: `atime`, `mtime`, `ctime` or their
nanoseconds; any birth time; `nlink`; OpenBSD file flags (`st_flags`);
extended attributes or ACLs.  It is a *preserved metadata subset*, not a full
copy of inode metadata.  Checksums are integrity checks against accidental
corruption, not cryptographic authentication; see SECURITY.md.

## Object identity

Enhanced muxfs defines **logical identity** as the tuple of the fields that
version 1 protects, together with the path.  Concretely:

* `eno` and the `eno` -> ino mapping are properties of the *physical* inode,
  not of the path.  `rename A B` changes names and recomputes the affected
  directory checksums but preserves the node's `eno` and metadata; `unlink A`
  clears the node's metadata and its assignment; a later `create B` allocates
  an `eno` again.  The allocator is monotonic within an array except that
  deleting the most recently created object returns its `eno` to the pool, so
  `eno` may be reused in a last-in-first-out pattern.  That is safe because the
  object's metadata and assignment are cleared before the value is reused, and
  it is not a silent wrap.
* A replacement out of band (for example `rename` of a temporary file over the
  name) that yields a *different physical inode* is still the same **logical**
  object for version 1 when every protected field is identical, because the
  protected tuple is all that version 1 claims to preserve.  If any protected
  field differs, the replacement is detected as corruption by
  `readback`/`audit` and repaired from an agreeing source.  This is why version
  1 needs no per-inode generation: it does not claim to distinguish physically
  distinct objects whose protected representation is equal.

`mtime` is intentionally outside this tuple; two objects that differ only in
`mtime` are considered equivalent by version 1.

## Limits

* `meta.db` is indexed by *internal inode*.  Its logical size is
  `(largest ino + 1) * msz`; a filesystem that uses very large inode numbers
  therefore produces a correspondingly large (possibly sparse) database.
  Sparse regions cost no blocks until written, but the logical size can be
  very large.  Dedicate whole filesystems with modest inode numbers to the
  array to keep this bounded.
* `assign.db` is indexed by `eno` and never shrinks; its size is proportional
  to the number of `eno` values ever allocated.
* Every persistent offset (`ino * msz`, `eno * 16`) is range-checked against
  `INT64_MAX` before use, because `off_t` is signed.  An index that cannot be
  represented fails the read/write rather than wrapping.
* `eno` does not wrap and is not reused.  If `eno` reached `UINT64_MAX`,
  allocation would fail (`emuxfs_state_eno_next_acquire` returns an error)
  rather than reuse a value.

## Version 2 (proposed, not implemented)

Version 2 is **not** implemented and must not be assumed.  It is recorded here
so a future decision is explicit.  It would be required to add any of:

* a checksum/magic/generation to `state.db` for torn-write detection;
* protected `mtime` (seconds and nanoseconds);
* OpenBSD file flags;
* a per-inode generation for stronger object identity.

A proposed `state.db` v2 layout (bytes on disk, no C structs):

```
offset 0,  4 bytes  magic 'E','M','S','D'
offset 4,  2 bytes  version = 2 (LE)
offset 6,  2 bytes  reserved = 0
offset 8,  8 bytes  generation (LE)
offset 16, 40 bytes payload: seq, mounted, working, restoring, degraded
offset 56, 4 bytes  crc32 of bytes [0,56), stored LE
total 60 bytes
```

Two such slots (A at offset 0, B at offset 60) would be written alternately;
the valid slot with the highest generation wins, and generation saturates
rather than wraps.  `muxfs.conf`'s `format_version` remains the authoritative
discriminator between v1 (40-byte file, no magic) and v2 (magic present), so a
reader never guesses from size alone.

A `crc32` over the header, generation and payload (excluding the checksum
field itself) detects accidental torn writes; it is the same accidental-corruption
primitive already used for file data, and is not a cryptographic MAC.

If version 2 preserved OpenBSD file flags (`st_flags`), the restore ordering
would matter: `chflags(2)`/`fchflags(2)` would have to be applied **after**
content, ownership and timestamps, because `SF_IMMUTABLE`/`SF_APPEND` would
otherwise make the node impossible to write, truncate, chown or delete.  A
restore must also clear restrictive flags before removing or overwriting an
existing node.  Version 1 does not preserve flags, so it neither suffers nor
benefits from this ordering; it simply does not reproduce them.

A migration would be an explicit command, never automatic in `mount`, and
would refuse dirty or ambiguous arrays.  Multi-device upgrades must be
resumable and must never mount a mix of v1 and v2; the array's `array_uuid`
and per-device `format_version` allow an incomplete upgrade to be detected and
either completed or rolled back.  No such command exists yet.
