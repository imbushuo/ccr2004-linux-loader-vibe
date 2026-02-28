/*
 * Minimal C library string/memory functions for bare-metal.
 * Used by libfdt and the loader itself.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t       *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (((uintptr_t)d | (uintptr_t)s) % 8 == 0) {
		while (n >= 8) {
			*(uint64_t *)d = *(const uint64_t *)s;
			d += 8; s += 8; n -= 8;
		}
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t       *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (d < s || d >= s + n) {
		/* No overlap or dst before src: forward copy */
		while (n--)
			*d++ = *s++;
	} else {
		/* Overlap with dst after src: backward copy */
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

void *memset(void *s, int c, size_t n)
{
	uint8_t *p = (uint8_t *)s;
	while (n--)
		*p++ = (uint8_t)c;
	return s;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *pa = (const uint8_t *)a;
	const uint8_t *pb = (const uint8_t *)b;
	while (n--) {
		if (*pa != *pb)
			return (int)*pa - (int)*pb;
		pa++; pb++;
	}
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const uint8_t *p = (const uint8_t *)s;
	uint8_t uc = (uint8_t)c;
	while (n--) {
		if (*p == uc)
			return (void *)p;
		p++;
	}
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
	const char *p = s;
	while (maxlen-- && *p)
		p++;
	return (size_t)(p - s);
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	char cc = (char)c;
	for (; *s; s++)
		if (*s == cc)
			last = s;
	if (cc == '\0')
		return (char *)s;
	return (char *)last;
}
