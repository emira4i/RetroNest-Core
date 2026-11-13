package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.File

class PpssppAssetInstallerTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private val ppsspp = "/data/app/lib/arm64/libppsspp_libretro_android.so"

    private val files = mapOf(
        "ppsspp/PPSSPP/compat.ini" to "compat",
        "ppsspp/PPSSPP/flash0/font/ltn0.pgf" to "pgf0",
        "ppsspp/PPSSPP/flash0/font/jpn0.pgf" to "jpn",
    )
    private var opens = 0

    private val source = object : AssetSource {
        override fun list(path: String): List<String> {
            val prefix = "$path/"
            return files.keys.filter { it.startsWith(prefix) }.map { it.removePrefix(prefix).substringBefore('/') }.distinct()
        }

        override fun open(path: String) = ByteArrayInputStream(files.getValue(path).toByteArray()).also { opens++ }
    }

    @Test
    fun decision_logic() {
        val wanted = PpssppAssetInstaller.stampFor(7)
        assertTrue(PpssppAssetInstaller.needsInstall(null, wanted, sentinelExists = false))
        assertTrue(PpssppAssetInstaller.needsInstall(PpssppAssetInstaller.stampFor(6), wanted, sentinelExists = true))
        assertTrue(PpssppAssetInstaller.needsInstall(wanted, wanted, sentinelExists = false))
        assertFalse(PpssppAssetInstaller.needsInstall(wanted, wanted, sentinelExists = true))
    }

    @Test
    fun stampFormatIsUnchanged_soCopiesMadeByEarlierBuildsStayValid() {
        assertEquals("v1|42", PpssppAssetInstaller.stampFor(42))
        assertEquals(".retronest_assets", PpssppAssetInstaller.STAMP_NAME)
    }

    @Test
    fun copiesOnce_thenRecopiesOnNewVersion_keepingOtherFiles() {
        var version = 1L
        val installer = PpssppAssetInstaller(source) { version }
        val systemDir = tmp.newFolder("system")

        installer.install(ppsspp, systemDir)
        val target = File(systemDir, "PPSSPP")
        assertEquals("pgf0", File(target, "flash0/font/ltn0.pgf").readText())
        assertEquals("compat", File(target, "compat.ini").readText())
        assertEquals(PpssppAssetInstaller.stampFor(1), File(target, PpssppAssetInstaller.STAMP_NAME).readText())
        assertEquals(3, opens)
        assertFalse(File(target, "compat.ini.tmp").exists())

        // The emulator's own data beside the assets survives; same version: nothing is copied again.
        val save = File(target, "PSP/SAVEDATA/x.bin").apply { parentFile!!.mkdirs(); writeText("save") }
        installer.install(ppsspp, systemDir)
        assertEquals(3, opens)

        version = 2
        File(target, "compat.ini").writeText("stale")
        installer.install(ppsspp, systemDir)
        assertEquals(6, opens)
        assertEquals("compat", File(target, "compat.ini").readText())
        assertEquals("save", save.readText())
        assertEquals(PpssppAssetInstaller.stampFor(2), File(target, PpssppAssetInstaller.STAMP_NAME).readText())
    }

    @Test
    fun recopiesWhenSentinelFontIsMissing() {
        val installer = PpssppAssetInstaller(source) { 1 }
        val systemDir = tmp.newFolder("system")
        installer.install(ppsspp, systemDir)
        File(systemDir, "PPSSPP/flash0/font/ltn0.pgf").delete()
        installer.install(ppsspp, systemDir)
        assertTrue(File(systemDir, "PPSSPP/flash0/font/ltn0.pgf").isFile)
    }

    @Test
    fun otherCores_andMissingPack_doNothing() {
        val systemDir = tmp.newFolder("system")
        PpssppAssetInstaller(source) { 1 }.install("/lib/libpcsx_rearmed_libretro_android.so", systemDir)
        assertFalse(File(systemDir, "PPSSPP").exists())
        assertEquals(0, opens)
        val empty = object : AssetSource {
            override fun list(path: String) = emptyList<String>()
            override fun open(path: String) = throw java.io.FileNotFoundException(path)
        }
        PpssppAssetInstaller(empty) { 1 }.install(ppsspp, systemDir)
        assertFalse(File(systemDir, "PPSSPP").exists())
    }
}
