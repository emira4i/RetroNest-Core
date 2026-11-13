/*
 * retronest_bridge.c — Libretro frontend core (video, AAudio audio, input, save states).
 *
 * Loads a Libretro core with dlopen/dlsym, registers frontend callbacks,
 * loads content and steps frames. Video frames are copied into a
 * bridge-owned, tightly packed buffer that jni_interface.c converts for Kotlin.
 * Audio goes through audio_aaudio.c; input lives in atomics so any thread may set it.
 *
 * Owned by native-bridge-agent.
 */
#include "retronest_bridge.h"
#include "audio_aaudio.h"
#include "video_gles.h"
#include "libretro.h"

#include <android/log.h>
#include <sys/auxv.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG      "RetroNest"
#define CORE_LOG_TAG "RetroNest-Core"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Core function pointers                                              */
/* ------------------------------------------------------------------ */

static unsigned (*p_retro_api_version)(void);
static void (*p_retro_init)(void);
static void (*p_retro_deinit)(void);
static void (*p_retro_set_environment)(retro_environment_t);
static void (*p_retro_set_video_refresh)(retro_video_refresh_t);
static void (*p_retro_set_audio_sample)(retro_audio_sample_t);
static void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*p_retro_set_input_poll)(retro_input_poll_t);
static void (*p_retro_set_input_state)(retro_input_state_t);
static void (*p_retro_get_system_info)(struct retro_system_info*);
static void (*p_retro_get_system_av_info)(struct retro_system_av_info*);
static bool (*p_retro_load_game)(const struct retro_game_info*);
static void (*p_retro_unload_game)(void);
static void (*p_retro_run)(void);

/* Optional symbols: a missing one disables the feature instead of failing rn_init(). */
static size_t (*p_retro_serialize_size)(void);
static bool (*p_retro_serialize)(void*, size_t);
static bool (*p_retro_unserialize)(const void*, size_t);
static void* (*p_retro_get_memory_data)(unsigned);
static size_t (*p_retro_get_memory_size)(unsigned);
static void (*p_retro_set_controller_port_device)(unsigned, unsigned);
static void (*p_retro_reset)(void);
static void (*p_retro_cheat_reset)(void);
static void (*p_retro_cheat_set)(unsigned, bool, const char*);

/* Phase 4.5 WS4: drop core audio while fast-forwarding / rewinding. */
static atomic_bool audio_muted;

/* RETRO_ENVIRONMENT_SHUTDOWN: the core asked to be closed (in-game "quit"). The emulation thread
 * polls this after each tick and ends the session instead of calling retro_run() again. */
static atomic_bool shutdown_requested;

/* ------------------------------------------------------------------ */
/* Bridge state                                                        */
/* ------------------------------------------------------------------ */

static void*            core_handle    = NULL;
static bool             core_inited    = false;   /* retro_init() has been called */
static rn_state_t       current_state  = RN_STATE_IDLE;
static rn_video_frame_t video_frame;
static rn_core_info_t   core_info;
static int              pixel_format   = RN_PIXEL_FORMAT_0RGB1555; /* Libretro default */

static uint8_t* frame_buffer      = NULL;
static size_t   frame_buffer_size = 0;

static void*  rom_data = NULL;        /* Must stay valid until retro_unload_game() */
static char*  rom_path_copy = NULL;

static char*  system_directory = NULL;
static char*  save_directory   = NULL;
static char*  core_path_copy   = NULL;
static uint64_t serialization_quirks = 0;

static char   last_error[256] = "";

/* Input — written by any thread (rn_set_input_*), read by cb_input_state on the emulation thread.
 * Relaxed ordering suffices: each value is independent and a one-frame delay is acceptable. */
#define RN_MAX_PORTS 4
static atomic_uint_least32_t input_masks[RN_MAX_PORTS];
/* Per port and stick: (uint16)x << 16 | (uint16)y, so both axes update together. */
static atomic_uint_least32_t input_analog[RN_MAX_PORTS][2];
/* Pointer (touch screen): bit 32 = pressed, (uint16)x << 16 | (uint16)y — one word so axes + press update together. */
static atomic_uint_least64_t input_pointer;

/* Disk control (stored for multi-disc support; not driven yet). */
static struct retro_disk_control_ext_callback disk_control;
static bool disk_control_set = false;

/* Rumble — written by the core on the emulation thread, polled by any thread. */
static atomic_ushort rumble_strong[RN_MAX_PORTS];
static atomic_ushort rumble_weak[RN_MAX_PORTS];

/* ------------------------------------------------------------------ */
/* Core options store (emulation thread only)                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char* key;
    char* desc;
    char* values;   /* '|'-separated allowed values */
    char* current;  /* current value; pointer handed to the core, replaced on change */
} rn_option_t;

typedef struct {
    char* key;
    char* value;
} rn_override_t;

static rn_option_t*   options          = NULL;  /* declared by the core */
static size_t         option_count     = 0;
static size_t         option_cap       = 0;
static rn_override_t* overrides        = NULL;  /* requested by Kotlin, kept even if undeclared */
static size_t         override_count   = 0;
static size_t         override_cap     = 0;
static bool           options_updated  = false;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void set_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
    LOGE("%s", last_error);
}

