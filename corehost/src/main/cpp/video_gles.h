/*
 * video_gles.h — GPU presentation (EGL + OpenGL ES 3.0, ES 2.0 fallback) for RetroNest, and the
 * OpenGL ES context / FBO for libretro hardware-rendered cores (SET_HW_RENDER).
 *
 * Implements the rn_video_* functions declared in retronest_bridge.h (Phase 4C contract) plus the
 * bridge-internal hooks below used by retronest_bridge.c / jni_interface.c. Owned by
 * native-bridge-agent. The lifecycle is documented at the top of video_gles.c.
 *
 * Threading: every function here runs on the EMULATION thread (the EGL context is current there),
 * except rn_video_hw_set_size(), which only records a size and may be called from any core thread.
 *
 * Window ownership: rn_video_set_window(window) TAKES OWNERSHIP of one ANativeWindow reference
 * (JNI acquires it with ANativeWindow_fromSurface). The module releases it on detach (NULL), when a
 * different window replaces it, on init failure, and in rn_video_shutdown().
 */
#ifndef RETRONEST_VIDEO_GLES_H
#define RETRONEST_VIDEO_GLES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct retro_hw_render_callback;

/* Bridge hook: a new core frame was copied into rn_get_video_frame(). Bumps the upload sequence
 * so rn_video_present() re-uploads; it never touches video_frame.frame_ready. */
void rn_video_on_frame(void);

/* Bridge hook (release_all / rn_deinit): destroys surface, context, display, GL objects, frees
 * buffers and stored shader sources and releases the window. Safe to call in any state. */
void rn_video_shutdown(void);

/* ---- Hardware rendering (SET_HW_RENDER) ---- */

/* SET_HW_RENDER handler (normally inside retro_load_game). Accepts OPENGLES2, OPENGLES3 and
 * OPENGLES_VERSION (2.x / 3.x up to the created context's version); rejects everything else.
 * Creates the context right away (current on a 1x1 pbuffer), fills get_current_framebuffer and
 * get_proc_address in `cb` and keeps a copy. */
bool rn_video_hw_set(struct retro_hw_render_callback* cb);
/* SET_HW_RENDER was accepted for the game being loaded (context_reset not called yet). */
bool rn_video_hw_requested(void);
/* After retro_load_game succeeded: context current, FBO of max_w x max_h, hw.context_reset(). */
bool rn_video_hw_start(unsigned max_w, unsigned max_h);
/* context_reset was called and frames come from the FBO (until rn_video_hw_context_destroy). */
bool rn_video_hw_active(void);
/* Max geometry changed (SET_SYSTEM_AV_INFO / SET_GEOMETRY): the FBO grows (never shrinks, same
 * name) at the next rn_video_hw_begin_frame(). Any thread; only records the size. */
void rn_video_hw_set_size(unsigned w, unsigned h);
/* Before retro_run / retro_(un)serialize: makes the context current, recovers a lost context,
 * applies pending FBO growth. False = the core must not run (no usable context). */
bool rn_video_hw_begin_frame(void);
/* RETRO_HW_FRAME_BUFFER_VALID frame of *width x *height (clamped in place to the FBO size). */
void rn_video_hw_on_frame(unsigned* width, unsigned* height);
/* Size of the last HW frame, 0 x 0 when there is none. */
void rn_video_hw_frame_size(unsigned* width, unsigned* height);
/* Reads the last HW frame back (glReadPixels) as opaque ARGB_8888, top row first, stride = width.
 * Returns a module-owned buffer valid until the next call, or NULL. Emulation thread. */
const uint32_t* rn_video_hw_read_frame(unsigned* width, unsigned* height);
/* rn_unload_game, BEFORE retro_unload_game: context current, hw.context_destroy(). The context and
 * FBO stay alive until rn_video_shutdown(). */
void rn_video_hw_context_destroy(void);
/* retro_load_game failed after SET_HW_RENDER was accepted: forget the request (no reset was done). */
void rn_video_hw_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* RETRONEST_VIDEO_GLES_H */
