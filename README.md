# The Enhanced Multiplexed File System (emuxfs)

emuxfs is a mirroring, checksumming and self-healing filesystem layer for
OpenBSD.  It mirrors a filesystem tree across a series of directories and uses
checksum databases to validate and restore files automatically.

> **IMPORTANT**
>
> **DO NOT USE EMUXFS TO STORE IMPORTANT DATA YET.**
>
> emuxfs has not been demonstrated stable.  It fixes correctness defects, adds
> sandboxing, testing and CI, and turns the project into something that can be
> audited, but it has not yet accumulated the evidence required for production
> use.  Using it as the only copy of important data will likely lead to data
> loss.

## Identity and provenance

* Project name: **The Enhanced Multiplexed File System**, short name
  **emuxfs**.
* Author: **David Uhden Collado** <daviduhden@gmail.com>.
* The executable is `emuxfs`; the manual page is `emuxfs(1)`; the on-disk
  directory is `.muxfs`; internal symbols use the `emuxfs_` prefix.

emuxfs began as a fork of the original Multiplexed File System written by
Stephen D. Adams <stephen@sdadams.org>, whose copyright notices are retained
in every file.  See [`NOTICE.md`](NOTICE.md) for provenance and the list of
files that come from the original project.

## Stability levels

Stability is described in terms of evidence rather than aspiration:

* **Experimental**: builds, but no automated evidence of correct behaviour.
* **Tested**: builds cleanly and passes the unit, integration and FUSE suites
  in CI on a real OpenBSD VM (amd64 and arm64), with no known correctness
  defects.
* **Hardened**: additionally, the sandbox (pledge/unveil) suite, the
  fault-injection suite (recovery from an interrupted create, update, delete,
  heal and sync), repeated mount/unmount cycles and a bounded parser fuzz run
  pass, and the on-disk format is frozen.

The evidence for both levels is one command, `make stability`: strict-warning
build, headless unit tests, the FUSE integration suite, the fault-injection
suite and a bounded libFuzzer run over the `muxfs.conf` parser.
`.github/workflows/ci.yml` runs the same sequence on OpenBSD 7.9 amd64 and
arm64.  A stability claim is never made from static analysis alone.

emuxfs is currently Experimental/Tested-in-progress.  It is not Hardened.

## Installation

```sh
# make
# make install
```

Build requirements: OpenBSD, `clang`, C17 and zlib.  The FUSE
implementation is the one shipped in the OpenBSD base system (FUSE 2.6
high-level API); there is no external FUSE dependency.  `make` builds one
translation unit per `.c` file; `make check` rebuilds with the project's strict
Clang warning set; `make faultbuild` builds the fault-injection variant from the
same modules.

The persistent `.muxfs` directory and `muxfs.conf` keep their historical
names so that arrays created by the original muxfs remain usable; only the
program-facing names are `emuxfs`.

## Usage

```
emuxfs format [-a checksum_algorithm] directory ...
emuxfs mount [-f] mount_point directory ...
emuxfs audit directory ...
emuxfs heal directory ...
emuxfs sync destination source ...
emuxfs version
```

Refer to the [emuxfs(1)](emuxfs.1) manual page for details.

## Documentation

| file | contents |
|------|----------|
| [`README.md`](README.md) | this file |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | internal structure and layering |
| [`ON_DISK_FORMAT.md`](ON_DISK_FORMAT.md) | the persistent format of the `.muxfs` directory |
| [`RECOVERY.md`](RECOVERY.md) | expected state after failures and how to recover |
| [`SECURITY.md`](SECURITY.md) | threat model, pledge/unveil policy, privilege model |
| [`TESTING.md`](TESTING.md) | unit, integration, crash and fuzz tests |
| [`COMPATIBILITY.md`](COMPATIBILITY.md) | OpenBSD 7.9, FUSE and format compatibility |
| [`NOTICE.md`](NOTICE.md) | provenance and licensing |
| [`CHANGES.md`](CHANGES.md) | change log |

## Contributing

Bug reports, usability reports and patches are welcome; please use the project
repository.

If you want to read the code then start with [`GLOSSARY.md`](GLOSSARY.md), then
[`emuxfs.h`](emuxfs.h), then [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Authors

* The Enhanced Multiplexed File System (emuxfs):
  David Uhden Collado <daviduhden@gmail.com>
* Original Multiplexed File System:
  Stephen D. Adams <stephen@sdadams.org>
