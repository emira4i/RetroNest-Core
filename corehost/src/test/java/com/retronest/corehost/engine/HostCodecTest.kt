package com.retronest.corehost.engine

import com.retronest.corehost.CoreHostProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/** Host half of the wire format; the app's CoreHostCodecTest asserts the same literals. */
class HostCodecTest {

    private val entries = mapOf(
        "corePath" to "/lib/core.so",
        "romPath" to "/cache/rom.z64",
        "systemDir" to "/files/system",
        "saveDir" to "/files/saves",
        "gameId" to "rom.z64",
        "title" to "Rom",
        "cheatsSupported" to "true",
    )

    @Test
    fun launchArgsDecode() {
        val expected = GameLaunchArgs("/lib/core.so", "/cache/rom.z64", "/files/system", "/files/saves", "rom.z64", "Rom", cheatsSupported = true)
        assertEquals(expected, HostCodec.launchArgs(entries::get))
    }

    @Test
    fun missingRequiredEntryIsRejected() {
        for (key in listOf(
            CoreHostProtocol.ARG_CORE_PATH, CoreHostProtocol.ARG_ROM_PATH,
            CoreHostProtocol.ARG_SAVE_DIR, CoreHostProtocol.ARG_GAME_ID,
        )) {
            val partial = entries - key
            assertNull(key, HostCodec.launchArgs(partial::get))
        }
    }

    @Test
    fun optionalEntriesDefaultToEmpty() {
        val partial = entries - CoreHostProtocol.ARG_TITLE - CoreHostProtocol.ARG_SYSTEM_DIR - CoreHostProtocol.ARG_CHEATS_SUPPORTED
        val decoded = HostCodec.launchArgs(partial::get)!!
        assertEquals("", decoded.title)
        assertEquals("", decoded.systemDir)
        assertEquals(false, decoded.cheatsSupported)
    }

    @Test
    fun statesEncodeToWireValues() {
        assertEquals(HostCodec.WireState(CoreHostProtocol.STATE_IDLE, null, null, false), HostCodec.encodeState(EngineState.Idle))
        assertEquals(HostCodec.WireState(CoreHostProtocol.STATE_LOADING, "Loading ROM…", null, false), HostCodec.encodeState(EngineState.Loading("Loading ROM…")))
        assertEquals(HostCodec.WireState(CoreHostProtocol.STATE_RUNNING, "Snes9x", null, true), HostCodec.encodeState(EngineState.Running("Snes9x", paused = true)))
        assertEquals(HostCodec.WireState(CoreHostProtocol.STATE_ERROR, "Failed", "bad", false), HostCodec.encodeState(EngineState.Error("Failed", "bad", restartable = true)))
        assertEquals(HostCodec.WireState(CoreHostProtocol.STATE_STOPPED, null, null, false), HostCodec.encodeState(EngineState.Stopped))
    }

    /** Same expected values as the app's SaveManagerTest: both sides must agree on the .srm path. */
    @Test
    fun sramPathMatchesTheAppsSaveManager() {
        assertEquals("Mario-1_v2.nes", SramFiles.sanitizeGameId("Mario-1_v2.nes"))
        assertEquals("a_b_c-" + crc("a/b\\c"), SramFiles.sanitizeGameId("a/b\\c"))
        assertEquals("game-" + crc("..."), SramFiles.sanitizeGameId("..."))
        assertEquals(File("/saves", "Mario-1_v2.nes.srm"), SramFiles.sramFileFor(File("/saves"), "Mario-1_v2.nes"))
    }

    private fun crc(raw: String): String {
        val c = java.util.zip.CRC32()
        c.update(raw.toByteArray(Charsets.UTF_8))
        return String.format(java.util.Locale.US, "%08x", c.value)
    }
}
