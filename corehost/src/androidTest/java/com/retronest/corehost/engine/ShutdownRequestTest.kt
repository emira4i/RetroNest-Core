package com.retronest.corehost.engine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.retronest.bridge.LibretroBridge
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * A core that asks to be closed (RETRO_ENVIRONMENT_SHUTDOWN) ends the session instead of being run on.
 *
 * Uses the test-only core from src/androidTest/cpp/shutdown_test_core.c, which sends the request on its
 * 30th frame. Two levels: the native bridge records the request and clears it on deinit; the real
 * [LibretroEmulationEngine] stops its loop, reports "game closed" and releases the core.
 */
@RunWith(AndroidJUnit4::class)
class ShutdownRequestTest {

    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context = instrumentation.targetContext
    private val root = File(context.filesDir, "shutdown-test")

    private fun prepare(): Pair<File, File> {
        root.deleteRecursively()
        File(root, "system").mkdirs()
        File(root, "saves").mkdirs()
        val rom = File(root, "content.bin").apply { writeBytes(ByteArray(16)) }
        val core = File(context.applicationInfo.nativeLibraryDir, CORE)
        check(core.isFile) { "$CORE is not in the test APK's nativeLibraryDir" }
        return core to rom
    }

    @Test(timeout = 60_000)
    fun bridge_recordsTheRequest_andClearsItOnDeinit() {
        val (core, rom) = prepare()
        var error: Throwable? = null
        // All bridge calls on one thread (the bridge's emulation-thread contract).
        val worker = Thread {
            val b = LibretroBridge
            try {
                check(b.nativeInit(core.path, File(root, "system").path, File(root, "saves").path)) { b.nativeGetLastError() }
                b.nativeSetAudioMuted(true)
                check(b.nativeLoadGame(rom.path)) { b.nativeGetLastError() }
                repeat(FRAMES_BEFORE_REQUEST - 1) { b.nativeRunFrame() }
                assertFalse("requested too early", b.nativeShutdownRequested())
                b.nativeRunFrame()
                assertTrue("request not recorded on frame $FRAMES_BEFORE_REQUEST", b.nativeShutdownRequested())
                b.nativeUnloadGame()
                b.nativeDeinit()
                assertFalse("request survived deinit", b.nativeShutdownRequested())
            } catch (t: Throwable) {
                error = t
                runCatching { b.nativeDeinit() }
            }
        }
        worker.start()
        worker.join(TimeUnit.SECONDS.toMillis(30))
        assertFalse("bridge test hung", worker.isAlive)
        error?.let { throw it }
        Log.i(TAG, "RESULT shutdown/bridge PASS")
    }

    @Test(timeout = 60_000)
    fun engine_endsTheSession_whenTheCoreAsksToBeClosed(): Unit = runBlocking<Unit> {
        val (core, rom) = prepare()
        val engine = LibretroEmulationEngine(AndroidStrings { context })
        val expectedTitle = context.getString(R.string.gameplay_engine_game_closed)
        try {
            engine.start(
                GameLaunchArgs(
                    corePath = core.path, romPath = rom.path,
                    systemDir = File(root, "system").path, saveDir = File(root, "saves").path,
                    gameId = "shutdown-test", title = "Shutdown test",
                ),
            )
            val running = withTimeout(20_000) { engine.state.first { it is EngineState.Running || it is EngineState.Error } }
            assertTrue("engine never ran the test core: $running", running is EngineState.Running)
            val end = withTimeout(20_000) { engine.state.first { it is EngineState.Error } } as EngineState.Error
            assertEquals(expectedTitle, end.title)
        } finally {
            engine.shutdown() // joins the emulation thread: the core is unloaded when this returns
        }
        assertEquals(EngineState.Stopped, engine.state.value)
        assertFalse("request still set after the engine released the core", LibretroBridge.nativeShutdownRequested())
        Log.i(TAG, "RESULT shutdown/engine PASS")
    }

    private companion object {
        const val TAG = "CoreSmoke"
        const val CORE = "libshutdowntest_libretro_android.so"
        const val FRAMES_BEFORE_REQUEST = 30
    }
}
