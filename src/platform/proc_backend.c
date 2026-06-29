/*
 * psDoom -- process backend selection.
 *
 * Holds the pointer to the active backend and defaults it to the platform's
 * native implementation. Tests (and a future replay backend) call
 * proc_backend_set() to swap it.
 */

#include "proc_backend.h"

#include <stddef.h>   /* NULL */

#if defined(__APPLE__) && !defined(PSD_NO_NATIVE_BACKEND)
/* The native macOS backend (defined in platform/macos/proc_macos.c). */
extern const proc_backend_t proc_macos_backend;
static const proc_backend_t *g_default_backend = &proc_macos_backend;
#else
/* No native backend compiled in (e.g. a host-only test build, or a platform
 * without a backend yet). Callers must proc_backend_set() one before use. */
static const proc_backend_t *g_default_backend = NULL;
#endif

static const proc_backend_t *g_backend; /* NULL => use the default */

const proc_backend_t *proc_backend(void)
{
    return g_backend != NULL ? g_backend : g_default_backend;
}

void proc_backend_set(const proc_backend_t *backend)
{
    g_backend = backend;
}
