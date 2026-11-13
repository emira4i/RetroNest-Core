package com.retronest.corehost.engine

import android.app.Service
import android.content.Intent
import android.graphics.Bitmap
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.Process
import android.os.RemoteException
import android.system.Os
import android.util.Log
import android.view.Surface
import com.retronest.corehost.CoreHostProtocol
import com.retronest.corehost.ICoreHost
import com.retronest.corehost.ICoreHostCallback
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The core host: runs ONE game session in the ":core" process (android:process in the manifest)
 * and exposes it through [ICoreHost]. Everything native (LibretroBridge, the cores, EGL, AAudio)
 * lives only in this process, so a crashing core takes down this process, never the app.
 *
 * It wraps the [LibretroEmulationEngine] unchanged and only translates the protocol:
 * Bundle → GameLaunchArgs, engine flows → [ICoreHostCallback], save states and thumbnails → files
 * at the paths the app passes (binder payloads are limited to ~1 MB). Core options
 * ([CoreOptionDefaults]) and PPSSPP's system files ([PpssppAssetInstaller]) are this module's own.
 *
 * One session per process: after [ICoreHost.shutdown] (or when the app unbinds / dies) the engine
 * is shut down and the process kills itself, so the next game starts with clean native globals.
 *
 * This module (:corehost, GPL-2.0-or-later) has no dependency on the app; it shares only the binder protocol (:corehost-api).
 */
