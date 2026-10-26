/*
 * Independent conformance test for the <stdckdint.h> shim.
 *
 * The system header (compiler resource header or libc) is not trusted here:
 * the fallback in compat/stdckdint.h is forced so that the code under test is
 * always the vendored implementation.  Every case checks both the overflow
 * indication and the value actually stored in the destination, and the
 * destination is compared against an independent arbitrary-precision oracle
 * built on __int128 rather than against another wrapper around the same
 * compiler builtin.
 *
 * The oracle reimplements the C23 7.20.1 contract from the mathematical
 * definition: interpret each operand's width-bit pattern in its own type,
 * compute the exact sum/difference/product, then convert that mathematical
 * value to the result type and record whether the conversion is
 * value-preserving.  The low bits of the two's complement result are compared
 * bit for bit.
 *
 * Exits non-zero if any check fails.
 */

#include <limits.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The header must announce itself with the C23 value. */
#ifndef __STDC_VERSION_STDCKDINT_H__
#error "stdckdint.h did not define __STDC_VERSION_STDCKDINT_H__"
#endif
#if __STDC_VERSION_STDCKDINT_H__ != 202311L
#error "unexpected __STDC_VERSION_STDCKDINT_H__"
#endif

/* __int128 is the reference arithmetic; silence the -Wpedantic complaint. */
__extension__ typedef __int128		i128;
__extension__ typedef unsigned __int128 u128;

static int failures;

#define CHECK(cond, what)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf(                                                \
			    "FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__); \
			failures++;                                            \
		}                                                              \
	} while (0)

static uint64_t
mask_of(unsigned width)
{
	return width >= 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

struct int_info {
	unsigned width;
	unsigned is_signed;
};

/* Value of a width-bit two's complement pattern in the given type. */
static i128
value_of(uint64_t bits, struct int_info in)
{
	uint64_t m = mask_of(in.width);

	bits &= m;
	if (in.is_signed && in.width < 64 &&
	    (bits & (UINT64_C(1) << (in.width - 1))))
		return (i128)bits - ((i128)1 << in.width);
	if (in.is_signed && in.width == 64)
		return (i128)(int64_t)bits;
	return (i128)bits;
}

/* Convert a mathematical value to the result type; returns overflow flag. */
static int
convert(i128 res, struct int_info rr, uint64_t *out)
{
	uint64_t m = mask_of(rr.width);
	int	 overflow;

	if (rr.is_signed) {
		i128 lo = -((i128)1 << (rr.width - 1));
		i128 hi = ((i128)1 << (rr.width - 1)) - 1;

		overflow = res < lo || res > hi;
	} else {
		overflow = res < 0 || (u128)res > (u128)m;
	}
	*out = (uint64_t)((u128)res & m);
	return overflow;
}

static int
oracle(int op, struct int_info ra, struct int_info rb, struct int_info rr,
    uint64_t a_bits, uint64_t b_bits, uint64_t *out)
{
	uint64_t ma, mb, a, b, m;
	u128	 amag, bmag, prod;
	int	 sa, sb, sign;

	if (op == 0)
		return convert(
		    value_of(a_bits, ra) + value_of(b_bits, rb), rr, out);
	if (op == 1)
		return convert(
		    value_of(a_bits, ra) - value_of(b_bits, rb), rr, out);

	/* Multiplication: track sign and magnitude so the product of two
	 * full-width unsigned operands still fits in unsigned __int128. */
	ma = mask_of(ra.width);
	mb = mask_of(rb.width);
	a = a_bits & ma;
	b = b_bits & mb;
	sa = ra.is_signed && (a >> (ra.width - 1) & 1);
	sb = rb.is_signed && (b >> (rb.width - 1) & 1);
	amag = sa ? (u128)(~a & ma) + 1 : (u128)a;
	bmag = sb ? (u128)(~b & mb) + 1 : (u128)b;
	prod = amag * bmag;
	sign = sa ^ sb;
	m = mask_of(rr.width);

	if (prod == 0) {
		*out = 0;
		return 0;
	}
	if (sign) {
		*out = (uint64_t)((~(prod & m)) + 1) & m;
		if (rr.is_signed)
			return prod > (u128)((i128)1 << (rr.width - 1));
		return 1;
	}
	*out = (uint64_t)(prod & m);
	if (rr.is_signed)
		return prod > (u128)(((i128)1 << (rr.width - 1)) - 1);
	return prod > (u128)m;
}

/* Boundary patterns; masked to the operand width at use. */
static const uint64_t vals[] = {
    UINT64_C(0),
    UINT64_C(1),
    UINT64_C(2),
    UINT64_C(3),
    UINT64_C(127),
    UINT64_C(128),
    UINT64_C(255),
    UINT64_C(256),
    UINT64_C(0x7fff),
    UINT64_C(0x8000),
    UINT64_C(0xffff),
    UINT64_C(0x10000),
    UINT64_C(0x7fffffff),
    UINT64_C(0x80000000),
    UINT64_C(0xffffffff),
    UINT64_C(0x100000000),
    UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000),
    UINT64_C(0xffffffffffffffff),
    UINT64_C(100),
    UINT64_C(1000),
    UINT64_C(65535),
};

