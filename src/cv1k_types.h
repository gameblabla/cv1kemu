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

#endif
