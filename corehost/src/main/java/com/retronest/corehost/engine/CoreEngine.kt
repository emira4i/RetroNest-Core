package com.retronest.corehost.engine

import android.graphics.Bitmap
import kotlinx.coroutines.flow.StateFlow

/*
 * Host-side copy of the engine types (the app has its own, com.retronest.domain.emulation); the modules
 * share no code, only the binder protocol in :corehost-api. Keep both in sync by hand.
 *
 * CoreEngine owns the emulation thread and is the ONLY caller of LibretroBridge.
 * It outlives the Surface: the UI attaches/detaches a FrameSink as the SurfaceView is
 * created/destroyed, so backgrounding the app (or, in Phase 4, rotating) never reloads the core.
 */

/** What to run. [gameId] is a stable per-ROM key used for save files (Phase 2: ROM file name). */
data class GameLaunchArgs(
    val corePath: String,
    val romPath: String,
    val systemDir: String,
    val saveDir: String,
    val gameId: String,
    val title: String,
    /** Phase 4.5 WS4: the core implements retro_cheat_set (from the app's core catalog; the host can't know). */
    val cheatsSupported: Boolean = false,
)

sealed interface EngineState {
    data object Idle : EngineState
    data class Loading(val message: String) : EngineState
    data class Running(val coreName: String, val paused: Boolean) : EngineState
    /** [restartable]: the engine can start the same game again ([CoreEngine.restart]). */
    data class Error(val title: String, val detail: String, val restartable: Boolean = false) : EngineState
    data object Stopped : EngineState
}

/**
 * Receives finished frames. Called on the EMULATION thread; implementations draw synchronously
 * and must not keep a reference to [pixels] after returning (the buffer is reused).
 * [pixels] is ARGB_8888, row-major, stride = [width]. [aspect] is the display aspect ratio.
 */
fun interface FrameSink {
    fun onFrame(pixels: IntArray, width: Int, height: Int, aspect: Float)
}

/** A serialized state plus a thumbnail of the frame shown when it was taken. */
class SaveStateSnapshot(val data: ByteArray, val thumbnail: Bitmap?)

/** Phase 4.5 WS4: per-game gameplay features. [maxSpeed] 1 = no fast forward. */
data class EngineCapabilities(
    val reset: Boolean = false,
    val maxSpeed: Int = 1,
    val rewind: Boolean = false,
    val cheats: Boolean = false,
)

interface CoreEngine {
    val state: StateFlow<EngineState>

    /** Starts the emulation thread: init core, load ROM, restore SRAM, then run at core FPS. */
    fun start(args: GameLaunchArgs)

    /** Attach (non-null) or detach (null) the frame consumer. Safe from any thread. */
    fun setFrameSink(sink: FrameSink?)

    /** Pause/resume stepping + audio. Safe from any thread (applied on the emulation thread). */
    fun setPaused(paused: Boolean)

    /** Player 1 RetroPadButtons mask. Safe from any thread, applied immediately. */
    fun setButtons(mask: Int)

    /**
     * Phase 4B: buttons that auto-repeat while held (turbo modules). The engine presses them for
     * 2 frames and releases them for 2 frames, OR-ed with [setButtons]. Safe from any thread.
     */
    fun setTurboButtons(mask: Int) {}

    /** Phase 4B: Player 1 analog stick ([stick] 0 = left, 1 = right), x/y in -32768..32767. */
    fun setAnalog(stick: Int, x: Int, y: Int) {}

    // ---------------------------------------------------------------- Phase 4C (locked)
    /**
     * GPU output: attach the SurfaceView's Surface (null on surfaceDestroyed; must not return
     * while the old surface is still being drawn, like setFrameSink). When GPU init fails the
     * engine keeps delivering frames to the FrameSink (software fallback).
     */
    fun setSurface(surface: android.view.Surface?) {}

    /** Shader preset to use (applied on the emulation thread; null = built-in passthrough). */
    fun setShader(source: ShaderSource?) {}

    /** Display rotation, degrees clockwise (0/90/180/270), applied by the GPU path. */
    fun setDisplayRotation(degrees: Int) {}

    /** Settings completion: turbo cycle length in frames (held N, released N). Default 3. */
    fun setTurboFrames(frames: Int) {}

    /** Settings completion: AAudio buffer size in bursts (0 = automatic). Applied on the emulation thread. */
    fun setAudioBufferBursts(bursts: Int) {}

    /** T2a: libretro pointer (touch screen), [x]/[y] normalized 0..1 over the frame. Safe from any thread. */
    fun setPointer(x: Float, y: Float, pressed: Boolean) {}

    /** Simultaneous-press window in frames (0..2), see AppSettings.pressWindowFrames. Safe from any thread. */
    fun setPressWindowFrames(frames: Int) {}

    /**
     * Phase 4.5: Settings → Rumble. Engines that own their rumble gate locally (in-process) may
     * ignore this; the core-host engine forwards it to the host process. Safe from any thread.
     */
    fun setRumbleEnabled(enabled: Boolean) {}

    /**
     * Phase 4.5: after the engine stopped unexpectedly (core-host process died), start the same
     * game again. False when this engine cannot restart (then the UI only offers Exit).
     */
    fun restart(): Boolean = false

    // ---------------------------------------------------------------- Phase 4.5 WS4 (locked)
    /** retro_reset between frames. */
    fun reset() {}

    /** Fast-forward multiplier 1 / 2 / 4. */
    fun setSpeed(multiplier: Int) {}

    /** Start / stop recording the rewind buffer. */
    fun setRewindEnabled(enabled: Boolean) {}

    /** While true, step backwards instead of running. */
    fun setRewinding(active: Boolean) {}

    /** Replaces all cheats (empty = none). */
    fun setCheats(codes: List<String>) {}

    /** What this game supports; available after the game loaded (all-off before / when unsupported). */
    suspend fun capabilities(): EngineCapabilities = EngineCapabilities()

    /** The fast-forward multiplier actually achieved (1 when not fast-forwarding). */
    val achievedSpeed: StateFlow<Int>
        get() = kotlinx.coroutines.flow.MutableStateFlow(1)

    /** True once frames are drawn by the GPU path (UI may then skip software drawing). */
    val gpuRendering: kotlinx.coroutines.flow.StateFlow<Boolean>
        get() = kotlinx.coroutines.flow.MutableStateFlow(false)

    /** Runs on the emulation thread between frames. Null if the core cannot serialize. */
    suspend fun saveState(): SaveStateSnapshot?

    /** Runs on the emulation thread between frames. */
    suspend fun loadState(data: ByteArray): Boolean

    /** Writes battery SRAM to disk if the game has any. Runs on the emulation thread. */
    suspend fun flushSram()

    /** Flushes SRAM, unloads the game, deinitializes the core and joins the thread. Idempotent. */
    fun shutdown()
}

/** Shader preset sources as sent over the binder (ICoreHost.setShader). */
class ShaderSource(val presetId: String, val vertex: String, val fragment: String, val linearFilter: Boolean)