/*
 * Run the exhaustive boundary grid for one (operand, operand, result) triple
 * and compare ckd_add/sub/mul against the oracle.
 */
#define RUN_CASE(AT, AS, AUT, BT, BS, BUT, RT, RS, RUT)                               \
	do {                                                                          \
		struct int_info ra = {sizeof(AT) * CHAR_BIT, (AS)};                   \
		struct int_info rb = {sizeof(BT) * CHAR_BIT, (BS)};                   \
		struct int_info rr = {sizeof(RT) * CHAR_BIT, (RS)};                   \
		size_t		i;                                                    \
		size_t		j;                                                    \
		int		k;                                                    \
                                                                                      \
		for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {                \
			for (j = 0; j < sizeof(vals) / sizeof(vals[0]); j++) {        \
				uint64_t av = vals[i] & mask_of(ra.width);            \
				uint64_t bv = vals[j] & mask_of(rb.width);            \
				AT	 a = (AT)(AUT)av;                             \
				BT	 b = (BT)(BUT)bv;                             \
				RT	 r;                                           \
				uint64_t want;                                        \
				int	 wf;                                          \
				int	 gf;                                          \
                                                                                      \
				for (k = 0; k < 3; k++) {                             \
					r = (RT)(RUT)UINT64_C(                        \
					    0x5a5a5a5a5a5a5a5a);                      \
					wf = oracle(                                  \
					    k, ra, rb, rr, av, bv, &want);            \
					switch (k) {                                  \
					case 0:                                       \
						gf = ckd_add(&r, a, b) != 0;          \
						break;                                \
					case 1:                                       \
						gf = ckd_sub(&r, a, b) != 0;          \
						break;                                \
					default:                                      \
						gf = ckd_mul(&r, a, b) != 0;          \
						break;                                \
					}                                             \
					if (gf != wf ||                               \
					    (uint64_t)(RUT)r != want) {               \
						if (failures < 40)                    \
							printf(                       \
							    "FAIL: %s "               \
							    "op=%d a=%#llx "          \
							    "b=%#llx gf=%d "          \
							    "wf=%d got=%#llx "        \
							    "want=%#llx\n",           \
							    #RT, k,                   \
							    (unsigned long long)      \
								av,                   \
							    (unsigned long long)      \
								bv,                   \
							    gf, wf,                   \
							    (unsigned long long)(RUT) \
								r,                    \
							    (unsigned long long)      \
								want);                \
						failures++;                           \
					}                                             \
				}                                                     \
			}                                                             \
		}                                                                     \
	} while (0)

/* Single-evaluation probes. */
static unsigned probe_calls;

static unsigned
probe(void)
{
	probe_calls++;
	return (1u);
}

static size_t	slots[4];
static unsigned slot_index;

static size_t *
next_slot(void)
{
	return (&slots[slot_index++]);
}

