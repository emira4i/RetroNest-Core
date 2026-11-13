package com.retronest.corehost.engine

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.Locale
import java.util.zip.CRC32

/** Crash-safe file write (host-side copy of the little the engine needs): old content or new, never torn. */
object AtomicFiles {
    private const val TEMP_SUFFIX = ".tmp"

    @Throws(IOException::class)
    fun write(target: File, bytes: ByteArray) {
        val dir = target.absoluteFile.parentFile
            ?: throw IOException("No parent directory for $target")
        if (!dir.isDirectory && !dir.mkdirs() && !dir.isDirectory) {
            throw IOException("Cannot create directory $dir")
        }
        val temp = File.createTempFile("${target.name}.", TEMP_SUFFIX, dir)
        try {
            FileOutputStream(temp).use { out ->
                out.write(bytes)
                out.flush()
                out.fd.sync()
            }
            try {
                Files.move(
                    temp.toPath(), target.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING,
                )
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } catch (e: IOException) {
            temp.delete()
            throw e
        }
    }
}

/**
 * Where the host keeps a game's battery SRAM: `<saveRoot>/<safe game id>.srm`. The app reads and
 * writes the same file (backup / import), so this MUST produce exactly what the app's
 * `SaveManager.sramFileFor` / `sanitizeGameId` produce; both sides have a test with the same
 * expected values.
 */
object SramFiles {
    const val SRAM_EXT = ".srm"
    private const val MAX_ID_LENGTH = 96
    private const val FALLBACK_ID = "game"
    private const val HASH_SUFFIX_LENGTH = 9 // "-" + 8 hex digits
    private val UNSAFE_CHARS = Regex("[^A-Za-z0-9._-]")

    fun sanitizeGameId(gameId: String): String {
        val cleaned = gameId
            .replace(UNSAFE_CHARS, "_")
            .trimStart('.')
        if (cleaned == gameId && cleaned.isNotEmpty() && cleaned.length <= MAX_ID_LENGTH) {
            return cleaned
        }
        val base = cleaned.take(MAX_ID_LENGTH - HASH_SUFFIX_LENGTH).ifEmpty { FALLBACK_ID }
        return "$base-${stableHash(gameId)}"
    }

    fun sramFileFor(saveRoot: File, gameId: String): File =
        File(saveRoot, sanitizeGameId(gameId) + SRAM_EXT)

    private fun stableHash(raw: String): String {
        val crc = CRC32()
        crc.update(raw.toByteArray(Charsets.UTF_8))
        return String.format(Locale.US, "%08x", crc.value)
    }
}
