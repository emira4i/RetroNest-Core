/*
 * jni_interface.c — JNI exports for com.retronest.bridge.LibretroBridge.
 *
 * Thin translation layer: converts Java types to C, forwards to retronest_bridge.h,
 * and converts core pixel formats to Android ARGB_8888 for Phase 1 software blitting.
 *
 * Owned by native-bridge-agent. Every exported name must match an `external fun`
 * in LibretroBridge.kt (Kotlin `object` → instance methods, so the second arg is jobject).
 */
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "retronest_bridge.h"
#include "audio_aaudio.h"
#include "video_gles.h"

#define JNI_FN(name) Java_com_retronest_bridge_LibretroBridge_##name

/* Returns a UTF-8 copy owned by the JVM, or NULL for a null jstring. Release with release_str. */
static const char* get_str(JNIEnv* env, jstring s) {
    return s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
}

static void release_str(JNIEnv* env, jstring s, const char* c) {
    if (s && c) (*env)->ReleaseStringUTFChars(env, s, c);
}

static jstring new_str_or(JNIEnv* env, const char* s, const char* fallback) {
    return (*env)->NewStringUTF(env, (s && s[0]) ? s : fallback);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

JNIEXPORT jboolean JNICALL
JNI_FN(nativeInit)(JNIEnv* env, jobject thiz, jstring core_path, jstring system_dir, jstring save_dir) {
    (void)thiz;
    const char* core = get_str(env, core_path);
    const char* sys  = get_str(env, system_dir);
    const char* save = get_str(env, save_dir);

    const bool ok = rn_init(core, sys, save);

    release_str(env, core_path, core);
    release_str(env, system_dir, sys);
    release_str(env, save_dir, save);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
JNI_FN(nativeDeinit)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    rn_deinit();
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeLoadGame)(JNIEnv* env, jobject thiz, jstring rom_path) {
    (void)thiz;
    const char* rom = get_str(env, rom_path);
    const bool ok = rn_load_game(rom);
    release_str(env, rom_path, rom);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
JNI_FN(nativeUnloadGame)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    rn_unload_game();
}

/* T2b (locked): the loaded game renders through a GLES hardware context (SET_HW_RENDER). */
JNIEXPORT jboolean JNICALL
JNI_FN(nativeUsesHwRender)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return rn_video_hw_active() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
JNI_FN(nativeRunFrame)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    rn_run_frame();
}

JNIEXPORT jint JNICALL
JNI_FN(nativeGetState)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)rn_get_state();
}

/* ------------------------------------------------------------------ */
/* Video                                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }

/* Converts a tightly packed core frame to opaque ARGB_8888 (0xAARRGGBB, as Bitmap.setPixels expects). */
static void convert_to_argb8888(const rn_video_frame_t* f, uint32_t* dst) {
    const uint8_t* src = (const uint8_t*)f->data;
    const unsigned w = f->width;
    const unsigned h = f->height;

    for (unsigned y = 0; y < h; y++) {
        const uint8_t* row = src + (size_t)y * f->pitch;
        uint32_t* out = dst + (size_t)y * w;

        switch (f->pixel_format) {
            case RN_PIXEL_FORMAT_XRGB8888: {
                const uint32_t* p = (const uint32_t*)row;
                for (unsigned x = 0; x < w; x++) out[x] = 0xFF000000u | (p[x] & 0x00FFFFFFu);
                break;
            }
            case RN_PIXEL_FORMAT_RGB565: {
                const uint16_t* p = (const uint16_t*)row;
                for (unsigned x = 0; x < w; x++) {
                    const uint32_t c = p[x];
                    out[x] = 0xFF000000u
                           | (expand5((c >> 11) & 0x1F) << 16)
                           | (expand6((c >> 5)  & 0x3F) << 8)
                           |  expand5( c        & 0x1F);
                }
                break;
            }
            case RN_PIXEL_FORMAT_0RGB1555:
            default: {
                const uint16_t* p = (const uint16_t*)row;
                for (unsigned x = 0; x < w; x++) {
                    const uint32_t c = p[x];
                    out[x] = 0xFF000000u
                           | (expand5((c >> 10) & 0x1F) << 16)
                           | (expand5((c >> 5)  & 0x1F) << 8)
                           |  expand5( c        & 0x1F);
                }
                break;
            }
        }
    }
}

