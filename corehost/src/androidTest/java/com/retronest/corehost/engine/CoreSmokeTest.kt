package com.retronest.corehost.engine

import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import com.retronest.bridge.LibretroBridge
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.Parameterized
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.zip.CRC32

/**
 * On-device smoke test for every bundled core: the real JNI bridge and the real core `.so`, driven the way
 * LibretroEmulationEngine drives them (init, the host's default core options, load, run frames, serialize),
 * minus the service, the surface and the UI. Content is the self-made programs from
 * tools/testroms/make_test_roms.py (androidTest assets) — no third-party ROM, no BIOS.
 *
 * Software-rendered cores need no Surface: the bridge copies every frame into its own buffer, which
 * [LibretroBridge.nativeGetFrameBuffer] reads back. (GLES cores — N64, PSP — would need EGL and are not here.)
 *
 * Per core: core loads, content loads, [FRAMES] frames run, a frame with sane dimensions was produced,
 * serialize size > 0, save state -> run more frames -> load state succeeds. Every message names the core.
 * One result line per core is logged under the tag [TAG] ("adb logcat -s CoreSmoke").
 *
 * Run: ./gradlew :corehost:connectedDebugAndroidTest (installs and later removes only the self-instrumenting
 * test package com.retronest.corehost.engine.test).
 */
@RunWith(Parameterized::class)
class CoreSmokeTest(private val case: Case) {

    data class Case(
        val core: String,         // core id, as in CORES_MANIFEST.json
        val system: String,
        val asset: String,        // testroms/<asset>.bin
        val extension: String,    // what the core expects as file extension
        /** The colour the test program paints the background (proves the program ran, not just the core). */
        val expect: Colour,
        /** Fraction of the frame height, from the top, that shows the painted screen (NDS: top screen only). */
        val region: Double = 1.0,
    ) {
        val label get() = "$core/$system"
        val library get() = "lib${core}_libretro_android.so"
        override fun toString() = label
    }

    @Test(timeout = 0)
    fun bootsRunsAndRoundTripsState() {
        val worker = Thread(null, ::runCase, "smoke-${case.core}", 64L * 1024 * 1024)
        var error: Throwable? = null
        worker.setUncaughtExceptionHandler { _, t -> error = t }
        worker.start()
        worker.join(TimeUnit.SECONDS.toMillis(CASE_TIMEOUT_S))
        if (worker.isAlive) {
            Log.e(TAG, "RESULT ${case.label} FAIL hung for more than ${CASE_TIMEOUT_S}s")
            fail("${case.label}: core did not finish within ${CASE_TIMEOUT_S}s (hung)")
        }
        error?.let {
            Log.e(TAG, "RESULT ${case.label} FAIL ${it.message}")
            throw it
        }
    }

    /** All bridge calls of one case run on this one thread (the bridge's emulation-thread contract). */
    private fun runCase() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val root = File(context.filesDir, "smoke").apply { deleteRecursively() }
        val systemDir = File(root, "system").apply { mkdirs() }
        val saveDir = File(root, "saves").apply { mkdirs() }
        val rom = File(root, "content.${case.extension}")
        instrumentation.context.assets.open("testroms/${case.asset}.bin").use { input ->
            rom.outputStream().use { input.copyTo(it) }
        }
        val corePath = File(context.applicationInfo.nativeLibraryDir, case.library)
        check(corePath.isFile) { "${case.label}: ${case.library} is not in the test APK's nativeLibraryDir" }