/* strdup() is POSIX, not ISO C11, so keep our own copy. */
static char* dup_or_null(const char* s) {
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    char* copy = malloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

static void free_str(char** s) {
    free(*s);
    *s = NULL;
}

static size_t bytes_per_pixel(int format) {
    return format == RN_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
}

static void apply_geometry(const struct retro_game_geometry* g) {
    core_info.base_width  = g->base_width;
    core_info.base_height = g->base_height;
    core_info.aspect_ratio = g->aspect_ratio > 0.0f
        ? g->aspect_ratio
        : (g->base_height ? (float)g->base_width / (float)g->base_height : 0.0f);
}

/* ------------------------------------------------------------------ */
/* Core options                                                        */
/* ------------------------------------------------------------------ */

static rn_override_t* find_override(const char* key) {
    for (size_t i = 0; i < override_count; i++) {
        if (strcmp(overrides[i].key, key) == 0) return &overrides[i];
    }
    return NULL;
}

static rn_option_t* find_option(const char* key) {
    for (size_t i = 0; i < option_count; i++) {
        if (strcmp(options[i].key, key) == 0) return &options[i];
    }
    return NULL;
}

/* True if `value` is one of the '|'-separated entries of `list` (or the list is empty). */
static bool value_allowed(const char* list, const char* value) {
    if (!list || !list[0]) return true;
    const size_t vlen = strlen(value);
    const char* p = list;
    while (*p) {
        const char* end = strchr(p, '|');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == vlen && memcmp(p, value, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static char* dup_n(const char* s, size_t n) {
    char* c = malloc(n + 1);
    if (c) { memcpy(c, s, n); c[n] = '\0'; }
    return c;
}

static void options_clear(void) {
    for (size_t i = 0; i < option_count; i++) {
        free(options[i].key);
        free(options[i].desc);
        free(options[i].values);
        free(options[i].current);
    }
    free(options);
    options = NULL;
    option_count = option_cap = 0;
}

static void overrides_clear(void) {
    for (size_t i = 0; i < override_count; i++) {
        free(overrides[i].key);
        free(overrides[i].value);
    }
    free(overrides);
    overrides = NULL;
    override_count = override_cap = 0;
}

/* Registers one option declared by the core. Strings are copied. */
static void declare_option(const char* key, const char* desc, const char* values,
                           const char* default_value) {
    if (!key || !key[0]) return;
    if (option_count == option_cap) {
        const size_t cap = option_cap ? option_cap * 2 : 32;
        rn_option_t* grown = realloc(options, cap * sizeof(*options));
        if (!grown) return;
        options = grown;
        option_cap = cap;
    }
    if (!values) values = "";

    const char* initial = default_value;
    size_t initial_len = initial ? strlen(initial) : 0;
    if (!initial || !initial[0]) { /* default = first allowed value */
        initial = values;
        const char* bar = strchr(values, '|');
        initial_len = bar ? (size_t)(bar - values) : strlen(values);
    }
    char* current = dup_n(initial, initial_len);

    /* A value requested before the declaration wins, if it is legal. */
    const rn_override_t* ov = find_override(key);
    if (ov && value_allowed(values, ov->value)) {
        free(current);
        current = dup_or_null(ov->value);
    }

    rn_option_t* o = &options[option_count];
    o->key     = dup_or_null(key);
    o->desc    = dup_or_null(desc ? desc : key);
    o->values  = dup_or_null(values);
    o->current = current;
    if (!o->key || !o->desc || !o->values || !o->current) {
        free(o->key); free(o->desc); free(o->values); free(o->current);
        return;
    }
    option_count++;
}

/* Joins the non-NULL values of a libretro option-value array with '|'. */
static char* join_values(const struct retro_core_option_value* vals) {
    size_t total = 1;
    size_t n = 0;
    for (; n < RETRO_NUM_CORE_OPTION_VALUES_MAX && vals[n].value; n++) total += strlen(vals[n].value) + 1;
    char* out = malloc(total);
    if (!out) return NULL;
    char* w = out;
    for (size_t i = 0; i < n; i++) {
        const size_t l = strlen(vals[i].value);
        if (i) *w++ = '|';
        memcpy(w, vals[i].value, l);
        w += l;
    }
    *w = '\0';
    return out;
}

static void options_from_v0(const struct retro_variable* vars) {
    options_clear();
    for (; vars && vars->key; vars++) {
        const char* spec = vars->value ? vars->value : "";
        const char* sep = strstr(spec, "; ");
        char* desc = sep ? dup_n(spec, (size_t)(sep - spec)) : dup_or_null(vars->key);
        declare_option(vars->key, desc, sep ? sep + 2 : spec, NULL);
        free(desc);
    }
}

static void options_from_v1(const struct retro_core_option_definition* defs) {
    options_clear();
    for (; defs && defs->key; defs++) {
        char* vals = join_values(defs->values);
        declare_option(defs->key, defs->desc, vals, defs->default_value);
        free(vals);
    }
}

static void options_from_v2(const struct retro_core_option_v2_definition* defs) {
    options_clear();
    for (; defs && defs->key; defs++) {
        char* vals = join_values(defs->values);
        declare_option(defs->key, defs->desc, vals, defs->default_value);
        free(vals);
    }
}

bool rn_set_core_option(const char* key, const char* value) {
    if (!key || !key[0] || !value || !value[0]) return false;

    rn_override_t* ov = find_override(key);
    if (!ov) {
        if (override_count == override_cap) {
            const size_t cap = override_cap ? override_cap * 2 : 32;
            rn_override_t* grown = realloc(overrides, cap * sizeof(*overrides));
            if (!grown) return false;
            overrides = grown;
            override_cap = cap;
        }
        char* k = dup_or_null(key);
        char* v = dup_or_null(value);
        if (!k || !v) { free(k); free(v); return false; }
        overrides[override_count].key   = k;
        overrides[override_count].value = v;
        override_count++;
    } else {
        char* v = dup_or_null(value);
        if (!v) return false;
        free(ov->value);
        ov->value = v;
    }

    rn_option_t* o = find_option(key);
    if (o && value_allowed(o->values, value) && strcmp(o->current, value) != 0) {
        char* v = dup_or_null(value);
        if (v) {
            free(o->current);
            o->current = v;
        }
    }
    options_updated = true;
    return true;
}

size_t rn_get_core_option_count(void) {
    return option_count;
}

bool rn_get_core_option(size_t index, const char** key, const char** value,
                        const char** desc, const char** values) {
    if (index >= option_count) return false;
    const rn_option_t* o = &options[index];
    if (key)    *key    = o->key;
    if (value)  *value  = o->current;
    if (desc)   *desc   = o->desc;
    if (values) *values = o->values;
    return true;
}

/* ---- Rumble ---- */

uint16_t rn_get_rumble(unsigned port) {
    if (port >= RN_MAX_PORTS) return 0;
    const uint16_t s = atomic_load_explicit(&rumble_strong[port], memory_order_relaxed);
    const uint16_t w = atomic_load_explicit(&rumble_weak[port], memory_order_relaxed);
    return s > w ? s : w;
}

static void reset_rumble(void) {
    for (unsigned p = 0; p < RN_MAX_PORTS; p++) {
        atomic_store_explicit(&rumble_strong[p], 0, memory_order_relaxed);
        atomic_store_explicit(&rumble_weak[p], 0, memory_order_relaxed);
    }
}

static bool cb_set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
    if (port >= RN_MAX_PORTS) return false;
    if (effect == RETRO_RUMBLE_STRONG) {
        atomic_store_explicit(&rumble_strong[port], strength, memory_order_relaxed);
    } else if (effect == RETRO_RUMBLE_WEAK) {
        atomic_store_explicit(&rumble_weak[port], strength, memory_order_relaxed);
    } else {
        return false;
    }
    return true;
}

/* ---- Performance interface ---- */

static retro_time_t cb_perf_get_time_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (retro_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* CPU features cores may ask for (e.g. to pick NEON code paths). NEON / ASIMD / VFPv3 / VFPv4 are architectural
 * on arm64; the crypto and CRC extras come from the kernel's hwcap bits (AArch64 HWCAP_* values). */
static uint64_t cb_perf_get_cpu_features(void) {
    uint64_t f = 0;
#if defined(__aarch64__)
    f |= RETRO_SIMD_NEON | RETRO_SIMD_ASIMD | RETRO_SIMD_VFPV3 | RETRO_SIMD_VFPV4;
    const unsigned long hw = getauxval(AT_HWCAP);
    if (hw & (1ul << 3)) f |= RETRO_SIMD_AES;    /* HWCAP_AES   */
#ifdef RETRO_SIMD_PCLMUL
    if (hw & (1ul << 4)) f |= RETRO_SIMD_PCLMUL; /* HWCAP_PMULL */
#endif
#ifdef RETRO_SIMD_SHA1
    if (hw & (1ul << 5)) f |= RETRO_SIMD_SHA1;   /* HWCAP_SHA1  */
#endif
#ifdef RETRO_SIMD_CRC32
    if (hw & (1ul << 7)) f |= RETRO_SIMD_CRC32;  /* HWCAP_CRC32 */
#endif
#endif
    return f;
}

static retro_perf_tick_t cb_perf_get_counter(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (retro_perf_tick_t)ts.tv_sec * 1000000000ull + (retro_perf_tick_t)ts.tv_nsec;
}

static void cb_perf_register(struct retro_perf_counter* c) { (void)c; }
static void cb_perf_start(struct retro_perf_counter* c)    { (void)c; }
static void cb_perf_stop(struct retro_perf_counter* c)     { (void)c; }
static void cb_perf_log(void) {}

/* ------------------------------------------------------------------ */
/* Libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

/* retro_log_printf_t has no tag argument, so it must NOT be aliased to __android_log_print. */
static void cb_log(enum retro_log_level level, const char* fmt, ...) {
    int prio;
    switch (level) {
        /* Debug chatter can be per-frame (MAME 2003-Plus: "read output port"): never log it. */
        case RETRO_LOG_DEBUG: return;
        case RETRO_LOG_INFO:  prio = ANDROID_LOG_INFO;  break;
        case RETRO_LOG_WARN:  prio = ANDROID_LOG_WARN;  break;
        default:              prio = ANDROID_LOG_ERROR; break;
    }
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(prio, CORE_LOG_TAG, fmt, ap);
    va_end(ap);
}

static void cb_video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    /* HW cores: the frame is in the FBO (video_gles.c). Only its size is recorded here. */
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        if (!rn_video_hw_active() || width == 0 || height == 0) return;
        rn_video_hw_on_frame(&width, &height); /* clamps to the FBO */
        video_frame.data         = NULL;          /* no CPU copy: JNI reads the FBO back */
        video_frame.width        = width;
        video_frame.height       = height;
        video_frame.pitch        = (size_t)width * 4;
        video_frame.pixel_format = RN_PIXEL_FORMAT_XRGB8888;
        video_frame.frame_ready  = true;
        return;
    }
    /* NULL = duplicate frame (GET_CAN_DUPE): keep showing the previous one. */
    if (!data || width == 0 || height == 0) {
        return;
    }

    const size_t row_bytes = width * bytes_per_pixel(pixel_format);
    const size_t needed    = row_bytes * height;

    if (needed > frame_buffer_size) {
        uint8_t* grown = realloc(frame_buffer, needed);
        if (!grown) {
            LOGE("Out of memory for %ux%u frame", width, height);
            return;
        }
        frame_buffer      = grown;
        frame_buffer_size = needed;
    }

    /* Copy row by row into a tightly packed buffer: the source pitch may exceed the row
     * width, and the last source row is not guaranteed to be pitch bytes long. */
    const uint8_t* src = (const uint8_t*)data;
    for (unsigned y = 0; y < height; y++) {
        memcpy(frame_buffer + y * row_bytes, src + y * pitch, row_bytes);
    }

    video_frame.data         = frame_buffer;
    video_frame.width        = width;
    video_frame.height       = height;
    video_frame.pitch        = row_bytes;
    video_frame.pixel_format = pixel_format;
    video_frame.frame_ready  = true;
    rn_video_on_frame(); /* GPU path re-uploads on its own sequence; frame_ready stays for JNI */
}

static void cb_audio_sample(int16_t left, int16_t right) {
    if (atomic_load_explicit(&audio_muted, memory_order_relaxed)) return;
    rn_audio_write_sample(left, right);
}

static size_t cb_audio_sample_batch(const int16_t* data, size_t frames) {
    if (atomic_load_explicit(&audio_muted, memory_order_relaxed)) return frames;
    return rn_audio_write(data, frames);
}

static void cb_input_poll(void) {
    /* Input is pushed asynchronously into atomics; nothing to poll. */
}

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port >= RN_MAX_PORTS) return 0;
    const uint32_t mask = (uint32_t)atomic_load_explicit(&input_masks[port], memory_order_relaxed);

    switch (device & RETRO_DEVICE_MASK) {
        case RETRO_DEVICE_JOYPAD:
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(uint16_t)(mask & 0xFFFFu);
            return (id < 16 && (mask & (1u << id))) ? 1 : 0;

        case RETRO_DEVICE_ANALOG:
            if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT || index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
                const uint32_t packed = (uint32_t)atomic_load_explicit(&input_analog[port][index],
                                                                       memory_order_relaxed);
                if (id == RETRO_DEVICE_ID_ANALOG_X) return (int16_t)(uint16_t)(packed >> 16);
                if (id == RETRO_DEVICE_ID_ANALOG_Y) return (int16_t)(uint16_t)(packed & 0xFFFFu);
                return 0;
            }
            if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) { /* digital buttons as full-scale analog */
                return (id < 16 && (mask & (1u << id))) ? 0x7FFF : 0;
            }
            return 0;

        case RETRO_DEVICE_POINTER: {
            if (port != 0 || index != 0) return 0;
            const uint64_t p = (uint64_t)atomic_load_explicit(&input_pointer, memory_order_relaxed);
            const bool pressed = (p >> 32) != 0;
            switch (id) {
                case RETRO_DEVICE_ID_POINTER_X:       return (int16_t)(uint16_t)(p >> 16);
                case RETRO_DEVICE_ID_POINTER_Y:       return (int16_t)(uint16_t)(p & 0xFFFFu);
                case RETRO_DEVICE_ID_POINTER_PRESSED:
                case RETRO_DEVICE_ID_POINTER_COUNT:   return pressed ? 1 : 0;
                default:                              return 0;
            }
        }

        default:
            return 0;
    }
}

