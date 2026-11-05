# Audit implementation record

This record maps the ten approved audit proposals to implementation and
remaining runtime evidence. It is not a production-readiness certificate.

| Proposal | Implementation | Verification required on OpenBSD |
|----------|----------------|----------------------------------|
| 1. Honest validation results | CI requires root/FUSE; missing crash binary and failed fuzz builds fail | Complete CI on both architectures |
| 2. Instrument parser | Fuzz builds compile every core source with selected instrumentation in a temporary directory | Run bounded fuzz suite and inspect instrumentation output |
| 3. Mirror opening fallback | Shared open/opendir helper validates metadata, consults later mirrors, preserves permission denials | Missing file/directory and permissions integration regressions |
| 4. Intermediate symlinks | Documented final-component limitation and directed investigation | Execute isolated path-safety regression; do not infer race resistance from static review |
| 5. Multiple recovery sources | Preflight requires matching source sequence and epoch before destination reconstruction | Reject differing histories, retain single-source recovery and partial-corruption fallback |
| 6. Fatal diagnostics | Contextual errno helper preserves fatal termination and dirty markers | Observe injected I/O failure and retained recovery state |
| 7. Traceable stability | README describes required commit/run/architecture evidence instead of asserting unverified production readiness | Attach successful CI URLs and tested commit to release records |
| 8. Measure before optimizing | Optional sandboxed benchmark reports latency/throughput and sampled daemon RSS for file and directory workloads | Collect repeated comparable baselines with 1/2/3 mirrors |
| 9. Shared CI | Both architectures invoke scripts/ci-openbsd.pl | Complete both VM jobs |
| 10. Small refactors | Open/opendir common logic extracted; disabled restoration shortcut removed | Strict build and unchanged recovery suite |

## Investigation boundaries

Checksums, sequence numbers and a matching epoch do not authenticate a source
or prove it is the newest history. With conflicting histories, select one
source explicitly. Multi-source recovery still validates content against
metadata and ancestor checksums; the preflight is an additional constraint,
not a new persistent format.

`O_NOFOLLOW` applies to the final pathname component. Pinned root descriptors
and unveil reduce scope but do not establish safety against concurrent
replacement of intermediate backing directories. Do not modify backing trees
concurrently in production. A comprehensive component-by-component resolver
would require its own review across all filesystem operations.

No performance improvement is claimed without measurements. The benchmark
creates only a private child of the operator-designated disposable mount and
never formats devices. It samples RSS rather than reporting a kernel lifetime
high-water mark. Crash injection demonstrates process-crash recovery, not
power-loss safety.

## Validation levels

Shell/Perl syntax, shellcheck, checkmake, Perl contract tests and the agent
knowledge validator can be checked on the development host. The C build,
FUSE integration, path safety, stress and crash suites require OpenBSD; host
static checks must never be reported as substitutes. No benchmark baseline
has been collected by this implementation on Linux.

## Development-host checks observed

- Agent knowledge validator: PASS, with AK004 semantic-review warning reviewed.
- Validator contract suite: 22 tests passed.
- checkmake: relative and absolute Makefile paths passed.
- ShellCheck and shell syntax: fuzz and common CI scripts passed.
- Perl syntax: integration, crash, stress and benchmark passed.
- YAML lint: workflow passed.
- Benchmark harness: 18 CSV lines and cleanup checked on a temporary ordinary
  Linux directory; these are harness checks, not emuxfs performance results.
- mandoc: only STYLE/missing host manual database diagnostics observed; no
  WARNING/ERROR diagnostics.
- Complete diff inspected for invariants and whitespace. OpenBSD builds and
  runtime suites remain unverified on the Linux development host.