        val started = System.nanoTime()
        val b = LibretroBridge
        try {
            check(b.nativeInit(corePath.path, systemDir.path, saveDir.path)) {
                "${case.label}: core failed to load: ${b.nativeGetLastError()}"
            }
            b.nativeSetAudioMuted(true)
            val coreName = "${b.nativeGetCoreName()} ${b.nativeGetCoreVersion()}".trim()
            for ((key, value) in CoreOptionDefaults.forCore(corePath.path)) b.nativeSetCoreOption(key, value)

            check(b.nativeLoadGame(rom.path)) { "${case.label}: content rejected: ${b.nativeGetLastError()}" }
            check(!b.nativeUsesHwRender()) { "${case.label}: core asked for a GLES context; this test is software-only" }
            check(b.nativeGetState() == LibretroBridge.STATE_RUNNING) {
                "${case.label}: state ${b.nativeGetState()} after load, expected RUNNING"
            }

            var pixels = IntArray(0)
            var framesWithVideo = 0
            fun frame(): Boolean {
                b.nativeRunFrame()
                val w = b.nativeGetFrameWidth()
                val h = b.nativeGetFrameHeight()
                if (w <= 0 || h <= 0) return false
                if (pixels.size < w * h) pixels = IntArray(w * h)
                return b.nativeGetFrameBuffer(pixels).also { if (it) framesWithVideo++ }
            }
            repeat(FRAMES) { frame() }
            val width = b.nativeGetFrameWidth()
            val height = b.nativeGetFrameHeight()
            check(framesWithVideo > 0) { "${case.label}: no video frame in $FRAMES frames" }
            check(width in 64..2048 && height in 64..2048) { "${case.label}: odd frame size ${width}x$height" }
            val shot = pixels.copyOf(width * (height * case.region).toInt())
            val dominant = shot.asIterable().groupingBy { it and 0xFFFFFF }.eachCount().maxBy { it.value }
            val dominantShare = dominant.value.toDouble() / shot.size
            val centre = dominant.key
            check(dominantShare > 0.5 && case.expect.matches(centre)) {
                "${case.label}: expected a ${case.expect} background from the test program, got #%06X on %.0f%% of the screen"
                    .format(centre, dominantShare * 100)
            }

            val stateSize = b.nativeGetStateSize()
            check(stateSize > 0) { "${case.label}: serialize size is 0 (no save states)" }
            val state = checkNotNull(b.nativeSaveState()) { "${case.label}: save state failed: ${b.nativeGetLastError()}" }
            check(state.size == stateSize) { "${case.label}: state is ${state.size} bytes, serialize size $stateSize" }
            repeat(AFTER_SAVE_FRAMES) { frame() }
            val afterSave = crc(pixels, width * height)
            check(b.nativeLoadState(state)) { "${case.label}: load state failed: ${b.nativeGetLastError()}" }
            // Informational: is the restore exact (same bytes re-serialized, same picture after the same frames)?
            val reserialized = b.nativeSaveState()
            val sameBytes = reserialized != null && reserialized.contentEquals(state)
            repeat(AFTER_SAVE_FRAMES) { frame() }
            val afterLoad = crc(pixels, width * height)
            check(b.nativeGetState() == LibretroBridge.STATE_RUNNING) { "${case.label}: not running after load state" }

            val ms = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started)
            Log.i(
                TAG,
                "RESULT ${case.label} PASS core='$coreName' frames=${FRAMES + 2 * AFTER_SAVE_FRAMES} video=${width}x$height " +
                    "videoFrames=$framesWithVideo background=#%06X(%.0f%%) state=${stateSize}B ".format(centre, dominantShare * 100) +
                    "restoreSameBytes=$sameBytes restoreSamePicture=${afterSave == afterLoad} fps=%.2f time=${ms}ms"
                        .format(b.nativeGetFps()),
            )
        } finally {
            b.nativeDeinit() // unloads the game too; idempotent
            root.deleteRecursively()
        }
    }

    private fun crc(pixels: IntArray, count: Int): Long {
        val crc = CRC32()
        for (i in 0 until count) {
            val p = pixels[i]
            crc.update(p); crc.update(p ushr 8); crc.update(p ushr 16)
        }
        return crc.value
    }

    companion object {
        const val TAG = "CoreSmoke"
        const val FRAMES = 300
        const val AFTER_SAVE_FRAMES = 60
        const val CASE_TIMEOUT_S = 120L

        @JvmStatic
        @Parameterized.Parameters(name = "{0}")
        fun cases(): List<Case> = listOf(
            Case("fceumm", "NES", "nes", "nes", Colour.LIGHT_BLUE),
            Case("mgba", "GB", "gb", "gb", Colour.GREY),
            Case("mgba", "GBC", "gbc", "gbc", Colour.GREEN),
            Case("mgba", "GBA", "gba", "gba", Colour.BLUE),
            Case("bsnes", "SNES", "snes", "sfc", Colour.RED),
            Case("clownmdemu", "Genesis", "md", "md", Colour.GREEN),
            Case("gearsystem", "SMS", "sms", "sms", Colour.RED),
            Case("stella", "Atari2600", "a26", "a26", Colour.RED),
            Case("pcsx_rearmed", "PS1", "psx", "exe", Colour.RED),
            Case("melondsds", "NDS", "nds", "nds", Colour.RED, region = 0.5),
        )
    }

    /** Coarse colour classes (each core has its own palette, so exact RGB values differ). */
    enum class Colour(private val test: (r: Int, g: Int, b: Int) -> Boolean) {
        RED({ r, g, b -> r >= 0x80 && g < 0x70 && b < 0x70 }),
        GREEN({ r, g, b -> g >= 0x60 && r < 0x50 && b < 0x50 }),
        BLUE({ r, g, b -> b >= 0x80 && r < 0x70 && g < 0x70 }),
        LIGHT_BLUE({ r, g, b -> b >= 0xC0 && b > r + 0x40 && g > r }),
        /** A mid grey, i.e. not the post-boot white and not black. */
        GREY({ r, g, b -> maxOf(r, g, b) - minOf(r, g, b) < 0x20 && r in 0x40..0xE0 });

        fun matches(rgb: Int) = test((rgb shr 16) and 0xFF, (rgb shr 8) and 0xFF, rgb and 0xFF)
    }
}
