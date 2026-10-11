#!/bin/sh
# Negative-compilation tests for the vendored <stdckdint.h>.
#
# C23 7.20.1 restricts the operands and the object pointed to by the result
# to integer types other than plain char, bool, a bit-precise integer type,
# and enumerated types.  These tests assert that the shim (and, underneath it,
# the compiler overflow builtins) reject the invalid uses that the supported
# compilers diagnose.  The diagnostic text is not compared, only whether the
# translation unit is accepted.
#
# Two invalid uses are knowingly not covered here because GCC's overflow
# builtins accept them without a diagnostic: a plain char result/operand and
# an enumerated operand.  They are documented in the audit report; a valid
# C23 program never uses them, so they do not affect conformance of accepted
# programs.
#
# Exits non-zero if a rejected use was accepted or a valid use was rejected.

set -u

TESTS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
COMPAT_DIR=$(CDPATH='' cd -- "$TESTS_DIR/.." && pwd)

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -pipe}
WARNINGS=${WARNINGS:--Wall -Wextra -Wpedantic}
CSTD=${CSTD:--std=c23}
FORCE=${FORCE:?FORCE (the force-fallback macro) must be set}

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail=0

# $1 = label, $2 = translation unit.
must_reject() {
	printf '%s\n' "$2" >"$tmp/case.c"
	if # shellcheck disable=SC2086
	    "$CC" $CFLAGS $CSTD $WARNINGS -I"$COMPAT_DIR" -D"$FORCE" \
		-fsyntax-only "$tmp/case.c" >/dev/null 2>&1
	then
		printf 'FAIL: %s was accepted\n' "$1"
		fail=1
	else
		printf 'ok: %s rejected\n' "$1"
	fi
}

# $1 = label, $2 = translation unit.
must_accept() {
	printf '%s\n' "$2" >"$tmp/case.c"
	if # shellcheck disable=SC2086
	    "$CC" $CFLAGS $CSTD $WARNINGS -I"$COMPAT_DIR" -D"$FORCE" \
		-fsyntax-only "$tmp/case.c" >/dev/null 2>&1
	then
		printf 'ok: %s accepted\n' "$1"
	else
		printf 'FAIL: %s was rejected\n' "$1"
		fail=1
	fi
}

PRE='#include <stdckdint.h>'
INT_MAIN='int main(void) { int r = 0; return ckd_add(&r, 1, 2); }'

must_accept valid_control "$PRE
$INT_MAIN"

must_reject bool_destination "$PRE
int main(void) { _Bool b = 0; return ckd_add(&b, 1, 2); }"

must_reject float_destination "$PRE
int main(void) { float f = 0; return ckd_add(&f, 1, 2); }"

must_reject non_pointer_destination "$PRE
int main(void) { int r = 0; return ckd_add(r, 1, 2); }"

must_reject enum_destination "$PRE
enum e { A };
int main(void) { enum e v = A; return ckd_add(&v, 1, 2); }"

must_reject float_operand "$PRE
int main(void) { int r = 0; float f = 0; return ckd_add(&r, f, 1); }"

must_reject pointer_operand "$PRE
int main(void) { int r = 0; int *p = 0; return ckd_add(&r, p, 1); }"

# Narrow but valid destination types must be accepted.
must_accept signed_char_destination "$PRE
int main(void) { signed char c = 0; return ckd_add(&c, 1, 2); }"

must_accept unsigned_long_long_destination "$PRE
int main(void) { unsigned long long u = 0; return ckd_mul(&u, 2, 3); }"

if [ "$fail" -ne 0 ]; then
	printf 'stdckdint compile-fail: failures\n' >&2
	exit 1
fi
printf 'stdckdint compile-fail: all checks passed\n'