static void
test_hand_written(void)
{
	int	  i;
	size_t	  z;
	long long ll;
	_Bool	  ov;

	/* int: min/max, both directions, stored value on overflow. */
	i = 0;
	ov = ckd_add(&i, INT_MAX, 1);
	CHECK(ov && i == INT_MIN, "int MAX+1 wraps to MIN");

	ov = ckd_sub(&i, INT_MIN, 1);
	CHECK(ov && i == INT_MAX, "int MIN-1 wraps to MAX");

	ov = ckd_mul(&i, INT_MAX, 2);
	CHECK(ov && i == -2, "int MAX*2 wraps to -2");

	ov = ckd_mul(&i, INT_MIN, -1);
	CHECK(ov && i == INT_MIN, "int MIN*-1 wraps to MIN");

	ov = ckd_add(&i, 0, 0);
	CHECK(!ov && i == 0, "int 0+0");

	ov = ckd_sub(&i, INT_MIN, INT_MIN);
	CHECK(!ov && i == 0, "int MIN-MIN");

	/* size_t: unsigned wrapping and exact representability. */
	z = 0;
	ov = ckd_add(&z, (size_t)SIZE_MAX, (size_t)1);
	CHECK(ov && z == 0, "size_t MAX+1 wraps to 0");

	ov = ckd_sub(&z, (size_t)0, (size_t)1);
	CHECK(ov && z == SIZE_MAX, "size_t 0-1 wraps to MAX");

	ov = ckd_mul(&z, (size_t)SIZE_MAX, (size_t)2);
	CHECK(ov && z == SIZE_MAX - 1, "size_t MAX*2 wraps");

	ov = ckd_mul(&z, (size_t)0, (size_t)SIZE_MAX);
	CHECK(!ov && z == 0, "size_t 0*MAX");

	/* narrow destination: representability is judged against it. */
	{
		unsigned short h = 0;

		ov = ckd_add(&h, 40000, 40000);
		CHECK(ov && h == 14464, "ushort 40000+40000 wraps to 14464");
		ov = ckd_add(&h, 30000, 30000);
		CHECK(!ov && h == 60000, "ushort 30000+30000 fits");
	}

	/* operands are promoted to infinite precision, not to a common
	 * operand type: the mathematical sum exceeds UINT_MAX but fits the
	 * wider result. */
	{
		unsigned long long q = 0;

		ov = ckd_add(&q, (unsigned)UINT_MAX, 1u);
		CHECK(!ov && q == (unsigned long long)UINT_MAX + 1ULL,
		    "widening add does not overflow");
	}

	/* mixed signedness: the negative value is a large unsigned result. */
	{
		unsigned long long q = 0;

		ov = ckd_add(&q, (int)-1, 0u);
		CHECK(ov && q == ULLONG_MAX, "mixed -1+0u wraps to ULLONG_MAX");
	}

	/* signed result, unsigned operand wider than result. */
	ll = 0;
	ov = ckd_add(&ll, (unsigned long long)-1, 0ULL);
	CHECK(ov && ll == -1, "ull -1 into long long wraps to -1");

	/* single evaluation of operands and of the result pointer. */
	probe_calls = 0;
	z = 0;
	(void)ckd_add(&z, probe(), probe());
	CHECK(probe_calls == 2, "operands evaluated exactly once");

	slot_index = 0;
	(void)ckd_add(next_slot(), (size_t)1, (size_t)2);
	CHECK(slot_index == 1, "result pointer evaluated exactly once");
	CHECK(slots[0] == 3, "result stored through pointer");

	/* The macro's value has type bool. */
	_Static_assert(
	    _Generic(ckd_add(&z, (size_t)1, (size_t)2), _Bool: 1, default: 0),
	    "ckd_add() must have type bool");
}

int
main(void)
{
	/* signed/signed, unsigned/unsigned, and wider/narrower results. */
	RUN_CASE(signed char, 1, unsigned char, signed char, 1, unsigned char,
	    signed char, 1, unsigned char);
	RUN_CASE(unsigned char, 0, unsigned char, unsigned char, 0,
	    unsigned char, unsigned char, 0, unsigned char);
	RUN_CASE(short, 1, unsigned short, short, 1, unsigned short, short, 1,
	    unsigned short);
	RUN_CASE(unsigned short, 0, unsigned short, unsigned short, 0,
	    unsigned short, unsigned short, 0, unsigned short);
	RUN_CASE(
	    int, 1, unsigned int, int, 1, unsigned int, int, 1, unsigned int);
	RUN_CASE(unsigned int, 0, unsigned int, unsigned int, 0, unsigned int,
	    unsigned int, 0, unsigned int);
	RUN_CASE(long long, 1, unsigned long long, long long, 1,
	    unsigned long long, long long, 1, unsigned long long);
	RUN_CASE(unsigned long long, 0, unsigned long long, unsigned long long,
	    0, unsigned long long, unsigned long long, 0, unsigned long long);
	RUN_CASE(size_t, 0, size_t, size_t, 0, size_t, size_t, 0, size_t);

	/* differing operand and result widths/signedness. */
	RUN_CASE(int, 1, unsigned int, int, 1, unsigned int, long long, 1,
	    unsigned long long);
	RUN_CASE(int, 1, unsigned int, unsigned int, 0, unsigned int, int, 1,
	    unsigned int);
	RUN_CASE(long long, 1, unsigned long long, int, 1, unsigned int, int, 1,
	    unsigned int);
	RUN_CASE(int, 1, unsigned int, int, 1, unsigned int, unsigned char, 0,
	    unsigned char);
	RUN_CASE(long long, 1, unsigned long long, unsigned long long, 0,
	    unsigned long long, unsigned char, 0, unsigned char);
	RUN_CASE(unsigned long long, 0, unsigned long long, int, 1,
	    unsigned int, long long, 1, unsigned long long);

	test_hand_written();

	if (failures != 0) {
		printf("%d stdckdint check(s) failed\n", failures);
		return (1);
	}
	printf("stdckdint conformance: all checks passed\n");
	return (0);
}
