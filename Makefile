# Makefile for emuxfs (The Enhanced Multiplexed File System).
#
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
#
# Toolchain policy: clang(1) only, C23 only.  The warning policy matches the
# other projects (openbar, openutils, wip-openbsd-src):
#
#   default:  -std=c23 -Wall -Wextra -Wpedantic
#   check:    the above plus -Wshadow -Wformat=2 -Wundef
#             -Wstrict-prototypes -Wmissing-prototypes -Wconversion
#             -Wsign-conversion, and -Werror
#
# emuxfs uses the FUSE implementation shipped with OpenBSD (libfuse in the
# base system, FUSE 2.6 high-level API).  There is no external FUSE dependency.
#
# GNU make extensions are not used; this file targets OpenBSD make(1).

CC = clang
DEBUGGER = lldb
CSTD = -std=c23

CFLAGS ?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CFLAGS += ${CSTD} ${WARNINGS}

# emuxfs uses the FUSE implementation shipped with OpenBSD (libfuse in the
# base system, FUSE 2.6 high-level API).  There is no external FUSE
# dependency and no libfuse3 requirement; see COMPATIBILITY.md.
#
# OpenBSD installs the base FUSE headers under /usr/include/fuse (fuse.h,
# fuse_common.h, fuse_opt.h, fuse_lowlevel.h) and the library as
# /usr/lib/libfuse.a, so the include directory must be named explicitly.  The
# same directory is what the base system's fuse.pc advertises.
CPPFLAGS += -I/usr/include/fuse
LDLIBS += -lfuse -lz

# Full strict set (openutils "check" policy).
CHECK_WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes \
	-Wconversion -Wsign-conversion

PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/sbin
MANDIR ?= ${PREFIX}/man

EMUXFS_DS_MALLOC ?= 0
.if ${EMUXFS_DS_MALLOC} == 1
DS = ds_malloc
.else
DS = ds
.endif

PROG = emuxfs

OBJ = chk.o \
	conf.o \
	desc.o \
	dev.o \
	${DS}.o \
	fault.o \
	format.o \
	lfile.o \
	mount.o \
	emuxfs.o \
	ops.o \
	sandbox.o \
	scan.o \
	state.o \
	sync.o \
	util.o \
	version.o

# Everything except the FUSE frontend and the program entry point; used by the
# headless unit tests so that integrity logic can be exercised without FUSE.
CORE_OBJ = chk.o \
	conf.o \
	desc.o \
	dev.o \
	${DS}.o \
	fault.o \
	format.o \
	lfile.o \
	sandbox.o \
	scan.o \
	state.o \
	sync.o \
	util.o \
	version.o

# Same objects built with fault injection enabled.  This is the normal
# modular build plus -DEMUXFS_FAULT_INJECTION, not a unity build.
FAULT_OBJ = chk.fault.o \
	conf.fault.o \
	desc.fault.o \
	dev.fault.o \
	${DS}.fault.o \
	fault.fault.o \
	format.fault.o \
	lfile.fault.o \
	mount.fault.o \
	emuxfs.fault.o \
	ops.fault.o \
	sandbox.fault.o \
	scan.fault.o \
	state.fault.o \
	sync.fault.o \
	util.fault.o \
	version.fault.o

all: ${PROG}

${PROG}: ${OBJ}
	${CC} ${LDFLAGS} -o $@ ${OBJ} ${LDLIBS}

