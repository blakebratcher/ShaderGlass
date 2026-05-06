#pragma once

// MSVC-only annotations used by ShaderGC. On non-MSVC compilers we map
// __declspec(noinline) to GCC/Clang's attribute; other __declspec uses are dropped.
#ifndef _MSC_VER
#  ifndef __declspec
#    define __declspec(x) __SHADERGC_DECLSPEC_##x
#  endif
#  define __SHADERGC_DECLSPEC_noinline __attribute__((noinline))

// MSVC-only "secure" CRT functions used by ShaderGC. Provide POSIX-equivalent
// shims so the code compiles unmodified on Linux. These are inline so each
// translation unit gets its own definition (no ODR issues).
#  include <algorithm>
#  include <cstring>
#  include <cstddef>
#  include <strings.h>

static inline int strcpy_s(char* dest, std::size_t destSize, const char* src)
{
    if (!dest || !src || destSize == 0) return 1;
    const std::size_t len = std::strlen(src);
    if (len + 1 > destSize) { dest[0] = '\0'; return 1; }
    std::memcpy(dest, src, len + 1);
    return 0;
}

static inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
#endif
