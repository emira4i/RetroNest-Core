package com.retronest.corehost.engine

import android.graphics.Bitmap
import android.util.Log
import android.view.Surface
import com.retronest.bridge.LibretroBridge
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.File
import java.io.IOException
import java.util.Locale
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.Semaphore
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.LockSupport

/**
 * Turbo timing: pressed for N frames, released for N frames, where N is the turbo rate
 * (2 fast / 3 normal / 5 slow). A cycle always starts in the pressed phase (frame 0), and
 * every turbo bit shares the same phase, so a combo like B+C presses both together.
 */
object TurboPattern {
    const val DEFAULT_FRAMES = 3
    val VALID_FRAMES = setOf(2, 3, 5)

    fun normalize(frames: Int): Int = if (frames in VALID_FRAMES) frames else DEFAULT_FRAMES

    fun isOn(frame: Long, frames: Int = DEFAULT_FRAMES): Boolean {
        val n = normalize(frames)
        return Math.floorMod(frame, (2 * n).toLong()) < n
    }

    fun effectiveMask(buttons: Int, turbo: Int, frame: Long, frames: Int = DEFAULT_FRAMES): Int =
        if (turbo != 0 && isOn(frame, frames)) buttons or turbo else buttons
}

/**
 * The only class that talks to [LibretroBridge].
 *
 * Threading:
 * - One dedicated thread ("RetroNest-Emulation", MAX priority) makes every native call except
 *   [LibretroBridge.nativeSetInputMask] (thread-safe, called directly by [setButtons]).
 * - Other threads talk to it through volatile flags ([setPaused], shutdown) and a command queue
 *   ([saveState], [loadState], [flushSram]) that is drained between frames and while paused.
 * - Frames go to the current [FrameSink] on the emulation thread, with [sinkLock] held around
 *   `onFrame`, so `setFrameSink(null)` returns only once no call into the old sink is in flight.
 * - The native bridge is a process-wide singleton, so emulation threads of different engine
 *   instances are serialized by [SESSION]: a new session waits until the previous one is deinitialized.
 */
