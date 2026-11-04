# Perl static-analysis remediation and CI conversion

## Baseline and scope

Five project-maintained Perl scripts were present at the start: test.pl,
benchmark.pl, and integration/crash/parallel.pl. No Perl modules, generated
Perl or vendored Perl were found. The existing dirty worktree contained the
previous authorized audit changes and was retained. The subsequent request
added scripts/ci-openbsd.pl, replacing the shared shell runner; final scope
is six scripts. No dependency installation or persistent environment changes.

Interpreter: Perl 5.44.0, x86_64-linux-thread-multi. Perl::Critic 1.156 from
/var/home/david/perl5/bin; local PERL5LIB prefix lib/perl5. No repository or
checked user .perlcriticrc was present; default policies and existing source
annotations remained active. No policies disabled or new suppressions added.
The supported runtime is OpenBSD base Perl; no new Perl features or non-core
modules were introduced. Compatibility was inspected, not executed against
an older interpreter; no numeric minimum Perl version is documented.

Baseline syntax: all five scripts passed. OpenBSD/FUSE integration, stress,
crash and legacy suites were not executed: Linux host, no OpenBSD FUSE runtime
or built emuxfs binaries. Legacy test.conf is destructive operator input and
was not used. Initial findings: 1659 (severity 5: 0, 4: 11, 3: 15, 2: 532,
1: 1101). Complete baseline and pass output are retained in perlcritic/.

## Ordered remediation passes

Threshold outputs are cumulative; the following counts do not double-count
findings from higher severity levels.

| Pass | Newly encountered at that severity | Remaining at that severity (original five files) | Change |
|------|------------------------------------|--------------------------------------------------|--------|
| 5 | 0 | 0 | No correction required |
| 4 | 11 | 7 | Localize benchmark signal handlers; explicit helper returns |
| 3 | 15 | 9 | Explicit undef record separator; whitespace-free regex /x formatting |
| 2 | 532 | 530 | Check configuration/file-read closes and partial-read closes |
| 1 | 1092 after prior passes (1101 baseline) | 1040 | Brace explicit print handles; preserve established quoting/precedence |

Every pass reran Perl::Critic at its threshold, all five syntax checks and
whitespace validation. An initially invalid braced bareword standard-handle
rewrite was detected by syntax checks and corrected to explicit glob handles
before final validation. It is not counted as a passing intermediate check.
The original five files finish at 1586 diagnostics: 73 fewer than baseline.
The new CI runner was separately checked at 5,4,3,2,1, then included in the
final six-file severity-1 audit (default exit 2 means findings remain).

## Meaningful corrections and limitations

- Benchmark signal handlers now have explicit local scope; helper return
  contracts are explicit. These are maintainability improvements.
- Regex /x additions contain no literal whitespace and retain ASCII classes.
  Localizing the slurp record separator to explicit undef preserves semantics.
- Ordinary read/configuration closes now propagate failure through the
  existing die/undef contracts; partial-read and path-probe failures are
  recorded. This improves failure detection, without changing successful I/O.
- The seek check explicitly tests definedness. Perl's sysseek uses `0 but true`
  at offset zero, so the original boolean check was not a demonstrated defect;
  this is a clarity improvement, not a claimed correctness fix.
- Braced lexical and glob output handles clarify print dispatch without
  changing message text or output routing.
- CI now runs with base Perl, list-form execution and scoped environment.
  Captured children retain raw wait status. Requirements and ordinary failure
  exit 1 remain. STYLE diagnostics are allowed; WARNING/ERROR and failed
  execution are rejected. The previous transient integration.log is replaced
  by captured output printed after completion. Test output is kept in memory,
  so extremely large CI logs remain a resource limitation.
- CI orchestration is covered by temporary mock commands; source prerequisites
  and installed paths are substituted in a temporary copy, without mounting,
  installing or changing the host. This does not establish OpenBSD behavior.

## Every remaining diagnostic

[remaining.tsv](perlcritic/remaining.tsv) lists every final finding with
severity, policy, file, line, column, diagnostic and assessment. Findings are
not suppressed. Some are intentional conventions, others explicitly deferred
robustness/structural improvements. In particular, unchecked fixture writes
must not be interpreted as proven safe merely because other diagnostic prints
are best effort. Wholesale error-propagation changes require OpenBSD regression
coverage and could mask the original test failure or alter cleanup.

## Counts by policy

