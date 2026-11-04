#!/bin/sh

# Compile every parser dependency with the selected instrumentation. Probe
# toolchain support separately so source compilation failures remain failures.
# Compiler flags and source lists are intentionally split as Make arguments.
# shellcheck disable=SC2086
set -eu

_cc=$1
_cflags=$2
_cppflags=$3
_runs=$4
_core_src=$5
_mode=${6:-run}
_fuzz_tmp=$(mktemp -d "${TMPDIR:-/tmp}/emuxfs-fuzz-build.XXXXXXXX")
trap 'rm -rf "$_fuzz_tmp"' EXIT HUP INT TERM
printf '%s\n' '#include <stddef.h>' '#include <stdint.h>' \
    'int LLVMFuzzerTestOneInput(const uint8_t *p, size_t n) { (void)p; (void)n; return 0; }' \
    >"$_fuzz_tmp/probe.c"
_san='-fsanitize=fuzzer-no-link,address'
_link_san='-fsanitize=fuzzer,address'
_output=tests/fuzz/fuzz_conf
_driver=''
if ${_cc} ${_cflags} -fsanitize=fuzzer,address \
    "$_fuzz_tmp/probe.c" -o "$_fuzz_tmp/probe" >"$_fuzz_tmp/probe.log" 2>&1; then
    echo '== fuzz instrumentation: libFuzzer + AddressSanitizer'
else
    if [ "$_mode" = build ]; then
        cat "$_fuzz_tmp/probe.log" >&2
        echo 'FAIL: fuzz-conf requires libFuzzer' >&2
        exit 1
    fi
    printf '%s\n' 'int main(void) { return 0; }' >"$_fuzz_tmp/probe.c"
    _san='-fsanitize=address'
    _link_san=$_san
    if ! ${_cc} ${_cflags} $_san "$_fuzz_tmp/probe.c" \
        -o "$_fuzz_tmp/probe" >"$_fuzz_tmp/probe.log" 2>&1; then
        _san=''
        _link_san=''
        echo '== sanitizer unavailable; mutation driver runs without instrumentation'
    else
        echo '== fuzz instrumentation: AddressSanitizer mutation driver'
    fi
    _output=tests/fuzz/fuzz_conf_standalone
    _driver=tests/fuzz/fuzz_main.c
fi
set --
for _source in $_core_src; do
    _object="$_fuzz_tmp/$(basename "$_source" .c).o"
    ${_cc} ${_cflags} ${_cppflags} -DEMUXFS= -I. $_san \
        -c "$_source" -o "$_object"
    set -- "$@" "$_object"
done
${_cc} ${_cflags} ${_cppflags} -DEMUXFS= -I. $_link_san \
    -o "$_output" tests/fuzz/fuzz_conf.c $_driver "$@" -lz
[ "$_mode" != build ] || exit 0
if [ -z "$_driver" ]; then
    "$_output" -runs="$_runs" -max_len=4096
else
    "$_output" "$_runs"
fi