/* Experimental / private flag bits are not part of the command identity. */
#define ENV_ID(c) ((c) & 0xFFFFu)
#define RN_MAX_SEEN_UNKNOWN 64

static void log_unknown_env(unsigned cmd) {
    static unsigned seen[RN_MAX_SEEN_UNKNOWN];
    static unsigned seen_count = 0;
    for (unsigned i = 0; i < seen_count; i++) {
        if (seen[i] == cmd) return;
    }
    if (seen_count < RN_MAX_SEEN_UNKNOWN) seen[seen_count++] = cmd;
    LOGI("Unhandled environment command %u (0x%x)", cmd, cmd);
}

/*
 * NULL data guard. libretro lets cores call some environment commands with data == NULL to ask
 * "is this supported?" (e.g. GET_DEVICE_POWER, GET_INPUT_BITMASKS). melonDS DS does this from
 * retro_set_environment, and every handler below writes through data, so never let a NULL reach
 * them: report support for the commands we handle, and refuse everything else.
 */
static bool env_supported_without_data(unsigned id) {
    switch (id) {
        case ENV_ID(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_CAN_DUPE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION):
        case ENV_ID(RETRO_ENVIRONMENT_GET_DEVICE_POWER):
        case ENV_ID(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION):
        case ENV_ID(RETRO_ENVIRONMENT_GET_FASTFORWARDING):
        case ENV_ID(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS):
        case ENV_ID(RETRO_ENVIRONMENT_GET_JIT_CAPABLE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER):
        case ENV_ID(RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES):
        case ENV_ID(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS):
        case ENV_ID(RETRO_ENVIRONMENT_GET_LANGUAGE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH):
        case ENV_ID(RETRO_ENVIRONMENT_GET_LOG_INTERFACE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION):
        case ENV_ID(RETRO_ENVIRONMENT_GET_PERF_INTERFACE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT):
        case ENV_ID(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY):
        case ENV_ID(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY):
        case ENV_ID(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_THROTTLE_STATE):
        case ENV_ID(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL):
        case ENV_ID(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE):
        case ENV_ID(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE):
        case ENV_ID(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE):
        case ENV_ID(RETRO_ENVIRONMENT_SET_GEOMETRY):
        case ENV_ID(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_MEMORY_MAPS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_MESSAGE):
        case ENV_ID(RETRO_ENVIRONMENT_SET_MESSAGE_EXT):
        case ENV_ID(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY):
        case ENV_ID(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL):
        case ENV_ID(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO):
        case ENV_ID(RETRO_ENVIRONMENT_SET_VARIABLES):
            return true;
        /* SET_HW_RENDER / GET_HW_RENDER_INTERFACE need data: a NULL query is refused (default). */
        default:
            return false;
    }
}