class LibretroEmulationEngine(
    private val strings: Strings,
    /** Core option defaults to apply after init and before the ROM loads (called on the emulation thread). */
    private val coreOptionsFor: (GameLaunchArgs) -> Map<String, String> = { emptyMap() },
    private val rumble: RumbleOutput? = null,
    /** Puts the files the core needs into the system dir before it loads (PPSSPP fonts); runs on the emulation thread, may throw. */
    private val prepareSystemFiles: (GameLaunchArgs) -> Unit = {},
) : CoreEngine {

    private val bridge = LibretroBridge

    private val _state = MutableStateFlow<EngineState>(EngineState.Idle)
    override val state: StateFlow<EngineState> = _state.asStateFlow()

    // ------------------------------------------------------------------ Cross-thread state

    private val lifecycleLock = Any()
    private var started = false                       // guarded by lifecycleLock
    private val isShutDown = AtomicBoolean(false)
    @Volatile private var running = false
    @Volatile private var thread: Thread? = null
    @Volatile private var desiredPaused = false
    @Volatile private var buttonsMask = 0         // what the core is shown (held, after grouping)
    @Volatile private var requestedMask = 0       // what the UI asked for
    @Volatile private var pressWindowFrames = 0
    private val grouper = PressGrouper()          // emulation thread only
    private var lastWindow = 0                    // emulation thread only
    private var lastSentMask = -1                 // emulation thread only
    @Volatile private var turboMask = 0
    @Volatile private var frameCounter = 0L
    @Volatile private var turboFrames = TurboPattern.DEFAULT_FRAMES
    @Volatile private var desiredAudioBursts = 0
    private var turboWasActive = false // emulation thread only

    private val sinkLock = Any()
    private var sink: FrameSink? = null               // guarded by sinkLock
    @Volatile private var sinkAttached = false        // re-deliver the last frame while paused

    // Phase 4C GPU output. Written by any thread, consumed on the emulation thread.
    @Volatile private var currentSurface: Surface? = null
    @Volatile private var desiredShader: ShaderSource? = null
    @Volatile private var desiredRotation = 0
    @Volatile private var loopActive = false          // emulation thread is in runLoop and drains commands
    /** A surface is current but its GPU attach has not been tried yet: the sink must not touch the canvas. */
    @Volatile private var gpuAttachPending = false
    private val _gpuRendering = MutableStateFlow(false)
    override val gpuRendering: StateFlow<Boolean> = _gpuRendering.asStateFlow()

    // Phase 4.5 WS4 gameplay features. Requests are written by any thread, consumed on the emulation thread.
    @Volatile private var desiredSpeed = 1
    @Volatile private var desiredRewindEnabled = false
    @Volatile private var desiredRewinding = false
    @Volatile private var desiredCheats: List<String> = emptyList()
    private val _achievedSpeed = MutableStateFlow(1)
    override val achievedSpeed: StateFlow<Int> = _achievedSpeed.asStateFlow()
    private val capabilitiesResult = CompletableDeferred<EngineCapabilities>()
    private val governor = SpeedGovernor()          // emulation thread only
    private var rewindBuffer: RewindBuffer? = null  // emulation thread only
    private var rewindSupported = false
    private var rewindStateSize = 0
    private var rewindFrames = 0L
    private var rewindFailures = 0                  // consecutive failed captures; rewinding stops after a few
    private var rewindBroken = false                // gave up (state too big / core can't serialize): don't retry
    private var cheatsSupported = false
    private var audioMuted = false

    private val commandLock = Any()
    private var acceptingCommands = false             // guarded by commandLock
    private val commands = ConcurrentLinkedQueue<Command<*>>()

    // ------------------------------------------------------------------ Emulation-thread-only state

    private var pixels = IntArray(0)
    private var frameWidth = 0
    private var frameHeight = 0
    private var frameAspect = 0f
    private var hasFrame = false
    private var coreName = ""
    private var appliedPaused = false
    private var sramFile: File? = null
    private var lastSram: ByteArray? = null
    private var loggedGpuFailure = false
    private var presentOnce = false                   // repaint once while paused (surface attach / shader change)
    private var ranFrame = false
    private var output = OutputPolicy(hwRender = false) // set after nativeLoadGame
    private var gpuFatal = false                      // HW core lost its only output: stop the loop
    private val gpu = GpuOutput(object : GpuOps<Surface> {
        override fun setSurface(surface: Surface?) = bridge.nativeSetSurface(surface)
        override fun setShader(source: ShaderSource) =
            bridge.nativeSetShader(source.vertex, source.fragment, source.linearFilter)
        override fun clearShader() = bridge.nativeClearShader()
        override fun setRotation(degrees: Int) = bridge.nativeSetRotation(degrees)
        override fun present() = bridge.nativePresent()
    })

    // ------------------------------------------------------------------ CoreEngine

    override fun start(args: GameLaunchArgs) {
        synchronized(lifecycleLock) {
            if (isShutDown.get()) {
                Log.w(TAG, "start() after shutdown() ignored")
                return
            }
            if (started) {
                Log.w(TAG, "start() called twice; ignored")
                return
            }
            started = true
            synchronized(commandLock) { acceptingCommands = true }
            running = true
            _state.value = EngineState.Loading(strings.get(R.string.gameplay_engine_loading_core))
            thread = Thread({ threadMain(args) }, THREAD_NAME).apply {
                priority = Thread.MAX_PRIORITY
                start()
            }
        }
    }

    override fun setFrameSink(sink: FrameSink?) {
        synchronized(sinkLock) { this.sink = sink }
        if (sink != null) {
            sinkAttached = true
            wakeEmulationThread()
        }
    }

    override fun setSurface(surface: Surface?) {
        currentSurface = surface
        if (!loopActive) return // runLoop attaches currentSurface when it starts
        if (surface != null) {
            gpuAttachPending = true
            if (enqueue { attachSurface(surface) } == null) gpuAttachPending = false
            return
        }
        // Detach: the Surface is destroyed as soon as we return, so wait (bounded) until native let go of it.
        val command = enqueue { detachSurface() } ?: return
        val latch = CountDownLatch(1)
        command.result.invokeOnCompletion { latch.countDown() }
        if (!latch.await(DETACH_TIMEOUT_MS, TimeUnit.MILLISECONDS)) Log.w(TAG, "GPU surface detach timed out")
    }

    override fun setShader(source: ShaderSource?) {
        desiredShader = source
        if (loopActive) enqueue { if (gpu.active) { applyShader(); presentOnce = true } }
    }

    override fun setDisplayRotation(degrees: Int) {
        desiredRotation = Math.floorMod(degrees, 360)
        if (loopActive) enqueue { if (gpu.active) { gpu.applyRotation(desiredRotation); presentOnce = true } }
    }

    override fun reset() {
        enqueue {
            if (bridge.nativeReset()) {
                rewindBuffer?.clear()
                applyCheats()
                presentOnce = true
            }
        }
    }

    override fun setSpeed(multiplier: Int) {
        desiredSpeed = multiplier
        wakeEmulationThread()
    }

    override fun setRewindEnabled(enabled: Boolean) {
        desiredRewindEnabled = enabled
        wakeEmulationThread()
    }

    override fun setRewinding(active: Boolean) {
        desiredRewinding = active
        wakeEmulationThread()
    }

    override fun setCheats(codes: List<String>) {
        desiredCheats = codes.toList()
        if (loopActive) enqueue { applyCheats() }
    }

    override suspend fun capabilities(): EngineCapabilities = capabilitiesResult.await()

    override fun setPaused(paused: Boolean) {
        desiredPaused = paused
        wakeEmulationThread()
    }

    override fun setButtons(mask: Int) {
        requestedMask = mask
        if (pressWindowFrames > 0) return // the emulation loop groups presses once per frame
        buttonsMask = mask
        applyInputMask()
    }

    override fun setPressWindowFrames(frames: Int) {
        pressWindowFrames = frames.coerceIn(0, MAX_PRESS_WINDOW)
        wakeEmulationThread()
    }

    /** Once per frame before the core runs (emulation thread): groups presses when a window is set. */
    private fun updateGroupedInput() {
        val window = pressWindowFrames
        if (window != lastWindow) {
            if (window > 0) {
                grouper.reset(buttonsMask)
            } else {
                buttonsMask = requestedMask
                applyInputMask()
            }
            lastWindow = window
            lastSentMask = -1
        }
        if (window == 0) return
        grouper.windowFrames = window
        buttonsMask = grouper.next(requestedMask)
        val effective = synchronized(this) {
            TurboPattern.effectiveMask(buttonsMask, turboMask, frameCounter, turboFrames)
        }
        if (effective != lastSentMask) {
            lastSentMask = effective
            bridge.nativeSetInputMask(0, effective)
        }
    }

    @Synchronized
    override fun setTurboButtons(mask: Int) {
        // Newly pressed turbo bits restart the cycle in the pressed phase, so a tap presses every bit
        // of a combo together for a full on-phase (instead of landing mid-cycle and firing once).
        if (mask and turboMask.inv() != 0) frameCounter = 0
        turboMask = mask
        applyInputMask()
    }

    override fun setTurboFrames(frames: Int) {
        turboFrames = TurboPattern.normalize(frames) // picked up by the next frame's mask
    }

    override fun setAudioBufferBursts(bursts: Int) {
        desiredAudioBursts = bursts.coerceIn(0, MAX_AUDIO_BURSTS)
        if (loopActive) enqueue { bridge.nativeSetAudioBufferBursts(desiredAudioBursts) }
    }

    /** Advances the turbo cycle by one emulated frame (atomic with mask application). */
    @Synchronized
    private fun advanceTurbo() {
        frameCounter++
    }

    override fun setAnalog(stick: Int, x: Int, y: Int) {
        bridge.nativeSetAnalog(0, stick, x, y)
    }

    override fun setPointer(x: Float, y: Float, pressed: Boolean) {
        bridge.nativeSetPointer(pointerToLibretro(x), pointerToLibretro(y), pressed)
    }

    /** Sends held buttons OR-ed with the turbo buttons while the turbo phase is "on". */
    @Synchronized
    private fun applyInputMask() {
        bridge.nativeSetInputMask(0, TurboPattern.effectiveMask(buttonsMask, turboMask, frameCounter, turboFrames))
    }

    override suspend fun saveState(): SaveStateSnapshot? = submit(fallback = null) {
        val data = bridge.nativeSaveState()
        if (data == null) {
            Log.w(TAG, "nativeSaveState failed: ${bridge.nativeGetLastError()}")
            null
        } else {
            SaveStateSnapshot(data, captureThumbnail())
        }
    }

    override suspend fun loadState(data: ByteArray): Boolean = submit(fallback = false) {
        bridge.nativeLoadState(data).also { ok ->
            if (!ok) {
                Log.w(TAG, "nativeLoadState failed: ${bridge.nativeGetLastError()}")
            } else {
                rewindBuffer?.clear() // the recorded timeline no longer leads to this point
                applyCheats()
            }
        }
    }

    override suspend fun flushSram() = submit(fallback = Unit) { writeSramIfChanged() }

    override fun shutdown() {
        synchronized(lifecycleLock) {
            if (!isShutDown.compareAndSet(false, true)) return
            running = false
            val t = thread
            if (t == null) {
                synchronized(commandLock) { acceptingCommands = false }
                _state.value = EngineState.Stopped
                return
            }
            if (t === Thread.currentThread()) return // the loop exits and cleans up on its own
            LockSupport.unpark(t)
            joinUninterruptibly(t)
            _state.value = EngineState.Stopped
        }
    }

    // ------------------------------------------------------------------ Emulation thread

    private fun threadMain(args: GameLaunchArgs) {
        if (!SESSION.tryAcquire()) {
            _state.value = EngineState.Loading(strings.get(R.string.gameplay_engine_waiting))
            SESSION.acquireUninterruptibly()
        }
        var initialized = false
        var loaded = false
        try {
            if (!running) return
            _state.value = EngineState.Loading(strings.get(R.string.gameplay_engine_loading_core))
            try {
                prepareSystemFiles(args)
            } catch (e: Exception) {
                Log.e(TAG, "Preparing system files failed", e)
                _state.value = EngineState.Error(
                    strings.get(R.string.gameplay_engine_files_failed),
                    strings.get(R.string.gameplay_engine_files_failed_detail),
                )
                return
            }
            initialized = bridge.nativeInit(args.corePath, args.systemDir, args.saveDir)
            if (!initialized) {
                fail(strings.get(R.string.gameplay_engine_core_failed))
                return
            }
            coreName = "${bridge.nativeGetCoreName()} ${bridge.nativeGetCoreVersion()}".trim()
            Log.i(TAG, "Core loaded: $coreName")
            applyCoreOptions(args)

            _state.value = EngineState.Loading(strings.get(R.string.gameplay_engine_loading_rom))
            loaded = bridge.nativeLoadGame(args.romPath)
            if (!loaded) {
                fail(strings.get(R.string.gameplay_engine_rom_failed))
                return
            }
            output = OutputPolicy(hwRender = bridge.nativeUsesHwRender())
            if (output.hwRender) Log.i(TAG, "Core renders with OpenGL ES (hardware context): GPU output only")
            sramFile = SramFiles.sramFileFor(File(args.saveDir), args.gameId)
            restoreSram()
            cheatsSupported = args.cheatsSupported
            capabilitiesResult.complete(measureCapabilities(args))
            try {
                runLoop()
            } finally {
                loopActive = false
                gpuAttachPending = false
                detachSurface()
            }
        } catch (t: Throwable) {
            Log.e(TAG, "Emulation thread crashed", t)
            _state.value = EngineState.Error(strings.get(R.string.gameplay_engine_crashed), t.message ?: t.javaClass.simpleName)
        } finally {
            capabilitiesResult.complete(EngineCapabilities()) // never loaded: nothing is offered
            rewindBuffer?.clear()
            rewindBuffer = null
            closeCommandQueue(runPending = loaded)
            try {
                if (loaded) {
                    writeSramIfChanged()
                    bridge.nativeUnloadGame()
                }
                bridge.nativeDeinit() // idempotent; also cleans up after a failed init
            } catch (t: Throwable) {
                Log.e(TAG, "Native teardown failed", t)
            } finally {
                hasFrame = false
                stopRumble()
                SESSION.release()
                if (isShutDown.get()) _state.value = EngineState.Stopped
                Log.i(TAG, "Emulation thread finished")
            }
        }
    }

    private fun runLoop() {
        var rawFps = bridge.nativeGetFps()
        var fps = FramePacing.sanitize(rawFps)
        var frameNs = FramePacing.frameNs(rawFps)
        Log.i(TAG, String.format(Locale.US, "Emulation started: %s @ %.2f fps", coreName, fps))

        appliedPaused = false
        ranFrame = false
        gpuFatal = false
        if (desiredPaused) applyPause(true)
        publishRunning()
        loopActive = true
        applyCheats() // a setCheats that arrived just before loopActive was only stored
        currentSurface?.let { attachSurface(it) }
        bridge.nativeSetAudioBufferBursts(desiredAudioBursts)

        var nextFrameAt = System.nanoTime()
        var statsStart = nextFrameAt
        var statsFrames = 0
        var emulatedFrames = 0L

        while (running) {
            drainCommands()
            if (gpuFatal) return // state already says why (HW core without a working GPU path)

            val wantPaused = desiredPaused
            if (wantPaused != appliedPaused) {
                applyPause(wantPaused)
                publishRunning()
                if (!wantPaused) {
                    nextFrameAt = System.nanoTime()
                    statsStart = nextFrameAt
                    statsFrames = 0
                }
            }

            val nativeState = bridge.nativeGetState()
            if (nativeState != LibretroBridge.STATE_RUNNING && nativeState != LibretroBridge.STATE_PAUSED) {
                val detail = bridge.nativeGetLastError()
                    .ifEmpty { strings.get(R.string.gameplay_engine_stopped_unexpectedly, nativeState) }
                _state.value = EngineState.Error(strings.get(R.string.gameplay_engine_stopped), detail)
                return
            }

            if (appliedPaused) {
                grouper.reset(0)
                val repaint = sinkAttached || presentOnce
                sinkAttached = false
                presentOnce = false
                if (repaint) {
                    if (gpu.active) {
                        if (ranFrame) presentGpu()
                    } else if (hasFrame && output.usesFrameSink(gpuActive = false)) {
                        deliverFrame() // repaint a freshly created surface while paused
                    }
                }
                LockSupport.parkNanos(PAUSED_PARK_NS)
                continue
            }
            sinkAttached = false

            syncGameplayModes()
            val rewinding = desiredRewinding && rewindBuffer != null
            var coreNs = 0L
            var emulated = 0
            if (rewinding) {
                setAudioMuted(true)
                val state = rewindBuffer?.stepBack()
                // Restoring a state shows nothing by itself: run one frame to produce the picture.
                if (state != null && bridge.nativeUnserializeFrom(state, state.size)) {
                    runCoreFrame()
                    emulated = 1
                }
            } else {
                val n = governor.achieved
                setAudioMuted(n > 1)
                repeat(n) {
                    val t0 = System.nanoTime()
                    runCoreFrame()
                    coreNs += System.nanoTime() - t0
                    emulated++
                    captureRewindState()
                }
            }
            emulatedFrames += emulated
            if (emulated > 0) {
                // Only the last frame of the tick is shown (HW cores rendered every frame into their FBO).
                if (gpu.active) presentGpu() else if (output.usesFrameSink(gpuActive = false)) presentFrame()
            }
            if (!rewinding && governor.achieved > 1 &&
                governor.onTick(System.nanoTime(), coreNs, frameNs)
            ) {
                Log.i(TAG, "Fast forward limited to ${governor.achieved}x (requested ${governor.requested}x)")
                _achievedSpeed.value = governor.achieved
            }
            pollRumble()

            // The game closed itself (in-game quit): end the session cleanly; SRAM is written on the way out.
            if (bridge.nativeShutdownRequested()) {
                Log.i(TAG, "Core requested shutdown: ending the session")
                _state.value = EngineState.Error(
                    strings.get(R.string.gameplay_engine_game_closed),
                    strings.get(R.string.gameplay_engine_game_closed_detail),
                )
                return
            }

            // The core may change its rate later (SET_SYSTEM_AV_INFO, e.g. PPSSPP 59.94 → 29.97).
            val newFps = bridge.nativeGetFps()
            if (newFps != rawFps) {
                rawFps = newFps
                fps = FramePacing.sanitize(newFps)
                frameNs = FramePacing.frameNs(newFps)
                Log.i(TAG, String.format(Locale.US, "Core frame rate changed: %.2f fps", fps))
            }

            // Deadline-based pacing: no drift accumulation, resync if we fall far behind.
            nextFrameAt += frameNs
            var now = System.nanoTime()
            if (now - nextFrameAt > frameNs * MAX_FRAMES_BEHIND) {
                nextFrameAt = now
            }
            while (running && nextFrameAt - now > 0) {
                LockSupport.parkNanos(nextFrameAt - now)
                now = System.nanoTime()
            }

            statsFrames++
            if (now - statsStart >= STATS_INTERVAL_NS) {
                val measured = statsFrames * 1_000_000_000.0 / (now - statsStart)
                val emulatedFps = emulatedFrames * 1_000_000_000.0 / (now - statsStart)
                Log.i(TAG, String.format(Locale.US, "Render FPS: %.1f (target %.2f) emulated: %.1f", measured, fps, emulatedFps))
                statsStart = now
                statsFrames = 0
                emulatedFrames = 0
            }
        }
        Log.i(TAG, "Emulation loop stopped")
    }

    /** One emulated frame with input grouping / turbo applied (emulation thread). */
    private fun runCoreFrame() {
        updateGroupedInput()
        if (pressWindowFrames > 0) {
            turboWasActive = turboMask != 0
        } else if (turboMask != 0) {
            turboWasActive = true
            applyInputMask() // frame N of the cycle is what this core frame sees
        } else if (turboWasActive) {
            turboWasActive = false
            applyInputMask() // turbo just stopped: make sure the plain held mask is what the core sees
        }
        bridge.nativeRunFrame()
        if (turboMask != 0) advanceTurbo()
        ranFrame = true
    }

    /** Applies the requested speed / rewind recording (allocates or frees the ring) before a tick. */
    private fun syncGameplayModes() {
        if (governor.requested != desiredSpeed) {
            governor.setRequested(desiredSpeed)
            _achievedSpeed.value = governor.achieved
        }
        val wantRewind = desiredRewindEnabled && rewindSupported && !rewindBroken
        if (wantRewind && rewindBuffer == null) {
            rewindBuffer = RewindBuffer(RewindBuffer.capacityFor(rewindStateSize))
            rewindFrames = 0
        } else if (!wantRewind && rewindBuffer != null) {
            rewindBuffer?.clear()
            rewindBuffer = null
        }
    }

    private fun captureRewindState() {
        val buffer = rewindBuffer ?: return
        if (++rewindFrames % RewindBuffer.CAPTURE_INTERVAL_FRAMES != 0L) return
        // The serialize size of some cores grows while playing: measure it again for every capture (cheap) so a
        // slot always fits, instead of failing every capture with "buffer too small" for the rest of the session.
        val size = bridge.nativeGetStateSize()
        if (size <= 0 || size > RewindBuffer.MAX_STATE_BYTES) {
            giveUpRewind("state size is now $size bytes")
            return
        }
        if (size != rewindStateSize) {
            Log.i(TAG, "Rewind state size changed: $rewindStateSize -> $size bytes")
            rewindStateSize = size
        }
        val dst = try {
            buffer.acquire(size)
        } catch (e: OutOfMemoryError) {
            // Out of heap for another slot: give up rewinding rather than crash the game.
            Log.w(TAG, "Rewind disabled: out of memory", e)
            rewindBuffer = null
            rewindBroken = true
            return
        }
        if (bridge.nativeSerializeInto(dst)) {
            rewindFailures = 0
            buffer.commit()
        } else {
            buffer.abort()
            if (++rewindFailures >= REWIND_MAX_FAILURES) giveUpRewind("the core failed to serialize $rewindFailures times in a row")
        }
    }

    /** Stops recording for the rest of the session; logs once (not per capture). */
    private fun giveUpRewind(reason: String) {
        Log.w(TAG, "Rewind stopped: $reason")
        rewindBuffer?.clear()
        rewindBuffer = null
        rewindBroken = true
    }

    private fun setAudioMuted(muted: Boolean) {
        if (muted == audioMuted) return
        audioMuted = muted
        bridge.nativeSetAudioMuted(muted)
    }

    /** Re-sends the last cheat list to the core (after load / reset / restart). */
    private fun applyCheats() {
        if (!cheatsSupported) return
        bridge.nativeCheatReset()
        desiredCheats.forEachIndexed { index, code -> bridge.nativeCheatSet(index, true, code) }
    }

    /** Once after load: what this game supports. Rewind needs a small state that serializes quickly. */
    private fun measureCapabilities(args: GameLaunchArgs): EngineCapabilities {
        var rewind = false
        if (!output.hwRender) {
            val size = bridge.nativeGetStateSize()
            if (size in 1..RewindBuffer.MAX_STATE_BYTES) {
                val buffer = ByteArray(size)
                var ok = bridge.nativeSerializeInto(buffer) // warm-up
                var avgNs = Long.MAX_VALUE
                if (ok) {
                    val t0 = System.nanoTime()
                    repeat(MEASURE_RUNS) { ok = ok && bridge.nativeSerializeInto(buffer) }
                    avgNs = (System.nanoTime() - t0) / MEASURE_RUNS
                }
                rewind = ok && RewindBuffer.isViable(size, avgNs)
                rewindStateSize = size
                Log.i(TAG, "Rewind probe: state $size bytes, serialize ${avgNs / 1000} us -> $rewind")
            } else {
                Log.i(TAG, "Rewind off: state size $size")
            }
        }
        rewindSupported = rewind
        return EngineCapabilities(reset = true, maxSpeed = 4, rewind = rewind, cheats = args.cheatsSupported)
    }

    private fun applyCoreOptions(args: GameLaunchArgs) {
        try {
            for ((key, value) in coreOptionsFor(args)) {
                if (!bridge.nativeSetCoreOption(key, value)) Log.i(TAG, "Core option $key not applied")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Applying core options failed", e)
        }
    }

    private fun pollRumble() {
        val out = rumble ?: return
        try {
            out.update(bridge.nativeGetRumble(0))
        } catch (e: Exception) {
            Log.w(TAG, "Rumble failed", e)
        }
    }

    private fun stopRumble() {
        try {
            rumble?.stop()
        } catch (_: Exception) {
        }
    }

    private fun applyPause(paused: Boolean) {
        if (paused) stopRumble()
        bridge.nativeSetPaused(paused)
        appliedPaused = paused
    }

    private fun publishRunning() {
        _state.value = EngineState.Running(coreName, appliedPaused)
    }

    private fun fail(title: String) {
        val detail = bridge.nativeGetLastError().ifEmpty { strings.get(R.string.gameplay_engine_unknown_error) }
        Log.e(TAG, "$title — $detail")
        _state.value = EngineState.Error(title, detail)
    }

    private fun presentGpu() {
        if (gpu.present()) return
        _gpuRendering.value = false
        if (output.gpuFailureIsFatal) {
            failGpu("present")
            return
        }
        Log.w(TAG, "GPU present failed; falling back to software output")
        // The GPU path does not consume the frame flag, so the next software tick delivers the frame.
        sinkAttached = true
    }

    /** HW cores have no software fallback: report the failure and stop the loop. */
    private fun failGpu(step: String) {
        Log.e(TAG, "GPU $step failed for a hardware-rendered core — ${bridge.nativeGetLastError()}")
        gpuFatal = true
        _state.value = EngineState.Error(
            strings.get(R.string.gameplay_engine_gpu_failed),
            strings.get(R.string.gameplay_engine_gpu_failed_detail),
        )
    }

    private fun attachSurface(surface: Surface) {
        if (!loopActive || currentSurface !== surface) return // superseded: a later command settles the state
        val ok = gpu.attach(surface)
        gpuAttachPending = false
        _gpuRendering.value = ok
        if (ok) {
            applyShader()
            gpu.applyRotation(desiredRotation)
            presentOnce = true
        } else if (output.gpuFailureIsFatal) {
            failGpu("surface attach")
        } else {
            if (!loggedGpuFailure) {
                loggedGpuFailure = true
                Log.w(TAG, "GPU surface attach failed; using the software FrameSink path")
            }
            sinkAttached = true
        }
    }

    private fun detachSurface() {
        gpu.detach()
        gpuAttachPending = false
        _gpuRendering.value = false
    }

    private fun applyShader() {
        val source = desiredShader // null = back to the built-in Sharp output
        if (!gpu.applyShader(source)) Log.w(TAG, "Shader '${source?.presetId ?: "(built-in)"}' was not applied")
    }

    /** Software path: fetch the newest frame and hand it to the sink (unless a GPU attach is pending). */
    private fun presentFrame() {
        if (fetchFrame() && !gpuAttachPending) deliverFrame()
    }

    /** Fetches the newest core frame into [pixels] without delivering it; true when it holds a new valid frame. */
    private fun fetchFrame(): Boolean {
        if (!bridge.nativeGetFrameBuffer(pixels)) {
            val needed = bridge.nativeGetFrameWidth() * bridge.nativeGetFrameHeight()
            if (needed <= pixels.size) return false    // no new frame this tick
            pixels = IntArray(needed)                  // first frame or resolution grew
            hasFrame = false
            if (!bridge.nativeGetFrameBuffer(pixels)) return false
        }

        val w = bridge.nativeGetFrameWidth()
        val h = bridge.nativeGetFrameHeight()
        if (w <= 0 || h <= 0 || w * h > pixels.size) {
            hasFrame = false
            return false
        }
        frameWidth = w
        frameHeight = h
        frameAspect = bridge.nativeGetAspectRatio().takeIf { it > 0f } ?: (w.toFloat() / h)
        hasFrame = true
        return true
    }

    /** [pixels] always holds the latest frame between frames, so it doubles as the thumbnail source. */
    private fun deliverFrame() {
        synchronized(sinkLock) {
            val target = sink ?: return
            try {
                target.onFrame(pixels, frameWidth, frameHeight, frameAspect)
            } catch (e: Exception) {
                Log.w(TAG, "FrameSink threw", e)
            }
        }
    }

    private fun captureThumbnail(): Bitmap? {
        // GPU mode / HW cores never copy frames to the CPU, so read the latest one now (FBO readback for HW).
        if (output.readsBackForThumbnail(gpu.active)) fetchFrame()
        if (!hasFrame) return null
        val w = frameWidth
        val h = frameHeight
        val opaque = IntArray(w * h) { pixels[it] or OPAQUE }
        return try {
            Bitmap.createBitmap(opaque, w, h, Bitmap.Config.ARGB_8888)
        } catch (e: RuntimeException) {
            Log.w(TAG, "Thumbnail capture failed", e)
            null
        }
    }

    // ------------------------------------------------------------------ SRAM (emulation thread)

    private fun restoreSram() {
        val file = sramFile ?: return
        if (!file.isFile) return
        try {
            val data = file.readBytes()
            if (bridge.nativeSetSram(data)) {
                lastSram = data
                Log.i(TAG, "Restored ${data.size} bytes of SRAM from ${file.name}")
            } else {
                // Size mismatch (or no SRAM): keep the user's file instead of overwriting it on exit.
                val backup = File(file.path + ".bak")
                backup.delete()
                val moved = file.renameTo(backup)
                Log.w(TAG, "SRAM in ${file.name} rejected by the core; moved aside=$moved")
            }
        } catch (e: IOException) {
            Log.w(TAG, "Could not read ${file.name}", e)
        }
    }

    private fun writeSramIfChanged() {
        val file = sramFile ?: return
        val data = bridge.nativeGetSram() ?: return
        if (lastSram?.contentEquals(data) == true) return
        try {
            AtomicFiles.write(file, data)
            lastSram = data
            Log.i(TAG, "Wrote ${data.size} bytes of SRAM to ${file.name}")
        } catch (e: IOException) {
            Log.e(TAG, "Could not write ${file.name}", e)
        }
    }

    // ------------------------------------------------------------------ Command queue

    /** Queues [block] for the emulation thread; null when the engine no longer accepts commands. */
    private fun enqueue(block: () -> Unit): Command<Unit>? {
        val command = Command(block, Unit)
        val accepted = synchronized(commandLock) {
            if (acceptingCommands) commands.add(command) else false
        }
        if (!accepted) return null
        wakeEmulationThread()
        return command
    }

    private suspend fun <T> submit(fallback: T, block: () -> T): T {
        val command = Command(block, fallback)
        val accepted = synchronized(commandLock) {
            if (acceptingCommands) commands.add(command) else false
        }
        if (!accepted) return fallback
        wakeEmulationThread()
        return command.result.await()
    }

    private fun drainCommands() {
        while (true) {
            val command = commands.poll() ?: return
            command.run()
        }
    }

    /** Stops accepting commands, then runs (game still loaded) or abandons whatever is queued. */
    private fun closeCommandQueue(runPending: Boolean) {
        synchronized(commandLock) { acceptingCommands = false }
        while (true) {
            val command = commands.poll() ?: return
            if (runPending) command.run() else command.abandon()
        }
    }

    private fun wakeEmulationThread() {
        thread?.let(LockSupport::unpark)
    }

    private class Command<T>(private val block: () -> T, private val fallback: T) {
        val result = CompletableDeferred<T>()

        fun run() {
            val value = try {
                block()
            } catch (e: Exception) {
                Log.e(TAG, "Engine command failed", e)
                fallback
            }
            result.complete(value)
        }

        fun abandon() {
            result.complete(fallback)
        }
    }

    private companion object {
        const val TAG = "RetroNest-Engine"
        const val THREAD_NAME = "RetroNest-Emulation"
        const val MAX_FRAMES_BEHIND = 4
        const val STATS_INTERVAL_NS = 5_000_000_000L
        const val PAUSED_PARK_NS = 8_000_000L
        const val OPAQUE = 0xFF000000.toInt()
        const val DETACH_TIMEOUT_MS = 500L
        const val MAX_AUDIO_BURSTS = 16
        const val MAX_PRESS_WINDOW = 2
        const val MEASURE_RUNS = 5
        /** Consecutive failed rewind captures after which recording stops for the session. */
        const val REWIND_MAX_FAILURES = 3

        /** One native session at a time: LibretroBridge holds a single global core. */
        val SESSION = Semaphore(1)

        fun joinUninterruptibly(t: Thread) {
            var interrupted = false
            while (true) {
                try {
                    t.join()
                    break
                } catch (_: InterruptedException) {
                    interrupted = true
                }
            }
            if (interrupted) Thread.currentThread().interrupt()
        }
    }
}

/** Normalized 0..1 -> libretro pointer range -32767..32767 (clamped). */
internal fun pointerToLibretro(v: Float): Int = ((v.coerceIn(0f, 1f) * 2f - 1f) * 32767f).toInt()
