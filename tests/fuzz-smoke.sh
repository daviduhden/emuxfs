#!/bin/sh

# Run the bounded fuzz smoke test using libFuzzer when available, then fall
# back to the standalone mutation driver with optional AddressSanitizer.
# Compiler flags and the object list arrive as Make variables and must split
# into separate command arguments, matching the original Make recipe.
# shellcheck disable=SC2086

_cc=$1
_cflags=$2
_cppflags=$3
_runs=$4
_core_obj=$5

if ${_cc} ${_cflags} ${_cppflags} -DEMUXFS= -I. \
	-fsanitize=fuzzer,address -o tests/fuzz/fuzz_conf \
	tests/fuzz/fuzz_conf.c ${_core_obj} -lz >/dev/null 2>&1; then
	echo "== fuzzing the muxfs.conf parser with libFuzzer (${_runs} runs)"
	tests/fuzz/fuzz_conf -runs="${_runs}" -max_len=4096
else
	rm -f tests/fuzz/fuzz_conf
	_san=""
	if ${_cc} ${_cflags} ${_cppflags} -DEMUXFS= -I. -fsanitize=address \
		-o tests/fuzz/fuzz_conf_standalone \
		tests/fuzz/fuzz_conf.c tests/fuzz/fuzz_main.c \
		${_core_obj} -lz >/dev/null 2>&1; then
		_san=" + AddressSanitizer"
	elif ${_cc} ${_cflags} ${_cppflags} -DEMUXFS= -I. \
		-o tests/fuzz/fuzz_conf_standalone \
		tests/fuzz/fuzz_conf.c tests/fuzz/fuzz_main.c \
		${_core_obj} -lz >/dev/null 2>&1; then
		_san=""
	else
		echo "SKIP: no libFuzzer and cannot build the mutation driver"
		exit 0
	fi
	echo "== fuzzing the muxfs.conf parser with the mutation driver${_san} (${_runs} runs)"
	tests/fuzz/fuzz_conf_standalone "${_runs}"
fi
