// xg_xs.dll implementation: skeleton. Each export logs its name and arguments once to stderr ([xg_xs]) and
// answers E_NOTIMPL. See xg_xs.h.
#include "xg_xs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

extern "C" {

static long g_once1 = 0, g_once2 = 0;

long XGCreateTextureComputer(void* a, void* b, void* c, void* out) {
    if (InterlockedCompareExchange(&g_once1, 1, 0) == 0) {
        std::fprintf(stderr,
                     "[xg_xs] XGCreateTextureComputer(%p, %p, %p, %p): the PC GPU driver computes textures itself; "
                     "answering E_NOTIMPL\n",
                     a, b, c, out);
        std::fflush(stderr);
    }
    return 0x80004001L;  // E_NOTIMPL
}

long XGSuggestSwizzleMode(unsigned format, unsigned flags, void* out) {
    if (InterlockedCompareExchange(&g_once2, 1, 0) == 0) {
        std::fprintf(stderr, "[xg_xs] XGSuggestSwizzleMode(format=%u, flags=%u, out=%p): not provided; answering "
                             "E_NOTIMPL\n",
                     format, flags, out);
        std::fflush(stderr);
    }
    return 0x80004001L;
}

} // extern "C"
