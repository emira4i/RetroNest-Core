package com.retronest.bridge

/**
 * Kotlin side of the native engine contract (mirrors app/src/main/cpp/retronest_bridge.h).
 *
 * The native bridge holds a single global core instance, so this is a singleton.
 * Threading: call every function from the same thread (the emulation thread), except that
 * [nativeDeinit] may be called from another thread once the emulation thread has been joined.
 */
object LibretroBridge {

    init {
        System.loadLibrary("retronest_bridge")
    }

    /** Engine states — must match rn_state_t. */
    const val STATE_IDLE = 0
    const val STATE_LOADED = 1
    const val STATE_RUNNING = 2
    const val STATE_PAUSED = 3

    /** Loads a core .so. [systemDir] / [saveDir] are handed to the core (BIOS and SRAM folders). */
    external fun nativeInit(corePath: String, systemDir: String?, saveDir: String?): Boolean

    /** Unloads any game, deinitializes the core and frees all native memory. Idempotent. */
    external fun nativeDeinit()

    external fun nativeLoadGame(romPath: String): Boolean
    external fun nativeUnloadGame()

    /**
     * T2b (locked): true after a successful [nativeLoadGame] when the core accepted a GLES hardware
     * render context (SET_HW_RENDER: N64 / PSP). Such a core draws into a native FBO on the
     * emulation thread; frames are only visible through the GPU path ([nativeSetSurface] +
     * [nativePresent]) — there is no software FrameSink fallback, and [nativeGetFrameBuffer] reads the
     * FBO back (thumbnails / screenshots).
     */
    external fun nativeUsesHwRender(): Boolean

    /** The core asked the frontend to close it (RETRO_ENVIRONMENT_SHUTDOWN); cleared by [nativeDeinit]. */
    external fun nativeShutdownRequested(): Boolean

    /** Runs exactly one emulated frame. */
    external fun nativeRunFrame()

    external fun nativeGetState(): Int

    /**
     * Writes the newest frame into [dst] as ARGB_8888 (stride = [nativeGetFrameWidth]).
     * Returns false if there is no new frame, or if [dst] is smaller than width × height.
     */
    external fun nativeGetFrameBuffer(dst: IntArray): Boolean
    external fun nativeGetFrameWidth(): Int
    external fun nativeGetFrameHeight(): Int

    external fun nativeGetCoreName(): String
    external fun nativeGetCoreVersion(): String

    /** Core frame rate (e.g. 60.10 for NTSC NES); 0 until a game is loaded. */
    external fun nativeGetFps(): Double

    /** Display aspect ratio reported by the core; 0 until a game is loaded. */
    external fun nativeGetAspectRatio(): Float

    /** Human-readable reason for the last failed call, or an empty string. */
    external fun nativeGetLastError(): String

    // ------------------------------------------------------------ Phase 2 (locked contract)

    /** Pauses/resumes stepping and the AAudio stream. Emulation thread. */
    external fun nativeSetPaused(paused: Boolean)

    /**
     * THREAD-SAFE: may be called from any thread (e.g. UI thread).
     * [mask] is a RetroPad bitmask (com.retronest.corehost.RetroPadButtons) for controller [port] (0..3).
     */
    external fun nativeSetInputMask(port: Int, mask: Int)

    /** THREAD-SAFE. [stick] 0 = left, 1 = right; [x]/[y] in -32768..32767. */
    external fun nativeSetAnalog(port: Int, stick: Int, x: Int, y: Int)

    /** Serializes the running game; null if the core cannot serialize. Emulation thread. */
    external fun nativeSaveState(): ByteArray?

    /** Restores a state produced by [nativeSaveState] for the same game/core. Emulation thread. */
    external fun nativeLoadState(data: ByteArray): Boolean

    /** Copy of battery SRAM, or null when the game has none. Emulation thread. */
    external fun nativeGetSram(): ByteArray?

    /**
     * Restores battery SRAM; call right after [nativeLoadGame], before the first frame.
     * Returns false if the game has no SRAM or the size differs. Emulation thread.
     */
    external fun nativeSetSram(data: ByteArray): Boolean

    // ------------------------------------------------------------ Phase 4A (locked contract)

    /**
     * Sets a core option (e.g. "mgba_gb_model" = "Autodetect"). May be called before
     * [nativeLoadGame]; the value is applied when the core declares the option. Emulation thread.
     */
    external fun nativeSetCoreOption(key: String, value: String): Boolean

    /**
     * Declared core options, one string per option: "key	value	description	v1|v2|v3".
     * Empty array when the core declares none. Emulation thread.
     */
    external fun nativeGetCoreOptions(): Array<String>

    /** THREAD-SAFE. Current rumble strength 0..65535 requested by the core for [port]. */
    external fun nativeGetRumble(port: Int): Int

    // ------------------------------------------------------------ Phase 4C (locked contract)

    /**
     * Attach (non-null) / detach (null) the output Surface for GPU rendering. False when EGL /
     * GLES init fails → keep using the software FrameSink path. Emulation thread.
     */
    external fun nativeSetSurface(surface: android.view.Surface?): Boolean

    /** Compile a shader preset (GLSL ES 1.00, see retronest_bridge.h). False keeps the previous one. */
    external fun nativeSetShader(vertexSource: String, fragmentSource: String, linearFilter: Boolean): Boolean

    /** Drops the current shader preset and returns to the built-in Sharp (passthrough) output. Emulation thread. */
    external fun nativeClearShader()

    /** Display rotation in degrees clockwise (0/90/180/270). Emulation thread. */
    external fun nativeSetRotation(degrees: Int)

    /** Draw + swap the newest frame. False when no surface is attached. Emulation thread. */
    external fun nativePresent(): Boolean

    // ------------------------------------------------------------ Settings completion (locked)

    /** AAudio buffer size in bursts, 1..16; 0 = automatic (adaptive). Emulation thread. */
    external fun nativeSetAudioBufferBursts(bursts: Int)

    // ------------------------------------------------------------ T2a (locked contract)

    /**
     * THREAD-SAFE. libretro pointer state for RETRO_DEVICE_POINTER: [x]/[y] in -32767..32767 over the
     * core's frame (-32767 = left/top edge), [pressed] = RETRO_DEVICE_ID_POINTER_PRESSED.
     */
    external fun nativeSetPointer(x: Int, y: Int, pressed: Boolean)

    // ------------------------------------------------------------ Phase 4.5 WS4 (locked)

    /** retro_reset between frames. False when the core has none or no game runs. Emulation thread. */
    external fun nativeReset(): Boolean

    /** retro_cheat_reset. False when the core exports no cheat API. Emulation thread. */
    external fun nativeCheatReset(): Boolean

    /** retro_cheat_set([index], [enabled], [code]). False when unsupported. Emulation thread. */
    external fun nativeCheatSet(index: Int, enabled: Boolean, code: String): Boolean

    /** THREAD-SAFE. While muted the native side drops core audio instead of queueing it. */
    external fun nativeSetAudioMuted(muted: Boolean)

    /** retro_serialize into [dst] (a reusable buffer of at least the serialize size); false on failure. Emulation thread. */
    external fun nativeSerializeInto(dst: ByteArray): Boolean

    /** retro_unserialize from [src]. Emulation thread. */
    external fun nativeUnserializeFrom(src: ByteArray, size: Int): Boolean

    /** retro_serialize_size, 0 when unsupported. Emulation thread. */
    external fun nativeGetStateSize(): Int
}
