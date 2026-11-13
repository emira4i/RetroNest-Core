cmake_minimum_required(VERSION 3.22.1)
project(retronest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libretro.h")
    message(FATAL_ERROR
        "libretro.h is missing. Run scripts/setup-phase1.ps1 to fetch the official header.")
endif()

add_library(retronest_bridge SHARED
    retronest_bridge.c
    jni_interface.c
    audio_aaudio.c
    video_gles.c
)

target_include_directories(retronest_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(retronest_bridge
    android   # ANativeWindow_fromSurface / ANativeWindow_release
    log
    dl        # dlopen / dlsym for Libretro cores
    aaudio    # Low-latency audio output (API 26+)
    m         # lround / lrint in the resampler
    EGL       # Phase 4C GPU presentation (video_gles.c)
    GLESv3    # ES 3.0 lib also exports the ES 2.0 entry points used by the ES 2 fallback
)

# 16 KB page-size compatibility (Android 15+ devices).
target_link_options(retronest_bridge PRIVATE "-Wl,-z,max-page-size=16384")
