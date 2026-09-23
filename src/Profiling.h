#pragma once

/*
 * Profiling.h -- thin wrapper over Tracy Profiler instrumentation macros.
 *
 * Why this file exists:
 *   `extern/` is git-ignored (`.gitignore: /extern/`), so Tracy -- like
 *   CommonLibSSE-NG -- is a *local, optional* dependency that may be entirely
 *   absent on a fresh clone. Source files therefore MUST NOT include
 *   <tracy/Tracy.hpp> unconditionally.
 *
 * Rules for this header:
 *   1. Guard on `TRACY_ENABLE` (not `__has_include`): when Tracy is not
 *      integrated at all, the headers do not exist and we still need to
 *      compile. `TRACY_ENABLE` is propagated PUBLIC by Tracy's own CMake
 *      options (extern/TracyProfiler/cmake/options.cmake) whenever the
 *      TracyClient target is linked.
 *   2. Only use own-prefixed macros (`SSE_*`). NEVER redefine Tracy's macro
 *      names as a fallback: the no-op branch of Tracy.hpp declares 87 macros
 *      whose parameter names differ from the real branch (e.g.
 *      `ZoneScopedN(x)` vs `ZoneScopedN( name )`), which trips C4005 and
 *      fails the build under /W4 /WX.
 *   3. Add new forwards here on demand. A missing forward is a compile error
 *      (not a silent no-op), which is the intended trade-off.
 *
 * See docs/tracy-integration-plan.md sections 3.2 / 3.3.
 */

#ifdef TRACY_ENABLE
    #include <tracy/Tracy.hpp>

    // Named zone. Preferred in hot paths: the name is a compile-time literal,
    // no allocation / formatting happens (unlike ZoneText / ZoneTextF /
    // ZoneName, which tracy_malloc on every call -- TracyScoped.hpp:89-165).
    #define SSE_ZONE(name) ZoneScopedN(name)
    // Zone named after the enclosing function.
    #define SSE_ZONE_FN ZoneScoped
    // Attach one 64-bit value to the current zone (8 bytes, no allocation).
    #define SSE_VALUE(value) ZoneValue(value)
    // Split a logical frame on the timeline (not tied to a rendered frame).
    #define SSE_FRAME(name) FrameMarkNamed(name)
    // Low-frequency event message. NEVER call this from a hot path.
    #define SSE_MSG_L(txt) TracyMessageL(txt)
#else
    #define SSE_ZONE(name) ((void)0)
    #define SSE_ZONE_FN ((void)0)
    #define SSE_VALUE(value) ((void)0)
    #define SSE_FRAME(name) ((void)0)
    #define SSE_MSG_L(txt) ((void)0)
#endif
