// C99/C23/GCC compatibility shim for the Xbox 360 XDK MSVC compiler.
// All .c files are compiled as C++ (/TP) to get inline, mixed declarations,
// for-loop declarations, etc. This header is force-included via project settings.

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// ===[ stdbool / nullptr ]===
// In C++ mode these are built-in. A stub stdbool.h exists in src/xbox360-xdk/
// so that #include <stdbool.h> in shared headers doesn't fail.

// UNUSED attribute polyfill (no-op on MSVC)
#ifndef UNUSED
#define UNUSED
#endif

// ===[ GCC Extension Replacements ]===
// MSVC doesn't support statement expressions ({...}) or typeof().
// We override the macros from utils.h with MSVC-compatible versions.
// These are defined BEFORE utils.h is included (via force-include).

#define forEach(type, item, array, count) \
    for (int item##_i_ = 0; item##_i_ < (int)(count); item##_i_++) \
    for (type* item = &(array)[item##_i_]; item; item = 0)

#define forEachIndexed(type, item, index, array, count) \
    for (int index = 0; index < (int)(count); index++) \
    for (type* item = &(array)[index]; item; item = 0)

#define repeat(n, it) for (int it = 0; it < (int)(n); it++)

// ===[ Safe Allocation Wrappers ]===
// GCC utils.h uses statement expressions ({...}); MSVC uses inline functions.
// Return void* — callers use safeCast() or explicit casts where needed in C++.

__forceinline void* _safeMalloc(size_t size, const char* file, int line) {
    void* p = malloc(size);
    if (!p) { fprintf(stderr, "FATAL: malloc(%zu) at %s:%d\n", size, file, line); abort(); }
    return p;
}
__forceinline void* _safeCalloc(size_t count, size_t size, const char* file, int line) {
    void* p = calloc(count, size);
    if (!p) { fprintf(stderr, "FATAL: calloc(%zu,%zu) at %s:%d\n", count, size, file, line); abort(); }
    return p;
}
__forceinline void* _safeRealloc(void* ptr, size_t size, const char* file, int line) {
    void* p = realloc(ptr, size);
    if (!p) { fprintf(stderr, "FATAL: realloc(%zu) at %s:%d\n", size, file, line); abort(); }
    return p;
}
__forceinline char* _safeStrdup(const char* str, const char* file, int line) {
    char* p = _strdup(str);
    if (!p) { fprintf(stderr, "FATAL: strdup at %s:%d\n", file, line); abort(); }
    return p;
}
__forceinline void* _safeMemalign(size_t alignment, size_t size, const char* file, int line) {
    void* p = _aligned_malloc(size, alignment);
    if (!p) { fprintf(stderr, "FATAL: memalign(%zu,%zu) at %s:%d\n", alignment, size, file, line); abort(); }
    return p;
}

// In C++, void* doesn't implicitly convert to T*. _SafeVoidPtr uses a template
// conversion operator to auto-cast void* to the target pointer type on assignment.
// Usage: int* p = safeMalloc(sizeof(int) * n); // auto-casts via operator T*()
struct _SafeVoidPtr {
    void* ptr;
    _SafeVoidPtr(void* p) : ptr(p) {}
    template<typename T> operator T*() const { return static_cast<T*>(ptr); }
};

#define safeMalloc(size)         _SafeVoidPtr(_safeMalloc((size), __FILE__, __LINE__))
#define safeCalloc(count, size)  _SafeVoidPtr(_safeCalloc((count), (size), __FILE__, __LINE__))
#define safeRealloc(ptr, size)   _SafeVoidPtr(_safeRealloc((void*)(ptr), (size), __FILE__, __LINE__))
#define safeMemalign(alignment, size) _SafeVoidPtr(_safeMemalign((alignment), (size), __FILE__, __LINE__))
#define safeStrdup(str)          _safeStrdup((str), __FILE__, __LINE__)

// requireNotNull: GCC statement expression -> MSVC inline function
// Returns _SafeVoidPtr so result auto-casts to the target pointer type in C++.
__forceinline void* _requireNotNull(void* ptr, const char* expr, const char* file, int line) {
    if (!ptr) { fprintf(stderr, "%s:%d: requireNotNull failed: '%s'\n", file, line, expr); abort(); }
    return ptr;
}
#define requireNotNull(ptr)             _SafeVoidPtr(_requireNotNull((void*)(ptr), #ptr, __FILE__, __LINE__))
#define requireNotNullMessage(ptr, msg) _SafeVoidPtr(_requireNotNull((void*)(ptr), (msg), __FILE__, __LINE__))

// strdup is called _strdup on MSVC
#ifndef strdup
#define strdup _strdup
#endif

// ===[ C99 Math Polyfills ]===
// MSVC 2010 lacks many C99 math functions.
#ifndef round
#define round(x) floor((x) + 0.5)
#endif
#ifndef roundf
#define roundf(x) floorf((x) + 0.5f)
#endif

// fmin/fmax: MSVC 2010 doesn't have C99 fmin/fmax. Use __min/__max or inline.
__forceinline double fmin(double a, double b) { return (a < b) ? a : b; }
__forceinline double fmax(double a, double b) { return (a > b) ? a : b; }
__forceinline float fminf(float a, float b) { return (a < b) ? a : b; }
__forceinline float fmaxf(float a, float b) { return (a > b) ? a : b; }

// INFINITY: MSVC 2010 doesn't define this C99 macro
#ifndef INFINITY
#define INFINITY ((float)(1e+300 * 1e+300))
#endif

// ===[ Math Constants ]===
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

// ===[ snprintf ]===
// MSVC 2010 has _snprintf but not C99 snprintf.
#ifndef snprintf
#define snprintf _snprintf
#endif

// ===[ POSIX clock polyfill ]===
// Xbox 360 doesn't have clock_gettime. Use QueryPerformanceCounter instead.
// Declared here, defined in main.cpp (needs <xtl.h> which we don't want in every TU).
#ifdef _XBOX
double _xdk_monotonic_ms(void);
#endif

// Guard so utils.h knows NOT to define its GCC-specific versions of these macros.
#define BUTTERSCOTCH_MSVC_COMPAT 1
