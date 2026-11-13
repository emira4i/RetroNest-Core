/*
 * Test-only Libretro core: paints a green screen for SHUTDOWN_AFTER_FRAMES frames, then asks the
 * frontend to close it (RETRO_ENVIRONMENT_SHUTDOWN), the way a real core does when a game ends or
 * cannot go on. Built by the :corehost buildShutdownTestCore task and packaged ONLY in the
 * androidTest APK (ShutdownRequestTest). It emulates nothing and needs no content of any value.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libretro.h"

#define WIDTH 160
#define HEIGHT 120
#define SHUTDOWN_AFTER_FRAMES 30

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static uint32_t frame_buffer[WIDTH * HEIGHT];
static unsigned frames;

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_init(void) { frames = 0; }
void retro_deinit(void) {}
void retro_reset(void) { frames = 0; }

void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "RetroNest shutdown test core";
    info->library_version = "1";
    info->valid_extensions = "bin";
    info->need_fullpath = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = WIDTH;
    info->geometry.base_height = HEIGHT;
    info->geometry.max_width = WIDTH;
    info->geometry.max_height = HEIGHT;
    info->geometry.aspect_ratio = (float)WIDTH / (float)HEIGHT;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
}

bool retro_load_game(const struct retro_game_info* game) {
    (void)game;
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    frames = 0;
    return environ_cb && environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num) {
    (void)type; (void)info; (void)num;
    return false;
}

void retro_unload_game(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_run(void) {
    static const int16_t silence[735 * 2];
    if (input_poll_cb) input_poll_cb();
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) frame_buffer[i] = 0x0000C000u;
    if (video_cb) video_cb(frame_buffer, WIDTH, HEIGHT, WIDTH * sizeof(uint32_t));
    if (audio_batch_cb) audio_batch_cb(silence, 735);
    if (++frames == SHUTDOWN_AFTER_FRAMES && environ_cb) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void* data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void* data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) { (void)index; (void)enabled; (void)code; }
void* retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