/*
 * Copies the latest frame into `dst` as ARGB_8888, row-major, stride = frame width.
 * Returns JNI_TRUE only when a new frame was written. Returns JNI_FALSE when there is
 * no new frame or `dst` is too small (caller checks nativeGetFrameWidth/Height and grows it).
 * The frame stays "ready" if dst was too small, so it is not lost.
 */
/*
 * HW cores: reads the FBO back (glReadPixels on the emulation thread, where the context is current)
 * whenever a HW frame exists — the engine only asks for thumbnails / screenshots, so the newest
 * frame is always returned, whether or not it was consumed before.
 */
static jboolean get_hw_frame(JNIEnv* env, jintArray dst, rn_video_frame_t* f) {
    unsigned w = 0, h = 0;
    rn_video_hw_frame_size(&w, &h);
    if (!w || !h) return JNI_FALSE;
    if ((*env)->GetArrayLength(env, dst) < (jsize)((size_t)w * h)) return JNI_FALSE;
    const uint32_t* argb = rn_video_hw_read_frame(&w, &h);
    if (!argb) return JNI_FALSE;
    (*env)->SetIntArrayRegion(env, dst, 0, (jsize)((size_t)w * h), (const jint*)argb);
    f->frame_ready = false;
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeGetFrameBuffer)(JNIEnv* env, jobject thiz, jintArray dst) {
    (void)thiz;
    rn_video_frame_t* f = rn_get_video_frame();
    if (dst && rn_video_hw_active()) {
        return get_hw_frame(env, dst, f);
    }
    if (!dst || !f->frame_ready || !f->data) {
        return JNI_FALSE;
    }

    const jsize needed = (jsize)((size_t)f->width * f->height);
    if ((*env)->GetArrayLength(env, dst) < needed) {
        return JNI_FALSE;
    }

    /* Critical section: no other JNI calls until released. */
    uint32_t* pixels = (uint32_t*)(*env)->GetPrimitiveArrayCritical(env, dst, NULL);
    if (!pixels) {
        return JNI_FALSE;
    }
    convert_to_argb8888(f, pixels);
    (*env)->ReleasePrimitiveArrayCritical(env, dst, pixels, 0);

    f->frame_ready = false;
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeGetFrameWidth)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)rn_get_video_frame()->width;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeGetFrameHeight)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)rn_get_video_frame()->height;
}

/* ------------------------------------------------------------------ */
/* Info                                                                */
/* ------------------------------------------------------------------ */

JNIEXPORT jstring JNICALL
JNI_FN(nativeGetCoreName)(JNIEnv* env, jobject thiz) {
    (void)thiz;
    return new_str_or(env, rn_get_core_info()->library_name, "Unknown");
}

JNIEXPORT jstring JNICALL
JNI_FN(nativeGetCoreVersion)(JNIEnv* env, jobject thiz) {
    (void)thiz;
    return new_str_or(env, rn_get_core_info()->library_version, "");
}

JNIEXPORT jdouble JNICALL
JNI_FN(nativeGetFps)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jdouble)rn_get_core_info()->fps;
}

JNIEXPORT jfloat JNICALL
JNI_FN(nativeGetAspectRatio)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jfloat)rn_get_core_info()->aspect_ratio;
}

JNIEXPORT jstring JNICALL
JNI_FN(nativeGetLastError)(JNIEnv* env, jobject thiz) {
    (void)thiz;
    return new_str_or(env, rn_get_last_error(), "");
}

/* ------------------------------------------------------------------ */
/* Phase 2: pause, input, save states, SRAM                            */
/* ------------------------------------------------------------------ */

