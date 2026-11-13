package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class CoreOptionDefaultsTest {

    private fun forLib(lib: String) = CoreOptionDefaults.forCore("/data/app/lib/arm64/$lib")

    @Test
    fun n64_keysCheckedAgainstMupen64plusCoreOptions() {
        // Keys / values checked against mupen64plus-libretro-nx libretro/libretro_core_options.h.
        val n64 = forLib("libmupen64plus_next_gles3_libretro_android.so")
        assertEquals("gliden64", n64["mupen64plus-rdp-plugin"])
        assertEquals("hle", n64["mupen64plus-rsp-plugin"])
        assertEquals("dynamic_recompiler", n64["mupen64plus-cpucore"])
        assertEquals("640x480", n64["mupen64plus-43screensize"])
        assertEquals("4:3", n64["mupen64plus-aspect"])
        assertEquals("0", n64["mupen64plus-EnableNativeResFactor"])
    }

    @Test
    fun psp_keysCheckedAgainstPpssppCoreOptions() {
        // Keys / values checked against ppsspp libretro/libretro_core_options.h.
        val psp = forLib("libppsspp_libretro_android.so")
        assertEquals("JIT", psp["ppsspp_cpu_core"])
        assertEquals("enabled", psp["ppsspp_fast_memory"])
        assertEquals("960x544", psp["ppsspp_internal_resolution"])
        assertEquals("enabled", psp["ppsspp_analog_is_circular"])
    }

    @Test
    fun theSameDefaultsTheAppCatalogUsedToCarry() {
        assertEquals(mapOf("mgba_skip_bios" to "ON", "mgba_gb_model" to "Autodetect"), forLib("libmgba_libretro_android.so"))
        assertEquals(
            mapOf("melonds_render_mode" to "software", "melonds_jit_enable" to "disabled"),
            forLib("libmelondsds_libretro_android.so"),
        )
        assertEquals(mapOf("mame_rotation_mode" to "internal"), forLib("libmame_libretro_android.so"))
        assertEquals(
            mapOf("mame2003-plus_skip_disclaimer" to "enabled", "mame2003-plus_skip_warnings" to "enabled"),
            forLib("libmame2003_plus_libretro_android.so"),
        )
        assertEquals(
            mapOf(
                "mame_current_skip_nagscreen" to "enabled",
                "mame_current_skip_gameinfo" to "enabled",
                "mame_current_skip_warnings" to "enabled",
            ),
            forLib("libmame2010_libretro_android.so"),
        )
        assertEquals(6, forLib("libmupen64plus_next_gles3_libretro_android.so").size)
        assertEquals(4, forLib("libppsspp_libretro_android.so").size)
    }

    @Test
    fun coresWithoutDefaults_andUnknownCores_getNothing() {
        listOf("fceumm", "bsnes", "snes9x", "clownmdemu", "gearsystem", "genesis_plus_gx", "stella", "pcsx_rearmed", "unknown")
            .forEach { assertTrue(it, forLib("lib${it}_libretro_android.so").isEmpty()) }
        assertTrue(CoreOptionDefaults.forCore("").isEmpty())
    }
}
