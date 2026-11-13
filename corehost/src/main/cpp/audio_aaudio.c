/*
 * audio_aaudio.c — AAudio low-latency output with a lock-free SPSC ring buffer,
 * linear resampling and dynamic rate control (DRC).
 *
 * Owned by native-bridge-agent. See audio_aaudio.h for the threading contract.
 *
 * Producer (emulation thread): resamples core-rate stereo int16 to the device rate and
 * writes into the ring. Consumer (AAudio data callback): copies out of the ring and pads
 * with silence on underflow. The callback never blocks, allocates or logs.
 *
 * DRC: the emulation thread is paced by its own timer, so the core clock and the audio
 * clock drift apart. Once per video frame the ring fill (sampled right after the frame's
 * audio was written, i.e. at its peak) is smoothed with an EMA and compared with a target:
 *     err  = clamp((target - fill_ema) / target, -1, 1)
 *     step = (core_rate / device_rate) / (1 + RN_DRC_MAX_DEVIATION * err)
 * `step` is input frames consumed per output frame; a low fill makes more output (and
 * vice versa), bounded to +-0.5 %, which is inaudible as pitch.
 */
#include "audio_aaudio.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_LOG_TAG "RetroNest-Audio"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  AUDIO_LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  AUDIO_LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, AUDIO_LOG_TAG, __VA_ARGS__)

#define RN_CHANNELS            2
#define RN_DRC_MAX_DEVIATION   0.005   /* +-0.5 % */
#define RN_DRC_EMA_ALPHA       0.1     /* ~10 video frames time constant */
#define RN_DEFAULT_BURSTS      2       /* initial AAudio buffer = 2 x framesPerBurst */
#define RN_MAX_AUTO_BURSTS     8
#define RN_MAX_USER_BURSTS     16
#define RN_MIN_RING_FRAMES     2048
#define RN_STAGING_FRAMES      256
#define RN_STOP_TIMEOUT_NS     (200LL * 1000 * 1000)
#define RN_TUNE_INTERVAL       60      /* video frames between xrun checks */
#define RN_STATS_INTERVAL      300     /* video frames between underrun log lines */

typedef struct {
    /* ---- Shared with the data callback (set before the stream starts) ---- */
    int16_t*        ring;            /* ring_frames * RN_CHANNELS samples */
    size_t          ring_frames;     /* power of two */
    size_t          ring_mask;
    size_t          prime_frames;    /* consumer waits for this fill before playing */
    _Atomic size_t  write_pos;       /* monotonic frame counters; written by producer */
    _Atomic size_t  read_pos;        /*                          written by consumer  */
    atomic_bool     primed;          /* consumer-owned */
    atomic_uint     underruns;       /* consumer increments, producer reads for stats */
    atomic_bool     reopen_pending;  /* set by the error callback / timing changes */

    /* ---- Emulation thread only ---- */
    AAudioStream*   stream;
    int32_t         device_rate;
    int32_t         burst;
    int32_t         buffer_frames;
    int32_t         buffer_capacity;
    int32_t         last_xruns;
    int             user_bursts;     /* 0 = auto */
    bool            active;          /* a game wants audio (between open and close) */
    bool            paused;
    int             retry_countdown; /* video frames until the next reopen attempt */
    int             retry_seconds;   /* backoff: 1, 2, 4 ... 32 s between failed reopens */
    unsigned        frame_counter;
    unsigned        logged_underruns;

    double          core_rate;
    double          core_fps;
    double          step_base;       /* core_rate / device_rate */
    double          step;            /* current input frames per output frame */
    double          phase;           /* position in [0, 1) between prev and next input */
    int16_t         prev_l, prev_r;
    double          target_fill;
    double          fill_ema;

    int16_t         staging[RN_STAGING_FRAMES * RN_CHANNELS];
    size_t          staging_count;
} rn_audio_t;

static rn_audio_t g_audio;

/* ------------------------------------------------------------------ */
/* AAudio callbacks (real-time: no locks, no allocation, no logging)   */
/* ------------------------------------------------------------------ */

