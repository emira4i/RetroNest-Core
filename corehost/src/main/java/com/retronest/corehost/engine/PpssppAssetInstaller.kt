package com.retronest.corehost.engine

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile

/** Read access to the host's assets (AssetManager on the device, a map in tests). */
interface AssetSource {
    /** Child names of the asset folder [path]; empty for a file or a missing path. */
    fun list(path: String): List<String>

    fun open(path: String): InputStream
}

/**
 * PPSSPP looks for its fonts and compat lists in `<systemDir>/PPSSPP/` (flash0/font (.pgf files), compat.ini,
 * font_atlas.*...). The pack ships as core-host assets `ppsspp/PPSSPP/...` (fetched by scripts/fetch-cores.ps1,
 * never committed) and is copied there by the host before the PPSSPP core loads. A stamp file records which
 * build made the copy; it is redone when the stamp is missing / different or the sentinel font is gone.
 * Files are overwritten one by one and nothing else in the folder is deleted: PPSSPP keeps its own memory
 * stick data (saves, caches) beside the assets.
 *
 * Both host processes (":core" / ":core2") may run this, so a copy holds a file lock besides the monitor.
 */
class PpssppAssetInstaller(
    private val assets: AssetSource,
    /** The versionCode of the package that ships the assets: a new build recopies the pack. */
    private val versionCode: () -> Long,
) {

    /** Copies the pack into [systemDir] when [corePath] is the PPSSPP core; a no-op for every other core. */
    @Synchronized
    @Throws(IOException::class)
    fun install(corePath: String, systemDir: File) {
        if (File(corePath).name != CORE_LIB) return
        // No pack in this build (fetch-cores.ps1 not run): nothing to copy; PPSSPP then runs without its fonts.
        if (assets.list(ASSET_ROOT).isEmpty()) return
        val target = File(systemDir, TARGET_DIR)
        val wanted = stampFor(versionCode())
        if (isCurrent(target, wanted)) return
        if (!target.isDirectory && !target.mkdirs() && !target.isDirectory) throw IOException("Can't create ${target.absolutePath}")
        RandomAccessFile(File(target, LOCK_NAME), "rw").use { lockFile ->
            lockFile.channel.lock().use {
                if (isCurrent(target, wanted)) return // the other host process finished the copy meanwhile
                val stamp = File(target, STAMP_NAME)
                stamp.delete() // an interrupted copy must not look complete
                copyTree(ASSET_ROOT, target)
                stamp.writeText(wanted)
            }
        }
    }

    private fun isCurrent(target: File, wanted: String): Boolean {
        val stamp = File(target, STAMP_NAME)
        val current = if (stamp.isFile) stamp.readText().trim() else null
        return !needsInstall(current, wanted, File(target, SENTINEL).isFile)
    }

    private fun copyTree(assetPath: String, dir: File) {
        for (name in assets.list(assetPath)) {
            val child = "$assetPath/$name"
            if (assets.list(child).isNotEmpty()) {
                val sub = File(dir, name)
                if (!sub.isDirectory && !sub.mkdirs()) throw IOException("Can't create ${sub.absolutePath}")
                copyTree(child, sub)
            } else {
                val out = File(dir, name)
                val temp = File(dir, "$name.tmp")
                try {
                    assets.open(child).use { input -> temp.outputStream().use { input.copyTo(it) } }
                    if (out.exists() && !out.delete()) throw IOException("Can't replace ${out.absolutePath}")
                    if (!temp.renameTo(out)) throw IOException("Can't write ${out.absolutePath}")
                } finally {
                    temp.delete()
                }
            }
        }
    }

    companion object {
        /** File name of the PPSSPP libretro core. */
        const val CORE_LIB = "libppsspp_libretro_android.so"
        const val ASSET_ROOT = "ppsspp/PPSSPP"
        const val TARGET_DIR = "PPSSPP"
        const val STAMP_NAME = ".retronest_assets"
        private const val LOCK_NAME = ".retronest_assets.lock"

        /** A file that must exist after a complete copy. */
        const val SENTINEL = "flash0/font/ltn0.pgf"

        /** Bump when the shipped file set changes without a new versionCode (dev builds). */
        private const val FORMAT = 1

        fun stampFor(versionCode: Long) = "v$FORMAT|$versionCode"

        /** Pure decision: copy unless the stamp matches the wanted one and the sentinel file is still there. */
        fun needsInstall(currentStamp: String?, wantedStamp: String, sentinelExists: Boolean): Boolean =
            currentStamp != wantedStamp || !sentinelExists
    }
}
