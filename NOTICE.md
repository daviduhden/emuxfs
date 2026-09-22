# NOTICE

**The Enhanced Multiplexed File System (emuxfs)**
Author: David Uhden Collado <daviduhden@gmail.com>

emuxfs is a fork of the original **muxfs** (the Multiplexed File System) by
Stephen D. Adams <stephen@sdadams.org>.

## Copyright and licensing

* The original muxfs code is copyright (c) 2022 Stephen D. Adams and is
  distributed under the ISC licence reproduced in [`LICENSE.md`](LICENSE.md).
  All original copyright notices are retained unmodified in the source files.
* New and modified files in the emuxfs fork are copyright (c) 2026 David
  Uhden Collado and are distributed under the same ISC terms.
* The original project's rule that patches had to assign their copyright to
  Stephen D. Adams applied to the upstream muxfs project.  It does not apply
  to this fork, whose modifications remain under the ISC licence and the
  attributions above.

## Naming

The project, the executable and the internal symbol prefix are `emuxfs`, but
the persistent names are kept from muxfs so that existing arrays remain
usable: the private directory is `.muxfs` and its configuration file is
`muxfs.conf`.  Branding does not override on-disk compatibility.

## Files from the original muxfs

`CHANGES.md`, `GLOSSARY.md`, `LICENSE.md`, `README.md`, `chk.c`, `chk.h`,
`conf.c`, `desc.c`, `dev.c`, `ds.c`, `ds.h`, `ds_malloc.c`, `format.c`,
`lfile.c`, `mount.c`, `emuxfs.c`, `emuxfs.h`, `ops.c`, `ops.h`, `scan.c`,
`state.c`, `sync.c`, `test.conf.dist`, `test.pl`, `util.c`, `version.c`.

## Files and changes introduced by emuxfs

`NOTICE.md`, `ARCHITECTURE.md`, `ON_DISK_FORMAT.md`, `RECOVERY.md`,
`SECURITY.md`, `TESTING.md`, `COMPATIBILITY.md`, `sandbox.c`, `sandbox.h`,
`fault.c`, `fault.h`, `tests/`, `.github/workflows/`.  Existing files were
also modified; the changes are visible in the project history.

## Removed

* `gen.c` generated an opaque checksum type that required undefined-behaviour
  type punning; the checksum state is now a concrete, correctly aligned type
  in `chk.h`.
* `mount.h` duplicated an incompatible definition of `struct emuxfs_args` and
  was not included anywhere.
* `SHA512SUMS` listed hashes of the repository's own files and was regenerated
  in the same change as those files, so it provided no independent guarantee.
  It was removed rather than kept as ceremony.
* `unity.c` was removed: the incremental build is the only supported build and
  the fault-injection build now reuses the same objects with
  `-DEMUXFS_FAULT_INJECTION`.
