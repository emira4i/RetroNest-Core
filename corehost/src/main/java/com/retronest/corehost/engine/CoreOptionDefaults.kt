package com.retronest.corehost.engine

import java.io.File

/**
 * Libretro core options the host sets before a game loads. Only the host knows libretro option
 * keys: the app only names the core to run ([GameLaunchArgs.corePath]). A future user-facing
 * setting that changes a core option must cross the boundary as a core-agnostic key (see
 * CoreHostProtocol) and be translated into option keys here.
 *
 * Keyed by the core's .so file name (the last segment of the core path the app sends).
 */
object CoreOptionDefaults {

    private val DEFAULTS: Map<String, Map<String, String>> = mapOf(
        "libmgba_libretro_android.so" to linkedMapOf(
            "mgba_skip_bios" to "ON",
            "mgba_gb_model" to "Autodetect",
        ),
        "libmelondsds_libretro_android.so" to linkedMapOf(
            "melonds_render_mode" to "software",
            "melonds_jit_enable" to "disabled",
        ),
        "libmame_libretro_android.so" to linkedMapOf(
            "mame_rotation_mode" to "internal",
        ),
        "libmame2003_plus_libretro_android.so" to linkedMapOf(
            "mame2003-plus_skip_disclaimer" to "enabled",
            "mame2003-plus_skip_warnings" to "enabled",
        ),
        "libmame2010_libretro_android.so" to linkedMapOf(
            "mame_current_skip_nagscreen" to "enabled",
            "mame_current_skip_gameinfo" to "enabled",
            "mame_current_skip_warnings" to "enabled",
        ),
        "libmupen64plus_next_gles3_libretro_android.so" to linkedMapOf(
            "mupen64plus-rdp-plugin" to "gliden64",
            "mupen64plus-rsp-plugin" to "hle",
            "mupen64plus-cpucore" to "dynamic_recompiler",
            "mupen64plus-aspect" to "4:3",
            "mupen64plus-43screensize" to "640x480",
            "mupen64plus-EnableNativeResFactor" to "0",
        ),
        PpssppAssetInstaller.CORE_LIB to linkedMapOf(
            // Keys / values checked against ppsspp libretro/libretro_core_options.h.
            "ppsspp_cpu_core" to "JIT",
            "ppsspp_fast_memory" to "enabled",
            "ppsspp_internal_resolution" to "960x544",
            "ppsspp_analog_is_circular" to "enabled",
        ),
    )

    /** Options for the core at [corePath] (empty for a core that needs none). */
    fun forCore(corePath: String): Map<String, String> = DEFAULTS[File(corePath).name].orEmpty()
}
