#include "png_jump.h"

#include <setjmp.h>

/* Inlining this boundary into a C++ caller would reintroduce longjmp UB. */
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
int snow_png_run(png_structp png, SnowPngOperation operation, void* context) {
    if (setjmp(png_jmpbuf(png)) != 0)
        return 0;
    operation(context);
    return 1;
}
