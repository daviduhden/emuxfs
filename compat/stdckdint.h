/*
 * <stdckdint.h> compatibility shim.
 *
 * C23 places the checked-integer macros in the C library, but a system can
 * have a C23-capable compiler while its libc still lacks the header (OpenBSD
 * 7.9 at the time of writing).  compat/ is searched ahead of the system
 * directories, so this file is what
 *
 *	#include <stdckdint.h>
 *
 * finds.  It is intentionally thin:
 *
 *   - a conforming system header reachable after compat/ is included with
 *     #include_next and used unchanged;
 *   - otherwise ckd_add(), ckd_sub() and ckd_mul() are defined in terms of
 *     the compiler overflow builtins.
 *
 * #include_next (rather than #include) is required: compat/ precedes the
 * system directories, so a plain include would find this very file again and
 * recurse.  #include_next resumes the search after compat/, which cannot
 * match this file.  It is a GCC/Clang extension, but this portability shim
 * already relies on those compilers for the overflow builtins.
 *
 * The fallback matches C23 7.20.1: the builtins evaluate both operands to
 * infinite precision, convert the mathematical result to the type of *result
 * (wrapping when it is not representable), store it, and report whether that
 * conversion changed the value.  Each operand and the result pointer are
 * evaluated exactly once.  This is the same formulation used by GCC's and
 * Clang's resource headers and by the musl <stdckdint.h> proposal (Jens
 * Gustedt, 2023-05-31, message-id <323b1aaf4c7e31010ead4eae127d0d580f319ce6.
 * 1685535458.git.Jens.Gustedt@inria.fr>); no code is copied from either, only
 * the builtin contract they all express.
 *
 * EMUXFS_STDCKDINT_FORCE_FALLBACK exists so that a test can exercise the
 * fallback on hosts whose libc already provides the header.  The build never
 * defines it.
 */

#ifndef EMUXFS_COMPAT_STDCKDINT_H
#define EMUXFS_COMPAT_STDCKDINT_H

#ifndef EMUXFS_STDCKDINT_FORCE_FALLBACK
#if defined(__has_include_next)
#if __has_include_next(<stdckdint.h>)
/*
 * GCC diagnoses the (intentional) GNU #include_next extension under
 * -Wpedantic; suppress exactly that diagnostic for this one directive so the
 * shim stays warning-free.  This does not hide a defect: the directive is the
 * mechanism that reaches the real header.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include_next <stdckdint.h>
#pragma GCC diagnostic pop
#endif
#endif
#endif /* !EMUXFS_STDCKDINT_FORCE_FALLBACK */

/*
 * A conforming C23 <stdckdint.h> defines __STDC_VERSION_STDCKDINT_H__ (C23
 * 7.20).  If it is not defined here, either no real header was found or it is
 * not conforming, so supply the operations.
 */
#ifndef __STDC_VERSION_STDCKDINT_H__
#if defined(__has_builtin)
#if !__has_builtin(__builtin_add_overflow) ||                                  \
    !__has_builtin(__builtin_sub_overflow) ||                                  \
    !__has_builtin(__builtin_mul_overflow)
#error "emuxfs needs the compiler overflow builtins for <stdckdint.h>"
#endif
#elif !defined(__GNUC__) && !defined(__clang__)
#error "emuxfs needs __builtin_*_overflow for <stdckdint.h>"
#endif

/*
 * The #ifndef guards keep the fallback from colliding with a partial or
 * non-conforming header that already defines some of the macros.
 */
#ifndef ckd_add
#define ckd_add(result, a, b)                                                  \
	((_Bool)__builtin_add_overflow((a), (b), (result)))
#endif
#ifndef ckd_sub
#define ckd_sub(result, a, b)                                                  \
	((_Bool)__builtin_sub_overflow((a), (b), (result)))
#endif
#ifndef ckd_mul
#define ckd_mul(result, a, b)                                                  \
	((_Bool)__builtin_mul_overflow((a), (b), (result)))
#endif

#define __STDC_VERSION_STDCKDINT_H__ 202311L
#endif /* !__STDC_VERSION_STDCKDINT_H__ */

#endif /* EMUXFS_COMPAT_STDCKDINT_H */