open class CoreHostService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    /** User-visible error texts; resolved in the ":core" process against the per-app locale. */
    private val strings by lazy { AndroidStrings { HostLocale.wrap(applicationContext) } }
    private val lock = Any()
    private var engine: LibretroEmulationEngine? = null       // guarded by lock
    private var romFd: ParcelFileDescriptor? = null           // guarded by lock; open for the session
    private val finished = AtomicBoolean(false)
    private val shutdownDone = CountDownLatch(1)
    private val sessionUsed = AtomicBoolean(false)
    private val rumbleEnabled = AtomicBoolean(true)
    /** PPSSPP's fonts / compat lists ship as this module's assets and are copied when a PSP game starts. */
    private val ppssppAssets by lazy {
        PpssppAssetInstaller(
            assets = object : AssetSource {
                override fun list(path: String) = assets.list(path)?.toList().orEmpty()
                override fun open(path: String) = assets.open(path)
            },
            versionCode = {
                val info = packageManager.getPackageInfo(packageName, 0)
                if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else @Suppress("DEPRECATION") info.versionCode.toLong()
            },
        )
    }

    @Volatile private var callback: ICoreHostCallback? = null
    @Volatile private var surface: Surface? = null
    /** A fatal host-side error was reported; the engine's own (paused/running) states are no longer forwarded. */
    @Volatile private var fatal = false
    private var fallbackCheck: Job? = null                     // guarded by lock

    private val binder = object : ICoreHost.Stub() {
        override fun start(args: Bundle?, callback: ICoreHostCallback?) = startSession(args, callback)

        override fun setSurface(surface: Surface?) = attachSurface(surface)

        override fun setPaused(paused: Boolean) { engine()?.setPaused(paused) }
        override fun setButtons(mask: Int) { engine()?.setButtons(mask) }
        override fun setTurboButtons(mask: Int) { engine()?.setTurboButtons(mask) }
        override fun setAnalog(stick: Int, x: Int, y: Int) { engine()?.setAnalog(stick, x, y) }
        override fun setPointer(x: Float, y: Float, pressed: Boolean) { engine()?.setPointer(x, y, pressed) }

        override fun setShader(presetId: String?, vertex: String?, fragment: String?, linearFilter: Boolean) {
            if (presetId == null || vertex == null || fragment == null) return
            engine()?.setShader(ShaderSource(presetId, vertex, fragment, linearFilter))
        }

        override fun clearShader() { engine()?.setShader(null) }
        override fun setDisplayRotation(degrees: Int) { engine()?.setDisplayRotation(degrees) }
        override fun setTurboFrames(frames: Int) { engine()?.setTurboFrames(frames) }
        override fun setAudioBufferBursts(bursts: Int) { engine()?.setAudioBufferBursts(bursts) }
        override fun setPressWindowFrames(frames: Int) { engine()?.setPressWindowFrames(frames) }
        override fun setRumbleEnabled(enabled: Boolean) = rumbleEnabled.set(enabled)

        override fun reset() { engine()?.reset() }
        override fun setSpeed(multiplier: Int) { engine()?.setSpeed(multiplier) }
        override fun setRewindEnabled(enabled: Boolean) { engine()?.setRewindEnabled(enabled) }
        override fun setRewinding(active: Boolean) { engine()?.setRewinding(active) }
        override fun setCheats(codes: Array<out String>?) { engine()?.setCheats(codes?.toList().orEmpty()) }

        override fun getCapabilities(): Bundle {
            val eng = engine() ?: return HostCodec.encodeCapabilities(EngineCapabilities())
            val caps = runBlocking { withTimeoutOrNull(CAPABILITIES_WAIT_MS) { eng.capabilities() } }
            return HostCodec.encodeCapabilities(caps ?: EngineCapabilities())
        }

        override fun saveState(statePath: String?, thumbnailPath: String?): Boolean {
            return writeState(statePath ?: return false, thumbnailPath)
        }

        override fun loadState(statePath: String?): Boolean {
            val eng = engine() ?: return false
            val data = try {
                File(statePath ?: return false).readBytes()
            } catch (e: IOException) {
                Log.w(TAG, "Could not read state file", e)
                return false
            }
            return runBlocking { eng.loadState(data) }
        }

        override fun flushSram() {
            val eng = engine() ?: return
            runBlocking { eng.flushSram() }
        }

        override fun shutdown() = finishSession(killSoon = true)
    }

    override fun onBind(intent: Intent?): IBinder = binder

    /** The app let go (exit, or it died): never keep a core running without a client. */
    override fun onUnbind(intent: Intent?): Boolean {
        finishSession(killSoon = false)
        return false
    }

    override fun onDestroy() {
        finishSession(killSoon = false)
        scope.cancel()
        super.onDestroy()
        // Native globals (core, EGL, audio) must not leak into a later session in this process.
        if (sessionUsed.get()) Process.killProcess(Process.myPid())
    }

    // ------------------------------------------------------------------ Session

    private fun engine(): LibretroEmulationEngine? = synchronized(lock) { engine }

    private fun startSession(bundle: Bundle?, cb: ICoreHostCallback?) {
        synchronized(lock) {
            if (engine != null || finished.get() || sessionUsed.get()) {
                // A host that is shutting down (or already ran a game) never takes a second session: tell the app
                // instead of leaving it on "Loading". The next connection gets a fresh process.
                Log.w(TAG, "start() refused: one session per host process")
                runCatching {
                    val wire = HostCodec.encodeState(
                        EngineState.Error(
                            strings.get(R.string.gameplay_host_start_failed),
                            strings.get(R.string.gameplay_host_busy_detail),
                        ),
                    )
                    cb?.onState(wire.kind, wire.text1, wire.text2, wire.paused)
                }
                return
            }
            sessionUsed.set(true)
            callback = cb
            try {
                cb?.asBinder()?.linkToDeath({ finishSession(killSoon = true) }, 0)
            } catch (_: RemoteException) {
                finishSession(killSoon = true)
                return
            }
            var args = bundle?.let { b -> HostCodec.launchArgs { b.getString(it) } }
            if (bundle == null || args == null) {
                reportFatal(strings.get(R.string.gameplay_host_start_failed), strings.get(R.string.gameplay_host_missing_args))
                return
            }
            val fd = romFdOf(bundle)
            if (fd != null) {
                romFd = fd
                val linked = linkRomFd(fd, args.romPath)
                if (linked == null) {
                    reportFatal(strings.get(R.string.gameplay_host_rom_share_failed), strings.get(R.string.gameplay_host_rom_share_failed_detail))
                    return
                }
                args = args.copy(romPath = linked)
            }
            rumbleEnabled.set(bundle.getBoolean(CoreHostProtocol.ARG_RUMBLE, true))
            val eng = LibretroEmulationEngine(
                strings = strings,
                coreOptionsFor = { launch -> CoreOptionDefaults.forCore(launch.corePath) },
                rumble = GatedRumbleOutput(RumbleController(this), rumbleEnabled::get),
                prepareSystemFiles = { launch ->
                    if (launch.systemDir.isNotEmpty()) ppssppAssets.install(launch.corePath, File(launch.systemDir))
                },
            )
            engine = eng
            eng.setFrameSink(softwareFallbackSink)
            surface?.let(eng::setSurface)
            scope.launch {
                eng.state.collect { state -> if (!fatal) sendState(state) }
            }
            scope.launch {
                eng.achievedSpeed.collect { speed -> send { it.onSpeed(speed) } }
            }
            scope.launch {
                eng.gpuRendering.collect { active -> send { it.onGpuRendering(active) } }
            }
            scope.launch { flushSramPeriodically(eng) }
            eng.start(args)
        }
    }

    /**
     * Shuts the engine down (flushes SRAM, unloads, deinitializes). Idempotent. [killSoon]: the
     * app asked (or died): end the process shortly after replying, even if it never unbinds.
     */
    private fun finishSession(killSoon: Boolean) {
        val eng: LibretroEmulationEngine?
        val fd: ParcelFileDescriptor?
        var waitForOther = false
        synchronized(lock) {
            if (!finished.compareAndSet(false, true)) {
                eng = null
                fd = null
                waitForOther = true
            } else {
                eng = engine
                fd = romFd
                romFd = null
                fallbackCheck?.cancel()
            }
        }
        if (waitForOther) {
            // Another thread is already shutting down (e.g. client death + unbind): don't let the
            // caller (onDestroy kills the process) cut the SRAM flush short.
            shutdownDone.await(SHUTDOWN_WAIT_MS, TimeUnit.MILLISECONDS)
        } else {
            try {
                eng?.shutdown() // blocks until the emulation thread has flushed SRAM and deinitialized
                if (fd != null) runCatching { fd.close() } // the ROM fd must outlive the core
            } finally {
                shutdownDone.countDown()
            }
        }
        if (killSoon && sessionUsed.get()) {
            stopSelf()
            Handler(Looper.getMainLooper()).postDelayed({ Process.killProcess(Process.myPid()) }, KILL_DELAY_MS)
        }
    }

    private fun attachSurface(newSurface: Surface?) {
        val old = surface
        val eng = engine()
        // Engine first: it marks the attach pending before any software frame can reach the sink.
        eng?.setSurface(newSurface)
        surface = newSurface
        // Detach is synchronous in the engine, so native has let go of the old Surface by now; when it is replaced
        // by a new one the engine holds its own ANativeWindow reference, so this Java-side copy can go either way.
        if (old != null && old !== newSurface) runCatching { old.release() }
    }

    // ------------------------------------------------------------------ Save states (binder thread)

    private fun writeState(statePath: String, thumbnailPath: String?): Boolean {
        val eng = engine() ?: return false
        val snapshot = runBlocking { eng.saveState() } ?: return false
        val thumbnail = snapshot.thumbnail
        try {
            writeViaTemp(File(statePath)) { it.write(snapshot.data) }
            if (thumbnail != null && thumbnailPath != null) {
                try {
                    writeViaTemp(File(thumbnailPath)) { out ->
                        if (!thumbnail.compress(Bitmap.CompressFormat.PNG, 100, out)) throw IOException("PNG encode failed")
                    }
                } catch (e: IOException) {
                    Log.w(TAG, "Thumbnail not written", e) // the state itself is still fine
                }
            }
            return true
        } catch (e: IOException) {
            Log.e(TAG, "Could not write the save state", e)
            return false
        } finally {
            thumbnail?.recycle()
        }
    }

    /** tmp + rename, so the app never reads a half-written file. */
    private fun writeViaTemp(target: File, write: (FileOutputStream) -> Unit) {
        val temp = File(target.path + ".tmp")
        try {
            FileOutputStream(temp).use(write)
            if (!temp.renameTo(target)) throw IOException("rename to ${target.name} failed")
        } catch (e: IOException) {
            temp.delete()
            throw e
        }
    }

    // ------------------------------------------------------------------ Software-fallback guard

    /**
     * The engine falls back to FrameSink frames when the GPU path fails, but those frames cannot
     * reach the app's Surface from here. A frame arriving while a surface is attached therefore
     * means the GPU output failed: retry the attach once, then report an error instead of a black
     * screen. Called on the emulation thread.
     */
    private val softwareFallbackSink = FrameSink { _, _, _, _ ->
        val current = surface ?: return@FrameSink
        synchronized(lock) {
            if (fatal || finished.get() || fallbackCheck?.isActive == true) return@FrameSink
            val eng = engine ?: return@FrameSink
            fallbackCheck = scope.launch {
                Log.w(TAG, "Software frame while a surface is attached: retrying the GPU attach")
                eng.setSurface(current)
                delay(GPU_RETRY_WAIT_MS)
                if (!eng.gpuRendering.value && surface != null && !finished.get()) {
                    eng.setPaused(true)
                    reportFatal(strings.get(R.string.gameplay_host_video_failed), strings.get(R.string.gameplay_host_video_failed_detail))
                }
            }
        }
    }

    // ------------------------------------------------------------------ Periodic SRAM

    /** In-game saves reach disk every [SRAM_FLUSH_INTERVAL_MS] while running, so a crash loses little. */
    private suspend fun flushSramPeriodically(eng: LibretroEmulationEngine) {
        while (scope.isActive && !finished.get()) {
            delay(SRAM_FLUSH_INTERVAL_MS)
            val state = eng.state.value
            if (state is EngineState.Running && !state.paused) eng.flushSram() // writes only when changed
        }
    }

    // ------------------------------------------------------------------ Callback

    private fun reportFatal(title: String, detail: String) {
        Log.e(TAG, "$title — $detail")
        fatal = true
        sendState(EngineState.Error(title, detail))
    }

    private fun sendState(state: EngineState) {
        val wire = HostCodec.encodeState(state)
        send { it.onState(wire.kind, wire.text1, wire.text2, wire.paused) }
    }

    private inline fun send(block: (ICoreHostCallback) -> Unit) {
        val cb = callback ?: return
        try {
            block(cb)
        } catch (e: RemoteException) {
            Log.w(TAG, "App unreachable", e) // linkToDeath shuts the session down
        } catch (e: RuntimeException) {
            Log.w(TAG, "Callback failed", e)
        }
    }

    // ------------------------------------------------------------------ ROM fd

    @Suppress("DEPRECATION")
    private fun romFdOf(bundle: Bundle): ParcelFileDescriptor? = try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            bundle.getParcelable(CoreHostProtocol.ARG_ROM_FD, ParcelFileDescriptor::class.java)
        } else {
            bundle.getParcelable(CoreHostProtocol.ARG_ROM_FD)
        }
    } catch (e: RuntimeException) {
        Log.w(TAG, "Bad ROM fd", e)
        null
    }

    /** `<cacheDir>/corehost_rom/<same file name>` → `/proc/self/fd/<our fd>`; null on failure. */
    private fun linkRomFd(fd: ParcelFileDescriptor, appRomPath: String): String? = try {
        val dir = File(cacheDir, ROM_LINK_DIR)
        dir.listFiles()?.forEach { it.delete() }
        if (!dir.isDirectory && !dir.mkdirs()) throw IOException("Can't create ${dir.path}")
        val link = File(dir, File(appRomPath).name)
        Os.symlink("/proc/self/fd/${fd.fd}", link.path)
        link.path
    } catch (e: Exception) {
        Log.e(TAG, "Could not link the ROM fd", e)
        null
    }

    private companion object {
        const val TAG = "RetroNest-CoreHost"
        const val KILL_DELAY_MS = 300L
        const val CAPABILITIES_WAIT_MS = 30_000L
        const val SHUTDOWN_WAIT_MS = 10_000L
        const val GPU_RETRY_WAIT_MS = 1_000L
        const val SRAM_FLUSH_INTERVAL_MS = 30_000L
        const val ROM_LINK_DIR = "corehost_rom"
    }
}
