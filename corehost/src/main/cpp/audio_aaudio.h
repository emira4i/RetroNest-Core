/*
 * audio_aaudio.h — AAudio output pipeline (internal to the native bridge).
 *
 * Owned by native-bridge-agent. Used only by retronest_bridge.c; not part of the JNI contract.
 *
 * Data path:
 *   core (emulation thread) -> linear resampler with dynamic rate control
 *   -> lock-free SPSC ring buffer (device-rate stereo int16)
 *   -> AAudio data callback (real-time thread, memcpy + silence padding only).
 *
 * Threading: every rn_audio_* function must be called from the emulation thread
 * (the only producer and the only thread that opens/starts/pauses/closes the stream).
 * The AAudio data callback is the only consumer; the error callback only sets a flag
 * that rn_audio_end_frame() / rn_audio_resume() act on (streams are never reopened
 * from inside an AAudio callback).
 */
#ifndef RETRONEST_AUDIO_AAUDIO_H
#define RETRONEST_AUDIO_AAUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opens and starts the stream for a core producing `core_rate` Hz at `core_fps` frames/s.
 * Returns false when no stream could be opened (the game should keep running silently).
 * Calling it while a stream is open closes the old one first.
 */
bool rn_audio_open(double core_rate, double core_fps);

/* Stops and closes the stream and frees every audio allocation. Idempotent. */
void rn_audio_close(void);

/* Pause / resume the stream (resume also performs a pending device-change reopen). */
void rn_audio_pause(void);
void rn_audio_resume(void);

/* Core timing changed (RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO). Safe without an open stream. */
void rn_audio_set_timing(double core_rate, double core_fps);

/*
 * Producer side. Interleaved stereo frames at the core rate. Frames that do not fit
 * into the ring are dropped. Returns `frames` (libretro: frames consumed).
 */
size_t rn_audio_write(const int16_t* data, size_t frames);
void   rn_audio_write_sample(int16_t left, int16_t right);

/*
 * Call once after every retro_run(): flushes single-sample staging, updates dynamic
 * rate control, auto-tunes the AAudio buffer size on xruns and reopens the stream after
 * a disconnect (headphones unplugged, Bluetooth route change).
 */
void rn_audio_end_frame(void);

/*
 * User buffer-size override in bursts (framesPerBurst units, 1..16).
 * 0 = automatic (start at 2 bursts, grow by one burst on device xruns).
 * Applied immediately when a stream is open, otherwise on the next open.
 */
void rn_audio_set_buffer_bursts(int bursts);

#ifdef __cplusplus
}
#endif

#endif /* RETRONEST_AUDIO_AAUDIO_H */