/*
 * RetroArch-private commands (0x800000 block, see RetroArch retroarch.h). They must not reach the
 * ENV_ID() dispatch: (3 | 0x800000) would alias GET_CAN_DUPE and write a bool into the function
 * pointer Mupen64Plus-Next asks for with GET_CLEAR_ALL_THREAD_WAITS_CB.
 */
#define RN_ENV_RETROARCH_BLOCK               0x800000u
#define RN_ENV_GET_CLEAR_ALL_THREAD_WAITS_CB (3u | RN_ENV_RETROARCH_BLOCK)

/* No frontend thread ever waits on the core here, so there is nothing to clear. */
static bool cb_clear_all_thread_waits(unsigned clear_threads, void* data) {
    (void)clear_threads; (void)data;
    return true;
}

static bool cb_environment_impl(unsigned cmd, void* data);

static bool cb_environment(unsigned cmd, void* data) {
    if (cmd & RN_ENV_RETROARCH_BLOCK) {
        if (cmd == RN_ENV_GET_CLEAR_ALL_THREAD_WAITS_CB && data) {
            *(retro_environment_t*)data = cb_clear_all_thread_waits;
            return true;
        }
        return false;
    }
    /* Experimental HW commands are matched on the FULL value: ENV_ID() would alias
     * SET_HW_SHARED_CONTEXT (44 | EXPERIMENTAL) to SET_SERIALIZATION_QUIRKS (44). */
    if (cmd == RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT) {
        return true; /* data is N/A (normally NULL); we have one context, shared anyway */
    }
    if (cmd == RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE) {
        return false; /* only Vulkan / D3D have one */
    }
    if (cmd == RETRO_ENVIRONMENT_SHUTDOWN) { /* data is N/A */
        LOGI("Core requested shutdown");
        atomic_store_explicit(&shutdown_requested, true, memory_order_relaxed);
        return true;
    }
    if (data == NULL) {
        return env_supported_without_data(ENV_ID(cmd));
    }
    return cb_environment_impl(cmd, data);
}