JNIEXPORT void JNICALL
JNI_FN(nativeSetPaused)(JNIEnv* env, jobject thiz, jboolean paused) {
    (void)env; (void)thiz;
    rn_set_paused(paused == JNI_TRUE);
}

/* Thread-safe (atomics in the bridge). Negative ports are rejected by the unsigned range check. */
JNIEXPORT void JNICALL
JNI_FN(nativeSetInputMask)(JNIEnv* env, jobject thiz, jint port, jint mask) {
    (void)env; (void)thiz;
    if (port < 0) return;
    rn_set_input_mask((unsigned)port, (uint32_t)mask);
}

static int16_t clamp_axis(jint v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

JNIEXPORT void JNICALL
JNI_FN(nativeSetAnalog)(JNIEnv* env, jobject thiz, jint port, jint stick, jint x, jint y) {
    (void)env; (void)thiz;
    if (port < 0 || stick < 0) return;
    rn_set_analog((unsigned)port, (unsigned)stick, clamp_axis(x), clamp_axis(y));
}

static int16_t clamp_pointer(jint v) {
    if (v < -0x7fff) return -0x7fff;
    if (v > 0x7fff) return 0x7fff;
    return (int16_t)v;
}

JNIEXPORT void JNICALL
JNI_FN(nativeSetPointer)(JNIEnv* env, jobject thiz, jint x, jint y, jboolean pressed) {
    (void)env; (void)thiz;
    rn_set_pointer(clamp_pointer(x), clamp_pointer(y), pressed == JNI_TRUE);
}

/* Copies `size` bytes into a new Java byte[]; NULL on allocation failure (exception pending). */
static jbyteArray new_byte_array(JNIEnv* env, const void* data, size_t size) {
    if (size > (size_t)INT32_MAX) return NULL;
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)size);
    if (!arr) return NULL;
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)size, (const jbyte*)data);
    return arr;
}

JNIEXPORT jbyteArray JNICALL
JNI_FN(nativeSaveState)(JNIEnv* env, jobject thiz) {
    (void)thiz;
    const size_t size = rn_get_state_size();
    if (size == 0 || size > (size_t)INT32_MAX) {
        return NULL;
    }
    void* buf = malloc(size);
    if (!buf) {
        return NULL;
    }
    jbyteArray result = NULL;
    if (rn_save_state(buf, size)) {
        result = new_byte_array(env, buf, size);
    }
    free(buf);
    return result;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeLoadState)(JNIEnv* env, jobject thiz, jbyteArray data) {
    (void)thiz;
    if (!data) return JNI_FALSE;
    const jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0) {
        return rn_load_state(NULL, 0) ? JNI_TRUE : JNI_FALSE; /* sets last_error */
    }
    jbyte* bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return JNI_FALSE;
    const bool ok = rn_load_state(bytes, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT); /* read-only: no copy-back */
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
JNI_FN(nativeGetSram)(JNIEnv* env, jobject thiz) {
    (void)thiz;
    size_t size = 0;
    const void* sram = rn_get_sram(&size);
    if (!sram || size == 0) {
        return NULL;
    }
    return new_byte_array(env, sram, size);
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeSetSram)(JNIEnv* env, jobject thiz, jbyteArray data) {
    (void)thiz;
    if (!data) return JNI_FALSE;
    size_t size = 0;
    void* sram = rn_get_sram(&size);
    if (!sram || size == 0) {
        return JNI_FALSE;
    }
    const jsize len = (*env)->GetArrayLength(env, data);
    if (len < 0 || (size_t)len != size) {
        return JNI_FALSE;
    }
    /* Copies straight into the core-owned buffer. */
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte*)sram);
    return JNI_TRUE;
}

/* ------------------------------------------------------------------ */
/* Phase 4A: core options, rumble                                      */
/* ------------------------------------------------------------------ */

