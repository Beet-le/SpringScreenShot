#pragma once

#include <png.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SnowPngOperation)(void* context);

/* Keep libpng's non-local error recovery below all C++ object lifetimes. */
int snow_png_run(png_structp png, SnowPngOperation operation, void* context);

#ifdef __cplusplus
}
#endif