static bool cb_environment_impl(unsigned cmd, void* data) {
    switch (ENV_ID(cmd)) {
        case ENV_ID(RETRO_ENVIRONMENT_GET_LOG_INTERFACE): {
            struct retro_log_callback* cb = (struct retro_log_callback*)data;
            cb->log = cb_log;
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT): {
            const enum retro_pixel_format fmt = *(const enum retro_pixel_format*)data;
            if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 &&
                fmt != RETRO_PIXEL_FORMAT_XRGB8888 &&
                fmt != RETRO_PIXEL_FORMAT_RGB565) {
                LOGW("Core requested unsupported pixel format %d", (int)fmt);
                return false;
            }
            pixel_format = (int)fmt;
            LOGI("Core requested pixel format: %d", pixel_format);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY):
            *(const char**)data = system_directory; /* NULL is valid: "no directory" */
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY):
            *(const char**)data = save_directory;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY):
            *(const char**)data = system_directory;
            return system_directory != NULL;
        case ENV_ID(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH):
            *(const char**)data = core_path_copy;
            return core_path_copy != NULL;
        case ENV_ID(RETRO_ENVIRONMENT_GET_CAN_DUPE):
            *(bool*)data = true;
            return true;

        /* ---- Core options ---- */
        case ENV_ID(RETRO_ENVIRONMENT_SET_VARIABLES):
            options_from_v0((const struct retro_variable*)data);
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS):
            options_from_v1((const struct retro_core_option_definition*)data);
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL): {
            const struct retro_core_options_intl* intl = (const struct retro_core_options_intl*)data;
            if (intl) options_from_v1(intl->us);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2): {
            const struct retro_core_options_v2* v2 = (const struct retro_core_options_v2*)data;
            if (v2) options_from_v2(v2->definitions); /* categories are ignored */
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL): {
            const struct retro_core_options_v2_intl* intl = (const struct retro_core_options_v2_intl*)data;
            if (intl && intl->us) options_from_v2(intl->us->definitions);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION):
            *(unsigned*)data = 2;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK):
            return true; /* accepted, visibility is ignored */
        case ENV_ID(RETRO_ENVIRONMENT_GET_VARIABLE): {
            struct retro_variable* var = (struct retro_variable*)data;
            if (!var || !var->key) return false;
            const rn_option_t* o = find_option(var->key);
            if (!o) {
                var->value = NULL;
                return false;
            }
            var->value = o->current;
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE):
            *(bool*)data = options_updated;
            options_updated = false;
            return true;

        /* ---- Geometry / timing ---- */
        case ENV_ID(RETRO_ENVIRONMENT_SET_GEOMETRY): {
            const struct retro_game_geometry* g = (const struct retro_game_geometry*)data;
            apply_geometry(g);
            /* max_* must not change here per libretro.h, but a larger base size still needs room. */
            rn_video_hw_set_size(g->base_width, g->base_height);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO): {
            const struct retro_system_av_info* av = (const struct retro_system_av_info*)data;
            apply_geometry(&av->geometry);
            rn_video_hw_set_size(av->geometry.max_width > av->geometry.base_width
                                     ? av->geometry.max_width : av->geometry.base_width,
                                 av->geometry.max_height > av->geometry.base_height
                                     ? av->geometry.max_height : av->geometry.base_height);
            core_info.fps         = av->timing.fps;
            core_info.sample_rate = av->timing.sample_rate;
            rn_audio_set_timing(av->timing.sample_rate, av->timing.fps);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE):
            *(float*)data = core_info.fps > 1.0 ? (float)core_info.fps : 60.0f;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_FASTFORWARDING):
            *(bool*)data = false;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE):
            *(int*)data = 3; /* video + audio */
            return true;

        /* ---- Input ---- */
        case ENV_ID(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS):
            return true; /* data is ignored per libretro.h */
        case ENV_ID(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS):
            *(unsigned*)data = RN_MAX_PORTS;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE): {
            struct retro_rumble_interface* r = (struct retro_rumble_interface*)data;
            r->set_rumble_state = cb_set_rumble_state;
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES):
            *(uint64_t*)data = (1ull << RETRO_DEVICE_JOYPAD) | (1ull << RETRO_DEVICE_ANALOG) |
                               (1ull << RETRO_DEVICE_POINTER);
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO):
            return true; /* Accepted; the RetroPad layout is fixed for now. */

        /* ---- Messages / misc ---- */
        case ENV_ID(RETRO_ENVIRONMENT_SET_MESSAGE): {
            const struct retro_message* msg = (const struct retro_message*)data;
            if (msg && msg->msg) LOGI("Core message: %s", msg->msg);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION):
            *(unsigned*)data = 0;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_MESSAGE_EXT): {
            const struct retro_message_ext* msg = (const struct retro_message_ext*)data;
            if (msg && msg->msg) LOGI("Core message: %s", msg->msg);
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_LANGUAGE):
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT):
            if (data) *(int*)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS):
            if (data) serialization_quirks = *(const uint64_t*)data;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_MEMORY_MAPS):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO):
        case ENV_ID(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL):
        case ENV_ID(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME):
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_PERF_INTERFACE): {
            struct retro_perf_callback* perf = (struct retro_perf_callback*)data;
            perf->get_time_usec    = cb_perf_get_time_usec;
            perf->get_cpu_features = cb_perf_get_cpu_features;
            perf->get_perf_counter = cb_perf_get_counter;
            perf->perf_register    = cb_perf_register;
            perf->perf_start       = cb_perf_start;
            perf->perf_stop        = cb_perf_stop;
            perf->perf_log         = cb_perf_log;
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION):
            *(unsigned*)data = 1; /* EXT interface supported */
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE):
            if (data) { disk_control = *(const struct retro_disk_control_ext_callback*)data; disk_control_set = true; }
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE):
            if (data) {
                memset(&disk_control, 0, sizeof(disk_control));
                memcpy(&disk_control, data, sizeof(struct retro_disk_control_callback));
                disk_control_set = true;
            }
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY):
        case ENV_ID(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE):
            return true; /* accepted; the AAudio buffer / frame pacing are frontend-controlled */
        case ENV_ID(RETRO_ENVIRONMENT_GET_THROTTLE_STATE): {
            struct retro_throttle_state* t = (struct retro_throttle_state*)data;
            t->mode = RETRO_THROTTLE_NONE;
            t->rate = core_info.fps > 1.0 ? (float)core_info.fps : 60.0f;
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_GET_DEVICE_POWER): {
            struct retro_device_power* pw = (struct retro_device_power*)data;
            pw->state   = RETRO_POWERSTATE_UNKNOWN;
            pw->seconds = RETRO_POWERSTATE_NO_ESTIMATE;
            pw->percent = -1;
            return true;
        }
        case ENV_ID(RETRO_ENVIRONMENT_SET_ROTATION):
            return false; /* not supported yet */
        /* ---- Hardware rendering (OpenGL ES only, video_gles.c) ---- */
        case ENV_ID(RETRO_ENVIRONMENT_SET_HW_RENDER):
            return rn_video_hw_set((struct retro_hw_render_callback*)data);
        case ENV_ID(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER):
            *(unsigned*)data = RETRO_HW_CONTEXT_OPENGLES3;
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_JIT_CAPABLE):
            *(bool*)data = true; /* Android allows JIT (dynarecs) */
            return true;
        case ENV_ID(RETRO_ENVIRONMENT_GET_VFS_INTERFACE):
            return false;
        default:
            log_unknown_env(cmd);
            return false;
    }
}

