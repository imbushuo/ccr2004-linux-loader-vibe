/* SPDX-License-Identifier: 0BSD */
/*
 * Bare-metal configuration for XZ Embedded.
 * Replaces the Linux kernel headers with freestanding equivalents.
 */

#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "xz.h"

/* Use only single-call mode: output buffer IS the dictionary, no separate
 * dict allocation needed.  This is ideal for our use case where the entire
 * compressed stream is in memory at once. */
#define XZ_DEC_SINGLE

/* ---- memory helpers (provided by str.c) ---- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#define memeq(a, b, size)  (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)

/* ---- tiny bump allocator (provided by loader.c) ---- */
void *xz_alloc(size_t size);

#define kmalloc(size, flags)  xz_alloc(size)
#define kfree(ptr)            ((void)(ptr))
#define kmalloc_obj(x)        xz_alloc(sizeof(x))

/* ---- misc kernel-ism replacements ---- */
#define min_t(type, a, b)  ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define min(a, b)          ((a) < (b) ? (a) : (b))

/* __always_inline */
#ifndef __always_inline
#  define __always_inline  __attribute__((always_inline)) inline
#endif

/*
 * vmalloc/vfree: only used in XZ_DYNALLOC paths which are compiled out when
 * XZ_DEC_DYNALLOC is not defined, but the C compiler still parses the code.
 * Provide stubs so compilation doesn't fail.
 */
#define vmalloc(size)  xz_alloc(size)
#define vfree(ptr)     ((void)(ptr))

static inline uint32_t get_le32(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;
	return (uint32_t)b[0]
	     | ((uint32_t)b[1] << 8)
	     | ((uint32_t)b[2] << 16)
	     | ((uint32_t)b[3] << 24);
}

#if __GNUC__ >= 7
#  define fallthrough  __attribute__((fallthrough))
#else
#  define fallthrough  ((void)0)
#endif

#endif /* XZ_CONFIG_H */