static aaudio_data_callback_result_t data_callback(AAudioStream* stream, void* user,
                                                   void* audio_data, int32_t num_frames) {
    (void)stream;
    rn_audio_t* a = (rn_audio_t*)user;
    int16_t* out = (int16_t*)audio_data;
    const size_t want = num_frames > 0 ? (size_t)num_frames : 0;

    const size_t r     = atomic_load_explicit(&a->read_pos, memory_order_relaxed);
    const size_t w     = atomic_load_explicit(&a->write_pos, memory_order_acquire);
    const size_t avail = w - r;

    if (!atomic_load_explicit(&a->primed, memory_order_relaxed)) {
        if (avail < a->prime_frames) {
            memset(out, 0, want * RN_CHANNELS * sizeof(int16_t));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        atomic_store_explicit(&a->primed, true, memory_order_relaxed);
    }

    const size_t take  = avail < want ? avail : want;
    const size_t idx   = r & a->ring_mask;
    const size_t first = take < a->ring_frames - idx ? take : a->ring_frames - idx;
    memcpy(out, a->ring + idx * RN_CHANNELS, first * RN_CHANNELS * sizeof(int16_t));
    memcpy(out + first * RN_CHANNELS, a->ring, (take - first) * RN_CHANNELS * sizeof(int16_t));
    /* Release: the producer may overwrite these slots only after our reads are done. */
    atomic_store_explicit(&a->read_pos, r + take, memory_order_release);

    if (take < want) {
        memset(out + take * RN_CHANNELS, 0, (want - take) * RN_CHANNELS * sizeof(int16_t));
        atomic_fetch_add_explicit(&a->underruns, 1u, memory_order_relaxed);
        atomic_store_explicit(&a->primed, false, memory_order_relaxed); /* re-buffer */
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* Runs on an AAudio-owned thread. Never touch the stream here: just flag a reopen. */
static void error_callback(AAudioStream* stream, void* user, aaudio_result_t error) {
    (void)stream;
    (void)error; /* DISCONNECTED (route change) or any fatal error: both need a new stream */
    rn_audio_t* a = (rn_audio_t*)user;
    atomic_store_explicit(&a->reopen_pending, true, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Stream management (emulation thread)                                */
/* ------------------------------------------------------------------ */

static double safe_fps(double fps) {
    return fps > 1.0 ? fps : 60.0;
}

static size_t next_pow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static const char* sharing_name(aaudio_sharing_mode_t m) {
    return m == AAUDIO_SHARING_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED";
}

static aaudio_result_t try_open(aaudio_sharing_mode_t sharing, int32_t rate, AAudioStream** out) {
    AAudioStreamBuilder* b = NULL;
    aaudio_result_t res = AAudio_createStreamBuilder(&b);
    if (res != AAUDIO_OK) return res;

    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, RN_CHANNELS);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, sharing);
    AAudioStreamBuilder_setDataCallback(b, data_callback, &g_audio);
    AAudioStreamBuilder_setErrorCallback(b, error_callback, &g_audio);

    res = AAudioStreamBuilder_openStream(b, out);
    AAudioStreamBuilder_delete(b);
    return res;
}

/* Stops and closes the stream; afterwards no callback can be running. Ring is kept. */
static void stream_teardown(void) {
    rn_audio_t* a = &g_audio;
    if (!a->stream) return;

    if (AAudioStream_requestStop(a->stream) == AAUDIO_OK) {
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        AAudioStream_waitForStateChange(a->stream, AAUDIO_STREAM_STATE_STOPPING, &next,
                                        RN_STOP_TIMEOUT_NS);
    }
    AAudioStream_close(a->stream);
    a->stream = NULL;
}

static int32_t effective_bursts(void) {
    return g_audio.user_bursts > 0 ? g_audio.user_bursts : RN_DEFAULT_BURSTS;
}

static void apply_buffer_size(int32_t bursts) {
    rn_audio_t* a = &g_audio;
    if (!a->stream || a->burst <= 0) return;
    int32_t frames = a->burst * bursts;
    if (a->buffer_capacity > 0 && frames > a->buffer_capacity) frames = a->buffer_capacity;
    const int32_t res = AAudioStream_setBufferSizeInFrames(a->stream, frames);
    a->buffer_frames = res > 0 ? res : AAudioStream_getBufferSizeInFrames(a->stream);
}

/* (Re)allocates the ring for the current device rate. Stream must be stopped/closed. */
static bool ring_setup(void) {
    rn_audio_t* a = &g_audio;
    const double vf    = (double)a->device_rate / safe_fps(a->core_fps); /* frames per video frame */
    const double burst = a->burst > 0 ? (double)a->burst : 256.0;

    a->target_fill  = 2.0 * vf + burst;
    a->prime_frames = (size_t)(0.75 * a->target_fill);

    size_t frames = next_pow2((size_t)(2.0 * a->target_fill) + 1);
    if (frames < RN_MIN_RING_FRAMES) frames = RN_MIN_RING_FRAMES;

    if (frames != a->ring_frames || !a->ring) {
        free(a->ring);
        a->ring = calloc(frames * RN_CHANNELS, sizeof(int16_t));
        if (!a->ring) {
            a->ring_frames = 0;
            a->ring_mask   = 0;
            ALOGE("Out of memory for audio ring (%zu frames)", frames);
            return false;
        }
        a->ring_frames = frames;
        a->ring_mask   = frames - 1;
    }

    atomic_store_explicit(&a->write_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&a->read_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&a->primed, false, memory_order_relaxed);
    atomic_store_explicit(&a->underruns, 0u, memory_order_relaxed);
    a->logged_underruns = 0;

    a->step_base     = a->core_rate / (double)a->device_rate;
    a->step          = a->step_base;
    a->phase         = 0.0;
    a->prev_l        = 0;
    a->prev_r        = 0;
    a->fill_ema      = a->target_fill;
    a->staging_count = 0;
    return true;
}

/* Opens a new stream (with fallbacks), sets up the ring and starts it unless paused. */
static bool stream_open_and_start(void) {
    rn_audio_t* a = &g_audio;
    stream_teardown();
    atomic_store_explicit(&a->reopen_pending, false, memory_order_relaxed);

    const int32_t requested = (int32_t)lround(a->core_rate);
    AAudioStream* s = NULL;
    aaudio_result_t res = try_open(AAUDIO_SHARING_MODE_EXCLUSIVE, requested, &s);
    if (res != AAUDIO_OK) {
        res = try_open(AAUDIO_SHARING_MODE_SHARED, requested, &s);
    }
    if (res != AAUDIO_OK) {
        /* Device refused the core rate: take its native rate and resample. */
        res = try_open(AAUDIO_SHARING_MODE_SHARED, AAUDIO_UNSPECIFIED, &s);
    }
    if (res != AAUDIO_OK || !s) {
        ALOGE("AAudio open failed: %s", AAudio_convertResultToText(res));
        return false;
    }

    if (AAudioStream_getFormat(s) != AAUDIO_FORMAT_PCM_I16 ||
        AAudioStream_getChannelCount(s) != RN_CHANNELS ||
        AAudioStream_getSampleRate(s) <= 0) {
        ALOGE("AAudio granted an unusable format (format %d, %d ch, %d Hz)",
              (int)AAudioStream_getFormat(s), (int)AAudioStream_getChannelCount(s),
              (int)AAudioStream_getSampleRate(s));
        AAudioStream_close(s);
        return false;
    }

    a->stream          = s;
    a->device_rate     = AAudioStream_getSampleRate(s);
    a->burst           = AAudioStream_getFramesPerBurst(s);
    a->buffer_capacity = AAudioStream_getBufferCapacityInFrames(s);
    a->last_xruns      = 0;
    apply_buffer_size(effective_bursts());

    if (!ring_setup()) {
        stream_teardown();
        return false;
    }

    ALOGI("AAudio stream open — requested %d Hz (core %.2f), actual %d Hz, framesPerBurst %d, "
          "buffer %d / capacity %d frames, sharing %s, perf mode %d, ring %zu frames, target fill %.0f",
          (int)requested, a->core_rate, (int)a->device_rate, (int)a->burst,
          (int)a->buffer_frames, (int)a->buffer_capacity,
          sharing_name(AAudioStream_getSharingMode(s)), (int)AAudioStream_getPerformanceMode(s),
          a->ring_frames, a->target_fill);

    if (!a->paused) {
        res = AAudioStream_requestStart(s);
        if (res != AAUDIO_OK) {
            ALOGE("AAudio start failed: %s", AAudio_convertResultToText(res));
            stream_teardown();
            return false;
        }
    }
    return true;
}

static void schedule_retry(void) {
    rn_audio_t* a = &g_audio;
    a->retry_seconds   = a->retry_seconds <= 0 ? 1 : (a->retry_seconds < 32 ? a->retry_seconds * 2 : 32);
    a->retry_countdown = (int)safe_fps(a->core_fps) * a->retry_seconds;
}

static void reopen_or_schedule(void) {
    rn_audio_t* a = &g_audio;
    ALOGI("Reopening audio stream");
    if (stream_open_and_start()) {
        a->retry_seconds = 0;
    } else {
        schedule_retry();
    }
}

/* ------------------------------------------------------------------ */
/* Producer                                                            */
/* ------------------------------------------------------------------ */

static void push_frames(const int16_t* in, size_t frames) {
    rn_audio_t* a = &g_audio;
    if (!a->stream || !a->ring || frames == 0) return;

    size_t w = atomic_load_explicit(&a->write_pos, memory_order_relaxed);
    /* Acquire: the consumer has finished reading every slot before read_pos. */
    const size_t r = atomic_load_explicit(&a->read_pos, memory_order_acquire);
    size_t space = a->ring_frames - (w - r);

    const double step  = a->step;
    double       phase = a->phase;
    int32_t      pl    = a->prev_l;
    int32_t      pr    = a->prev_r;
    int16_t*     ring  = a->ring;
    const size_t mask  = a->ring_mask;

    for (size_t i = 0; i < frames; i++) {
        const int32_t cl = in[i * 2];
        const int32_t cr = in[i * 2 + 1];
        while (phase < 1.0) {
            if (space > 0) { /* overflow: drop output, but keep the resampler phase */
                int16_t* dst = ring + (w & mask) * RN_CHANNELS;
                dst[0] = (int16_t)lrint(pl + (cl - pl) * phase);
                dst[1] = (int16_t)lrint(pr + (cr - pr) * phase);
                w++;
                space--;
            }
            phase += step;
        }
        phase -= 1.0;
        pl = cl;
        pr = cr;
    }

    a->phase  = phase;
    a->prev_l = (int16_t)pl;
    a->prev_r = (int16_t)pr;
    /* Release: publishes the samples written above before the new write_pos. */
    atomic_store_explicit(&a->write_pos, w, memory_order_release);
}

static void flush_staging(void) {
    rn_audio_t* a = &g_audio;
    if (a->staging_count) {
        push_frames(a->staging, a->staging_count);
        a->staging_count = 0;
    }
}

size_t rn_audio_write(const int16_t* data, size_t frames) {
    if (data && frames) {
        flush_staging(); /* keep sample order if a core mixes both callbacks */
        push_frames(data, frames);
    }
    return frames;
}

void rn_audio_write_sample(int16_t left, int16_t right) {
    rn_audio_t* a = &g_audio;
    a->staging[a->staging_count * 2]     = left;
    a->staging[a->staging_count * 2 + 1] = right;
    if (++a->staging_count == RN_STAGING_FRAMES) flush_staging();
}

/* ------------------------------------------------------------------ */
/* Per-frame maintenance                                               */
/* ------------------------------------------------------------------ */

static void update_drc(void) {
    rn_audio_t* a = &g_audio;
    const size_t w = atomic_load_explicit(&a->write_pos, memory_order_relaxed);
    const size_t r = atomic_load_explicit(&a->read_pos, memory_order_relaxed);
    const double fill = (double)(w - r);

    a->fill_ema += RN_DRC_EMA_ALPHA * (fill - a->fill_ema);
    double err = (a->target_fill - a->fill_ema) / a->target_fill;
    if (err > 1.0) err = 1.0;
    if (err < -1.0) err = -1.0;
    a->step = a->step_base / (1.0 + RN_DRC_MAX_DEVIATION * err);
}

static void auto_tune(void) {
    rn_audio_t* a = &g_audio;
    const int32_t xruns = AAudioStream_getXRunCount(a->stream);
    if (xruns <= a->last_xruns) return;
    a->last_xruns = xruns;

    if (a->user_bursts > 0 || a->burst <= 0) return; /* user override wins */
    const int32_t bursts = a->buffer_frames / a->burst;
    if (bursts >= RN_MAX_AUTO_BURSTS) return;
    if (a->buffer_capacity > 0 && a->buffer_frames + a->burst > a->buffer_capacity) return;

    apply_buffer_size(bursts + 1);
    ALOGI("Device xruns: %d — AAudio buffer raised to %d frames", (int)xruns, (int)a->buffer_frames);
}

void rn_audio_end_frame(void) {
    rn_audio_t* a = &g_audio;
    if (!a->active) return;

    if (atomic_exchange_explicit(&a->reopen_pending, false, memory_order_acquire)) {
        reopen_or_schedule();
        return;
    }
    if (!a->stream) {
        if (--a->retry_countdown <= 0) reopen_or_schedule();
        return;
    }

    flush_staging();
    update_drc();

    a->frame_counter++;
    if (a->frame_counter % RN_TUNE_INTERVAL == 0) auto_tune();
    if (a->frame_counter % RN_STATS_INTERVAL == 0) {
        const unsigned u = atomic_load_explicit(&a->underruns, memory_order_relaxed);
        if (u != a->logged_underruns) {
            ALOGW("Ring underruns: %u total (ratio adj %+.3f%%)", u,
                  (a->step_base / a->step - 1.0) * 100.0);
            a->logged_underruns = u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool rn_audio_open(double core_rate, double core_fps) {
    rn_audio_t* a = &g_audio;
    rn_audio_close();

    if (core_rate <= 0.0) {
        ALOGW("Core reports no audio sample rate; audio disabled");
        return false;
    }
    a->core_rate       = core_rate;
    a->core_fps        = core_fps;
    a->active          = true;
    a->paused          = false;
    a->retry_countdown = 0;
    a->retry_seconds   = 0;
    a->frame_counter   = 0;
    if (!stream_open_and_start()) {
        schedule_retry();
        return false;
    }
    return true;
}

void rn_audio_close(void) {
    rn_audio_t* a = &g_audio;
    stream_teardown();
    free(a->ring);
    const int user_bursts = a->user_bursts;
    /* Atomics are plain zero-initialisable objects; nothing is running at this point. */
    memset(a, 0, sizeof(*a));
    a->user_bursts = user_bursts; /* preference survives game changes */
}

void rn_audio_pause(void) {
    rn_audio_t* a = &g_audio;
    a->paused = true;
    if (a->stream) AAudioStream_requestPause(a->stream);
}

void rn_audio_resume(void) {
    rn_audio_t* a = &g_audio;
    if (!a->active) return;
    a->paused = false;

    if (atomic_exchange_explicit(&a->reopen_pending, false, memory_order_acquire) || !a->stream) {
        reopen_or_schedule();
        return;
    }
    const aaudio_result_t res = AAudioStream_requestStart(a->stream);
    if (res != AAUDIO_OK) {
        ALOGW("AAudio resume failed (%s); reopening", AAudio_convertResultToText(res));
        reopen_or_schedule();
    }
}

void rn_audio_set_timing(double core_rate, double core_fps) {
    rn_audio_t* a = &g_audio;
    if (core_rate == a->core_rate && core_fps == a->core_fps) return;
    a->core_rate = core_rate;
    a->core_fps  = core_fps;
    if (a->active && core_rate > 0.0) {
        /* Ring sizing and the requested rate depend on timing: rebuild on the next frame. */
        atomic_store_explicit(&a->reopen_pending, true, memory_order_release);
    }
}

void rn_audio_set_buffer_bursts(int bursts) {
    rn_audio_t* a = &g_audio;
    if (bursts < 0) bursts = 0;
    if (bursts > RN_MAX_USER_BURSTS) bursts = RN_MAX_USER_BURSTS;
    a->user_bursts = bursts;
    if (a->stream) apply_buffer_size(effective_bursts());
}
