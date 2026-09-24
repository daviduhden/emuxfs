/* fuzz_main.c */
/*
 * Copyright (c) 2022 Stephen D. Adams <stephen@sdadams.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see NOTICE.md).
 *
 * Standalone mutation driver for LLVMFuzzerTestOneInput().  It exists so that
 * 'make fuzz-smoke' still exercises the parser on toolchains whose clang has
 * no libFuzzer runtime (OpenBSD's base clang, for example).  It is combined
 * with fuzz_conf.c and run under AddressSanitizer when that is available, and
 * without it otherwise.  It is not built by the normal build.
 *
 *   cc ... fuzz_conf.c fuzz_main.c -o fuzz_conf_standalone
 *   ./fuzz_conf_standalone [runs]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static uint64_t
xs64(uint64_t *s)
{
	uint64_t x;

	x = *s;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return x;
}

int
main(int argc, char *argv[])
{
	/*
	 * A syntactically complete configuration, used as the base for
	 * mutations in half of the iterations.  The rest are random bytes.
	 */
	static const char seed[] =
	    "format_version=1\n"
	    "array_uuid=00112233-4455-6677-8899-aabbccddeeff\n"
	    "chk_alg=md5\n"
	    "seq_zero_time=0\n";
	uint64_t state = 0x9e3779b97f4a7c15ULL;
	uint8_t buf[4096];
	unsigned long runs, i, muts;
	size_t len, j;

	runs = 20000;
	if (argc > 1) {
		char *end;

		end = nullptr;
		runs = strtoul(argv[1], &end, 10);
		if ((end == nullptr) || (*end != '\0'))
			runs = 20000;
	}

	for (i = 0; i < runs; ++i) {
		if ((i & 1UL) == 0) {
			len = sizeof(seed) - 1;
			memcpy(buf, seed, len);
			muts = 1 + (unsigned long)(xs64(&state) % 8);
			for (j = 0; j < muts; ++j)
				buf[xs64(&state) % len] = (uint8_t)xs64(&state);
		} else {
			len = 1 + (size_t)(xs64(&state) % sizeof(buf));
			for (j = 0; j < len; ++j)
				buf[j] = (uint8_t)xs64(&state);
		}
		LLVMFuzzerTestOneInput(buf, len);
	}

	printf("fuzz: %lu inputs parsed without a crash\n", runs);
	return 0;
}
