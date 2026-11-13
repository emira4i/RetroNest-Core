package com.retronest.corehost.engine

/**
 * Frame-accurate grouping of newly pressed buttons (see AppSettings.pressWindowFrames).
 * Pure and single-threaded: call [next] exactly once per emulated frame, before the core runs.
 *
 * - New presses (requested and not applied) are held back; the first one opens the window, later ones
 *   join it. When [windowFrames] frames have elapsed since it opened, every pending bit is applied together.
 * - Releases are immediate. A pending bit released before the window closes is still delivered for
 *   exactly one frame when the window closes ("tapped").
 * - Already-applied bits are never delayed.
 * - [windowFrames] == 0 is a passthrough.
 */
class PressGrouper(var windowFrames: Int = 0) {
    private var applied = 0
    private var pending = 0
    private var tapped = 0
    private var open = false
    private var elapsed = 0

    /** Forgets all state; [held] is treated as already applied (no re-delay). */
    fun reset(held: Int = 0) {
        applied = held
        pending = 0
        tapped = 0
        open = false
        elapsed = 0
    }

    fun next(requested: Int): Int {
        if (windowFrames <= 0) {
            reset(requested)
            return requested
        }
        applied = applied and requested
        val released = pending and requested.inv()
        tapped = tapped or released
        pending = pending and requested
        val fresh = requested and applied.inv() and pending.inv()
        val wasOpen = open
        if (wasOpen) elapsed++
        if (fresh != 0) {
            pending = pending or fresh
            if (!open) {
                open = true
                elapsed = 0
            }
        }
        var tappedNow = 0
        if (open && elapsed >= windowFrames) {
            applied = applied or pending
            tappedNow = tapped
            pending = 0
            tapped = 0
            open = false
            elapsed = 0
        }
        return applied or tappedNow
    }
}
