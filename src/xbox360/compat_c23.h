// C23 compatibility shim for GCC 9.x (libxenon toolchain)
// This header is force-included via -include for the Xbox 360 build.
// We use -std=gnu2x which gives us [[maybe_unused]] and bool as keyword,
// but nullptr is not available until GCC 13, so we polyfill it.

#include <stdbool.h>

#ifndef nullptr
#define nullptr NULL
#endif