/* ------------------------------------------------------------------ */
/* Core loading                                                        */
/* ------------------------------------------------------------------ */

#define LOAD_SYM(sym) do {                                              \
        *(void**)(&p_##sym) = dlsym(core_handle, #sym);                 \
        if (!p_##sym) {                                                 \
            set_error("Missing core symbol: %s", #sym);                 \
            return false;                                               \
        }                                                               \
    } while (0)

static bool load_symbols(void) {
    LOAD_SYM(retro_api_version);
    LOAD_SYM(retro_init);
    LOAD_SYM(retro_deinit);
    LOAD_SYM(retro_set_environment);
    LOAD_SYM(retro_set_video_refresh);
    LOAD_SYM(retro_set_audio_sample);
    LOAD_SYM(retro_set_audio_sample_batch);
    LOAD_SYM(retro_set_input_poll);
    LOAD_SYM(retro_set_input_state);
    LOAD_SYM(retro_get_system_info);
    LOAD_SYM(retro_get_system_av_info);
    LOAD_SYM(retro_load_game);
    LOAD_SYM(retro_unload_game);
    LOAD_SYM(retro_run);

    *(void**)(&p_retro_serialize_size)  = dlsym(core_handle, "retro_serialize_size");
    *(void**)(&p_retro_serialize)       = dlsym(core_handle, "retro_serialize");
    *(void**)(&p_retro_unserialize)     = dlsym(core_handle, "retro_unserialize");
    *(void**)(&p_retro_get_memory_data) = dlsym(core_handle, "retro_get_memory_data");
    *(void**)(&p_retro_get_memory_size) = dlsym(core_handle, "retro_get_memory_size");
    *(void**)(&p_retro_set_controller_port_device) = dlsym(core_handle, "retro_set_controller_port_device");
    *(void**)(&p_retro_reset)        = dlsym(core_handle, "retro_reset");
    *(void**)(&p_retro_cheat_reset)  = dlsym(core_handle, "retro_cheat_reset");
    *(void**)(&p_retro_cheat_set)    = dlsym(core_handle, "retro_cheat_set");
    if (!p_retro_serialize_size || !p_retro_serialize || !p_retro_unserialize) {
        LOGW("Core does not export retro_serialize*: save states disabled");
    }
    return true;
}

static void reset_input(void) {
    for (unsigned p = 0; p < RN_MAX_PORTS; p++) {
        atomic_store_explicit(&input_masks[p], 0, memory_order_relaxed);
        atomic_store_explicit(&input_analog[p][0], 0, memory_order_relaxed);
        atomic_store_explicit(&input_analog[p][1], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&input_pointer, 0, memory_order_relaxed);
}

/* Releases everything owned by the bridge. Safe to call in any state. */
static void release_all(void) {
    if (core_inited && p_retro_deinit) {
        p_retro_deinit();
    }
    core_inited = false;

    rn_audio_close(); /* idempotent; frees the audio ring even if no game was loaded */
    rn_video_shutdown(); /* EGL surface/context/display + window reference (idempotent) */

    if (core_handle) {
        dlclose(core_handle);
        core_handle = NULL;
    }
    p_retro_serialize_size  = NULL;
    p_retro_serialize       = NULL;
    p_retro_unserialize     = NULL;
    p_retro_get_memory_data = NULL;
    p_retro_get_memory_size = NULL;
    p_retro_set_controller_port_device = NULL;
    p_retro_reset = NULL;
    p_retro_cheat_reset = NULL;
    p_retro_cheat_set = NULL;
    atomic_store_explicit(&audio_muted, false, memory_order_relaxed);
    atomic_store_explicit(&shutdown_requested, false, memory_order_relaxed);
    reset_input();
    reset_rumble();
    options_clear();
    overrides_clear();
    options_updated = false;
    serialization_quirks = 0;
    memset(&disk_control, 0, sizeof(disk_control));
    disk_control_set = false;

    free(frame_buffer);
    frame_buffer      = NULL;
    frame_buffer_size = 0;

    free(rom_data);
    rom_data = NULL;
    free_str(&rom_path_copy);

    free_str((char**)&core_info.library_name);
    free_str((char**)&core_info.library_version);
    free_str((char**)&core_info.valid_extensions);
    free_str(&system_directory);
    free_str(&save_directory);
    free_str(&core_path_copy);

    memset(&core_info, 0, sizeof(core_info));
    memset(&video_frame, 0, sizeof(video_frame));
    pixel_format  = RN_PIXEL_FORMAT_0RGB1555;
    current_state = RN_STATE_IDLE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool rn_init(const char* core_path, const char* system_dir, const char* save_dir) {
    if (current_state != RN_STATE_IDLE) {
        LOGW("rn_init called while a core is active; reinitializing");
        rn_deinit();
    }
    last_error[0] = '\0';

    if (!core_path) {
        set_error("Core path is null");
        return false;
    }
    LOGI("Loading core: %s", core_path);

    core_handle = dlopen(core_path, RTLD_LAZY | RTLD_LOCAL);
    if (!core_handle) {
        set_error("dlopen failed: %s", dlerror());
        return false;
    }

    if (!load_symbols()) {
        release_all();
        return false;
    }

    const unsigned api = p_retro_api_version();
    if (api != RETRO_API_VERSION) {
        set_error("Core API version %u does not match frontend %u", api, (unsigned)RETRO_API_VERSION);
        release_all();
        return false;
    }

    core_path_copy   = dup_or_null(core_path);
    system_directory = dup_or_null(system_dir);
    save_directory   = dup_or_null(save_dir);

    /* Environment first: cores may query it from inside retro_init(). */
    p_retro_set_environment(cb_environment);
    p_retro_set_video_refresh(cb_video_refresh);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_sample_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);

    p_retro_init();
    core_inited = true;

    struct retro_system_info sys = {0};
    p_retro_get_system_info(&sys);
    core_info.library_name     = dup_or_null(sys.library_name);
    core_info.library_version  = dup_or_null(sys.library_version);
    core_info.valid_extensions = dup_or_null(sys.valid_extensions);
    core_info.need_fullpath    = sys.need_fullpath;

    LOGI("Core loaded: %s %s (extensions: %s)",
         core_info.library_name ? core_info.library_name : "?",
         core_info.library_version ? core_info.library_version : "?",
         core_info.valid_extensions ? core_info.valid_extensions : "?");

    current_state = RN_STATE_LOADED;
    return true;
}

void rn_deinit(void) {
    if (current_state == RN_STATE_IDLE && !core_handle) {
        return;
    }
    rn_unload_game();
    release_all();
    LOGI("Core deinitialized");
}

static bool read_file(const char* path, void** out_data, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        set_error("Cannot open ROM: %s", path);
        return false;
    }

    bool ok = false;
    void* buf = NULL;
    long size = -1;

    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        set_error("ROM is empty or unreadable: %s", path);
        goto done;
    }

    buf = malloc((size_t)size);
    if (!buf) {
        set_error("Out of memory reading ROM (%ld bytes)", size);
        goto done;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        set_error("Short read on ROM: %s", path);
        free(buf);
        buf = NULL;
        goto done;
    }

    *out_data = buf;
    *out_size = (size_t)size;
    ok = true;

done:
    fclose(f);
    return ok;
}

