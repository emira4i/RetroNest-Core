/*
 * retronest_bridge.h — Native engine interface contract (Bridge layer).
 *
 * Owned by native-bridge-agent. Consumed only by jni_interface.c.
 * Kotlin mirrors this contract in com.retronest.bridge.LibretroBridge.
 *
 * Threading: every rn_* function must be called from the same thread
 * (the emulation thread). The bridge holds a single global core instance.
 */
#ifndef RETRONEST_BRIDGE_H
#define RETRONEST_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Engine state. Values are mirrored by LibretroBridge.STATE_* in Kotlin — keep in sync. */
typedef enum {
    RN_STATE_IDLE    = 0, /* No core loaded            */
    RN_STATE_LOADED  = 1, /* Core loaded, no game      */
    RN_STATE_RUNNING = 2, /* Game loaded and running   */
    RN_STATE_PAUSED  = 3  /* Game loaded, not stepping */
} rn_state_t;

/* Pixel formats. Values match enum retro_pixel_format in libretro.h. */
typedef enum {
    RN_PIXEL_FORMAT_0RGB1555 = 0,
    RN_PIXEL_FORMAT_XRGB8888 = 1,
    RN_PIXEL_FORMAT_RGB565   = 2
} rn_pixel_format_t;

/* Latest video frame produced by the core, copied into a bridge-owned buffer. */
typedef struct {
    const void* data;          /* Pixel data (bridge-owned, valid until next rn_run_frame / rn_deinit) */
    unsigned    width;         /* Frame width in pixels                                             */
    unsigned    height;        /* Frame height in pixels                                            */
    size_t      pitch;         /* Bytes per scanline                                                */
    int         pixel_format;  /* rn_pixel_format_t                                                 */
    bool        frame_ready;   /* True when a new frame arrived since it was last consumed          */
} rn_video_frame_t;

/* Core and content information (strings are bridge-owned copies, valid until rn_deinit). */
typedef struct {
    const char* library_name;
    const char* library_version;
    const char* valid_extensions;
    bool        need_fullpath;
    double      fps;           /* From retro_get_system_av_info, 0 until a game is loaded */
    double      sample_rate;   /* From retro_get_system_av_info, 0 until a game is loaded */
    unsigned    base_width;
    unsigned    base_height;
    float       aspect_ratio;  /* Display aspect ratio; falls back to base_width / base_height */
} rn_core_info_t;

/* Lifecycle */
bool rn_init(const char* core_path, const char* system_dir, const char* save_dir);
void rn_deinit(void);
bool rn_load_game(const char* rom_path);
void rn_unload_game(void);
void rn_run_frame(void);

/* ------------------------------------------------------------------ */
/* Phase 2 contract (locked — implementation in retronest_bridge.c)    */
/* ------------------------------------------------------------------ */

/*
 * Pause / resume. Paused: rn_run_frame() does nothing and the AAudio stream is paused.
 * Only valid in RUNNING / PAUSED; moves the state between them. Emulation thread.
 */
void rn_set_paused(bool paused);

/*
 * Input — THREAD-SAFE (atomics), may be called from any thread, e.g. the UI thread.
 * mask: bit N = RETRO_DEVICE_ID_JOYPAD_N (B=0, Y=1, SELECT=2, START=3, UP=4, DOWN=5,
 * LEFT=6, RIGHT=7, A=8, X=9, L=10, R=11, L2=12, R2=13, L3=14, R3=15).
 * Ports 0..3. Read by cb_input_state on the next retro_run().
 */
void rn_set_input_mask(unsigned port, uint32_t mask);
/* stick: 0 = left (RETRO_DEVICE_INDEX_ANALOG_LEFT), 1 = right. x/y in -32768..32767. */
void rn_set_analog(unsigned port, unsigned stick, int16_t x, int16_t y);

/*
 * Save states (retro_serialize*). Emulation thread, game must be RUNNING or PAUSED.
 * rn_get_state_size() returns 0 when the core cannot serialize.
 */
size_t rn_get_state_size(void);
bool   rn_save_state(void* dst, size_t size);
bool   rn_load_state(const void* src, size_t size);

/*
 * Battery SRAM (retro_get_memory_data/size with RETRO_MEMORY_SAVE_RAM).
 * Returns the core-owned buffer (NULL / size 0 when the game has no battery save).
 * Write into it right after rn_load_game() to restore; read it to flush. Emulation thread.
 */
void* rn_get_sram(size_t* out_size);

/* ------------------------------------------------------------------ */
/* Phase 4A contract (locked) — core options + rumble                  */
/* ------------------------------------------------------------------ */