| Severity | Policy | Baseline five files | Final six files |
|----------|--------|---------------------|-----------------|
| 4 | Variables::RequireLocalizedPunctuationVars | 9 | 7 |
| 4 | Subroutines::RequireFinalReturn | 2 | 0 |
| 3 | Variables::RequireInitializationForLocalVars | 1 | 0 |
| 3 | Subroutines::ProhibitExcessComplexity | 2 | 2 |
| 3 | RegularExpressions::RequireExtendedFormatting | 5 | 0 |
| 3 | Modules::ProhibitExcessMainComplexity | 1 | 1 |
| 3 | ErrorHandling::RequireCarping | 5 | 5 |
| 3 | ControlStructures::ProhibitNegativeExpressionsInUnlessAndUntilConditions | 1 | 1 |
| 2 | Variables::ProhibitPunctuationVars | 190 | 206 |
| 2 | ValuesAndExpressions::RequireNumberSeparators | 6 | 6 |
| 2 | ValuesAndExpressions::ProhibitNoisyQuotes | 4 | 5 |
| 2 | ValuesAndExpressions::ProhibitMagicNumbers | 50 | 57 |
| 2 | ValuesAndExpressions::ProhibitEscapedCharacters | 1 | 1 |
| 2 | ValuesAndExpressions::ProhibitEmptyQuotes | 2 | 2 |
| 2 | RegularExpressions::RequireLineBoundaryMatching | 34 | 36 |
| 2 | RegularExpressions::RequireDotMatchAnything | 34 | 36 |
| 2 | Modules::RequireVersionVar | 5 | 6 |
| 2 | InputOutput::RequireCheckedClose | 19 | 12 |
| 2 | ControlStructures::ProhibitPostfixControls | 174 | 180 |
| 2 | ControlStructures::ProhibitCStyleForLoops | 13 | 13 |
| 2 | BuiltinFunctions::ProhibitBooleanGrep | 0 | 1 |
| 1 | ValuesAndExpressions::ProhibitInterpolationOfLiterals | 618 | 618 |
| 1 | RegularExpressions::ProhibitEscapedMetacharacters | 3 | 3 |
| 1 | RegularExpressions::ProhibitEnumeratedClasses | 4 | 4 |
| 1 | InputOutput::RequireCheckedSyscalls | 133 | 141 |
| 1 | InputOutput::RequireBracedFileHandleWithPrint | 57 | 3 |
| 1 | CodeLayout::RequireTidyCode | 2 | 4 |
| 1 | CodeLayout::ProhibitParensWithBuiltins | 284 | 285 |

## Exact validation commands and results

For critic commands, invocation environment was:

```sh
export PATH=/var/home/david/perl5/bin:$PATH
export PERL5LIB=/var/home/david/perl5/lib/perl5
perlcritic --version
perl -v
```

For each N in 5, 4, 3, 2, 1, before/after original-five-file outputs:

```sh
perlcritic --severity N --verbose '%s|%p|%f|%l|%c|%m\n' test.pl tests/benchmark.pl tests/integration/*.pl
```

The added runner was checked at every threshold with the same format and
`scripts/ci-openbsd.pl` as its input. Final audit (1635 diagnostics across six files: 0 at severity 5, 7 at 4,
9 at 3, 561 at 2 and 1058 at 1):

```sh
perlcritic --severity 1 --verbose '%s|%p|%f|%l|%c|%m\n' test.pl scripts/ci-openbsd.pl tests/benchmark.pl tests/integration/*.pl
for f in test.pl scripts/ci-openbsd.pl tests/benchmark.pl tests/integration/*.pl; do perl -c "$f"; done
python3 tests/ci-perl-check.py
yamllint .github/workflows/ci.yml
git diff --check
python3 scripts/validate-agent-knowledge.py --scope worktree
```

All six syntax checks and CI mock fixtures passed. Benchmark invocation was
also checked with missing arguments, missing directory, counts 0/-1/x/newline,
and two ordinary Linux temporary-directory runs: 18 CSV lines and complete
cleanup each time. These are harness checks, not filesystem performance.
No shell script was changed in this remediation; ShellCheck does not analyze
the new Perl runner. The previously requested shared CI shell file was removed.

## Assessment

Static-analysis findings were reduced without global exclusions, new modules
or broad quote/precedence churn. Remaining diagnostics are transparent; this
is not full Perl::Critic compliance. OpenBSD runtime and older-base-Perl checks
remain required before deployment. Further structural refactoring is justified
only with measured maintenance benefit and runtime regression evidence.
