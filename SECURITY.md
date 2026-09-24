# Security model and sandbox policy

emuxfs is storage software.  Its primary security goal is **silent
corruption is worse than an explicit failure**: an operation must either
succeed with verified data or fail loudly.  This is a correctness/integrity
goal, not a confidentiality goal.

## Threat model

In scope:

* Accidental corruption of a device (bit rot, interrupted writes, a device
  that went offline, external tools modifying a device by mistake).
* A device that is partially written, stale, or missing.
* A local user of the mounted filesystem manipulating names or paths to make
  the filesystem touch something it should not (`.muxfs`, `..`, symlinks).
* Bugs in the filesystem itself producing inconsistent on-disk state; these
  must be detectable and recoverable.

Out of scope:

* An attacker who can already write to the `.muxfs` databases or to a device
  with root privileges.  Such an attacker can forge checksums.
* Cryptographic authentication.  CRC32, MD5 and SHA-1 detect accidental
  corruption only; MD5 and SHA-1 are not collision resistant.
* Confidentiality.  emuxfs does not encrypt anything.
* Denial of service by a user who can fill a device.

### Threat classes

These are deliberately not conflated:

* **Accidental corruption** (bit rot, interrupted writes): detected by
  checksums and repaired when an agreeing source exists.
* **Hardware I/O failure**: reported as an I/O error and the device is marked
  degraded; it is not reported as checksum corruption until the data can
  actually be read.
* **A malicious backing-directory owner or local root** who can rewrite both
  data and the `.muxfs` databases: *not* defended against.  CRC32, MD5 and
  SHA-1 are integrity checks against accident, not authentication; such an
  attacker can forge them.  Likewise a device that is modified coherently (data
  and metadata together) is indistinguishable from a legitimate older or newer
  history; emuxfs refuses to choose in ambiguous repair situations rather than
  pretend to authenticate.  One thing that *is* defended against is
  redirection: the private directory and each private object are opened with
  `O_NOFOLLOW`, so replacing `.muxfs` (or a database) with a symlink causes a
  loud failure rather than a read or write to an attacker-chosen object.
* **An untrusted user of the mounted filesystem**: constrained by path
  sanitisation, `*at` operations relative to directory descriptors,
  `O_NOFOLLOW`, and the empty/`.`/`..`/`.muxfs` component checks.

## Privilege model

emuxfs runs as root.  It needs root to:

* `chown(2)` and `chmod(2)` nodes while formatting, restoring and applying
  `chown`/`chmod` operations;
* access the mirror directories regardless of their ownership.

FUSE callbacks do **not** run with root's effective identity.  The callbacks
switch the effective uid/gid to the requesting user (`emuxfs_eids_set`) around
each operation that touches the underlying filesystem, so the ordinary
permission checks of the mirror filesystem are enforced for that user.  The
daemon also drops root's supplementary groups before lowering its effective
uid (it keeps none of its own: its privileged work runs with euid 0), so a
callback cannot pass a group check that the requesting user's own credentials
would fail.  The requesting user's *supplementary* group list is not
reconstructed, so a group shared through a supplementary group may be
conservatively denied; this is deliberate (deny rather than over-grant) and
does not require reading `/etc/group` inside the sandbox.  This is otherwise
the same model as the original Multiplexed File System.

FUSE callbacks are executed serially: the native libfuse event loop
(`fuse_loop(3)`) is used, not `fuse_loop_mt(3)`, so no two callbacks can race
while the process's effective credentials are lowered, and no callback can
observe another callback's identity.  `fuse_setup(3)` is invoked with a null
multithreaded flag, which selects the single-threaded loop.

No privilege separation (privsep) was added.  A split would require passing
open directory descriptors, paths and results between privileged and
unprivileged processes, which is a large increase in the attack surface and
the code that must be audited.  For a storage daemon, the single-process model
with a tight pledge/unveil policy was judged to be the smaller risk.  This may
be revisited; the format is frozen at version 1, so it would not require a
format change.

## pledge(2) policy

The promise strings live in `sandbox.h` and the calls in `sandbox.c`; nothing
else in the program calls `pledge(2)`.  The promises are derived from the
system calls actually reachable after the pledge point:

| command        | promises                                                    |
|----------------|-------------------------------------------------------------|
| `version`      | `stdio`                                                     |
| `format`       | `stdio rpath wpath cpath fattr chown`                       |
| `audit`/`heal` | `stdio rpath wpath cpath fattr chown unix`                  |
| `sync`         | `stdio rpath wpath cpath fattr chown unix`                  |
| `mount`        | `stdio rpath wpath cpath fattr chown id unix`               |

Rationale:

* `stdio` — descriptor I/O (`read`/`write`/`pread`/`pwrite`), `fsync`,
  `ftruncate`, `mmap`, `getdents`, `gettimeofday`, `getentropy`,
  `kqueue`/`kevent`, `sysconf`.
* `rpath` — `openat` for read, `fstatat`, `fstatfs`/`fstatvfs`,
  `faccessat`, `readlinkat`.
