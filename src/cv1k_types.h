#ifndef CV1K_TYPES_H
#define CV1K_TYPES_H

/*
 * cv1k_types.h
 * Small fixed-width type layer kept C89-compatible enough for old C compilers.
 * This sandbox assumes unsigned char=8 bits, unsigned short=16 bits,
 * unsigned long=32 bits or wider. The build performs runtime checks at start.
 */

typedef unsigned char cv1k_u8;
typedef signed char cv1k_s8;
typedef unsigned short cv1k_u16;
typedef signed short cv1k_s16;
typedef unsigned int cv1k_u32;
typedef signed int cv1k_s32;

typedef unsigned int cv1k_bool;

#ifndef CV1K_TRUE
#define CV1K_TRUE 1U
#define CV1K_FALSE 0U
#endif

#define CV1K_UNUSED(x) ((void)(x))

#if defined(__GNUC__) || defined(__clang__)
#define CV1K_HOT __attribute__((hot))
#define CV1K_COLD __attribute__((cold))
#define CV1K_ALWAYS_INLINE inline __attribute__((always_inline))
#define CV1K_PURE __attribute__((pure))
#define CV1K_LIKELY(x) __builtin_expect(!!(x), 1)
#define CV1K_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CV1K_ALIGNED(n) __attribute__((aligned(n)))
#else
#define CV1K_HOT
#define CV1K_COLD
#define CV1K_ALWAYS_INLINE inline
#define CV1K_PURE
#define CV1K_LIKELY(x) (x)
#define CV1K_UNLIKELY(x) (x)
#define CV1K_ALIGNED(n)
#endif

#define CV1K_CACHE_ALIGN 64U

#endif