bool rn_load_game(const char* rom_path) {
    if (current_state != RN_STATE_LOADED) {
        set_error("Cannot load game: core not in LOADED state (state=%d)", (int)current_state);
        return false;
    }
    if (!rom_path) {
        set_error("ROM path is null");
        return false;
    }
    LOGI("Loading ROM: %s", rom_path);

    struct retro_game_info game = {0};
    rn_video_hw_cancel(); /* SET_HW_RENDER applies to this load only */
    rom_path_copy = dup_or_null(rom_path);
    game.path = rom_path_copy;

    if (!core_info.need_fullpath) {
        size_t size = 0;
        if (!read_file(rom_path, &rom_data, &size)) {
            free_str(&rom_path_copy);
            return false;
        }
        game.data = rom_data;
        game.size = size;
    }

    if (!p_retro_load_game(&game)) {
        set_error("Core rejected the ROM: %s", rom_path);
        rn_video_hw_cancel();
        free(rom_data);
        rom_data = NULL;
        free_str(&rom_path_copy);
        return false;
    }

    struct retro_system_av_info av = {0};
    p_retro_get_system_av_info(&av);
    apply_geometry(&av.geometry);
    core_info.fps         = av.timing.fps;
    core_info.sample_rate = av.timing.sample_rate;

    LOGI("Game loaded — base: %ux%u, max: %ux%u, fps: %.2f, aspect: %.3f",
         av.geometry.base_width, av.geometry.base_height,
         av.geometry.max_width, av.geometry.max_height,
         av.timing.fps, (double)core_info.aspect_ratio);

    if (rn_video_hw_requested()) {
        /* The core renders with OpenGL ES: context + FBO now, then context_reset (which is where
         * N64 / PSP cores create their GL resources and finish booting). */
        const unsigned fbo_w = av.geometry.max_width > av.geometry.base_width
                                   ? av.geometry.max_width : av.geometry.base_width;
        const unsigned fbo_h = av.geometry.max_height > av.geometry.base_height
                                   ? av.geometry.max_height : av.geometry.base_height;
        if (!rn_video_hw_start(fbo_w, fbo_h)) {
            set_error("This game needs GPU rendering (OpenGL ES), which failed to start on this device");
            rn_video_hw_context_destroy();
            p_retro_unload_game();
            free(rom_data);
            rom_data = NULL;
            free_str(&rom_path_copy);
            return false;
        }
        LOGI("Hardware rendering active (OpenGL ES)");
    }

    reset_input();
    reset_rumble();
    if (p_retro_set_controller_port_device) {
        p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    }
    if (!rn_audio_open(av.timing.sample_rate, av.timing.fps)) {
        LOGW("Audio unavailable; running without sound (will retry)");
    }

    current_state = RN_STATE_RUNNING;
    return true;
}