JNIEXPORT jboolean JNICALL
JNI_FN(nativeSetCoreOption)(JNIEnv* env, jobject thiz, jstring key, jstring value) {
    (void)thiz;
    const char* k = get_str(env, key);
    const char* v = get_str(env, value);
    const bool ok = (k && v) ? rn_set_core_option(k, v) : false;
    release_str(env, key, k);
    release_str(env, value, v);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* Appends `s` to dst (bounded), replacing tab/newline so the field separators stay unambiguous. */
static size_t append_field(char* dst, size_t pos, size_t cap, const char* s) {
    for (; s && *s && pos + 1 < cap; s++) {
        dst[pos++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    }
    return pos;
}

/* Each entry: "key\tvalue\tdescription\tv1|v2|v3". */
JNIEXPORT jobjectArray JNICALL
JNI_FN(nativeGetCoreOptions)(JNIEnv* env, jobject thiz) {
    (void)thiz;
    jclass str_class = (*env)->FindClass(env, "java/lang/String");
    if (!str_class) return NULL;

    const size_t count = rn_get_core_option_count();
    jobjectArray arr = (*env)->NewObjectArray(env, (jsize)count, str_class, NULL);
    if (!arr) return NULL;

    for (size_t i = 0; i < count; i++) {
        const char *key = NULL, *value = NULL, *desc = NULL, *values = NULL;
        if (!rn_get_core_option(i, &key, &value, &desc, &values)) continue;

        const size_t cap = strlen(key) + strlen(value) + strlen(desc) + strlen(values) + 8;
        char* buf = malloc(cap);
        if (!buf) continue;
        size_t pos = append_field(buf, 0, cap, key);
        buf[pos++] = '\t';
        pos = append_field(buf, pos, cap, value);
        buf[pos++] = '\t';
        pos = append_field(buf, pos, cap, desc);
        buf[pos++] = '\t';
        pos = append_field(buf, pos, cap, values);
        buf[pos] = '\0';

        jstring js = (*env)->NewStringUTF(env, buf);
        free(buf);
        if (!js) return arr; /* exception pending */
        (*env)->SetObjectArrayElement(env, arr, (jsize)i, js);
        (*env)->DeleteLocalRef(env, js);
    }
    return arr;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeGetRumble)(JNIEnv* env, jobject thiz, jint port) {
    (void)env; (void)thiz;
    if (port < 0) return 0;
    return (jint)rn_get_rumble((unsigned)port);
}

/* ------------------------------------------------------------------ */
/* Phase 4C: GPU video                                                 */
/* ------------------------------------------------------------------ */

/*
 * Acquires an ANativeWindow reference (ANativeWindow_fromSurface) and hands OWNERSHIP of it to
 * rn_video_set_window, which releases it on detach / replacement / failure / rn_deinit.
 * null surface = detach (always true).
 */
JNIEXPORT jboolean JNICALL
JNI_FN(nativeSetSurface)(JNIEnv* env, jobject thiz, jobject surface) {
    (void)thiz;
    if (!surface) {
        return rn_video_set_window(NULL) ? JNI_TRUE : JNI_FALSE;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        return JNI_FALSE;
    }
    return rn_video_set_window(window) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeSetShader)(JNIEnv* env, jobject thiz, jstring vertex_src, jstring fragment_src,
                        jboolean linear_filter) {
    (void)thiz;
    const char* vs = get_str(env, vertex_src);
    const char* fs = get_str(env, fragment_src);
    const bool ok = (vs && fs) ? rn_video_set_shader(vs, fs, linear_filter == JNI_TRUE) : false;
    release_str(env, vertex_src, vs);
    release_str(env, fragment_src, fs);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
JNI_FN(nativeClearShader)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    rn_video_clear_shader();
}

JNIEXPORT void JNICALL
JNI_FN(nativeSetRotation)(JNIEnv* env, jobject thiz, jint degrees) {
    (void)env; (void)thiz;
    rn_video_set_rotation((int)degrees);
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativePresent)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return rn_video_present() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
JNI_FN(nativeSetAudioBufferBursts)(JNIEnv* env, jobject thiz, jint bursts) {
    (void)env; (void)thiz;
    /* rn_audio_set_buffer_bursts clamps to 0..RN_MAX_USER_BURSTS and is safe without a stream. */
    rn_audio_set_buffer_bursts((int)bursts);
}

/* Phase 4.5 WS4 (locked) */
JNIEXPORT jboolean JNICALL
JNI_FN(nativeReset)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return rn_reset() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeCheatReset)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return rn_cheat_reset() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeCheatSet)(JNIEnv* env, jobject thiz, jint index, jboolean enabled, jstring code) {
    (void)thiz;
    const char* c = get_str(env, code);
    const bool ok = c ? rn_cheat_set((unsigned)index, enabled == JNI_TRUE, c) : false;
    release_str(env, code, c);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeShutdownRequested)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return rn_shutdown_requested() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
