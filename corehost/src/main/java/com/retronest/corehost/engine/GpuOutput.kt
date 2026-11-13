package com.retronest.corehost.engine


/**
 * T2b: where frames go for the loaded core.
 *
 * - Software cores ([hwRender] false) keep the Phase 4C behaviour: GPU path when a surface is
 *   attached, FrameSink otherwise, and a GPU attach / present failure falls back to the FrameSink.
 * - Hardware-rendered cores (LibretroBridge.nativeUsesHwRender: N64 / PSP) draw into a native FBO.
 *   Only the GPU path can show it: no FrameSink at all, and a GPU failure ends the session with
 *   a "GPU rendering failed" error (see LibretroEmulationEngine.failGpu) instead of silently showing nothing.
 */
internal class OutputPolicy(val hwRender: Boolean) {
    /** Deliver frames (and paused repaints) to the FrameSink while the GPU path is not active. */
    fun usesFrameSink(gpuActive: Boolean): Boolean = !gpuActive && !hwRender

    /** Read the frame back for a thumbnail: GPU mode and HW cores never copy frames to the CPU. */
    fun readsBackForThumbnail(gpuActive: Boolean): Boolean = gpuActive || hwRender

    /** A failed GPU attach / present is fatal (true) or falls back to software (false). */
    val gpuFailureIsFatal: Boolean get() = hwRender
}

/** Frame period for the pacing loop; cores may change their fps later (SET_SYSTEM_AV_INFO). */
internal object FramePacing {
    const val DEFAULT_FPS = 60.0
    private val VALID_FPS = 1.0..240.0

    fun sanitize(fps: Double): Double = fps.takeIf { it in VALID_FPS } ?: DEFAULT_FPS

    fun frameNs(fps: Double): Long = (1_000_000_000.0 / sanitize(fps)).toLong()
}

/** Native GPU operations (the LibretroBridge Phase 4C calls); emulation thread only. */
internal interface GpuOps<S : Any> {
    fun setSurface(surface: S?): Boolean
    fun setShader(source: ShaderSource): Boolean

    /** Back to the built-in Sharp output. */
    fun clearShader()
    fun setRotation(degrees: Int)
    fun present(): Boolean
}

/**
 * GPU/software output state machine. Not thread-safe: used from the emulation thread only.
 *
 * - [attach] success makes it [active] (frames are drawn by [present], the FrameSink is skipped).
 * - [attach] failure stays software.
 * - [present] returning false (surface lost) releases the native surface and drops to software
 *   until the next successful [attach].
 * - [detach] always releases the native surface.
 */
internal class GpuOutput<S : Any>(private val ops: GpuOps<S>) {
    var active = false
        private set
    private var nativeAttached = false

    /** True when GPU rendering is now active. The caller applies shader/rotation afterwards. */
    fun attach(surface: S): Boolean {
        val ok = try {
            ops.setSurface(surface)
        } catch (e: Exception) {
            false
        }
        nativeAttached = ok
        active = ok
        return ok
    }

    fun detach() {
        active = false
        if (!nativeAttached) return
        nativeAttached = false
        try {
            ops.setSurface(null)
        } catch (_: Exception) {
        }
    }

    fun applyShader(source: ShaderSource?): Boolean {
        if (!active) return false
        return try {
            if (source == null) {
                ops.clearShader()
                true
            } else {
                ops.setShader(source)
            }
        } catch (_: Exception) {
            false
        }
    }

    fun applyRotation(degrees: Int) {
        if (active) ops.setRotation(degrees)
    }

    /** Draws the newest frame. False when inactive, or when the surface was lost (then drops to software). */
    fun present(): Boolean {
        if (!active) return false
        val ok = try {
            ops.present()
        } catch (_: Exception) {
            false
        }
        if (!ok) detach()
        return ok
    }
}