void rn_unload_game(void) {
    if (current_state != RN_STATE_RUNNING && current_state != RN_STATE_PAUSED) {
        return;
    }
    rn_audio_close(); /* stop the consumer before the core goes away */
    if (rn_video_hw_active()) {
        /* Like RetroArch: context_destroy (context current) before retro_unload_game. */
        rn_video_hw_context_destroy();
    }
    p_retro_unload_game();
    free(rom_data);
    rom_data = NULL;
    free_str(&rom_path_copy);
    reset_rumble();
    atomic_store_explicit(&input_pointer, 0, memory_order_relaxed);
    video_frame.frame_ready = false;
    current_state = RN_STATE_LOADED;
    LOGI("Game unloaded");
}

void rn_run_frame(void) {
    if (current_state == RN_STATE_RUNNING) {
        if (!rn_video_hw_begin_frame()) {
            /* No usable GL context: running the core would crash it. The engine reports the
             * failure through nativePresent / nativeSetSurface. */
            static bool logged = false;
            if (!logged) { logged = true; LOGE("HW core frame skipped: no usable GL context"); }
            return;
        }
        p_retro_run();
        if (!atomic_load_explicit(&audio_muted, memory_order_relaxed)) rn_audio_end_frame();
    }
}

void rn_set_paused(bool paused) {
    if (current_state != RN_STATE_RUNNING && current_state != RN_STATE_PAUSED) {
        LOGW("rn_set_paused(%d) ignored in state %d", (int)paused, (int)current_state);
        return;
    }
    const rn_state_t target = paused ? RN_STATE_PAUSED : RN_STATE_RUNNING;
    if (target == current_state) return;

    current_state = target;
    if (paused) {
        rn_audio_pause();
    } else {
        rn_audio_resume();
    }
    LOGI("Emulation %s", paused ? "paused" : "resumed");
}

void rn_set_input_mask(unsigned port, uint32_t mask) {
    if (port >= RN_MAX_PORTS) return;
    atomic_store_explicit(&input_masks[port], mask, memory_order_relaxed);
}

void rn_set_analog(unsigned port, unsigned stick, int16_t x, int16_t y) {
    if (port >= RN_MAX_PORTS || stick > 1) return;
    const uint32_t packed = ((uint32_t)(uint16_t)x << 16) | (uint32_t)(uint16_t)y;
    atomic_store_explicit(&input_analog[port][stick], packed, memory_order_relaxed);
}

void rn_set_pointer(int16_t x, int16_t y, bool pressed) {
    const uint64_t packed = ((uint64_t)(pressed ? 1u : 0u) << 32) | ((uint64_t)(uint16_t)x << 16) |
                            (uint64_t)(uint16_t)y;
    atomic_store_explicit(&input_pointer, packed, memory_order_relaxed);
}

static bool game_active(void) {
    return current_state == RN_STATE_RUNNING || current_state == RN_STATE_PAUSED;
}

size_t rn_get_state_size(void) {
    if (!game_active() || !p_retro_serialize_size || !p_retro_serialize || !p_retro_unserialize) {
        return 0;
    }
    return p_retro_serialize_size();
}

bool rn_save_state(void* dst, size_t size) {
    if (!game_active()) {
        set_error("Cannot save state: no game running (state=%d)", (int)current_state);
        return false;
    }
    const size_t needed = rn_get_state_size();
    if (needed == 0) {
        set_error("Core does not support save states");
        return false;
    }
    if (!dst || size < needed) {
        set_error("Save state buffer too small (%zu < %zu bytes)", size, needed);
        return false;
    }
    if (!rn_video_hw_begin_frame()) {
        set_error("Cannot save state: no usable GL context");
        return false;
    }
    if (!p_retro_serialize(dst, needed)) {
        set_error("Core failed to serialize state");
        return false;
    }
    return true;
}

bool rn_load_state(const void* src, size_t size) {
    if (!game_active()) {
        set_error("Cannot load state: no game running (state=%d)", (int)current_state);
        return false;
    }
    const size_t expected = rn_get_state_size();
    if (expected == 0) {
        set_error("Core does not support save states");
        return false;
    }
    if (!src || size == 0) {
        set_error("Save state is empty");
        return false;
    }
    if (size != expected) {
        /* Some cores' serialize size varies slightly; the core makes the definitive check. */
        LOGW("Save state size %zu differs from current serialize size %zu", size, expected);
    }
    if (!rn_video_hw_begin_frame()) {
        set_error("Cannot load state: no usable GL context");
        return false;
    }
    if (!p_retro_unserialize(src, size)) {
        set_error("Core rejected the save state (%zu bytes)", size);
        return false;
    }
    return true;
}

bool rn_shutdown_requested(void) {
    return atomic_load_explicit(&shutdown_requested, memory_order_relaxed);
}

void rn_audio_set_muted(bool muted) {
    atomic_store_explicit(&audio_muted, muted, memory_order_relaxed);
}

bool rn_reset(void) {
    if (!game_active() || !p_retro_reset) return false;
    if (!rn_video_hw_begin_frame()) return false;
    p_retro_reset();
    return true;
}

bool rn_cheat_reset(void) {
    if (!game_active() || !p_retro_cheat_reset) return false;
    p_retro_cheat_reset();
    return true;
}

bool rn_cheat_set(unsigned index, bool enabled, const char* code) {
    if (!game_active() || !p_retro_cheat_set || !code) return false;
    p_retro_cheat_set(index, enabled, code);
    return true;
}

void* rn_get_sram(size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!game_active() || !p_retro_get_memory_data || !p_retro_get_memory_size) {
        return NULL;
    }
    void* data = p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!data || size == 0) {
        return NULL;
    }
    if (out_size) *out_size = size;
    return data;
}

rn_state_t rn_get_state(void) {
    return current_state;
}

rn_video_frame_t* rn_get_video_frame(void) {
    return &video_frame;
}

rn_core_info_t* rn_get_core_info(void) {
    return &core_info;
}

const char* rn_get_last_error(void) {
    return last_error;
}
