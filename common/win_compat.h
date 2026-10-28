/*
 * WiVRn VR streaming — Windows (MSVC) compatibility shims, force-included on Windows builds only.
 */
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef ptrdiff_t ssize_t;

#define __builtin_unreachable() __assume(0)

#ifdef __cplusplus
// libstdc++ pulls these in transitively; MSVC's STL doesn't. Cheaper than touching every header.
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

extern "C++" {
inline size_t strlcpy(char * dst, const char * src, size_t size)
{
	size_t len = strlen(src);
	if (size)
	{
		size_t n = len < size - 1 ? len : size - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}

inline int setenv(const char * name, const char * value, int overwrite)
{
	if (not overwrite and getenv(name))
		return 0;
	return _putenv_s(name, value);
}
}
#endif
#endif