JNI_FN(nativeSetAudioMuted)(JNIEnv* env, jobject thiz, jboolean muted) {
    (void)env; (void)thiz;
    rn_audio_set_muted(muted == JNI_TRUE);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeGetStateSize)(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    const size_t size = rn_get_state_size();
    return size > (size_t)INT32_MAX ? 0 : (jint)size;
}

/*
 * Save states go through a reusable native scratch buffer, never through GetPrimitiveArrayCritical: the
 * core's retro_serialize / retro_unserialize (plus EGL, mutexes and logging inside rn_save_state / rn_load_state)
 * must not run while the GC is blocked. Both functions are called from the emulation thread only, so the
 * buffer needs no lock; it is reused (no per-frame allocation while rewinding) and a very large one is freed
 * again after use.
 */
static void*  g_state_scratch = NULL;
static size_t g_state_scratch_cap = 0;
#define STATE_SCRATCH_KEEP_MAX ((size_t)8 * 1024 * 1024)

static void* state_scratch(size_t n) {
    if (n > g_state_scratch_cap) {
        void* grown = realloc(g_state_scratch, n);
        if (!grown) return NULL;
        g_state_scratch = grown;
        g_state_scratch_cap = n;
    }
    return g_state_scratch;
}

static void state_scratch_trim(void) {
    if (g_state_scratch_cap > STATE_SCRATCH_KEEP_MAX) {
        free(g_state_scratch);
        g_state_scratch = NULL;
        g_state_scratch_cap = 0;
    }
}

/* Serializes into a caller-owned (reusable) byte[] via the scratch buffer. */
JNIEXPORT jboolean JNICALL
JNI_FN(nativeSerializeInto)(JNIEnv* env, jobject thiz, jbyteArray dst) {
    (void)thiz;
    if (!dst) return JNI_FALSE;
    const size_t len = (size_t)(*env)->GetArrayLength(env, dst);
    const size_t needed = rn_get_state_size();
    /* Too small / unsupported: let rn_save_state report it (a NULL dst is rejected there). */
    void* scratch = (needed > 0 && len >= needed) ? state_scratch(needed) : NULL;
    if (!scratch) return rn_save_state(NULL, len) ? JNI_TRUE : JNI_FALSE;
    const bool ok = rn_save_state(scratch, needed);
    if (ok) (*env)->SetByteArrayRegion(env, dst, 0, (jsize)needed, (const jbyte*)scratch);
    state_scratch_trim();
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeUnserializeFrom)(JNIEnv* env, jobject thiz, jbyteArray src, jint size) {
    (void)thiz;
    if (!src || size <= 0 || size > (*env)->GetArrayLength(env, src)) return JNI_FALSE;
    void* scratch = state_scratch((size_t)size);
    if (!scratch) return JNI_FALSE;
    (*env)->GetByteArrayRegion(env, src, 0, size, (jbyte*)scratch);
    if ((*env)->ExceptionCheck(env)) return JNI_FALSE;
    const bool ok = rn_load_state(scratch, (size_t)size);
    state_scratch_trim();
    return ok ? JNI_TRUE : JNI_FALSE;
}