* `wpath` — `openat` for write.
* `cpath` — `mkdirat`, `unlinkat`, `renameat`, `symlinkat`.
* `fattr` — `fchmodat`, `utimensat`.
* `chown` — `fchownat` with an arbitrary owner, used only by restore and by
  the `chown` operation.
* `id` — `seteuid`/`setegid`/`setgroups`, used by the FUSE callbacks.
* `unix` — `syslog(3)`, which uses an `AF_UNIX` socket.

`mount(2)` and `unmount(2)` are privileged and are not covered by any
promise.  The FUSE lifecycle therefore performs `fuse_setup()` (which mounts
the filesystem) **before** the process is pledged.  Teardown does not call
`unmount(2)`: `fuse_destroy()` invokes the destroy callback, which flushes
buffered data and persists the array, and the kernel detaches the filesystem
when the process closes the FUSE device on exit.  This avoids a pledge
violation during shutdown.  The high-level API uses the same descriptor
syscalls (`read`/`write`/`poll`/`sigaction`) as before, so the promise table
above is unchanged.

The promise set is deliberately a little broader than strictly necessary
(for example `wpath` is present for commands that mostly read) because the
cost of an over-tight promise in storage software is a killed process and
possible loss of a pending operation.  Each promise above is still tied to a
concrete operation.

## unveil(2) policy

Before any operation, each array directory is unveiled with `rwc`.  After the
last one, `unveil(NULL, NULL)` locks the policy.  The result:

* Only the specified devices are visible; no other path can be opened,
  created, renamed or removed.
* Nothing else is unveiled.  The FUSE device and the mount point are opened
  by `fuse_setup()` before the lock, so they do not need to remain visible.
* The mount point and every array directory are canonicalised (with
  `realpath(3)`) before the sandbox is configured.  A topology in which the
  mount point and a mirror directory contain one another is refused (see
  below): otherwise the serving process could traverse the mount it is
  serving and deadlock, and the mounted filesystem would become part of its
  own backing storage.

`mount` unveils the device directories after `fuse_setup()` has mounted the
filesystem, but before serving any request.  `format`, `audit`, `heal` and
`sync` unveil before they touch the devices.

`mount` refuses a self-referential topology: if the canonical mount point is
equal to, contains, or is contained in any canonical array directory, it
exits with an explicit diagnostic instead of mounting.  This is a
configuration error, not an untrusted-user attack; it is rejected because the
resulting deadlock or recursion would be silent and hard to diagnose.

## Path handling

* Every path from FUSE goes through `emuxfs_path_sanitize`, which strips
  leading slashes and rejects any path containing a `.`, `..`, empty or
  `.muxfs` component.  A path that reaches `.muxfs` is a bug or an attack,
  and is failed rather than resolved.
* Operations are relative to an already-open directory descriptor
  (`openat`/`fstatat`/`mkdirat`/`unlinkat`/`renameat`/`symlinkat`/
  `readlinkat`/`faccessat`), which removes most path-based TOCTOU windows.
* The private directory `.muxfs` is opened once with
  `O_RDONLY|O_DIRECTORY|O_NOFOLLOW`, and `muxfs.conf`, `state.db`, `meta.db`,
  `assign.db` and `lfile/` are then opened relative to that descriptor with
  `O_NOFOLLOW`.  A symlink planted at `.muxfs` itself (for example by an
  external tool or a mistakenly modified device) therefore cannot redirect a
  metadata read or write: the descriptor is pinned to the directory that was
  checked.  Long-lived descriptors use `O_CLOEXEC`.
* Opening a file with `O_TRUNC` is turned into the normal truncate operation
  before the file is opened, so the truncation updates `meta.db` and the
  content checksum.  `O_TRUNC` is never passed to `openat(2)` directly, which
  would truncate the backing file behind emuxfs's back and leave `meta.db`
  describing the old contents.
* Restoring a regular file refuses to follow a symlink (`O_NOFOLLOW`) and
  removes a pre-existing node of the wrong type first.

## Logging

Diagnostics go to `syslog(3)` under the `user` facility, using
`openlog_r`/`vsyslog_r` with per-instance state.  Messages name the device
index and the operation, and distinguish detection from repair.  They may
include file paths, which can be sensitive; restrict the permissions of the
log file as described in emuxfs(1).  Logging is never used as a substitute for
propagating an error to the caller.

## Known gaps

* No cryptographic integrity: a malicious writer with device access can forge
  checksums.
* Automatic healing refuses to repair a node when two or more mounted devices
  hold internally valid but *different* copies of it: a checksum proves a copy
  matches its own metadata, not that it is the newest or the authoritative
  one, so emuxfs does not pick a "first valid" source.  `sync`, where the
  operator designates the source, is the explicit override; see RECOVERY.md.
  A single internally valid copy is still used even if it might be stale,
  because no metadata distinguishes "stale but coherent" from "current".
* No encryption, no quotas, no file locking (`flock` is rejected with
  `EOPNOTSUPP`), and no hard links.
* Timestamps are not checksummed, so timestamp corruption is not detected.