.SUFFIXES: .c .o .fault.o
.c.o: emuxfs.h chk.h ds.h fault.h ops.h sandbox.h
	${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -c -o $@ $<

.c.fault.o: emuxfs.h chk.h ds.h fault.h ops.h sandbox.h
	${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -DEMUXFS_FAULT_INJECTION -c -o $@ $<

# Strict-warning build: same policy as the openutils 'check' target.
# Any diagnostic is treated as an error; -Wno-* is not used.  -k keeps the
# build going after the first failing object so that a CI run reports every
# diagnostic at once instead of one file per run.
check:
	${MAKE} clean
	${MAKE} -k WARNINGS="${CHECK_WARNINGS} -Werror" ${PROG}

# Headless unit tests: no FUSE, no mounts.
unittest: tests/unit/test_core
tests/unit/test_core: tests/unit/test_core.c ${CORE_OBJ}
	${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -I. \
	    -o $@ tests/unit/test_core.c ${CORE_OBJ} -lz

# Full integration suite (Perl).  Requires root and a working FUSE device; it
# creates and destroys its own temporary sandbox.
integration: ${PROG}
	perl tests/integration/integration.pl

# Parallel-load stress test (Perl).  Requires root and a working FUSE device;
# it creates and destroys its own temporary sandbox.
paralleltest: ${PROG}
	EMUXFS="$(pwd)/emuxfs" perl tests/integration/parallel.pl

# Legacy end-to-end suite (Perl).  Requires test.conf and root.
legacytest: ${PROG}
	perl test.pl

test: unittest
check-tests: test integration

# Optional libFuzzer target.  Not built by 'all'; 'fuzz-smoke' runs a bounded
# session of it as part of the stability target.
fuzz-conf: tests/fuzz/fuzz_conf.c ${CORE_OBJ}
	${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -I. \
	    -fsanitize=fuzzer,address \
	    -o tests/fuzz/fuzz_conf tests/fuzz/fuzz_conf.c ${CORE_OBJ} -lz

# Bounded fuzzing smoke run: the muxfs.conf parser must survive a fixed number
# of mutations without a crash or sanitizer report.  libFuzzer is used when the
# toolchain provides it; otherwise the standalone mutation driver
# (tests/fuzz/fuzz_main.c) is built, under AddressSanitizer if available.  This
# keeps the step meaningful on OpenBSD's base clang, which has no libFuzzer.
FUZZ_RUNS ?= 20000
fuzz-smoke: ${CORE_OBJ}
	@if ${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -I. \
	    -fsanitize=fuzzer,address -o tests/fuzz/fuzz_conf \
	    tests/fuzz/fuzz_conf.c ${CORE_OBJ} -lz >/dev/null 2>&1; then \
		echo "== fuzzing the muxfs.conf parser with libFuzzer (${FUZZ_RUNS} runs)"; \
		tests/fuzz/fuzz_conf -runs=${FUZZ_RUNS} -max_len=4096; \
	else \
		rm -f tests/fuzz/fuzz_conf; \
		san=""; \
		if ${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -I. -fsanitize=address \
		    -o tests/fuzz/fuzz_conf_standalone \
		    tests/fuzz/fuzz_conf.c tests/fuzz/fuzz_main.c \
		    ${CORE_OBJ} -lz >/dev/null 2>&1; then \
			san=" + AddressSanitizer"; \
		elif ${CC} ${CFLAGS} ${CPPFLAGS} -DEMUXFS= -I. \
		    -o tests/fuzz/fuzz_conf_standalone \
		    tests/fuzz/fuzz_conf.c tests/fuzz/fuzz_main.c \
		    ${CORE_OBJ} -lz >/dev/null 2>&1; then \
			san=""; \
		else \
			echo "SKIP: no libFuzzer and cannot build the mutation driver"; \
			exit 0; \
		fi; \
		echo "== fuzzing the muxfs.conf parser with the mutation driver$$san (${FUZZ_RUNS} runs)"; \
		tests/fuzz/fuzz_conf_standalone ${FUZZ_RUNS}; \
	fi

# Crash-consistency build: the normal modular objects plus fault injection.
faultbuild: emuxfs-fault
emuxfs-fault: ${FAULT_OBJ}
	${CC} ${LDFLAGS} -o $@ ${FAULT_OBJ} ${LDLIBS}

faulttest: ${PROG} emuxfs-fault
	EMUXFS="$(pwd)/emuxfs" EMUXFS_FAULT="$(pwd)/emuxfs-fault" \
	    perl tests/integration/crash.pl

# Complete pre-release validation.  CI runs the same sequence on OpenBSD 7.9
# amd64 and arm64; this target is the local equivalent.  The FUSE suites
# require root and /dev/fuse0 and skip themselves when unavailable; fuzzing
# needs a clang with libFuzzer.  Nothing else may be claimed as "stable"
# until this passes.
stability: check
	${MAKE} unittest
	./tests/unit/test_core
	${MAKE} integration
	${MAKE} paralleltest
	${MAKE} faulttest
	${MAKE} fuzz-smoke

install: ${PROG}
	install -o root -g bin -m 0755 ${PROG} ${DESTDIR}${BINDIR}/${PROG}
	install -o root -g bin -m 0644 emuxfs.1 \
	    ${DESTDIR}${MANDIR}/man1/emuxfs.1

clean:
	rm -f ${PROG} emuxfs-fault \
	    ${FAULT_OBJ} \
	    tests/unit/test_core tests/fuzz/fuzz_conf \
	    tests/fuzz/fuzz_conf_standalone \
	    ${OBJ} >/dev/null 2>&1 || true

.PHONY: all check unittest integration paralleltest legacytest test check-tests \
	fuzz-conf fuzz-smoke faultbuild faulttest stability install clean