/*
 * Core options (RETRO_ENVIRONMENT_SET_VARIABLES, SET_CORE_OPTIONS v1/v2/_INTL, GET_VARIABLE,
 * GET_VARIABLE_UPDATE, GET_CORE_OPTIONS_VERSION). Values requested by Kotlin are kept even before
 * the core declares the option (applied when it does). Emulation thread.
 */
bool rn_set_core_option(const char* key, const char* value); /* sets the "updated" flag */
size_t rn_get_core_option_count(void);
/* i-th declared option: key, current value, description, '|'-separated allowed values. */
bool rn_get_core_option(size_t index, const char** key, const char** value,
                        const char** desc, const char** values);

/*
 * Rumble (RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE). Strength 0..65535 = max(strong, weak) the
 * core currently requests for [port]. THREAD-SAFE (atomic), poll after each frame.
 */
uint16_t rn_get_rumble(unsigned port);

/* ------------------------------------------------------------------ */
/* Phase 4C contract (locked) — GPU video (OpenGL ES 3.0, ES 2.0 fallback) */
/* ------------------------------------------------------------------ */
/*
 * All rn_video_* calls happen on the EMULATION thread (EGL context is current there).
 *
 * Shader contract (GLSL ES 1.00 so the same source works in ES2 and ES3 contexts):
 *   attribute vec2 a_position;   // clip-space quad, rotation already applied by u_mvp
 *   attribute vec2 a_texCoord;   // 0..1 over the VISIBLE frame (not the padded texture)
 *   uniform mat4  u_mvp;         // rotation (0/90/180/270) + aspect-fit scale
 *   uniform sampler2D u_texture; // RGBA, frame in the top-left of a possibly larger texture
 *   uniform vec2  u_sourceSize;  // frame size in pixels (e.g. 256x224)
 *   uniform vec2  u_textureSize; // allocated texture size in pixels
 *   uniform vec2  u_outputSize;  // on-screen size of the frame quad in pixels (after fit)
 *   varying vec2  v_texCoord;    // = a_texCoord * (u_sourceSize / u_textureSize)
 * A preset is one vertex + one fragment source and a filter (nearest / linear).
 */
struct ANativeWindow;

/* Attach (non-NULL) or detach (NULL) the output window; creates/destroys EGL surface + context
 * as needed. Returns false when EGL/GLES init fails — caller falls back to software (Canvas). */
bool rn_video_set_window(struct ANativeWindow* window);
/* Compile + link a preset. On failure returns false and keeps the previous (or built-in
 * passthrough) program; the GL info log goes to logcat. May be called before a window exists. */
bool rn_video_set_shader(const char* vertex_src, const char* fragment_src, bool linear_filter);
/* Drops the current preset and returns to the built-in passthrough ("Sharp") output. Safe without a window. */
void rn_video_clear_shader(void);
/* Display rotation, degrees clockwise: 0, 90, 180 or 270. */
void rn_video_set_rotation(int degrees);
/* Upload the newest core frame (if any), draw it aspect-fit (display aspect from the core) with
 * the current preset on black, eglSwapBuffers. Returns false when no window/context. */
bool rn_video_present(void);

/* T2a (locked): libretro pointer (RETRO_DEVICE_POINTER) for touch-screen systems. THREAD-SAFE (atomics).
 * x/y in -0x7fff..0x7fff over the core frame; pressed → RETRO_DEVICE_ID_POINTER_PRESSED; count = 1. */
void rn_set_pointer(int16_t x, int16_t y, bool pressed);

/* Settings completion (locked): AAudio buffer size in bursts (1..16, 0 = automatic). Emulation thread.
 * Implemented by audio_aaudio.c (rn_audio_set_buffer_bursts); exposed to JNI as nativeSetAudioBufferBursts. */

/* Phase 4.5 WS4 (locked): reset, cheats, audio mute. Emulation thread (mute: thread-safe, atomic). */
bool rn_reset(void);                                            /* retro_reset; false if unsupported */
bool rn_cheat_reset(void);                                      /* retro_cheat_reset */
bool rn_cheat_set(unsigned index, bool enabled, const char* code); /* retro_cheat_set */
/* While muted, core audio is dropped (not queued), so AAudio latency cannot grow. */
void rn_audio_set_muted(bool muted);
bool rn_shutdown_requested(void);                               /* core sent RETRO_ENVIRONMENT_SHUTDOWN */

/* Queries */
rn_state_t        rn_get_state(void);
rn_video_frame_t* rn_get_video_frame(void);
rn_core_info_t*   rn_get_core_info(void);
const char*       rn_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* RETRONEST_BRIDGE_H */
